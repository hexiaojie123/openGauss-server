# SQL 限流 syscache 重构设计方案

## 1. 背景

本方案在开源 openGauss 代码仓中实现，作为参考设计，后续迁移到闭源透明多写（PDB 隔离）代码仓中使用。因此：

- 所有改动为**增量代码**，不修改现有文件中的已有逻辑，便于整体移植。
- 新建 `pg_catalog.gs_sql_limit_rule` 表，不修改原 `gs_sql_limit` 表。
- 现有 `SqlLimitMain` 后台线程及相关代码**不做调整**，迁移时直接拿增量代码。
- 当前 `pdbOid` 固定填 0，迁移到透明多写环境后再按实际 PDB OID 填充。

当前 SQL 限流已经支持基于 `unique_sql_id` 和 SQL 关键词的并发控制。现有实现以 `pg_catalog.gs_sql_limit` 作为持久化规则表，同时在实例级共享内存中维护一套自建规则缓存：

- `g_instance.sqlLimit_cxt.uniqueSqlIdLimits`：按 `unique_sql_id` 匹配规则。
- `g_instance.sqlLimit_cxt.keywordsLimits[]`：按 SQL 类型保存关键词规则链表。
- `g_instance.sqlLimit_cxt.limitRegistry`：按 `limit_id` 管理所有规则。
- `SqlLimitMain` 后台线程：初始化规则缓存，在备机上周期扫描系统表同步变化，并定期清理失效规则。

运行时入口主要在 `LimitCurrentQuery()` 和 `UnlimitCurrentQuery()`：

- SQL 开始执行前，按 `unique_sql_id` 和关键词匹配限流规则。
- 命中后检查 `max_concurrency`，超过则拒绝 SQL。
- 接受 SQL 后增加当前并发，SQL 结束时减少当前并发。

现有实现的问题：

- 规则元数据同时存在于系统表和自建共享内存缓存，事务语义、回滚语义和备机同步逻辑复杂。
- 后台线程通过定期扫描维护规则缓存，存在延迟和额外维护成本。
- `gs_create_sql_limit()` / `gs_update_sql_limit()` 同时修改系统表和内存缓存，容易出现两边状态不一致。（新方案使用 `_v2` 后缀函数避免冲突）
- 当前 `ShouldRejectQuery()` 先判断再增加并发，存在并发窗口，多个会话可能同时通过检查。
- `GSSQLLIMITRULE` 已在 `syscache.h` 中声明，但 `syscache.cpp` 未配置对应 `cacheinfo[]`，当前不能作为有效 syscache 使用。

## 2. 设计目标

1. 使用 syscache 承载 SQL 限流规则元数据。
2. 依赖系统 catalog invalidation 传播规则新增、更新、删除，取消自建规则同步流程。
3. 保留共享内存中的运行时统计和并发计数，避免把高频变化状态写入系统表或 syscache。
4. 保持管理函数接口兼容，使用 `_v2` 后缀避免与旧函数同名冲突。
5. 修复并发检查与并发计数之间的竞态。
6. 简化备机场景规则可见性，不再依赖 `SqlLimitMain` 周期扫描。
7. 规则表 `gs_sql_limit_rule` 上限 1000 条，超出时 `gs_create_sql_limit_v2` 报错拒绝。

## 3. 总体架构

重构后分为两层：

### 3.1 规则层

规则层由 `pg_catalog.gs_sql_limit_rule` 和 syscache 管理。

规则包括：

- `limit_id`（全局单调递增）
- `limit_name`
- `enable`
- `work_node`
- `max_concurrency`
- `start_time`
- `end_time`
- `limit_type`
- `hash`
- `unique_sql_id`
- `keyword`
- `rule_version`
- `users`

运行时通过 syscache 读取规则，不再从 `g_instance.sqlLimit_cxt` 中读取规则对象。

### 3.2 状态层

统计和并发计数不放 syscache，放在本节点共享内存 hash 表中。

stats key：

```c
typedef struct SqlLimitStatsKey {
    Oid pdbOid;
    uint64 limitId;
    uint64 ruleVersion;
} SqlLimitStatsKey;
```

stats value：

```c
typedef struct SqlLimitStatsEntry {
    SqlLimitStatsKey key;
    pg_atomic_uint64 hitCount;
    pg_atomic_uint64 rejectCount;
    pg_atomic_uint64 currConcurrency;
} SqlLimitStatsEntry;
```

状态层以 `(pdbOid, limitId, ruleVersion)` 为 key。当前开源环境 `pdbOid` 固定填 0，迁移到透明多写时再按实际 PDB OID 填充。规则新增时不需要预创建状态，运行时命中规则后按需创建。规则更新后递增 `ruleVersion`，新版本从 0 开始计数，旧版本保留至 `currConcurrency == 0` 后清理。规则删除后，旧 stats entry 保留至 `currConcurrency == 0` 且规则已不存在时清理。Stats entry 采用惰性清理：`UnlimitCurrentQuery` 释放并发后若 `currConcurrency == 0`，检查 syscache 中是否仍存在该 `(limitId, ruleVersion)` 对应的规则，不存在则删除 stats entry。

## 4. Catalog 设计

### 4.1 `gs_sql_limit_rule` 表设计

新建 `pg_catalog.gs_sql_limit_rule` 表，包含以下字段：

```c
int8 limit_id;         -- 规则唯一标识，全局单调递增，用于保证优先级（越大越高）和索引有序性
name limit_name;       -- 规则名称
bool enable;           -- 规则是否生效，true 生效，false 暂停
int4 work_node;        -- 生效节点，0 表示当前节点
int4 max_concurrency;  -- 最大并发数
timestamp start_time;  -- 规则生效起始时间
timestamp end_time;    -- 规则生效结束时间
text limit_type;       -- 规则类型（'sqlId'/'select'/'insert'/'update'/'delete'），同时作为 syscache 查询 key
int8 hash;             -- syscache 快速定位用，使用 hash_any 计算，SQLID 类型填 unique_sql_id 的 hash 值（uint32 转 int8），其他类型填 0，后续可扩展对字符串字段算 hash
int8 unique_sql_id;    -- 源自原 limit_opt 拆分（SQLID 部分），SQLID 类型规则的匹配 key，非 SQLID 规则填 0
text keyword;          -- 源自原 limit_opt 拆分（关键词部分），逗号分隔的关键词列表，匹配时按顺序查找，SQLID 类型为空
int8 rule_version;     -- 规则版本号，创建时为 1，每次 update 递增，用于 stats key 隔离新旧版本
text users;            -- 生效用户范围
```

### 4.2 索引设计

syscache 底层必须依赖唯一索引。使用一个组合唯一索引，同时满足多种查询场景，且只占用一个 syscache 资源。

```c
DECLARE_UNIQUE_INDEX(gs_sql_limit_enable_type_hash_id_index,
    ..., on gs_sql_limit_rule using btree(enable bool_ops, limit_type text_ops, hash int8_ops, limit_id int8_ops));
```

说明：

- `(enable, limit_type, hash, limit_id)` 四列组合唯一索引。
- `enable` 作为首列，查询时自动过滤未启用的规则。
- `limit_type` 作为第二列，按 SQL 类型缩小候选范围。
- `hash` 作为第三列，SQLID 类型规则可通过 hash 快速定位。
- `limit_id` 保证唯一性，同时 `limit_id` 全局单调递增确保索引有序。

## 5. Syscache 设计

只占用一个 syscache，通过组合索引的前缀匹配满足不同查询场景。

在 `src/include/utils/syscache.h` 中补充 cache id：

```c
GSSQLLIMIT
```

在 `src/common/backend/utils/cache/syscache.cpp` 的 `cacheinfo[]` 中增加：

```c
{GsSqlLimitRuleRelationId,
    GsSqlLimitRuleEnableTypeHashIdIndex,
    4,
    {Anum_gs_sql_limit_enable,
     Anum_gs_sql_limit_limit_type,
     Anum_gs_sql_limit_hash,
     Anum_gs_sql_limit_limit_id},
    128}
```

匹配规则时使用前缀 key 查询：

- SQLID 规则：`SearchSysCacheList3(GSSQLLIMIT, BoolGetDatum(true), CStringGetTextDatum("sqlId"), Int64GetDatum(hash))`，通过 hash 快速定位。
- 关键词规则：`SearchSysCacheList2(GSSQLLIMIT, BoolGetDatum(true), CStringGetTextDatum(limitType))`，枚举该类型下所有启用的规则。
- 单条规则查询（`gs_select_sql_limit_v2`）直接扫描 catalog 表，不经过 syscache。

## 6. 运行时匹配流程

### 6.1 SQL 开始执行

`LimitCurrentQuery(commandTag, queryString)` 调整为：

1. 快速跳过：
   - `enable_sql_limit = off`
   - 当前事务已 abort
   - 当前用户是 superuser
   - `commandTag` 或 `queryString` 为空
2. 根据 `commandTag` 计算 `limit_type`（text）。
3. 根据 `u_sess->unique_sql_cxt.unique_sql_id` 计算 hash，通过 `SearchSysCacheList3(GSSQLLIMIT, true, "sqlId", hash)` 查 SQLID 候选规则。
4. 通过 `SearchSysCacheList2(GSSQLLIMIT, true, limitType)` 查当前 SQL 类型下的关键词候选规则。
5. 对候选规则逐条做有效性判断：
   - 时间窗口
   - 主备节点
   - 用户范围
   - SQLID 或关键词命中（按 `keyword` 中逗号分隔的关键词顺序匹配）
6. 按 6.2 优先级规则选出最终命中的一条规则，短路返回。
7. 对命中规则做原子并发预约。按 `(pdbOid, limitId, ruleVersion)` 在共享内存 hash 中查找或创建 `SqlLimitStatsEntry`，CAS 预约 `currConcurrency`。
8. 将预约成功的 `(pdbOid, limitId, ruleVersion)` 记录到 `u_sess->sqlLimit_ctx.limitSqls`，SQL 结束时按此 key 释放并发。

### 6.2 规则优先级

一个 SQL 只生效一条规则，采用短路机制，命中即返回。优先级规则：

1. **类型优先级**：SQLID 规则优先于关键词规则。先查 SQLID 候选，命中则直接使用，不再查关键词规则。
2. **关键词规则内部优先级**：
   - `keyword` 为空（无关键词限制）的规则优先级最高，匹配该类型下所有 SQL。
   - `keyword` 非空时，`limit_id` 越小优先级越高。

### 6.3 原子并发预约

当前逻辑先判断 `currConcurrency >= maxConcurrency`，接受后再增加并发，存在竞态。重构时改为 CAS 预约：

```c
static bool TryReserveSqlLimit(SqlLimitStatsEntry *entry, uint64 maxConcurrency)
{
    uint64 oldValue;
    uint64 newValue;

    do {
        oldValue = pg_atomic_read_u64(&entry->currConcurrency);
        if (oldValue >= maxConcurrency) {
            return false;
        }
        newValue = oldValue + 1;
    } while (!pg_atomic_compare_exchange_u64(&entry->currConcurrency, &oldValue, newValue));

    return true;
}
```

每个 SQL 只生效一条规则，预约逻辑简化为：

1. 对命中规则做 CAS 预约 `currConcurrency`。
2. 预约失败则拒绝 SQL，增加对应规则的 `rejectCount`。

### 6.4 SQL 结束执行

`UnlimitCurrentQuery()` 不再读取规则元数据，只处理 session 中记录的 `(pdbOid, limitId, ruleVersion)`：

1. 遍历 `u_sess->sqlLimit_ctx.limitSqls`。
2. 按 `(pdbOid, limitId, ruleVersion)` 在 stats HTAB 中查找 `SqlLimitStatsEntry`。
3. 原子减少 `currConcurrency`。
4. 若释放后 `currConcurrency == 0`，检查 syscache 中是否仍存在该 `(limitId, ruleVersion)` 对应的规则，不存在则删除 stats entry（惰性清理）。
5. 清空 session 记录。

即使规则已被更新或删除，也能按 session 记录的版本正确释放此前预约的并发。

## 7. 快速判断（Fast Path）

为避免无规则时每条 SQL 都查 syscache，在 `g_instance.sqlLimit_cxt.fastPathStates` 中按 PDB 维护摘要。

### 7.1 摘要数据结构

```c
typedef struct SqlLimitFastPathEntry {
    Oid pdbOid;
    pg_atomic_uint32 dirty;
    pg_atomic_uint32 activeRuleCount;
} SqlLimitFastPathEntry;
```

语义：

- `dirty = 1`：摘要不可信，需要检查或刷新。
- `dirty = 0 && activeRuleCount = 0`：当前 PDB 确认无规则，可以跳过 SQL limit。
- `dirty = 0 && activeRuleCount > 0`：当前 PDB 存在规则，需要进入 SQL limit 判断。

新建 PDB entry 初始值：`dirty = 1`，其余字段为 0。

当前开源环境 `pdbOid` 固定填 0，迁移到透明多写时再按实际 PDB 维护。

### 7.2 Syscache Callback

注册 `GSSQLLIMIT` 的 syscache callback：

```c
CacheRegisterThreadSyscacheCallback(GSSQLLIMIT, SqlLimitSyscacheCallback, (Datum)0);
```

LSC 开启时注册到 thread/LSC invalidation context；非 LSC 时自动退化到 session callback。

callback 只做轻量标脏：

```c
static void SqlLimitSyscacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    MarkSqlLimitFastPathDirty(CurrentPdbOidOrAll());
}
```

如果 callback 中无法可靠获取 PDB，则保守标记所有 PDB summary dirty。

### 7.3 新线程首次校验

为解决"新线程创建前本节点已收到但尚未消费失效消息"的问题，每个线程第一次使用某个 PDB 的 SQL limit fast path 时，要校验一次当前规则状态。

线程本地记录已验证 PDB 集合 `verifiedPdbSet`。

首次进入某 PDB：

```text
RefreshSqlLimitFastPath(pdbOid)
  -> 查/扫当前 PDB 的 gs_sql_limit_rule
  -> 计算 activeRuleCount
  -> dirty = 0
  -> 标记本线程已验证该 PDB
```

后续 SQL 直接读取 `g_instance` 中的 PDB summary。

### 7.4 SQL 执行入口

替换当前 `postgres.cpp` 中的入口判断：

```c
if (g_instance.sqlLimit_cxt.entryCount > 0) {
    LimitCurrentQuery(...);
}
```

改为：

```c
if (SqlLimitNeedCheck(commandTag)) {
    LimitCurrentQuery(commandTag, queryString);
}
```

`SqlLimitNeedCheck()` 逻辑：

1. 幂等注册 syscache callback。
2. 获取当前 PDB fast path entry。
3. 如果当前线程首次使用该 PDB，刷新 summary。
4. 如果 `dirty = 0` 且 `activeRuleCount = 0`，返回 false。
5. 否则返回 true。

## 8. 管理函数调整

### 8.1 `gs_create_sql_limit_v2`

调整点：

- 不再调用 `CreateSqlLimit()` 创建规则内存对象。
- 创建前检查 `gs_sql_limit_rule` 中当前规则数量，若 >= 1000 则报错拒绝。
- 校验参数后只插入 `gs_sql_limit_rule`。
- 填充 `enable`（默认 `true`）、`hash`、`unique_sql_id`、`keyword`、`rule_version`（初始值 1）。
- 插入后调用 `CatalogUpdateIndexes()`。
- `limit_id` 必须全局单调递增，建议从共享原子计数生成，重启后从系统表最大值初始化。

### 8.2 `gs_update_sql_limit_v2`

调整点：

- 不再调用 `UpdateSqlLimit()`。
- 只更新系统表。
- 递增 `rule_version`。新 SQL 使用新版本 stats，旧版本 stats 保留至 `currConcurrency == 0` 后清理。
- 若 `keyword` 或 `enable` 内容变化，同步更新 `hash`、`unique_sql_id` 等派生字段。
- 如果产品语义要求 SQLID 唯一，在更新前通过 syscache 检查冲突。

### 8.3 `gs_delete_sql_limit_v2`

调整点：

- 不再调用 `DeleteSqlLimitCache()`。
- 只删除系统表。删除后 syscache 查不到规则，新 SQL 不再创建 stats entry。
- 已执行 SQL 仍按 session 记录的 `(pdbOid, limitId, ruleVersion)` 释放并发。
- 旧 stats entry 在 `currConcurrency == 0` 且规则已不存在时由后台清理。

### 8.4 `gs_select_sql_limit_v2`

调整点：

- 规则信息从 syscache 或系统表读取。
- `hit_count`、`reject_count`、`curr_concurrency` 从 stats HTAB 读取。
- 如果 stats 不存在，返回 0。

`gs_select_sql_limit_all_v2()` 可以直接扫描系统表，逐条读取 stats。

## 9. 后台线程

现有 `SqlLimitMain` 及 `SQL_LIMIT_THREAD` 相关代码**不做调整**，保持原样。

新表 `gs_sql_limit_rule` 的运行时统计清理由新增代码处理，不依赖也不修改原有后台线程。迁移时直接拿增量代码即可。

## 10. 事务和一致性

### 10.1 事务限制

规则调整（创建、更新、删除）**禁止在显式事务中执行**。管理函数入口需检查 `IsTransactionBlock()`，若在显式事务中调用则报错拒绝。

原因：

- 简化规则变更的一致性模型，避免事务内多步操作导致中间态。
- 规则变更立即提交，其他会话通过 catalog invalidation 即时可见，无需等待事务结束。
- 减少长事务持锁对规则变更并发度的影响。

### 10.2 备机场景

备机通过 WAL replay 获得系统表变化，catalog invalidation 机制负责失效本地缓存。运行时不再需要周期扫描 `gs_sql_limit_rule`。

### 10.3 Update/Delete 与活跃 SQL

#### Update

规则 update 后递增 `rule_version`。新 SQL 使用新版本 stats `(pdbOid, limitId, newRuleVersion)`，`hitCount/rejectCount/currConcurrency` 从 0 开始。旧版本 stats entry 保留，直到旧 SQL 结束并释放（`currConcurrency == 0`），然后通过惰性清理删除旧版本 stats entry。

#### Delete

删除 catalog tuple 后，后续 syscache 查不到规则，新 SQL 不再创建 stats entry。删除前已执行的 SQL 仍按 session 记录的 `(pdbOid, limitId, ruleVersion)` 释放并发。旧 stats entry 在 `currConcurrency == 0` 且规则已不存在时，通过惰性清理删除。

## 11. 兼容性

对外函数接口使用 `_v2` 后缀，避免与旧函数同名冲突：

```sql
gs_create_sql_limit_v2(limit_name, limit_type, work_node, max_concurrency,
                    start_time, end_time, keyword, users)

gs_update_sql_limit_v2(limit_id, limit_name, work_node, max_concurrency,
                    start_time, end_time, keyword, users)

gs_select_sql_limit_v2(limit_id)
gs_select_sql_limit_all_v2()
gs_delete_sql_limit_v2(limit_id)
```

系统表新增字段对用户可见。如果不希望用户感知内部字段，可以通过系统视图屏蔽，或在文档中声明为内部实现字段。

## 12. 升级方案

升级 SQL 需要完成：

1. 创建新表 `pg_catalog.gs_sql_limit_rule`，包含所有字段：`limit_id`、`limit_name`、`enable`、`work_node`、`max_concurrency`、`start_time`、`end_time`、`limit_type`、`hash`、`unique_sql_id`、`keyword`、`rule_version`、`users`。
2. 创建组合唯一索引 `(enable, limit_type, hash, limit_id)`。
3. 注册 syscache。
4. 创建管理函数（`_v2` 后缀）。
5. 更新 rollback SQL。

## 13. 代码改造清单

### Catalog

- `src/include/catalog/gs_sql_limit_rule.h`
- `src/include/catalog/indexing.h`
- `src/common/backend/catalog/catalog.cpp`
- `src/common/backend/utils/cache/knl_globalsysdbcache.cpp`
- `src/include/catalog/upgrade_sql/...`
- `src/include/catalog/rollback_sql/...`

### Syscache

- `src/include/utils/syscache.h`
- `src/common/backend/utils/cache/syscache.cpp`

### SQL 限流

- `src/include/workload/sql_limit_base.h`
- `src/include/workload/sql_limit_process.h`
- `src/gausskernel/cbb/workload/sql_limit_base.cpp`
- `src/gausskernel/cbb/workload/sql_limit_process.cpp`
- `src/gausskernel/cbb/workload/sql_limit_mgr.cpp`

## 14. 测试计划

### 14.1 功能测试

- 创建 SQLID 限流规则，验证命中和拒绝。
- 创建 `select/insert/update/delete` 关键词限流规则，验证命中和拒绝。
- 指定 `users`，验证仅目标用户生效。
- 指定 `start_time/end_time`，验证窗口内外行为。
- 指定 `work_node`，验证主备节点行为。

### 14.2 事务测试

- 显式事务中调用 `gs_create_sql_limit_v2`，验证报错拒绝。
- 显式事务中调用 `gs_update_sql_limit_v2`，验证报错拒绝。
- 显式事务中调用 `gs_delete_sql_limit_v2`，验证报错拒绝。
- 非事务块中正常调用，验证规则立即生效。

### 14.3 并发测试

- `max_concurrency = 1`，多会话并发执行同一 SQL，验证只有一个会话通过。
- 同时存在 SQLID 和关键词规则时，验证 SQLID 规则优先命中，关键词规则不生效。
- SQL 执行中删除规则，验证 SQL 结束后并发可以正常释放。

### 14.4 syscache invalidation 测试

- 会话 A 创建规则并提交，会话 B 立即执行目标 SQL，验证规则生效。
- 会话 A 更新规则并提交，会话 B 验证新规则生效。
- 会话 A 删除规则并提交，会话 B 验证规则不再生效。

### 14.5 升级测试

- 创建 `gs_sql_limit_rule` 表成功，索引和 syscache 注册正确。
- 管理函数在新表上正常工作。
- rollback 后系统表、索引、函数可恢复。

### 14.6 Fast Path 测试

- 无规则时执行 SQL，验证 fast path 跳过，不触发 syscache 查询。
- 创建规则后，验证 fast path summary 刷新，后续 SQL 进入限流判断。
- 删除所有规则后，验证 summary 更新为 `activeRuleCount = 0`，后续 SQL 再次跳过。
- 新线程首次执行 SQL，验证触发 `RefreshSqlLimitFastPath`。

## 15. 风险和边界

1. syscache 只能缓存规则元数据，不能承载高频统计字段。
2. 组合唯一索引 `(enable, limit_type, hash, limit_id)` 是 syscache 的唯一入口，不能基于普通索引配置 syscache。
3. `limit_id` 必须全局单调递增，重启后需从系统表最大值初始化。
4. `keyword` 中关键词匹配仍是字符串顺序匹配，不解决 SQL 解析语义误匹配问题。
5. `hash` 目前仅用于 SQLID 类型，其他类型填 0。后续扩展时需要考虑 hash 冲突处理。

## 16. 推荐落地顺序

验证使用集中式模式（不启用 `ENABLE_MULTIPLE_NODES`），不需要分布式场景验证。

1. 创建新表 `gs_sql_limit_rule` 及组合唯一索引，配置 syscache。
2. 新增 stats HTAB，实现运行时匹配逻辑。
3. 实现 Fast Path 摘要和 syscache callback。
4. 新增管理函数（`_v2` 后缀），操作新表。
5. 完成回归测试。
