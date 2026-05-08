## ADDED Requirements

### Requirement: SQLID rule takes priority over keyword rules
系统 SHALL 先查询 SQLID 候选规则，命中则直接使用，不再查询关键词规则。

#### Scenario: SQL matches SQLID rule
- **WHEN** SQL 的 `unique_sql_id` hash 匹配到启用的 SQLID 规则
- **THEN** 使用该 SQLID 规则，不查询关键词规则

#### Scenario: SQL has no SQLID rule but matches keyword
- **WHEN** SQL 没有 SQLID 规则命中，但匹配到关键词规则
- **THEN** 使用关键词规则

### Requirement: Keyword rule priority ordering
系统 SHALL 按以下优先级选择关键词规则：无关键词（`keyword` 为空）优先级最高；有关键词时 `limit_id` 越小优先级越高。一个 SQL 只生效一条规则。

#### Scenario: No-keyword rule matches all SQL of that type
- **WHEN** 存在一条 `limit_type='select'`、`keyword` 为空的规则
- **THEN** 所有 SELECT 语句命中该规则（前提是没有 SQLID 规则命中）

#### Scenario: Smaller limit_id wins among keyword rules
- **WHEN** 存在 `limit_id=1`（keyword='table_a'）和 `limit_id=5`（keyword='table_b'）的规则，SQL 包含 'table_a'
- **THEN** 命中 `limit_id=1` 的规则（短路返回）

### Requirement: Short-circuit on first match
系统 SHALL 在命中第一条规则后立即返回，不再继续匹配其他规则。

#### Scenario: SQLID match short-circuits
- **WHEN** SQL 匹配到 SQLID 规则
- **THEN** 不再枚举关键词候选规则

### Requirement: LimitCurrentQuery reads rules from syscache
系统 SHALL 通过 `SearchSysCacheList3` 查 SQLID 候选，通过 `SearchSysCacheList2` 查关键词候选，不再从自建内存缓存读取规则。

#### Scenario: SQLID lookup via SearchSysCacheList3
- **WHEN** 查询 SQLID 规则
- **THEN** 使用 `SearchSysCacheList3(GSSQLLIMIT, true, "sqlId", hash)` 获取候选，逐条比对 `unique_sql_id`

#### Scenario: Keyword lookup via SearchSysCacheList2
- **WHEN** 查询关键词规则
- **THEN** 使用 `SearchSysCacheList2(GSSQLLIMIT, true, limitType)` 枚举该类型所有启用规则

### Requirement: UnlimitCurrentQuery releases by session-recorded key
系统 SHALL 在 SQL 结束时按 session 中记录的 `(pdbOid, limitId, ruleVersion)` 释放并发，不读取规则元数据。

#### Scenario: Release after rule update
- **WHEN** SQL 执行期间规则被更新（`ruleVersion` 递增）
- **THEN** SQL 结束时按旧 `ruleVersion` 释放并发，不受更新影响

#### Scenario: Release after rule delete
- **WHEN** SQL 执行期间规则被删除
- **THEN** SQL 结束时仍按 session 记录的 key 释放并发，stats entry 延迟清理

### Requirement: Rule validity checks per candidate
系统 SHALL 对每个候选规则逐条检查：时间窗口（`start_time/end_time`）、主备节点（`work_node`）、用户范围（`users`）、SQLID 或关键词命中。

#### Scenario: Rule outside time window
- **WHEN** 当前时间在 `end_time` 之后
- **THEN** 该规则不命中，继续匹配下一条

#### Scenario: Rule user mismatch
- **WHEN** 当前用户不在 `users` 范围内
- **THEN** 该规则不命中，继续匹配下一条
