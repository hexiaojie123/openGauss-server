## ADDED Requirements

### Requirement: gs_sql_limit_rule table creation
系统 SHALL 新建 `pg_catalog.gs_sql_limit_rule` 表，包含字段：`limit_id`（int8，全局单调递增）、`limit_name`（name）、`enable`（bool）、`work_node`（int4）、`max_concurrency`（int4）、`start_time`（timestamp）、`end_time`（timestamp）、`limit_type`（text，取值 'sqlId'/'select'/'insert'/'update'/'delete'）、`hash`（int8）、`unique_sql_id`（int8）、`keyword`（text，逗号分隔）、`rule_version`（int8，初始值 1）、`users`（text）。

#### Scenario: Table exists after upgrade
- **WHEN** 执行升级脚本
- **THEN** `pg_catalog.gs_sql_limit_rule` 表存在，包含所有指定字段，schema 为 `pg_catalog`

### Requirement: Composite unique index on gs_sql_limit_rule
系统 SHALL 在 `gs_sql_limit_rule` 上创建组合唯一索引 `(enable, limit_type, hash, limit_id)`，使用 btree。

#### Scenario: Index enforces uniqueness
- **WHEN** 插入两条 `enable`、`limit_type`、`hash`、`limit_id` 完全相同的记录
- **THEN** 第二次插入失败，报唯一约束冲突

### Requirement: GSSQLLIMIT syscache registration
系统 SHALL 注册 syscache `GSSQLLIMIT`，cacheinfo 配置 4 个 key 列（`enable`, `limit_type`, `hash`, `limit_id`），初始 128 slot。

#### Scenario: Syscache lookup by prefix keys
- **WHEN** 调用 `SearchSysCacheList2(GSSQLLIMIT, true, "sqlId")` 或 `SearchSysCacheList3(GSSQLLIMIT, true, "sqlId", hashValue)`
- **THEN** 返回匹配前缀 key 的所有缓存规则列表

#### Scenario: Catalog invalidation triggers syscache refresh
- **WHEN** 会话 A 插入/更新/删除 `gs_sql_limit_rule` 中的规则并提交
- **THEN** 会话 B 后续查询 syscache 时自动获取最新数据
