## Context

openGauss SQL 限流当前通过 `pg_catalog.gs_sql_limit` 系统表 + 自建共享内存缓存实现规则管理，后台线程 `SqlLimitMain` 周期扫描同步。问题：规则元数据双写导致一致性复杂，备机同步有延迟，更新/删除时活跃 SQL 并发计数释放不可靠，并发检查与计数之间存在竞态窗口。

本方案为增量实现，不修改现有 `gs_sql_limit` 表和 `SqlLimitMain` 线程，便于整体迁移到闭源透明多写（PDB 隔离）仓库。当前 `pdbOid` 固定填 0，迁移后按实际 PDB OID 填充。

## Goals / Non-Goals

**Goals:**

- 使用 syscache 统一规则元数据，依赖 catalog invalidation 传播变更，取消自建规则同步
- 运行时统计和并发计数保留在共享内存 HTAB 中，用 CAS 原子操作消除竞态
- `ruleVersion` 机制解决规则更新/删除时旧版本并发计数的安全释放
- Fast Path 摘要在无规则时跳过 syscache 查询
- 保持管理函数接口兼容
- 所有代码为增量，可直接移植到闭源仓

**Non-Goals:**

- 不修改现有 `gs_sql_limit` 表
- 不修改 `SqlLimitMain` 后台线程
- 不做分布式模式（`ENABLE_MULTIPLE_NODES`）的独立功能验证。SQL 限流为集中式特性，新增的 syscache（`GSSQLLIMITRULE`）通过 `#ifndef ENABLE_MULTIPLE_NODES` 宏隔离，在分布式编译中不包含相关枚举值和 cacheinfo 条目，确保分布式构建不受影响。集中式编译下 openGauss 默认仍定义 `PGXC`，当前 `SysCacheSize` 为 125；新增 1 个 syscache 后为 126，cache id 最大为 125，仍满足 shared invalidation message 中 `int8 id` / `cacheId < CHAR_MAX` 的限制。后续若继续新增集中式 syscache，需要重新评估 127 上限
- 不从旧表迁移数据
- 不解决 SQL 解析语义误匹配问题（关键词匹配仍为字符串顺序匹配）

## Decisions

### D1: 单 syscache + 组合唯一索引 vs 多 syscache

**选择**: 一个 `GSSQLLIMIT` syscache，组合唯一索引 `(enable, limit_type, hash, limit_id)`，通过前缀 key 查询覆盖不同场景。

**理由**: 多 syscache 占用更多共享内存和维护成本。组合索引的前缀匹配机制天然支持 `SearchSysCacheList2`（关键词场景）和 `SearchSysCacheList3`（SQLID 场景），一个 syscache 即可满足。

**替代方案**: 为 SQLID 和关键词各配一个 syscache（3 个 syscache 总计）。资源占用高，且需要维护多个 cacheinfo 条目。

### D2: limit_type 使用 text 类型

**选择**: `limit_type` 保持 `text` 类型（`'sqlId'`/`'select'`/`'insert'`/`'update'`/`'delete'`），作为 syscache key。

**理由**: 保持与现有接口一致，避免引入额外枚举映射。`SearchSysCacheList` 接受 `Datum` 参数，text 类型可直接传递。

**替代方案**: 用 `int4 limit_kind` 替代 text。节省 syscache 比较开销，但增加枚举定义和维护成本，且与现有接口不兼容。

### D3: stats key 包含 ruleVersion

**选择**: stats key 为 `(pdbOid, limitId, ruleVersion)`。

**理由**: 规则 update 后递增 `ruleVersion`，新 SQL 使用新版本 stats entry（从 0 开始），旧版本保留至 `currConcurrency == 0` 后清理。避免新旧规则统计混淆和并发计数错误释放。

**替代方案**: 不包含 `ruleVersion`，update 时原地重置计数。风险：正在执行的旧规则 SQL 释放时会把并发减到新规则的计数上。

### D4: CAS 原子并发预约

**选择**: 使用 `pg_atomic_compare_exchange_u64` 做 CAS 预约 `currConcurrency`。

**理由**: 当前实现先判断 `>= maxConcurrency` 再增加并发，两步之间有竞态窗口，多会话可能同时通过检查。CAS 将判断和增加合并为原子操作。

### D5: Fast Path 摘要仅保留 dirty 和 activeRuleCount

**选择**: `SqlLimitFastPathEntry` 只包含 `pdbOid`、`dirty`、`activeRuleCount`。

**理由**: 完整的限流判断需要在 syscache 中逐条检查时间窗口、用户范围等条件，摘要无法完整表达。只需区分"可能有规则"和"确认无规则"即可，避免摘要过于复杂导致同步困难。

**替代方案**: 在摘要中增加 `activeKindMask` 和 `hasSqlidRule` 位图。增加复杂度但收益有限——只要"可能有规则"就需要进入完整判断。

### D6: 禁止显式事务中操作规则

**选择**: 管理函数入口检查 `IsTransactionBlock()`，显式事务中调用则报错。

**理由**: 简化一致性模型，规则变更立即提交，其他会话通过 catalog invalidation 即时可见。避免长事务持锁影响规则变更并发度。

### D7: limit_id 全局单调递增

**选择**: `limit_id` 从共享原子计数生成，重启后从系统表最大值初始化。

**理由**: 保证索引有序性和优先级判定确定性。`limit_id` 同时作为索引尾列保证唯一性。

### D8: hash 列使用 hash_any

**选择**: `hash` 列使用通用 `hash_any` 函数计算，返回 `uint32` 存入 `int8` 列。

**理由**: 当前对 `unique_sql_id`（int8）算 hash，后续需支持对字符串字段算 hash。`hash_any` 接受通用字节流输入，int8 和字符串均可处理，避免后续扩展时更换 hash 函数。

**替代方案**: `hashint8` 仅支持 int8 输入，无法满足后续字符串 hash 需求。

## Risks / Trade-offs

- **[syscache 资源占用]** → 组合索引 4 列 cacheinfo，单 syscache 预分配 128 slot。规则表上限 1000 条，资源可控。
- **[syscache id 上限]** → shared invalidation message 使用 `int8 id` 表示 catcache/syscache id，非负 id 需小于 `CHAR_MAX`。集中式当前新增 1 个 syscache 后仍未超过限制，但余量有限，后续新增 syscache 需优先复核该上限。
- **[hash 冲突]** → 使用 `hash_any` 计算 hash 值（当前对 `unique_sql_id` 算 hash，后续可扩展对字符串算 hash），`hash_any` 返回 `uint32`，存入 `int8` 列。hash 冲突时 `SearchSysCacheList3` 返回多条候选，需逐条比对。hash 冲突概率低，额外开销可接受。
- **[keyword 文本匹配开销]** → 关键词匹配为逗号分隔字符串顺序查找，规则多时有一定开销。当前规模下可接受。
- **[stats entry 累积]** → 规则频繁更新/删除时旧 stats entry 保留至 `currConcurrency == 0`。采用惰性清理：`UnlimitCurrentQuery` 释放并发后若 `currConcurrency == 0`，检查 syscache 中是否仍存在该 `(limitId, ruleVersion)`，不存在则删除 entry。无需额外后台线程或周期清理。
- **[新线程首次校验开销]** → 每个线程首次使用某 PDB 的 fast path 时需刷新摘要。线程池场景下开销分摊到首次请求，后续请求无额外开销。

## Migration Plan

### 升级

1. `CREATE TABLE pg_catalog.gs_sql_limit_rule` 及所有字段
2. `CREATE UNIQUE INDEX gs_sql_limit_enable_type_hash_id_index`
3. 补充 syscache cacheinfo 注册
4. 创建管理函数（`_v2` 后缀）
5. 更新 rollback SQL

### 回滚

1. 删除管理函数（`_v2` 后缀）
2. `DROP INDEX gs_sql_limit_enable_type_hash_id_index`
3. `DROP TABLE pg_catalog.gs_sql_limit_rule`
4. 移除 syscache 注册

### 兼容性

- 对外函数接口使用 `_v2` 后缀（`gs_create/update/delete/select_sql_limit_v2`），避免与旧函数同名冲突
- 旧表 `gs_sql_limit` 和 `SqlLimitMain` 线程不受影响
- 不从旧表迁移数据，新旧系统独立运行

## Open Questions

_(全部已解决)_
