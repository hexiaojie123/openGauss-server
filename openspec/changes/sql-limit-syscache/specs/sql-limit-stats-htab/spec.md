## ADDED Requirements

### Requirement: Stats HTAB with composite key
系统 SHALL 在共享内存中创建 stats HTAB，key 为 `(pdbOid, limitId, ruleVersion)`，value 包含原子计数器 `hitCount`、`rejectCount`、`currConcurrency`。`pdbOid` 当前固定为 0。

#### Scenario: Stats entry created on first rule hit
- **WHEN** SQL 首次命中某条限流规则
- **THEN** 在 stats HTAB 中按 `(pdbOid=0, limitId, ruleVersion)` 创建 entry，计数从 0 开始

#### Scenario: Stats entry reused for same rule version
- **WHEN** 后续 SQL 命中同一 `(limitId, ruleVersion)` 的规则
- **THEN** 复用已有 stats entry，原子增加 `hitCount` 和 `currConcurrency`

### Requirement: CAS atomic concurrency reservation
系统 SHALL 使用 CAS (`pg_atomic_compare_exchange_u64`) 原子地判断并预约并发计数，消除判断与增加之间的竞态窗口。

#### Scenario: Concurrency within limit
- **WHEN** `currConcurrency < maxConcurrency` 时 CAS 预约
- **THEN** 预约成功，`currConcurrency` 原子加 1，SQL 被接受

#### Scenario: Concurrency at limit
- **WHEN** `currConcurrency >= maxConcurrency` 时 CAS 预约
- **THEN** 预约失败，SQL 被拒绝，`rejectCount` 原子加 1

#### Scenario: Concurrent sessions competing for last slot
- **WHEN** `maxConcurrency = 1`，多会话同时 CAS 预约
- **THEN** 仅一个会话预约成功，其余会话被拒绝

### Requirement: Rule version isolation on update
系统 SHALL 在规则更新时递增 `ruleVersion`，新版本 stats entry 从 0 开始计数，旧版本保留至 `currConcurrency == 0` 后清理。

#### Scenario: Active SQL on old version continues
- **WHEN** 规则更新时旧版本有 3 个活跃 SQL（`currConcurrency = 3`）
- **THEN** 旧版本 stats entry 保留，这 3 个 SQL 结束时正确释放到旧版本

#### Scenario: New SQL uses new version
- **WHEN** 规则更新后新 SQL 命中该规则
- **THEN** 新 SQL 使用 `(pdbOid, limitId, newRuleVersion)` 的 stats entry，从 0 开始计数

### Requirement: Stats entry deferred cleanup on delete
系统 SHALL 在规则删除后保留旧 stats entry，直到 `currConcurrency == 0` 且规则已不存在时清理。

#### Scenario: Active SQL survives rule deletion
- **WHEN** 规则被删除时仍有活跃 SQL 在执行
- **THEN** 活跃 SQL 结束时按 session 记录的 `(pdbOid, limitId, ruleVersion)` 正确释放并发，stats entry 延迟清理

### Requirement: Lazy cleanup of stale stats entries
系统 SHALL 在 `UnlimitCurrentQuery` 释放并发时，若释放后 `currConcurrency == 0`，检查 syscache 中是否仍存在该 `(limitId, ruleVersion)` 对应的规则，不存在则删除 stats entry。

#### Scenario: Lazy cleanup after rule deletion
- **WHEN** 最后一个活跃 SQL 结束释放并发，`currConcurrency` 降为 0，且该规则已从 syscache 中删除
- **THEN** 删除该 stats entry，释放共享内存

#### Scenario: Lazy cleanup after rule update
- **WHEN** 旧 `ruleVersion` 的最后一个活跃 SQL 结束，`currConcurrency` 降为 0，且 syscache 中只有新 `ruleVersion` 的规则
- **THEN** 删除旧版本 stats entry

#### Scenario: No cleanup when rule still exists
- **WHEN** 活跃 SQL 结束释放并发，`currConcurrency` 降为 0，但 syscache 中该 `(limitId, ruleVersion)` 的规则仍存在
- **THEN** 保留 stats entry，不删除
