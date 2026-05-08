## ADDED Requirements

### Requirement: Fast path summary per PDB
系统 SHALL 在共享内存中按 PDB 维护 `SqlLimitFastPathEntry`，包含 `dirty` 标志和 `activeRuleCount`。`pdbOid` 当前固定为 0。

#### Scenario: No rules — skip syscache
- **WHEN** `dirty = 0` 且 `activeRuleCount = 0`
- **THEN** `SqlLimitNeedCheck` 返回 false，跳过 syscache 查询，SQL 直接执行

#### Scenario: Rules exist — enter full check
- **WHEN** `dirty = 0` 且 `activeRuleCount > 0`
- **THEN** `SqlLimitNeedCheck` 返回 true，进入完整限流判断

#### Scenario: Dirty summary — enter full check
- **WHEN** `dirty = 1`
- **THEN** `SqlLimitNeedCheck` 返回 true，进入完整限流判断

### Requirement: Syscache callback marks fast path dirty
系统 SHALL 注册 `GSSQLLIMIT` 的 syscache callback，在 catalog invalidation 时标脏对应 PDB 的 fast path entry。

#### Scenario: Rule creation triggers dirty
- **WHEN** 新规则插入 `gs_sql_limit_rule` 并提交
- **THEN** syscache callback 触发，标记 fast path entry 为 `dirty = 1`

#### Scenario: Rule deletion triggers dirty
- **WHEN** 规则从 `gs_sql_limit_rule` 删除并提交
- **THEN** syscache callback 触发，标记 fast path entry 为 `dirty = 1`

#### Scenario: Callback cannot determine PDB
- **WHEN** callback 中无法可靠获取当前 PDB OID
- **THEN** 保守标记所有 PDB summary 为 dirty

### Requirement: New thread first-use verification
系统 SHALL 在每个线程首次使用某 PDB 的 fast path 时刷新摘要，解决线程创建前已消费的 invalidation 消息不可见问题。

#### Scenario: Thread first access refreshes summary
- **WHEN** 新线程首次调用 `SqlLimitNeedCheck`
- **THEN** 调用 `RefreshSqlLimitFastPath(pdbOid)`，扫描 `gs_sql_limit_rule` 计算 `activeRuleCount`，设置 `dirty = 0`

#### Scenario: Subsequent access reads cached summary
- **WHEN** 线程已验证过某 PDB 后再次调用 `SqlLimitNeedCheck`
- **THEN** 直接读取 `g_instance` 中的 PDB summary，不重新扫描

### Requirement: SqlLimitNeedCheck replaces entry count check
系统 SHALL 用 `SqlLimitNeedCheck(commandTag)` 替换当前 `g_instance.sqlLimit_cxt.entryCount > 0` 入口判断。

#### Scenario: Entry point replacement
- **WHEN** SQL 开始执行
- **THEN** 入口判断调用 `SqlLimitNeedCheck(commandTag)`，该函数内部处理 fast path 检查和 syscache callback 幂等注册
