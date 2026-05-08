## ADDED Requirements

### Requirement: gs_create_sql_limit_v2 operates on new table
`gs_create_sql_limit_v2` SHALL 只插入 `gs_sql_limit_rule` 表，填充 `enable`（默认 true）、`hash`、`unique_sql_id`、`keyword`、`rule_version`（初始值 1），不再调用 `CreateSqlLimit()` 创建内存对象。`limit_id` 从共享原子计数生成。

#### Scenario: Create rule inserts into new table
- **WHEN** 调用 `gs_create_sql_limit_v2(...)` 成功
- **THEN** `gs_sql_limit_rule` 表中新增一条记录，`rule_version = 1`，`enable = true`

#### Scenario: limit_id is globally monotonic
- **WHEN** 连续创建多条规则
- **THEN** 每条规则的 `limit_id` 严格递增

### Requirement: Rule table upper limit of 1000
`gs_create_sql_limit_v2` SHALL 在创建前检查 `gs_sql_limit_rule` 中当前规则数量，若 >= 1000 则报错拒绝。

#### Scenario: Create rejected when limit reached
- **WHEN** `gs_sql_limit_rule` 中已有 1000 条规则，调用 `gs_create_sql_limit_v2`
- **THEN** 报错拒绝，提示规则数量已达上限

#### Scenario: Create allowed below limit
- **WHEN** `gs_sql_limit_rule` 中有 999 条规则，调用 `gs_create_sql_limit_v2`
- **THEN** 创建成功，规则数量变为 1000

### Requirement: gs_update_sql_limit_v2 increments rule_version
`gs_update_sql_limit_v2` SHALL 只更新 `gs_sql_limit_rule` 表，递增 `rule_version`。不再调用 `UpdateSqlLimit()`。

#### Scenario: Update increments version
- **WHEN** 调用 `gs_update_sql_limit_v2(limitId, ...)` 成功
- **THEN** 对应规则的 `rule_version` 递增，新 SQL 使用新版本 stats entry

#### Scenario: Update syncs derived fields
- **WHEN** `keyword` 或 `enable` 变化
- **THEN** 同步更新 `hash`、`unique_sql_id` 等派生字段

### Requirement: gs_delete_sql_limit_v2 removes from new table only
`gs_delete_sql_limit_v2` SHALL 只删除 `gs_sql_limit_rule` 表中的记录，不再调用 `DeleteSqlLimitCache()`。已执行 SQL 仍按 session 记录的 key 释放并发。

#### Scenario: Delete removes catalog tuple
- **WHEN** 调用 `gs_delete_sql_limit_v2(limitId)` 成功
- **THEN** `gs_sql_limit_rule` 中对应记录被删除，后续 syscache 查不到该规则

#### Scenario: Active SQL unaffected by delete
- **WHEN** 删除规则时有活跃 SQL 在执行
- **THEN** 活跃 SQL 结束时按旧 key 释放并发，stats entry 延迟清理

### Requirement: gs_select_sql_limit_v2 reads from catalog and stats HTAB
`gs_select_sql_limit_v2` SHALL 从 catalog/syscache 读取规则元数据，从 stats HTAB 读取 `hit_count`、`reject_count`、`curr_concurrency`。stats 不存在时返回 0。

#### Scenario: Select returns rule with stats
- **WHEN** 调用 `gs_select_sql_limit_v2(limitId)`
- **THEN** 返回规则元数据及对应 stats entry 的实时计数

#### Scenario: Select returns zeros when no stats
- **WHEN** 规则存在但从未被命中（无 stats entry）
- **THEN** `hit_count`、`reject_count`、`curr_concurrency` 返回 0

### Requirement: Management functions reject explicit transactions
所有规则管理函数（create/update/delete）SHALL 在入口检查 `IsTransactionBlock()`，若在显式事务中调用则报错拒绝。

#### Scenario: Create in explicit transaction
- **WHEN** 在 `BEGIN ... gs_create_sql_limit_v2(...)` 中调用
- **THEN** 报错拒绝，提示不允许在显式事务中操作限流规则

#### Scenario: Update in explicit transaction
- **WHEN** 在显式事务中调用 `gs_update_sql_limit_v2`
- **THEN** 报错拒绝

#### Scenario: Delete in explicit transaction
- **WHEN** 在显式事务中调用 `gs_delete_sql_limit_v2`
- **THEN** 报错拒绝

#### Scenario: Normal call outside transaction
- **WHEN** 在非事务块中调用管理函数
- **THEN** 正常执行，规则变更立即提交
