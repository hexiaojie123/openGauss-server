/*
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * sql_limit_base.h
 *
 * The file is used to provide the base structure of sql limit.
 *
 * IDENTIFICATION
 *	  src/include/workload/sql_limit_base.h
 *
 * -------------------------------------------------------------------------
 *
 * == Architecture Overview (V2 - syscache-based SQL Limit) ==
 *
 * SQL Limit V2 provides per-rule concurrency control for SQL statements,
 * using a syscache-backed design instead of the V1 shared-inmemory HTAB model.
 *
 * Key components:
 *   1. Catalog table: gs_sql_limit_rule (OID 9060) stores rule definitions
 *      persistently.  Columns include limit_id, limit_name, enable, limit_type,
 *      max_concurrency, keyword, users, time_window, hash, unique_sql_id,
 *      rule_version, work_node.
 *
 *   2. Syscache: GSSQLLIMITRULE caches rule tuples for fast lookup during
 *      query execution.  Three SearchSysCacheList variants support:
 *        - List1(enable)           — fast path count
 *        - List2(enable, type)     — keyword-type rules
 *        - List3(enable, type, hash) — SQLID rules (hash-partitioned)
 *
 *   3. Stats HTAB: Shared-memory hash table keyed by (pdbOid, limitId,
 *      ruleVersion) tracks per-rule-version runtime statistics:
 *      hitCount, rejectCount, currConcurrency.  Uses atomic uint64 fields
 *      for lock-free reads; writes protected by SqlLimitV2StatsLock (LWLock).
 *
 *   4. Fast Path HTAB: Per-PDB entry with atomic activeRuleCount and dirty
 *      flag.  When activeRuleCount==0, query execution skips syscache lookup
 *      entirely (fast path).  Marked dirty on any catalog change via relcache
 *      callback; refreshed on next SqlLimitNeedCheck() call.
 *
 *   5. Session tracking: SqlLimitSessionEntry records (pdbOid, limitId,
 *      ruleVersion) for each matched rule in u_sess->sqlLimit_ctx.limitSqls.
 *      On query completion, UnlimitCurrentQueryV2() iterates these entries
 *      to decrement currConcurrency in the stats HTAB.
 *
 * == Migration Notes ==
 *
 * When porting to another openGauss version or branch:
 *
 *   - Catalog: The gs_sql_limit_rule table definition is in
 *     src/include/catalog/gs_sql_limit_rule.h.  The upgrade SQL (CREATE TABLE,
 *     function definitions, syscache registration) is in
 *     src/include/catalog/upgrade_sql/upgrade_catalog_maindb/.
 *     Copy all three: catalog header, upgrade SQL, and pg_proc entries.
 *
 *   - Syscache registration: GSSQLLIMITRULE must be added to the syscache
 *     ID enum and the cache info array.  See the catalog header for the
 *     expected cache parameters (nkeys, key types).
 *
 *   - LWLock: SqlLimitV2StatsLock must be registered in the LWLock
 *     infrastructure.  Add to the main LWLock array or use the tranche
 *     mechanism.
 *
 *   - GUC parameter: enable_sql_limit (bool, PGC_SIGHUP) controls the
 *     feature globally.  Already exists for V1; V2 reuses the same flag.
 *
 *   - Hook points: The V2 check is invoked from exec_simple_query() in
 *     src/gausskernel/process/tcop/postgres.cpp:
 *       - SqlLimitNeedCheck(commandTag)  — fast-path check before parsing
 *       - LimitCurrentQueryV2(commandTag, queryString)  — after parsing
 *       - UnlimitCurrentQueryV2()  — at query end
 *
 *   - Thread safety: All shared state uses atomics or LWLocks.  No mutexes
 *     or pthread primitives.  Memory contexts are SHARED_CONTEXT for HTABs.
 *
 *   - Struct padding: SqlLimitStatsKey and SqlLimitSessionEntry have explicit
 *     __pad fields between Oid (4 bytes) and uint64 (8 bytes) to ensure
 *     deterministic hashing via tag_hash.  DO NOT remove these padding
 *     fields — they prevent uninitialized bytes from causing hash mismatches.
 *
 *   - CAS concurrency reservation: TryReserveSqlLimit() uses a CAS loop
 *     (pg_atomic_compare_exchange_u64) for atomic reservation.  This is
 *     correct under ARM64's weak CAS semantics because the loop retries
 *     on spurious failure.
 *
 *   - Version isolation: Each gs_update_sql_limit_v2 increments rule_version,
 *     which is part of the stats HTAB key.  This means updated rules start
 *     with fresh stats counters (currConcurrency=0).  Old stats entries are
 *     lazily cleaned up by LazyCleanupSqlLimitStatsEntry() when concurrency
 *     drops to 0.
 *
 *   - Rule priority: In LimitCurrentQueryV2, SQLID rules are checked first
 *     (Step 1).  If no SQLID match, keyword rules are checked (Step 2).
 *     Among keyword rules, empty-keyword rules have highest priority, then
 *     smallest limit_id wins as tiebreaker.
 *
 *   - User matching: Uses GetUserId() (not GetCurrentRoleId()).  In the
 *     thread pool model, GetCurrentRoleId() returns InvalidOid for
 *     non-superuser sessions, which would break user-scoped rules.
 *
 *   - Superuser bypass: superuser() check is at the top of
 *     SqlLimitNeedCheck() and LimitCurrentQueryV2(), ensuring superusers
 *     are never subject to rate limiting.
 *
 *   - Transaction safety: All management functions (create/update/delete)
 *     reject explicit transactions (IsTransactionBlock() check).  This
 *     prevents partial catalog updates and stats inconsistency.
 *
 *   - Rule deletion: Deleting a rule does NOT immediately remove stats
 *     entries or abort running queries.  Active queries complete normally
 *     (their session entries still release concurrency).  Stats entries
 *     are cleaned lazily when currConcurrency drops to 0.
 *
 *   - max_concurrency=0 semantics: Setting max_concurrency to 0 means
 *     ALL matching queries are rejected (since currConcurrency >= 0 is
 *     always true).  This is intentional and tested.
 *
 *   - limit_id monotonicity: limit_id is generated from an atomic counter
 *     (v2EntryIdSequence).  Deleted IDs are NOT reused.  The counter is
 *     initialized from the catalog on startup (InitSqlLimitV2EntryIdSequence).
 *
 *   - max_concurrency type: Stored as int4 in catalog but used as uint64
 *     internally.  The gs_select_sql_limit_v2 function returns it as INT4OID
 *     in the result tuple — this matches the catalog column type.
 *
 *   - hash computation for SQLID: hash_any() on the 64-bit unique_sql_id
 *     produces a uint32, which is stored as int64 in the catalog's hash
 *     column.  The syscache uses this hash value for partitioned lookup.
 *
 *   - keyword matching: Comma-separated keywords in the catalog keyword
 *     field.  strstr() is used for substring matching.  Empty/NULL keyword
 *     matches ALL queries of the specified limit_type.
 *
 *   - PDB support: pdbOid is fixed to 0 (no PDB in open-source openGauss).
 *     The field exists for future COTS compatibility.  All hash lookups
 *     and stats entries use pdbOid=0.
 *
 *   - Test files:
 *     - src/test/regress/sql/sql_limit_v2.sql — single-session regression
 *     - src/test/regress/expected/sql_limit_v2.out — expected output
 *     - src/test/regress/sql/sql_limit_v2_concurrent.sh — concurrent tests
 *
 */
#ifndef SQL_LIMIT_BASE_H
#define SQL_LIMIT_BASE_H

#include "postgres.h"
#include "utils/timestamp.h"
#include "nodes/pg_list.h"
#include "commands/dbcommands.h"
#include "portability/instr_time.h"
#include "utils/atomic.h"

#define SQLID_TYPE "sqlId"
#define SELECT_TYPE "select"
#define UPDATE_TYPE "update"
#define INSERT_TYPE "insert"
#define DELETE_TYPE "delete"

/* V2 syscache-based SQL limit constants */
#define SQL_LIMIT_V2_STATS_INIT_SIZE 128   /* Initial HTAB bucket count for stats */
#define SQL_LIMIT_V2_RULE_MAX_COUNT 1000   /* Max rules allowed in gs_sql_limit_rule */

typedef enum {
    SQL_TYPE_UNIQUE_SQLID = -1,
    SQL_TYPE_SELECT = 0,
    SQL_TYPE_INSERT,
    SQL_TYPE_UPDATE,
    SQL_TYPE_DELETE,
    SQL_TYPE_OTHER
} SqlType;

typedef struct {
    TimestampTz startTime;  /* start time, 0 means no start limit */
    TimestampTz endTime;    /* end time, 0 means no end limit */
} TimeWindow;


/* limit statistics structure */
typedef struct {
    volatile uint64 hitCount;
    volatile uint64 rejectCount;
    volatile uint64 currConcurrency;
} LimitStats;

/* SQL limit common structure */
typedef struct SqlLimit {
    uint64 limitId;
    SqlType sqlType;
    List *databases;
    List *users;
    uint64 maxConcurrency;
    int workNode;
    bool isValid;
    TransactionId xmin;

    TimeWindow timeWindow;
    LimitStats stats;

    union {
        struct {
            uint64 uniqueSqlId;
        } uniqueSql;

        struct {
            List *keywords;
        } keyword;
    } typeData;
} SqlLimit;

typedef struct KeywordsLimitNode {
    dlist_node node;
    SqlLimit* limit;
} KeywordsLimitNode;

typedef struct SqlLimitHashEntry {
    uint64 limitId;
    SqlType sqlType;
    SqlLimit *limit;
    dlist_node *keywordsNode;
} SqlLimitHashEntry;

typedef struct UniqueSqlIdHashEntry {
    uint64 uniqueSqlId;
    SqlLimit *limit;
} UniqueSqlIdHashEntry;

/* ---- V2: syscache-based SQL limit structures ---- */

/*
 * Stats HTAB key: uniquely identifies a specific rule version within a PDB.
 * IMPORTANT: The __pad field between pdbOid and limitId is mandatory.
 * tag_hash() hashes raw bytes including padding. Without explicit zeroed
 * padding, uninitialized stack bytes cause hash mismatches between lookup
 * and insertion, leading to "stats entry not found for release" errors.
 */
typedef struct SqlLimitStatsKey {
    Oid pdbOid;         /* Pluggable database OID; fixed to 0 in open-source */
    uint32 __pad;       /* Explicit padding — DO NOT REMOVE */
    uint64 limitId;     /* Rule's unique ID from gs_sql_limit_rule */
    uint64 ruleVersion; /* Incremented on each update; isolates stats */
} SqlLimitStatsKey;

/*
 * Stats HTAB entry: runtime statistics for a single rule version.
 * All counters are atomic uint64 for lock-free reads.
 * Writes (increment/decrement) are done under SqlLimitV2StatsLock.
 */
typedef struct SqlLimitStatsEntry {
    SqlLimitStatsKey key;            /* Hash key (must be first for tag_hash) */
    pg_atomic_uint64 hitCount;       /* Queries that passed the limit check */
    pg_atomic_uint64 rejectCount;    /* Queries rejected (over max_concurrency) */
    pg_atomic_uint64 currConcurrency;/* Currently running queries under this rule */
} SqlLimitStatsEntry;

/*
 * Fast path entry: per-PDB summary to skip syscache lookups when no rules exist.
 * dirty=1 means rule count may have changed and needs refresh.
 * activeRuleCount>0 means there are enabled rules, so full check is needed.
 */
typedef struct SqlLimitFastPathEntry {
    Oid pdbOid;                      /* PDB OID (hash key) */
    pg_atomic_uint32 dirty;          /* 1=needs refresh, 0=clean */
    pg_atomic_uint32 activeRuleCount;/* Number of enabled rules in catalog */
} SqlLimitFastPathEntry;

/*
 * Session-level record of a matched rule, stored in u_sess->sqlLimit_ctx.limitSqls.
 * On query completion, UnlimitCurrentQueryV2() casts this to SqlLimitStatsKey
 * and calls ReleaseSqlLimitStatsConcurrency(). The struct layout must exactly
 * match SqlLimitStatsKey (same fields, same offsets) for this cast to work.
 */
typedef struct SqlLimitSessionEntry {
    Oid pdbOid;         /* Must match SqlLimitStatsKey.pdbOid offset */
    uint32 __pad;       /* Must match SqlLimitStatsKey.__pad offset */
    uint64 limitId;     /* Must match SqlLimitStatsKey.limitId offset */
    uint64 ruleVersion; /* Must match SqlLimitStatsKey.ruleVersion offset */
} SqlLimitSessionEntry;


void TimeWindowInit(TimeWindow *window);
void TimeWindowSet(TimeWindow *window, TimestampTz start, TimestampTz end);
bool TimeWindowContainsTime(const TimeWindow *window);
bool TimeWindowContainsTimestamp(const TimeWindow *window, TimestampTz ts);

void LimitStatsInit(LimitStats *stats);
void LimitStatsUpdateHit(LimitStats *stats);
void LimitStatsUpdateReject(LimitStats *stats);
void LimitStatsUpdateConcurrency(LimitStats *stats, bool increase);
bool LimitStatsTryIncreaseConcurrency(LimitStats *stats, uint64 maxConcurrency);
void LimitStatsDecreaseConcurrency(LimitStats *stats);
void LimitStatsReset(LimitStats *stats);

SqlLimit *SqlLimitCreate(uint64 limitId, SqlType sqlType);
void SqlLimitDestroy(SqlLimit *limit);
void SqlLimitSetDatabases(SqlLimit *limit, List *databases);
void SqlLimitSetUsers(SqlLimit *limit, List *users);
bool SqlLimitIsValidTime(const SqlLimit *limit);
bool SqlLimitIsValidNode(const SqlLimit *limit);
bool SqlLimitIsValidDatabases(const SqlLimit *limit);
bool SqlLimitIsValidUsers(const SqlLimit *limit);
bool SqlLimitIsHit(const SqlLimit *limit, const char *queryString, uint64 queryId);
bool SqlLimitIsExceedMaxConcurrency(const SqlLimit *limit);
void SqlLimitClear(SqlLimit *limit);
bool IsKeywordsLimit(SqlType sqlType);

NON_EXEC_STATIC void SqlLimitMain();

#endif /* SQL_LIMIT_BASE_H */