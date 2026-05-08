# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在本仓库中工作提供指导。

## 项目概述

openGauss 是开源关系型数据库管理系统，基于 PostgreSQL 深度分支，具备多项架构增强：线程池模型、列存储（CStore）、内存表（MOT）、向量化执行、分布式查询（PGXC）。代码量约 270 万行 C/C++。

## 编译命令

### 前置依赖
- GCC 7.3.0+ 或 10.3.x，CMake 3.16.5+，Flex，Bison
- 需要三方二进制库（通过 `-3rd` 参数指定）

### macOS 编译（OrbStack）

openGauss 需要 Linux 环境编译。macOS 上通过 **OrbStack** 使用 openEuler 24.03 ARM64 虚拟机。binarylibs 位置：项目根目录的上级目录下的 `binarylibs/`。编译产物：`mppdb_temp_install/`。数据库实例：虚拟机主目录 `~/openGauss_data/`（即 `/home/shangshanfei/openGauss_data/`）。

```bash
oe-gaussdb() { orbctl run -m openeuler-24.03-arm64 -w "$(pwd)" bash -lc "$*"; }
```

**编译策略**：优先使用增量编译（快，几分钟）；仅在首次编译或增量失败时使用全量。

```bash
# 增量编译（优先，只编译变更部分）
oe-gaussdb 'LD_LIBRARY_PATH="" make -sj && make install -sj'

# 全量编译（首次编译或增量失败时）
oe-gaussdb 'sh build.sh -m debug -3rd "$(dirname "$(pwd)")/binarylibs"'

# 其他示例：查看编译目录
oe-gaussdb 'ls -la debug_ent/'

# binarylibs 路径也可通过环境变量指定
# BINARYLIBS="$(dirname "$(pwd)")/binarylibs"
# oe-gaussdb "sh build.sh -m debug -3rd $BINARYLIBS"
```

### Linux 编译
```bash
# Release 编译（推荐）
./build.sh -m release -3rd /path/to/binarylibs

# Debug 编译
./build.sh -m debug -3rd /path/to/binarylibs

# 编译并打包
./build.sh -m release -3rd /path/to/binarylibs -pkg
```

### 数据库初始化与启动

编译完成后需要初始化数据目录并启动数据库实例。以下命令在 OrbStack 虚拟机内执行（通过 `oe-gaussdb`）。

**环境变量**（首次使用需设置）：
```bash
export GAUSSHOME=~/openGauss-server/mppdb_temp_install
export PATH=$GAUSSHOME/bin:$PATH
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
```

**初始化数据目录**（仅需执行一次）：
```bash
# 单节点模式初始化，-w 指定数据库超级用户密码，-D 指定数据目录
gs_initdb -w <密码> -D ~/openGauss_data --nodename "sgnode" --locale="en_US.UTF-8"
```

**启动数据库**：
```bash
gs_ctl start -D ~/openGauss_data -Z single_node
```

**停止数据库**：
```bash
gs_ctl stop -D ~/openGauss_data -Z single_node
```

**重启数据库**（修改配置后常用）：
```bash
gs_ctl restart -D ~/openGauss_data -Z single_node
```

**连接数据库**：
```bash
gsql -d postgres -p 5432 -r
```
- `-d postgres`：连接 `postgres` 数据库
- `-p 5432`：端口号（默认 5432，可在 `postgresql.conf` 中修改）
- `-r`：启用 readline 行编辑支持（方向键历史、Tab 补全）

**修改端口**（可选，初始化后）：
```bash
sed -i "s/^#port =/port = <新端口>/" ~/openGauss_data/postgresql.conf
```

### 测试
```bash
# 回归测试（创建临时实例）
make check

# 单节点快速回归
make fastcheck_single

# 运行指定测试
make fastcheck EXTRA_TESTS="test_name"

# 对已运行实例做安装检查
make installcheck

# 单元测试
make ut

# 并行回归
make installcheck-parallel
```

SQL 测试文件在 `src/test/regress/sql/`，期望输出在 `src/test/regress/expected/`。测试调度：`parallel_schedule`、`serial_schedule`。

### 代码格式化
```bash
clang-format -i <file>
```
配置文件 `.clang-format`：LLVM 风格，120 列宽，4 空格缩进，指针靠左。

## 架构

### 入口
- **Main**：`src/gausskernel/process/main/main.cpp` → 分发到 `PostmasterMain()`（正常）、`PostgresMain()`（单用户）或 `BootStrapProcessMain()`（initdb）
- **Postmaster**：`src/gausskernel/process/postmaster/postmaster.cpp` — 创建共享内存，fork 后端线程

### 查询处理流水线
```
SQL 文本
  → Parser (src/common/backend/parser/)
  → Analyzer
  → Rewriter (src/gausskernel/optimizer/rewrite/)
  → Planner (src/gausskernel/optimizer/plan/planner.cpp: planner() → standard_planner() → subquery_planner())
  → Executor (src/gausskernel/runtime/executor/execMain.cpp: ExecutorStart → ExecutorRun)
```
- **Tcop**（`src/gausskernel/process/tcop/postgres.cpp`）：`exec_simple_query()` 是后端循环的主查询入口
- **分布式查询**：通过 `pgxc_planner()` 钩子路由，由 stream 线程执行（`src/gausskernel/process/stream/`）

### 存储栈
```
Executor 节点
  → tableam API（表访问方法抽象层）
  → Buffer Manager (src/gausskernel/storage/buffer/bufmgr.cpp)
  → Page 管理 (src/gausskernel/storage/page/)
  → Storage Manager (src/gausskernel/storage/smgr/smgr.cpp) — 函数指针分发到 md/seg 后端
  → 文件系统
```

**存储引擎**：heap（行存）、ustore（undo）、cstore（列存）、mot（内存）

### 优化器
`src/gausskernel/optimizer/` — 路径选择（`path/`）、计划生成（`plan/`）、SQL 改写（`rewrite/`）、预编译语句（`prep/`）

### 关键目录
| 路径 | 用途 |
|---|---|
| `src/gausskernel/runtime/executor/` | 逐元组执行器 |
| `src/gausskernel/runtime/vecexecutor/` | 向量化（批处理）执行器 |
| `src/gausskernel/runtime/opfusion/` | SQL Bypass — 简单查询快速路径 |
| `src/gausskernel/storage/access/` | 访问方法（heap, btree, hash, gin, gist, ustore） |
| `src/gausskernel/storage/cstore/` | 列存储引擎 |
| `src/gausskernel/storage/mot/` | 内存表引擎 |
| `src/gausskernel/storage/replication/` | WAL 复制与恢复 |
| `src/gausskernel/security/` | 审计、加密、TDE、访问策略 |
| `src/gausskernel/process/threadpool/` | 线程池（替代 PG 的进程模型） |
| `src/common/backend/` | 目录、解析器、节点、工具（共享基础设施） |
| `src/common/pgxc/` | 分布式协调/数据节点基础设施 |
| `src/include/knl/` | KNL 抽象层（线程本地存储、GUC 参数） |

### 线程模型（与 PostgreSQL 的关键区别）
openGauss 使用**线程池**而非 PostgreSQL 的每连接一个进程模型。Worker 线程（`ThreadPoolWorker`）无状态地服务会话。线程本地状态通过 `THR_LOCAL` 宏访问（替代 PG 的全局变量）。关键结构：`knl_thread.h`、`knl_session.h`、`knl_instance.h`。

### 内存管理
所有分配通过 **MemoryContext** 系统进行（`src/common/backend/utils/mmgr/mcxt.cpp`）。上下文分层 — 每查询、每会话、每事务。使用 `palloc()`/`pfree()`（不要用 malloc）。上下文在父级重置时自动清理。

### 错误处理
使用 `ereport()` 宏，严重级别：DEBUG、LOG、INFO、NOTICE、WARNING、ERROR（中止事务）、FATAL（杀死会话）、PANIC（数据库重启）。模式：
```cpp
ereport(ERROR, (errcode(ERRCODE_XXX), errmsg("message")));
```
使用 `PG_TRY`/`PG_CATCH` 做结构化异常处理。

## 代码约定

- 语言：C/C++（`.c`、`.cpp`、`.h` 头文件）
- 错误码定义在 `src/include/cm/cm_errcodes.h`
- GUC 参数：实例级在 `knl_instance_attr_*`，会话级在 `knl_session_attr_*`
- 系统目录遵循 PostgreSQL 命名：`pg_class`、`pg_type` 等
- 二进制工具在 `src/bin/`：`gsql`（客户端）、`gs_ctl`（服务控制）、`pg_dump`、`pg_basebackup`、`initdb`、`pg_upgrade`

## Git 工作流（SQL 限流功能）

sql-limit-syscache 功能的分支策略：

- **`sql-limit`** — 仅存放 openspec 设计产物（proposal、design、specs、tasks）
- **`sql-limit-cc-*`** — Claude Code 实现分支
- **`sql-limit-codex-*`** — Codex 实现分支

规则：
- openspec 内容变更（设计更新、任务勾选）需同步到**所有**分支
- 实现代码仅放入各自的编码分支
- 按**任务组**提交（非逐任务）：同一任务组的所有实现 + 测试任务一起提交
- 通过 cherry-pick 或 reset/amend 跨分支同步 openspec 变更
