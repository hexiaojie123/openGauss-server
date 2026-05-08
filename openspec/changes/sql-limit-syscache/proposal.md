## Why

当前 SQL 限流实现同时维护系统表 `gs_sql_limit` 和自建共享内存缓存，事务语义复杂、备机同步延迟、规则更新/删除时并发计数释放不可靠，且存在并发检查与并发计数之间的竞态窗口。需要利用 syscache 机制统一规则元数据管理，简化一致性模型，修复并发竞态，并支持后续迁移到闭源透明多写（PDB 隔离）环境。

## What Changes

- 新建 `pg_catalog.gs_sql_limit_rule` 系统表，替代原 `gs_sql_limit` 的规则元数据职责（不从旧表迁移数据）
- 新建组合唯一索引 `(enable, limit_type, hash, limit_id)`，配置一个 syscache `GSSQLLIMIT`，通过前缀 key 查询满足 SQLID 和关键词两种匹配场景
- 新增共享内存 stats HTAB，以 `(pdbOid, limitId, ruleVersion)` 为 key 管理运行时统计和并发计数，使用 CAS 原子操作消除竞态
- 重构 `LimitCurrentQuery()` / `UnlimitCurrentQuery()` 运行时入口，改为从 syscache 查规则、从 HTAB 管理状态
- 新增 Fast Path 摘要机制，无规则时跳过 syscache 查询
- 新增 syscache callback 实现轻量标脏
- 调整管理函数 `gs_create/update/delete/select_sql_limit` 操作新表
- 规则变更禁止在显式事务中执行
- 现有 `SqlLimitMain` 后台线程和 `gs_sql_limit` 表不做调整（增量代码，迁移时直接移植）

## Capabilities

### New Capabilities

- `sql-limit-rule-catalog`: 新系统表 `gs_sql_limit_rule`、组合唯一索引、syscache `GSSQLLIMIT` 注册与 cacheinfo 配置
- `sql-limit-stats-htab`: 共享内存 stats HTAB、CAS 原子并发预约、`ruleVersion` 隔离新旧版本、旧 entry 延迟清理
- `sql-limit-runtime-match`: 运行时匹配流程重构（SQLID 优先、关键词次之、短路机制）、`SqlLimitNeedCheck` 入口替换
- `sql-limit-fast-path`: Fast Path 摘要、syscache callback 标脏、新线程首次校验
- `sql-limit-mgmt-funcs`: 管理函数调整（create/update/delete/select 操作新表，显式事务检查）

### Modified Capabilities

_(无 — 所有改动为增量代码，不修改现有规格行为)_

## Impact

- **Catalog**: 新增 `gs_sql_limit_rule.h`、索引定义、`indexing.h` 更新
- **Syscache**: `syscache.h` 补充 `GSSQLLIMITRULE`（`#ifndef ENABLE_MULTIPLE_NODES` 宏隔离）、`syscache.cpp` 补充 `cacheinfo[]` 条目（同样宏隔离），集中式编译下 syscache 机制完全满足需求，分布式构建不受影响
- **SQL 限流核心**: `sql_limit_base.h/cpp`、`sql_limit_process.h/cpp`、`sql_limit_mgr.cpp` 新增匹配和状态管理代码
- **管理函数**: 对应 `.sql` 文件和 C 实现文件新增函数
- **升级脚本**: `upgrade_sql/` 和 `rollback_sql/` 新增建表和函数注册
- **不影响的模块**: `gs_sql_limit` 旧表、`SqlLimitMain` 后台线程、现有 `g_instance.sqlLimit_cxt` 自建缓存
