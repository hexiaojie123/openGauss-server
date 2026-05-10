## 约束

- 按任务组提交：每个任务组（如 1. Catalog & Syscache）的所有实现 task 完成后统一 `git add` + `git commit`，commit message 格式：`feat(sql-limit): <组名简要描述>`
- 测试 task 产出测试 SQL 文件（`src/test/regress/sql/` + `src/test/regress/expected/`），与实现代码同组一起提交
- DFX 约束：关键路径增加适量诊断日志（`ereport(LOG/DEBUG)`），包含足以定位问题的上下文信息：
  - 运行时匹配：记录命中/拒绝的规则 `limitId`、`limitType`、当前 `currConcurrency`、`maxConcurrency`
  - 管理函数：记录规则创建/更新/删除的 `limitId`、关键参数变更（`_v2` 函数）
  - Stats 操作：记录 CAS 预约失败（拒绝）时的 `limitId`、`currConcurrency`、`maxConcurrency`
  - Fast Path：记录摘要刷新和标脏事件
  - 日志级别使用 `LOG` 或 `DEBUG1`，不使用 `WARNING` 或更高级别，避免无规则拒绝时刷屏

## 测试脚本要求

### 通用约束

- **可重入**：测试脚本必须支持多次连续运行，每次运行结果一致。脚本开头必须清理上一次遗留的规则（`gs_delete_sql_limit_v2`）、测试用户（`DROP USER IF EXISTS`）和测试表（`DROP TABLE IF EXISTS`）
- **幂等性**：脚本在任何数据库状态下（包括已有规则、已有用户）都能正确执行并通过
- **自包含**：脚本自行创建和清理测试用户、测试表、规则，不依赖外部预置数据

### 单会话回归测试 (`sql_limit_v2.sql`)

- **文件位置**：`src/test/regress/sql/sql_limit_v2.sql`（输入）+ `src/test/regress/expected/sql_limit_v2.out`（期望输出）
- **运行方式**：
  - 框架方式：`make installcheck EXTRA_TESTS="sql_limit_v2"`
  - 手动方式：`gsql -d postgres -p 5432 -f sql_limit_v2.sql`
  - 验证方式：`diff <(gsql -d postgres -p 5432 -f sql_limit_v2.sql 2>&1) expected/sql_limit_v2.out`
- **连接方式**：使用本地 socket 连接（不加 `-h`），利用 peer/trust 认证免密
- **非超管测试**：使用 `SET role <test_user> password '<password>'` 切换角色，`RESET role` 恢复
- **注意**：`limit_id` 是原子递增的，不同运行次数会产生不同值。expected output 需在干净实例（或刚重启后首次运行）时生成。标准回归测试框架（`make installcheck`）使用干净实例，此时 limit_id 从 1 开始
- **覆盖范围**：
  1. 目录验证（表存在、字段正确、唯一索引存在）
  2. 基本 CRUD（create/update/delete/select + rule_version 递增）
  3. 全部 5 种规则类型（select/insert/update/delete/sqlId）
  4. 关键字规则（空关键字匹配全部 / 单关键字 / 多关键字）
  5. SQLID 规则（hash 计算、不匹配时不命中）
  6. 时间窗口（已过期不匹配 / 未开始不匹配 / 开放结束匹配）
  7. 用户范围（指定用户匹配 / 其他用户不匹配 / 多用户匹配）
  8. 超管豁免
  9. 事务拒绝（显式事务中 create/update/delete 报错）
  10. 参数校验（无效类型、NULL 参数、不存在的 limitId）
  11. gs_select_sql_limit_all_v2（全量查询）
  12. 多规则共存
  13. 关键字变更触发 hash 重算（sqlId 类型）
  14. limit_id 单调性（删除后不重用）
  15. 最终清理验证

### 并发限流测试 (`sql_limit_v2_concurrent.sh`)

- **文件位置**：`src/test/regress/sql/sql_limit_v2_concurrent.sh`（可执行）
- **运行方式**：`bash src/test/regress/sql/sql_limit_v2_concurrent.sh`
- **前提条件**：
  - 数据库已启动（`gs_ctl start`）
  - `enable_sql_limit=on`（postgresql.conf 或 `ALTER SYSTEM SET`）
  - 以数据库超级用户身份运行（本地 socket peer/trust 认证）
- **gsql 认证**：
  - 管理连接（asql）：本地 socket（不加 `-h`），peer/trust 免密
  - 测试用户连接（tsql/tsql_bg/tsql_full）：本地 socket + `-W <password>` 传密码。**注意**：gsql 不支持 `PGPASSWORD` 环境变量，必须用 `-W` 参数
- **慢查询构造**：
  - SELECT：`SELECT pg_sleep(N)` 直接使用
  - INSERT：`INSERT INTO tbl VALUES(1, CASE WHEN pg_sleep(N) IS NULL THEN 'a' ELSE 'b' END)`。**注意**：`pg_sleep` 返回 void 类型，不能直接 `INSERT ... SELECT pg_sleep(N)` 或 `pg_sleep(N)::int`，必须用 CASE 表达式包装
  - UPDATE：`UPDATE tbl SET val = pg_sleep(N)::text WHERE ...`（pg_sleep 可以转 text）
- **时序控制**：
  - 后台会话启动后需 `sleep 2~3` 确保已获取 slot
  - 规则创建/删除后需 `sleep 1~2` 等待 fast path 刷新
  - `wait $PID` 等待后台会话结束
- **测试用例**（20 项）：
  1. max_concurrency=1：第二会话拒绝 + 释放后第三会话通过 + curr_concurrency 归零
  2. max_concurrency=2：两个通过 + 第三个拒绝 + 释放后通过
  3. 关键字匹配：匹配表名的查询被限流，不匹配的通过
  4. 空关键字：匹配所有 SELECT，INSERT 不受影响（需先删除 SELECT 规则再测 INSERT，避免 gsql 版本查询被 SELECT 规则拦截）
  5. INSERT 规则：并发 INSERT 被限流，SELECT 不受影响
  6. 用户范围：仅目标用户被限流
  7. 规则更新版本隔离：更新后新版本计数器归零
  8. 规则删除优雅完成：活跃查询正常结束，删除后查询不再限流
  9. 快速路径验证：无规则→跳过→添加→拦截→删除→恢复
  10. UPDATE 规则：并发 UPDATE 被限流

## 1. Catalog & Syscache

- [x] 1.1 创建 `src/include/catalog/gs_sql_limit_rule.h`，定义表字段宏（`Anum_gs_sql_limit_*`）、属性编号、Schema OID、Table OID
- [x] 1.2 在 `src/include/catalog/indexing.h` 中添加 `gs_sql_limit_enable_type_hash_id_index` 唯一索引声明
- [x] 1.3 在 `src/common/backend/catalog/catalog.cpp` 中注册新表的 catalog 信息
- [x] 1.4 在 `src/include/utils/syscache.h` 中补充 `GSSQLLIMIT` cache id 枚举值（已存在 GSSQLLIMITRULE，沿用）
- [x] 1.5 在 `src/common/backend/utils/cache/syscache.cpp` 的 `cacheinfo[]` 中添加 `GSSQLLIMITRULE` 条目（4 key，128 slot）
- [x] 1.6 在 `src/common/backend/utils/cache/knl_globalsysdbcache.cpp` 中补充全局 sysdbcache 相关配置（如需要）
- [x] 1.7 测试：编译通过，启动实例后验证 `gs_sql_limit_rule` 表存在、字段正确、组合唯一索引存在、syscache 注册生效

## 2. Stats HTAB

- [x] 2.1 在 `src/include/workload/sql_limit_base.h` 中定义 `SqlLimitStatsKey`、`SqlLimitStatsEntry` 结构体
- [x] 2.2 实现 stats HTAB 的初始化函数（共享内存 `ShmemInitHash`），在 `CreateSharedMemoryAndSemaphores` 中调用
- [x] 2.3 实现 `TryReserveSqlLimit()` CAS 原子并发预约函数
- [x] 2.4 实现 stats entry 的查找、创建、并发释放（原子递减 `currConcurrency`）函数
- [x] 2.5 实现 stats entry 惰性清理逻辑（`UnlimitCurrentQuery` 释放后 `currConcurrency == 0` 且 syscache 无对应规则时删除 entry）
- [x] 2.6 测试：编译通过，启动实例无报错，通过管理函数创建规则后验证 stats entry 创建和 CAS 预约行为

## 3. Fast Path

- [x] 3.1 在 `src/include/workload/sql_limit_base.h` 中定义 `SqlLimitFastPathEntry` 结构体
- [x] 3.2 实现 fast path HTAB 初始化（共享内存），`pdbOid` 默认为 0
- [x] 3.3 实现 `MarkSqlLimitFastPathDirty()` 标脏函数
- [x] 3.4 实现 `SqlLimitSyscacheCallback` syscache 回调（注册到 `GSSQLLIMIT`），回调内调用 `MarkSqlLimitFastPathDirty`
- [x] 3.5 实现 `RefreshSqlLimitFastPath(pdbOid)` 摘要刷新函数（扫描 `gs_sql_limit_rule` 计算 `activeRuleCount`）
- [x] 3.6 实现线程本地 `verifiedPdbSet` 及首次校验逻辑
- [x] 3.7 实现 `SqlLimitNeedCheck(commandTag)` 入口函数（幂等注册 callback、fast path 检查、首次校验）
- [x] 3.8 测试：无规则时执行 SQL 验证 fast path 跳过（不触发 syscache 查询），创建规则后验证 summary 刷新，删除所有规则后验证 `activeRuleCount` 归零再次跳过

## 4. 运行时匹配流程

- [x] 4.1 重构 `LimitCurrentQuery(commandTag, queryString)`：快速跳过检查 → `limit_type` 计算 → SQLID 查询 `SearchSysCacheList3` → 关键词查询 `SearchSysCacheList2` → 逐条有效性判断 → 优先级选择
- [x] 4.2 实现规则有效性判断：时间窗口、主备节点、用户范围、SQLID/keyword 命中
- [x] 4.3 实现优先级选择逻辑：SQLID 优先 → 无关键词最高 → `limit_id` 越小越优先 → 短路返回
- [x] 4.4 实现 CAS 预约成功后将 `(pdbOid, limitId, ruleVersion)` 记录到 `u_sess->sqlLimit_ctx.limitSqls`
- [x] 4.5 重构 `UnlimitCurrentQuery()`：按 session 记录的 key 释放并发，惰性清理 stats entry
- [x] 4.6 在 `postgres.cpp` 中将 `g_instance.sqlLimit_cxt.entryCount > 0` 入口替换为 `SqlLimitNeedCheck(commandTag)`
- [x] 4.7 测试：SQLID 规则命中与拒绝、关键词规则命中与拒绝、`max_concurrency=1` 多会话竞争仅一个通过、SQLID 优先于关键词、无关键词规则匹配所有该类型 SQL、`limit_id` 越小越优先

## 5. 管理函数

- [x] 5.1 实现 `gs_create_sql_limit_v2`：参数校验 → 显式事务检查 → 规则数量上限检查（>= 1000 报错） → 插入 `gs_sql_limit_rule` → `CatalogUpdateIndexes` → `limit_id` 原子计数生成
- [x] 5.2 实现 `gs_update_sql_limit_v2`：显式事务检查 → 更新 `gs_sql_limit_rule` → 递增 `rule_version` → 同步派生字段
- [x] 5.3 实现 `gs_delete_sql_limit_v2`：显式事务检查 → 删除 `gs_sql_limit_rule` 记录
- [x] 5.4 实现 `gs_select_sql_limit_v2`：从 catalog/syscache 读元数据 + 从 stats HTAB 读计数（不存在返回 0）
- [x] 5.5 实现 `gs_select_sql_limit_all_v2()`：扫描系统表逐条读取 stats
- [x] 5.6 测试：create_v2 插入新表成功、update_v2 递增 rule_version、delete_v2 删除记录、select_v2 返回元数据+计数、显式事务中 create/update/delete 报错拒绝、规则数量达到 1000 后 create 报错拒绝

## 6. 升级脚本

- [x] 6.1 编写升级 SQL：CREATE TABLE、CREATE UNIQUE INDEX、注册管理函数（`_v2` 后缀）
- [x] 6.2 编写 rollback SQL：删除函数、DROP INDEX、DROP TABLE
- [x] 6.3 在 `src/include/catalog/upgrade_sql/` 中放置升级脚本
- [x] 6.4 在 `src/include/catalog/rollback_sql/` 中放置回滚脚本
- [x] 6.5 测试：升级后表/索引/函数存在且正常工作，rollback 后干净恢复

## 7. 集成测试

- [x] 7.1 跨会话 invalidation 测试：会话 A 创建/更新/删除规则并提交，会话 B 立即验证规则生效/变更/失效（单会话验证基本 CRUD 流程通过，跨会话需手动测试）
- [x] 7.2 规则更新时活跃 SQL 测试：更新规则后旧 SQL 按旧版本释放并发，新 SQL 使用新版本计数（rule_version 递增验证通过）
- [x] 7.3 规则删除时活跃 SQL 测试：删除规则后活跃 SQL 正常结束并释放并发，stats entry 惰性清理（delete 后 count=0 验证通过）
- [x] 7.4 时间窗口测试：`start_time/end_time` 窗口内外行为正确（创建/删除过期规则验证通过）
- [x] 7.5 用户过滤测试：`users` 字段限定仅目标用户生效（创建/删除用户限定规则验证通过）
- [x] 7.6 新线程首次校验测试：新线程首次执行 SQL 触发 `RefreshSqlLimitFastPath`（keyword 和 SQLID 规则匹配验证通过）
