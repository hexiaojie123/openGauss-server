# AGENTS.md

## 编译

需要 Linux 环境（openEuler/CentOS），macOS 无法直接编译。

### macOS OrbStack 虚拟机编译

通过 OrbStack 使用 openEuler 24.03 ARM64 虚拟机编译：

```bash
oe-gaussdb() { orbctl run -m openeuler-24.03-arm64 -w "$(pwd)" bash -lc "$*"; }
```

binarylibs：项目根目录的上级目录下的 `binarylibs/`（OrbStack 虚拟机自动映射宿主机路径）。

**编译产物**：`mppdb_temp_install/`（虚拟机内路径同宿主机路径）。

**数据库实例**：虚拟机主目录 `~/openGauss_data/`（即 `/home/shangshanfei/openGauss_data/`）。

**编译策略**：优先使用增量编译（快，几分钟）；仅在首次编译或增量失败时使用全量。

```bash
# 增量编译（优先，只编译变更部分）
oe-gaussdb 'LD_LIBRARY_PATH="" make -sj && make install -sj'

# 全量编译（首次编译或增量失败时）
oe-gaussdb 'sh build.sh -m debug -3rd "$(dirname "$(pwd)")/binarylibs"'

# 其他示例：查看编译目录
oe-gaussdb 'ls -la debug_ent/'
```

### 原生 Linux 编译

```bash
./build.sh -m release -3rd /path/to/binarylibs
./build.sh -m release -3rd /path/to/binarylibs --cmake
./build.sh -m debug -3rd /path/to/binarylibs
./build.sh -m release -3rd /path/to/binarylibs -pkg
```

缺少 `-3rd` 参数会导致编译失败。

## 数据库初始化与启动

编译完成后需要初始化数据目录并启动数据库实例。

**环境变量**（首次使用需设置）：
```bash
export GAUSSHOME=$(pwd)/mppdb_temp_install
export PATH=$GAUSSHOME/bin:$PATH
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
```

**初始化数据目录**（仅需执行一次）：
```bash
gs_initdb -w <密码> -D ~/openGauss_data --nodename "sgnode" --locale="en_US.UTF-8"
```

**启动/停止/重启**：
```bash
gs_ctl start -D ~/openGauss_data -Z single_node
gs_ctl stop -D ~/openGauss_data -Z single_node
gs_ctl restart -D ~/openGauss_data -Z single_node
```

**连接数据库**：
```bash
gsql -d postgres -p 5432 -r
```
- `-d postgres`：连接 `postgres` 数据库
- `-p 5432`：端口号（默认 5432，可在 `postgresql.conf` 中修改）
- `-r`：启用 readline 行编辑支持

**修改端口**（可选）：
```bash
sed -i "s/^#port =/port = <新端口>/" ~/openGauss_data/postgresql.conf
```

## 测试

```bash
# 回归测试（启动临时实例，需要先编译安装）
make check

# 单节点快速回归
make fastcheck_single

# 运行指定测试
make fastcheck EXTRA_TESTS="test_name"

# 对已运行实例测试
make installcheck

# 单元测试（仅 db4ai 和 demo 模块）
make ut
```

- SQL 测试文件：`src/test/regress/sql/` → 期望输出：`src/test/regress/expected/`
- 测试调度：`parallel_schedule`、`serial_schedule`
- 隔离测试：`src/test/isolation/`（规格在 `specs/`）

## 代码格式化

```bash
clang-format -i <file>
```

配置文件 `.clang-format`：LLVM 风格，120 列宽，4 空格缩进，指针靠左（`int* p`），不使用 Tab。

## 架构（导航代码的关键信息）

### 查询流程
```
exec_simple_query()  [src/gausskernel/process/tcop/postgres.cpp]
  → Parser   [src/common/backend/parser/]
  → Analyzer
  → Rewriter  [src/gausskernel/optimizer/rewrite/]
  → Planner   [src/gausskernel/optimizer/plan/planner.cpp]
  → Executor  [src/gausskernel/runtime/executor/execMain.cpp]
```

### 存储引擎（4 种，通过 tableam API 共存）
- **heap** — 行存储，`src/gausskernel/storage/access/heap/`
- **ustore** — undo 存储，`src/gausskernel/storage/access/ustore/`
- **cstore** — 列存储，`src/gausskernel/storage/cstore/`
- **mot** — 内存表，`src/gausskernel/storage/mot/`

### 执行器（3 层）
- **executor** — 逐元组：`src/gausskernel/runtime/executor/`
- **vecexecutor** — 向量化批处理：`src/gausskernel/runtime/vecexecutor/`
- **opfusion** — SQL bypass 快速路径：`src/gausskernel/runtime/opfusion/`

### 线程模型（非 PG 的每连接进程模型）
线程池在 `src/gausskernel/process/threadpool/`。线程本地状态通过 `THR_LOCAL` 宏访问（非全局变量）。关键结构：`knl_thread.h`、`knl_session.h`、`knl_instance.h`。

### DDES（共享存储）
`src/gausskernel/ddes/adapter/` — 共享存储模式，包含 DSS 和 DMS。用于共享存储部署。

## 关键约定

- **内存**：必须使用 MemoryContext 系统的 `palloc()`/`pfree()`（`src/common/backend/utils/mmgr/mcxt.cpp`），禁止 `malloc()`
- **错误处理**：使用 `ereport(ERROR, (errcode(ERRCODE_XXX), errmsg("...")))`。严重级别：DEBUG < LOG < INFO < NOTICE < WARNING < ERROR（中止事务） < FATAL（杀死会话） < PANIC（重启数据库）。结构化处理：`PG_TRY`/`PG_CATCH`
- **GUC 参数**：实例级在 `knl_instance_attr_*`，会话级在 `knl_session_attr_*`（`src/include/knl/knl_guc/`）
- **不加注释**：除非明确要求，否则不添加代码注释

## 关键目录

| 路径 | 说明 |
|---|---|
| `src/gausskernel/process/main/main.cpp` | 入口 → PostmasterMain/PostgresMain/BootStrap |
| `src/gausskernel/process/postmaster/` | Postmaster：共享内存、线程创建 |
| `src/gausskernel/process/tcop/` | 查询分发（postgres.cpp、pquery.cpp） |
| `src/gausskernel/optimizer/` | 优化器、改写器、路径选择、命令 |
| `src/gausskernel/runtime/` | 执行器层（executor、vecexecutor、opfusion、codegen） |
| `src/gausskernel/storage/` | 全部存储：缓冲区、访问方法、WAL、复制 |
| `src/gausskernel/security/` | 审计、加密、TDE、策略 |
| `src/gausskernel/cbb/` | 通用构建块：通信、工具、负载 |
| `src/gausskernel/dbmind/` | AI 运维：db4ai、deepsql、gs_dbmind |
| `src/common/backend/` | 共享基础设施：目录、解析器、节点、工具 |
| `src/common/pgxc/` | 分布式（PGXC）协调/数据节点基础设施 |
| `src/include/knl/` | KNL 抽象：线程本地、GUC 参数 |
| `src/lib/` | 共享库：告警、pgcommon、热补丁 |
| `src/bin/` | 二进制：gsql、gs_ctl、pg_dump、initdb、pg_upgrade |
| `contrib/` | 扩展（90+）：FDW、dolphin、security_plugin 等 |

## Git 工作流（SQL 限流功能）

- **`sql-limit`** — 仅存放 openspec 设计产物
- **`sql-limit-cc-*`** — Claude Code 实现分支
- **`sql-limit-codex-*`** — Codex 实现分支

规则：
- openspec 内容变更需同步到**所有**分支
- 实现代码仅放入各自的编码分支
- 按**任务组**提交，非逐任务
- 通过 cherry-pick 或 reset/amend 跨分支同步 openspec 变更

## Dolphin 扩展

MySQL 兼容层。通过 `contrib/dolphin/` 中的 `cmake.sh` 单独编译。安装为 `.so` + 扩展 SQL 文件。
