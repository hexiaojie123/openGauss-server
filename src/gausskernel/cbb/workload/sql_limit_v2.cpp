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
 * sql_limit_v2.cpp
 *
 * V2 syscache-based SQL limit implementation: stats HTAB, fast path,
 * runtime matching, and management functions.
 *
 * IDENTIFICATION
 *    src/gausskernel/cbb/workload/sql_limit_v2.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "utils/atomic.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "catalog/gs_sql_limit_rule.h"
#include "catalog/indexing.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/hash.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/lmgr.h"
#include "workload/sql_limit_v2.h"
#include "workload/sql_limit_process.h"
#include "utils/inval.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/fmgroids.h"
#include "funcapi.h"
#include "utils/acl.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "lib/stringinfo.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

/* ============================================================
 * Stats HTAB — Shared-memory hash table for runtime statistics
 *
 * Design: The stats HTAB lives in shared memory (SHARED_CONTEXT)
 * so all backend threads can read/update it.  Keys are (pdbOid,
 * limitId, ruleVersion) triples.  Each entry tracks hitCount,
 * rejectCount, and currConcurrency as atomic uint64 values.
 *
 * Lifecycle:
 *   - Created once during postmaster startup (InitSqlLimitV2StatsHTAB)
 *   - Entries created on-demand via FindOrCreateSqlLimitStatsEntry()
 *   - Entries lazily cleaned up when currConcurrency drops to 0 and
 *     the rule no longer exists in catalog (LazyCleanupSqlLimitStatsEntry)
 *
 * Concurrency model:
 *   - Reads of counters: lock-free via pg_atomic_read_u64
 *   - Writes (increment/decrement): under SqlLimitV2StatsLock (LW_EXCLUSIVE)
 *   - CAS reservation: TryReserveSqlLimit uses pg_atomic_compare_exchange_u64
 *     inside LW_EXCLUSIVE to prevent TOCTOU between read and compare-and-swap
 *
 * Migration: When porting, ensure the HTAB uses tag_hash (not oid_hash)
 * because the key is a composite struct, not a single Oid.
 * ============================================================ */

static void CreateSqlLimitV2MemoryContext(void)
{
    g_instance.sqlLimit_cxt.statsCxt = AllocSetContextCreate(g_instance.instance_context,
        "SQL limit V2 stats context",
        ALLOCSET_DEFAULT_MINSIZE,
        ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE,
        SHARED_CONTEXT);
}

/*
 * InitStatsHTAB — Create the shared stats hash table.
 * Uses tag_hash because keys are composite structs (SqlLimitStatsKey),
 * not simple OIDs.  The hash function operates on raw bytes, so all
 * padding must be zero-initialized (see __pad field).
 */
static void InitStatsHTAB(void)
{
    HASHCTL hashCtl;
    errno_t rc = memset_s(&hashCtl, sizeof(hashCtl), 0, sizeof(hashCtl));
    securec_check(rc, "\0", "\0");

    hashCtl.keysize = sizeof(SqlLimitStatsKey);
    hashCtl.entrysize = sizeof(SqlLimitStatsEntry);
    hashCtl.hash = tag_hash;
    hashCtl.hcxt = g_instance.sqlLimit_cxt.statsCxt;

    g_instance.sqlLimit_cxt.statsHTAB = hash_create(
        "SQL limit V2 stats HTAB",
        SQL_LIMIT_V2_STATS_INIT_SIZE,
        &hashCtl,
        HASH_ELEM | HASH_FUNCTION | HASH_SHRCTX);
}

void InitSqlLimitV2StatsHTAB(void)
{
    if (g_instance.sqlLimit_cxt.v2Inited) {
        return;
    }

    CreateSqlLimitV2MemoryContext();
    MemoryContext oldCxt = MemoryContextSwitchTo(g_instance.sqlLimit_cxt.statsCxt);
    InitStatsHTAB();
    pg_atomic_init_u64(&g_instance.sqlLimit_cxt.v2EntryIdSequence, 1);
    MemoryContextSwitchTo(oldCxt);
    g_instance.sqlLimit_cxt.v2Inited = true;
}

/*
 * TryReserveSqlLimit — Atomically reserve a concurrency slot via CAS.
 *
 * Returns true if currConcurrency was successfully incremented (was < max).
 * Returns false if already at or above maxConcurrency.
 *
 * Called under LW_EXCLUSIVE (SqlLimitV2StatsLock) to prevent TOCTOU:
 * without the lock, two threads could both read the same oldValue and
 * both CAS successfully, exceeding the limit.
 *
 * The CAS loop handles ARM64's weak compare-and-exchange semantics
 * (spurious failures are retried automatically).
 */
bool TryReserveSqlLimit(SqlLimitStatsEntry *entry, uint64 maxConcurrency)
{
    uint64 oldValue;
    uint64 newValue;

    do {
        oldValue = pg_atomic_read_u64(&entry->currConcurrency);
        if (oldValue >= maxConcurrency) {
            return false;
        }
        newValue = oldValue + 1;
    } while (!pg_atomic_compare_exchange_u64(&entry->currConcurrency, &oldValue, newValue));

    return true;
}

/*
 * FindOrCreateSqlLimitStatsEntry — Lookup or create a stats entry.
 *
 * Uses HASH_ENTER (creates if not found).  New entries have zeroed counters.
 * Called under LW_EXCLUSIVE from LimitCurrentQueryV2's reservation phase.
 */
SqlLimitStatsEntry *FindOrCreateSqlLimitStatsEntry(const SqlLimitStatsKey *key)
{
    bool found = false;
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(
        g_instance.sqlLimit_cxt.statsHTAB, (void *)key, HASH_ENTER, &found);

    if (!found) {
        pg_atomic_init_u64(&entry->hitCount, 0);
        pg_atomic_init_u64(&entry->rejectCount, 0);
        pg_atomic_init_u64(&entry->currConcurrency, 0);
    }

    return entry;
}

static SqlLimitStatsEntry *FindSqlLimitStatsEntry(const SqlLimitStatsKey *key)
{
    bool found = false;
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(
        g_instance.sqlLimit_cxt.statsHTAB, (void *)key, HASH_FIND, &found);

    return found ? entry : NULL;
}

/*
 * ReleaseSqlLimitStatsConcurrency — Decrement currConcurrency for a stats entry.
 *
 * Called by UnlimitCurrentQueryV2 at query completion.  If currConcurrency
 * drops to 0, triggers LazyCleanupSqlLimitStatsEntry to check if the rule
 * still exists in catalog.  If not, removes the stats entry to reclaim memory.
 */
void ReleaseSqlLimitStatsConcurrency(const SqlLimitStatsKey *key)
{
    LWLockAcquire(SqlLimitV2StatsLock, LW_EXCLUSIVE);

    SqlLimitStatsEntry *entry = FindSqlLimitStatsEntry(key);
    if (entry == NULL) {
        LWLockRelease(SqlLimitV2StatsLock);
        ereport(LOG, (errmsg("SQL limit V2: stats entry not found for release, "
                             "limitId=%lu, ruleVersion=%lu", key->limitId, key->ruleVersion)));
        return;
    }

    uint64 oldValue = pg_atomic_fetch_sub_u64(&entry->currConcurrency, 1);
    ereport(DEBUG1, (errmsg("SQL limit V2: release concurrency, limitId=%lu, ruleVersion=%lu, "
                            "oldConcurrency=%lu", key->limitId, key->ruleVersion, oldValue)));

    /* Lazy cleanup if currConcurrency dropped to 0 */
    if (oldValue == 1) {
        LazyCleanupSqlLimitStatsEntry(entry);
    }

    LWLockRelease(SqlLimitV2StatsLock);
}

/*
 * LazyCleanupSqlLimitStatsEntry — Remove stats entry if rule no longer exists.
 *
 * Called when currConcurrency drops to 0.  Scans the catalog to check if
 * the rule with the exact (limitId, ruleVersion) still exists.  If not,
 * removes the stats entry from the HTAB.
 *
 * This lazy approach means:
 *   - Deleted rules don't leave orphan stats entries
 *   - Updated rules (new version) clean up old-version entries
 *   - No immediate cleanup needed on rule deletion
 *
 * Performance note: This does a sequential scan of gs_sql_limit_rule.
 * For deployments with many rules, consider an index on (limit_id, rule_version).
 */
void LazyCleanupSqlLimitStatsEntry(SqlLimitStatsEntry *entry)
{
    /* Check if the rule still exists in catalog for this (limitId, ruleVersion) */
    bool ruleExists = false;

    Relation rel = heap_open(GsSqlLimitRuleRelationId, AccessShareLock);
    TupleDesc tupdesc = RelationGetDescr(rel);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isNull = false;
        Datum limitIdDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_id, tupdesc, &isNull);
        if (isNull) continue;
        Datum ruleVerDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_rule_version, tupdesc, &isNull);
        if (isNull) continue;

        if (DatumGetUInt64(limitIdDat) == entry->key.limitId &&
            DatumGetUInt64(ruleVerDat) == entry->key.ruleVersion) {
            ruleExists = true;
            break;
        }
    }

    systable_endscan(scan);
    heap_close(rel, AccessShareLock);

    if (!ruleExists) {
        ereport(LOG, (errmsg("SQL limit V2: lazy cleanup stats entry, "
                             "limitId=%lu, ruleVersion=%lu",
                             entry->key.limitId, entry->key.ruleVersion)));
        (void)hash_search(g_instance.sqlLimit_cxt.statsHTAB,
            (void *)&entry->key, HASH_REMOVE, NULL);
    }
}

/*
 * InitSqlLimitV2EntryIdSequence — Initialize the limit_id counter on startup.
 *
 * Scans gs_sql_limit_rule catalog to find the max limit_id, then sets
 * the atomic counter to max+1.  Called once during postmaster startup.
 * Ensures limit_ids are monotonically increasing even after restart.
 * Deleted IDs are never reused.
 */
void InitSqlLimitV2EntryIdSequence(void)
{
    /* Scan catalog to find the max limit_id and initialize sequence */
    Relation rel = heap_open(GsSqlLimitRuleRelationId, AccessShareLock);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
    TupleDesc tupleDesc = RelationGetDescr(rel);
    HeapTuple tuple;
    uint64 maxLimitId = 0;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isNull = false;
        Datum limitIdDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_id, tupleDesc, &isNull);
        if (!isNull) {
            uint64 limitId = DatumGetUInt64(limitIdDat);
            if (limitId > maxLimitId) {
                maxLimitId = limitId;
            }
        }
    }

    systable_endscan(scan);
    heap_close(rel, AccessShareLock);

    pg_atomic_write_u64(&g_instance.sqlLimit_cxt.v2EntryIdSequence, maxLimitId + 1);
    ereport(LOG, (errmsg("SQL limit V2: entryIdSequence initialized to %lu", maxLimitId + 1)));
}

/* ============================================================
 * Fast Path HTAB — Per-PDB optimization to skip syscache lookups
 *
 * When no rules are active (activeRuleCount==0), SqlLimitNeedCheck()
 * returns false immediately, avoiding all syscache overhead.
 *
 * Dirty flag mechanism:
 *   - Set to 1 by SqlLimitRelcacheCallback() on any gs_sql_limit_rule
 *     catalog change (insert/update/delete).
 *   - Checked in SqlLimitNeedCheck(); if dirty, calls RefreshSqlLimitFastPath()
 *     to recount enabled rules via syscache.
 *   - After refresh, dirty=0 and activeRuleCount has the current count.
 *
 * Migration: The relcache callback (CacheRegisterSessionRelcacheCallback)
 * is per-session in the thread pool model.  Each worker thread registers
 * once via v2CallbackRegistered THR_LOCAL guard.
 * ============================================================ */

static void InitFastPathHTAB(void)
{
    HASHCTL hashCtl;
    errno_t rc = memset_s(&hashCtl, sizeof(hashCtl), 0, sizeof(hashCtl));
    securec_check(rc, "\0", "\0");

    hashCtl.keysize = sizeof(Oid);
    hashCtl.entrysize = sizeof(SqlLimitFastPathEntry);
    hashCtl.hash = oid_hash;
    hashCtl.hcxt = g_instance.sqlLimit_cxt.statsCxt;

    g_instance.sqlLimit_cxt.fastPathHTAB = hash_create(
        "SQL limit V2 fast path HTAB",
        16,
        &hashCtl,
        HASH_ELEM | HASH_FUNCTION | HASH_SHRCTX);

    /* Pre-create entry for pdbOid = 0 */
    bool found = false;
    Oid pdbOidZero = 0;
    SqlLimitFastPathEntry *entry = (SqlLimitFastPathEntry *)hash_search(
        g_instance.sqlLimit_cxt.fastPathHTAB, (void *)&pdbOidZero, HASH_ENTER, &found);
    if (!found) {
        pg_atomic_init_u32(&entry->pdbOid, 0);
        pg_atomic_init_u32(&entry->dirty, 1); /* dirty by default, need refresh */
        pg_atomic_init_u32(&entry->activeRuleCount, 0);
    }
}

void InitSqlLimitV2FastPathHTAB(void)
{
    /* Fast path HTAB is initialized together with stats HTAB */
    if (!g_instance.sqlLimit_cxt.v2Inited) {
        InitSqlLimitV2StatsHTAB();
    }

    if (g_instance.sqlLimit_cxt.fastPathHTAB != NULL) {
        return;
    }

    MemoryContext oldCxt = MemoryContextSwitchTo(g_instance.sqlLimit_cxt.statsCxt);
    InitFastPathHTAB();
    MemoryContextSwitchTo(oldCxt);
}

void MarkSqlLimitFastPathDirty(Oid pdbOid)
{
    if (g_instance.sqlLimit_cxt.fastPathHTAB == NULL) {
        return;
    }

    if (pdbOid != InvalidOid) {
        bool found = false;
        SqlLimitFastPathEntry *entry = (SqlLimitFastPathEntry *)hash_search(
            g_instance.sqlLimit_cxt.fastPathHTAB, (void *)&pdbOid, HASH_FIND, &found);
        if (found) {
            pg_atomic_write_u32(&entry->dirty, 1);
        }
    } else {
        /* Mark all PDBs dirty */
        HASH_SEQ_STATUS status;
        SqlLimitFastPathEntry *entry = NULL;
        hash_seq_init(&status, g_instance.sqlLimit_cxt.fastPathHTAB);
        while ((entry = (SqlLimitFastPathEntry *)hash_seq_search(&status)) != NULL) {
            pg_atomic_write_u32(&entry->dirty, 1);
        }
    }
}

void SqlLimitRelcacheCallback(Datum arg, Oid relid)
{
    /* Only care about gs_sql_limit_rule changes */
    if (relid == InvalidOid || relid == GsSqlLimitRuleRelationId) {
        MarkSqlLimitFastPathDirty(InvalidOid); /* conservative: mark all dirty */
    }
}

void RefreshSqlLimitFastPath(Oid pdbOid)
{
    if (g_instance.sqlLimit_cxt.fastPathHTAB == NULL) {
        return;
    }

    bool found = false;
    SqlLimitFastPathEntry *fpEntry = (SqlLimitFastPathEntry *)hash_search(
        g_instance.sqlLimit_cxt.fastPathHTAB, (void *)&pdbOid, HASH_ENTER, &found);
    if (!found) {
        fpEntry->pdbOid = pdbOid;
        pg_atomic_init_u32(&fpEntry->dirty, 1);
        pg_atomic_init_u32(&fpEntry->activeRuleCount, 0);
    }

    /* Count enabled rules via syscache */
    struct catclist *catlist = SearchSysCacheList1(GSSQLLIMITRULE, BoolGetDatum(true));
    uint32 activeCount = catlist->n_members;
    ReleaseSysCacheList(catlist);

    pg_atomic_write_u32(&fpEntry->activeRuleCount, activeCount);
    pg_atomic_write_u32(&fpEntry->dirty, 0);

    ereport(DEBUG1, (errmsg("SQL limit V2: refresh fast path, pdbOid=%u, activeRuleCount=%u",
                            pdbOid, activeCount)));
}

/* ============================================================
 * SqlLimitNeedCheck — Fast-path entry point for query execution
 *
 * Called from exec_simple_query() BEFORE parsing to decide whether
 * the query needs SQL limit checking.  Returns false if:
 *   - enable_sql_limit GUC is off
 *   - Transaction is in ABORT state
 *   - Current user is superuser
 *   - No enabled rules exist in catalog (fast path)
 *
 * This is the first gate.  Even if it returns true, the actual
 * limit enforcement happens in LimitCurrentQueryV2() after parsing.
 *
 * Thread safety: Uses THR_LOCAL v2CallbackRegistered for one-time
 * relcache callback registration per worker thread.
 * ============================================================ */

/* Thread-local: whether this thread has registered the syscache callback */
static THR_LOCAL bool v2CallbackRegistered = false;

bool SqlLimitNeedCheck(const char *commandTag)
{
    if (!u_sess->attr.attr_common.enable_sql_limit || IsAbortedTransactionBlockState() || superuser()) {
        return false;
    }

    if (commandTag == NULL || strlen(commandTag) == 0) {
        return false;
    }

    /* Idempotent callback registration - use relcache callback instead of syscache */
    if (!v2CallbackRegistered) {
        CacheRegisterSessionRelcacheCallback(SqlLimitRelcacheCallback, (Datum)0);
        v2CallbackRegistered = true;
    }

    /* Ensure fast path is initialized */
    InitSqlLimitV2FastPathHTAB();

    Oid pdbOid = 0; /* fixed to 0 in open source */

    /* Check fast path */
    bool found = false;
    SqlLimitFastPathEntry *fpEntry = (SqlLimitFastPathEntry *)hash_search(
        g_instance.sqlLimit_cxt.fastPathHTAB, (void *)&pdbOid, HASH_FIND, &found);

    if (!found) {
        /* First time: refresh and return true */
        RefreshSqlLimitFastPath(pdbOid);
        return true;
    }

    if (pg_atomic_read_u32(&fpEntry->dirty) == 1) {
        /* Dirty: need refresh, then re-check */
        RefreshSqlLimitFastPath(pdbOid);
        return pg_atomic_read_u32(&fpEntry->activeRuleCount) > 0;
    }

    /* Clean: check activeRuleCount */
    return pg_atomic_read_u32(&fpEntry->activeRuleCount) > 0;
}

/* ============================================================
 * Runtime Matching V2 — Rule lookup and concurrency enforcement
 *
 * LimitCurrentQueryV2() is the core function called from
 * exec_simple_query() AFTER parsing.  It performs two-phase lookup:
 *
 *   Phase 1 (SQLID rules):
 *     - Computes hash of unique_sql_id from the parsed query
 *     - Looks up syscache with SearchSysCacheList3(enable, 'sqlId', hash)
 *     - Verifies exact unique_sql_id match (hash collision check)
 *     - Checks time window, node, and user constraints
 *
 *   Phase 2 (Keyword rules):
 *     - Only entered if no SQLID match found
 *     - Maps commandTag to limit_type (SELECT/INSERT/UPDATE/DELETE)
 *     - Looks up syscache with SearchSysCacheList2(enable, limit_type)
 *     - Checks keyword substring match against query string
 *     - Priority: empty keyword > smallest limit_id
 *
 *   Reservation:
 *     - After finding a matching rule, acquires LW_EXCLUSIVE on StatsLock
 *     - Calls TryReserveSqlLimit (CAS loop) to atomically increment
 *       currConcurrency if below maxConcurrency
 *     - If reservation fails: ereport(ERROR) to reject the query
 *     - If reservation succeeds: records SqlLimitSessionEntry in session
 *       for later release by UnlimitCurrentQueryV2()
 *
 * Migration: The two-phase design separates SQLID (exact match, fast)
 * from keyword (substring match, slower).  SQLID rules use hash-based
 * syscache lookup for O(1) performance.  Keyword rules iterate all
 * enabled rules of the given type.
 * ============================================================ */

/*
 * ComputeSqlLimitHash — Hash a unique_sql_id for syscache lookup.
 *
 * Uses hash_any on the full 64-bit value, then truncates to int64
 * for storage in the catalog's hash column (int8).  The syscache
 * uses this hash for partitioned lookup of sqlId-type rules.
 */
static Datum ComputeSqlLimitHash(uint64 uniqueSqlId)
{
    uint32 hashVal = DatumGetUInt32(hash_any((const unsigned char *)&uniqueSqlId, sizeof(uniqueSqlId)));
    return Int64GetDatum((int64)hashVal);
}

/*
 * Check if current time is within the rule's time window.
 */
static bool IsWithinTimeWindow(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    TimestampTz startTime = 0;
    TimestampTz endTime = 0;

    Datum startDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_start_time, tupdesc, &isNull);
    if (!isNull) {
        startTime = DatumGetTimestampTz(startDat);
    }

    Datum endDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_end_time, tupdesc, &isNull);
    if (!isNull) {
        endTime = DatumGetTimestampTz(endDat);
    }

    TimestampTz now = GetCurrentTimestamp();

    if (startTime != 0 && now < startTime) {
        return false;
    }
    if (endTime != 0 && now > endTime) {
        return false;
    }
    return true;
}

/*
 * Check if the current node matches the rule's work_node.
 */
static bool IsMatchingNode(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum nodeDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_work_node, tupdesc, &isNull);
    if (isNull) {
        return true;
    }
    int32 workNode = DatumGetInt32(nodeDat);
    /* 0 means current node */
    if (workNode == 0) {
        return true;
    }
    /* In non-distributed mode, any non-zero value is considered standby check */
    return !RecoveryInProgress();
}

/*
 * IsMatchingUser — Check if current user is in the rule's user list.
 *
 * IMPORTANT: Uses GetUserId() not GetCurrentRoleId().  In the thread pool
 * model, GetCurrentRoleId() returns InvalidOid for non-superuser sessions,
 * which would break user-scoped rules.  GetUserId() correctly returns the
 * authenticated user OID in all cases.
 *
 * Empty/NULL users field means all users are matched.
 */
static bool IsMatchingUser(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum usersDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_users, tupdesc, &isNull);
    if (isNull) {
        return true; /* no restriction */
    }

    text *usersText = DatumGetTextP(usersDat);
    char *usersStr = text_to_cstring(usersText);
    if (usersStr == NULL || strlen(usersStr) == 0) {
        pfree_ext(usersStr);
        return true;
    }

    /* Parse comma-separated user list and check current user */
    char *currentUserName = GetUserNameFromId(GetUserId());
    bool matched = false;

    char *savedPtr = NULL;
    char *token = strtok_r(usersStr, ",", &savedPtr);
    while (token != NULL) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (strcmp(token, currentUserName) == 0) {
            matched = true;
            break;
        }
        token = strtok_r(NULL, ",", &savedPtr);
    }

    pfree_ext(usersStr);
    return matched;
}

/*
 * Check if the query matches a keyword rule.
 */
static bool IsMatchingKeyword(HeapTuple tuple, TupleDesc tupdesc, const char *queryString)
{
    bool isNull = false;
    Datum kwDat = heap_getattr(tuple, Anum_gs_sql_limit_rule_keyword, tupdesc, &isNull);
    if (isNull) {
        return true; /* empty keyword matches all */
    }

    text *kwText = DatumGetTextP(kwDat);
    char *kwStr = text_to_cstring(kwText);
    if (kwStr == NULL || strlen(kwStr) == 0) {
        pfree_ext(kwStr);
        return true; /* empty keyword matches all */
    }

    /* Check each keyword (comma-separated) against query string */
    bool matched = false;
    char *kwCopy = pstrdup(kwStr);
    char *savedPtr = NULL;
    char *token = strtok_r(kwCopy, ",", &savedPtr);
    while (token != NULL) {
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (strlen(token) > 0 && strstr(queryString, token) != NULL) {
            matched = true;
            break;
        }
        token = strtok_r(NULL, ",", &savedPtr);
    }

    pfree_ext(kwCopy);
    pfree_ext(kwStr);
    return matched;
}

/*
 * Get max_concurrency from a rule tuple.
 */
static uint64 GetRuleMaxConcurrency(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum dat = heap_getattr(tuple, Anum_gs_sql_limit_rule_max_concurrency, tupdesc, &isNull);
    if (isNull) {
        return 0;
    }
    return (uint64)DatumGetInt64(dat);
}

/*
 * Get limit_id from a rule tuple.
 */
static uint64 GetRuleLimitId(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum dat = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_id, tupdesc, &isNull);
    if (isNull) {
        return 0;
    }
    return DatumGetUInt64(dat);
}

/*
 * Get rule_version from a rule tuple.
 */
static uint64 GetRuleRuleVersion(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum dat = heap_getattr(tuple, Anum_gs_sql_limit_rule_rule_version, tupdesc, &isNull);
    if (isNull) {
        return 0;
    }
    return DatumGetUInt64(dat);
}

/*
 * Get unique_sql_id from a rule tuple.
 */
static uint64 GetRuleUniqueSqlId(HeapTuple tuple, TupleDesc tupdesc)
{
    bool isNull = false;
    Datum dat = heap_getattr(tuple, Anum_gs_sql_limit_rule_unique_sql_id, tupdesc, &isNull);
    if (isNull) {
        return 0;
    }
    return DatumGetUInt64(dat);
}

/* ---- Syscache-based helper functions (no TupleDesc needed) ---- */

/*
 * Map uppercase commandTag from CreateCommandTag() to the canonical
 * lowercase limit_type stored in gs_sql_limit_rule.
 * Returns NULL if the commandTag is not a recognized SQL limit type.
 */
static const char* MapCommandTagToLimitType(const char *commandTag)
{
    SqlType sqlType = GetSqlLimitType(commandTag);
    switch (sqlType) {
        case SQL_TYPE_SELECT:
            return SELECT_TYPE;
        case SQL_TYPE_INSERT:
            return INSERT_TYPE;
        case SQL_TYPE_UPDATE:
            return UPDATE_TYPE;
        case SQL_TYPE_DELETE:
            return DELETE_TYPE;
        default:
            return NULL;
    }
}

static bool IsWithinTimeWindowSC(HeapTuple tuple)
{
    bool isNull = false;
    TimestampTz startTime = 0;
    TimestampTz endTime = 0;
    Datum startDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_start_time, &isNull);
    if (!isNull) {
        startTime = DatumGetTimestampTz(startDat);
    }
    Datum endDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_end_time, &isNull);
    if (!isNull) {
        endTime = DatumGetTimestampTz(endDat);
    }
    TimestampTz now = GetCurrentTimestamp();
    if (startTime != 0 && now < startTime) {
        return false;
    }
    if (endTime != 0 && now > endTime) {
        return false;
    }
    return true;
}

static bool IsMatchingNodeSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum nodeDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_work_node, &isNull);
    if (isNull) {
        return true;
    }
    int workNode = DatumGetInt32(nodeDat);
    if (workNode == 0) {
        return true; /* 0 means all nodes */
    }
    /* Check if current node matches */
    if (workNode == 1 && !RecoveryInProgress()) {
        return true; /* master/primary */
    }
    if (workNode == 2 && RecoveryInProgress()) {
        return true; /* standby */
    }
    return false;
}

static bool IsMatchingUserSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum usersDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_users, &isNull);
    if (isNull) {
        return true;
    }
    text *usersText = DatumGetTextP(usersDat);
    char *usersStr = text_to_cstring(usersText);
    if (usersStr == NULL || strlen(usersStr) == 0) {
        pfree_ext(usersStr);
        return true;
    }
    char *currentUserName = GetUserNameFromId(GetUserId());
    bool matched = false;
    char *savedPtr = NULL;
    char *token = strtok_r(usersStr, ",", &savedPtr);
    while (token != NULL) {
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        if (strcmp(token, currentUserName) == 0) {
            matched = true;
            break;
        }
        token = strtok_r(NULL, ",", &savedPtr);
    }
    pfree_ext(usersStr);
    return matched;
}

static bool IsMatchingKeywordSC(HeapTuple tuple, const char *queryString)
{
    bool isNull = false;
    Datum kwDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_keyword, &isNull);
    if (isNull) {
        return true;
    }
    text *kwText = DatumGetTextP(kwDat);
    char *kwStr = text_to_cstring(kwText);
    if (kwStr == NULL || strlen(kwStr) == 0) {
        pfree_ext(kwStr);
        return true;
    }
    bool matched = false;
    char *kwCopy = pstrdup(kwStr);
    char *savedPtr = NULL;
    char *token = strtok_r(kwCopy, ",", &savedPtr);
    while (token != NULL) {
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        if (strlen(token) > 0 && strstr(queryString, token) != NULL) {
            matched = true;
            break;
        }
        token = strtok_r(NULL, ",", &savedPtr);
    }
    pfree_ext(kwCopy);
    pfree_ext(kwStr);
    return matched;
}

static uint64 GetRuleMaxConcurrencySC(HeapTuple tuple)
{
    bool isNull = false;
    Datum dat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_max_concurrency, &isNull);
    if (isNull) return 0;
    return (uint64)DatumGetInt64(dat);
}

static uint64 GetRuleLimitIdSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum dat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_limit_id, &isNull);
    if (isNull) return 0;
    return DatumGetUInt64(dat);
}

static uint64 GetRuleRuleVersionSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum dat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_rule_version, &isNull);
    if (isNull) return 0;
    return DatumGetUInt64(dat);
}

static uint64 GetRuleUniqueSqlIdSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum dat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_unique_sql_id, &isNull);
    if (isNull) return 0;
    return DatumGetUInt64(dat);
}

/* Check if a keyword rule has empty keyword (matches all SQL of that type) */
static bool IsEmptyKeywordSC(HeapTuple tuple)
{
    bool isNull = false;
    Datum kwDat = SysCacheGetAttr(GSSQLLIMITRULE, tuple, Anum_gs_sql_limit_rule_keyword, &isNull);
    if (isNull) return true;
    text *kwText = DatumGetTextP(kwDat);
    return (VARSIZE(kwText) - VARHDRSZ == 0);
}

/*
 * LimitCurrentQueryV2 — Core rate-limiting enforcement for a single query.
 *
 * Called from exec_simple_query() after parsing.  Performs two-phase rule
 * matching (SQLID first, then keyword), then atomically reserves a
 * concurrency slot or rejects the query with ERRCODE_FEATURE_NOT_SUPPORTED.
 *
 * Important implementation notes:
 *   - matchedKey is zero-initialized with memset_s to ensure deterministic
 *     hashing in FindOrCreateSqlLimitStatsEntry (padding bytes matter!)
 *   - Session entry uses palloc0 (not palloc) for the same reason
 *   - sessEntry->__pad is explicitly set to 0 for clarity
 *   - GetUserId() is used (not GetCurrentRoleId()) because in the thread
 *     pool model, GetCurrentRoleId() returns InvalidOid for non-superusers
 *
 * Hook point: src/gausskernel/process/tcop/postgres.cpp, exec_simple_query(),
 *   after WhereToSendCommand and before exec_simple_query_body.
 */
void LimitCurrentQueryV2(const char *commandTag, const char *queryString)
{
    if (!u_sess->attr.attr_common.enable_sql_limit || IsAbortedTransactionBlockState() || superuser()) {
        return;
    }
    if (commandTag == NULL || strlen(commandTag) == 0 || queryString == NULL || strlen(queryString) == 0) {
        return;
    }
    if (!g_instance.sqlLimit_cxt.v2Inited) {
        return;
    }

    /* Step 1: Try SQLID rules via syscache */
    uint64 uniqueSqlId = u_sess->unique_sql_cxt.unique_sql_id;
    SqlLimitStatsKey matchedKey;
    errno_t rc = memset_s(&matchedKey, sizeof(matchedKey), 0, sizeof(matchedKey));
    securec_check(rc, "\0", "\0");
    uint64 matchedMaxConcurrency = 0;
    bool found = false;

    if (uniqueSqlId != 0) {
        Datum hashDatum = ComputeSqlLimitHash(uniqueSqlId);
        int64 hashVal = DatumGetInt64(hashDatum);

        struct catclist *catlist = SearchSysCacheList3(GSSQLLIMITRULE,
            BoolGetDatum(true), CStringGetTextDatum(SQLID_TYPE), Int64GetDatum(hashVal));

        for (int i = 0; i < catlist->n_members; i++) {
            HeapTuple tuple = t_thrd.lsc_cxt.FetchTupleFromCatCList(catlist, i);
            if (!IsWithinTimeWindowSC(tuple)) continue;
            if (!IsMatchingNodeSC(tuple)) continue;
            if (!IsMatchingUserSC(tuple)) continue;

            uint64 ruleSqlId = GetRuleUniqueSqlIdSC(tuple);
            if (ruleSqlId != uniqueSqlId) continue; /* hash collision */

            /* SQLID match found */
            matchedKey.pdbOid = 0;
            matchedKey.limitId = GetRuleLimitIdSC(tuple);
            matchedKey.ruleVersion = GetRuleRuleVersionSC(tuple);
            matchedMaxConcurrency = GetRuleMaxConcurrencySC(tuple);
            found = true;
            break;
        }
        ReleaseSysCacheList(catlist);
    }

    /* Step 2: If no SQLID match, try keyword rules via syscache */
    if (!found) {
        const char *limitTypeStr = MapCommandTagToLimitType(commandTag);
        if (limitTypeStr == NULL) {
            return; /* not a recognized SQL type for keyword rules */
        }

        struct catclist *catlist = SearchSysCacheList2(GSSQLLIMITRULE,
            BoolGetDatum(true), CStringGetTextDatum(limitTypeStr));

        /* Priority: empty keyword first, then smallest limit_id */
        uint64 bestLimitId = UINT64_MAX;
        HeapTuple bestTuple = NULL;
        bool bestIsEmptyKw = false;
        bool haveCandidate = false;

        for (int i = 0; i < catlist->n_members; i++) {
            HeapTuple kwTuple = t_thrd.lsc_cxt.FetchTupleFromCatCList(catlist, i);
            if (!IsWithinTimeWindowSC(kwTuple)) continue;
            if (!IsMatchingNodeSC(kwTuple)) continue;
            if (!IsMatchingUserSC(kwTuple)) continue;
            if (!IsMatchingKeywordSC(kwTuple, queryString)) continue;

            /* Valid candidate */
            bool isEmptyKw = IsEmptyKeywordSC(kwTuple);
            uint64 lid = GetRuleLimitIdSC(kwTuple);

            /* Empty keyword has highest priority */
            if (isEmptyKw) {
                if (!bestIsEmptyKw || lid < bestLimitId) {
                    bestIsEmptyKw = true;
                    bestLimitId = lid;
                    bestTuple = kwTuple;
                    haveCandidate = true;
                }
            } else if (!bestIsEmptyKw && lid < bestLimitId) {
                bestLimitId = lid;
                bestTuple = kwTuple;
                haveCandidate = true;
            }
        }

        if (haveCandidate && bestTuple != NULL) {
            matchedKey.pdbOid = 0;
            matchedKey.limitId = GetRuleLimitIdSC(bestTuple);
            matchedKey.ruleVersion = GetRuleRuleVersionSC(bestTuple);
            matchedMaxConcurrency = GetRuleMaxConcurrencySC(bestTuple);
            found = true;
        }

        ReleaseSysCacheList(catlist);
    }

    if (!found) {
        return; /* no rule matched */
    }

    /* Step 3: CAS reservation */
    LWLockAcquire(SqlLimitV2StatsLock, LW_EXCLUSIVE);
    SqlLimitStatsEntry *statsEntry = FindOrCreateSqlLimitStatsEntry(&matchedKey);

    if (!TryReserveSqlLimit(statsEntry, matchedMaxConcurrency)) {
        pg_atomic_fetch_add_u64(&statsEntry->rejectCount, 1);
        LWLockRelease(SqlLimitV2StatsLock);

        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("The request is over max concurrency of SQL limit, "
                    "the request will be rejected. limitId: %lu", matchedKey.limitId),
                errdetail("max concurrency: %lu", matchedMaxConcurrency)));
        return; /* ereport(ERROR) does not return */
    }

    /* Only count as hit after successful reservation */
    pg_atomic_fetch_add_u64(&statsEntry->hitCount, 1);

    ereport(LOG, (errmsg("SQL limit V2: reserved concurrency, limitId=%lu, ruleVersion=%lu",
                         matchedKey.limitId, matchedKey.ruleVersion)));
    LWLockRelease(SqlLimitV2StatsLock);

    /* Step 4: Record in session */
    MemoryContext oldCxt = MemoryContextSwitchTo(SESS_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_CBB));
    SqlLimitSessionEntry *sessEntry = (SqlLimitSessionEntry *)palloc0(sizeof(SqlLimitSessionEntry));
    sessEntry->pdbOid = matchedKey.pdbOid;
    sessEntry->__pad = 0;
    sessEntry->limitId = matchedKey.limitId;
    sessEntry->ruleVersion = matchedKey.ruleVersion;
    u_sess->sqlLimit_ctx.limitSqls = lappend(u_sess->sqlLimit_ctx.limitSqls, (void *)sessEntry);
    MemoryContextSwitchTo(oldCxt);
}

/*
 * UnlimitCurrentQueryV2 — Release concurrency slots at query completion.
 *
 * Called from exec_simple_query() in the PG_CATCH/PG_FINALLY path.
 * Iterates u_sess->sqlLimit_ctx.limitSqls and decrements currConcurrency
 * for each recorded match.  The SqlLimitSessionEntry is cast directly
 * to SqlLimitStatsKey — their layouts must match exactly (same field
 * order, same __pad placement).
 *
 * Hook point: src/gausskernel/process/tcop/postgres.cpp, exec_simple_query(),
 *   in the PG_CATCH block and after normal query completion.
 */
void UnlimitCurrentQueryV2(void)
{
    if (u_sess->sqlLimit_ctx.limitSqls == NIL) {
        return;
    }

    foreach_cell(cell, u_sess->sqlLimit_ctx.limitSqls) {
        SqlLimitSessionEntry *sessEntry = (SqlLimitSessionEntry *)lfirst(cell);
        ReleaseSqlLimitStatsConcurrency((SqlLimitStatsKey *)sessEntry);
    }

    list_free_deep(u_sess->sqlLimit_ctx.limitSqls);
    u_sess->sqlLimit_ctx.limitSqls = NIL;
}

/* ============================================================
 * Management Functions V2 — gs_create/update/delete/select_sql_limit_v2
 *
 * These are SQL-callable functions that manage the gs_sql_limit_rule catalog.
 * They operate directly on the heap relation (not through syscache) for
 * writes, and the syscache invalidation callback handles refreshing the
 * fast path automatically.
 *
 * All management functions enforce:
 *   - superuser() check
 *   - enable_sql_limit GUC must be on
 *   - Not in recovery (standby)
 *   - Not in an explicit transaction (IsTransactionBlock)
 *   - Rule count limit (SQL_LIMIT_V2_RULE_MAX_COUNT = 1000)
 *
 * Migration: The function SQL signatures are defined in upgrade SQL files:
 *   src/include/catalog/upgrade_sql/upgrade_catalog_maindb/upgrade-post_catalog_maindb_93_088.sql
 * Copy the CREATE FUNCTION statements when porting.
 *
 * Catalog columns (gs_sql_limit_rule, OID 9060):
 *   1. limit_id (int8) — auto-generated, atomic, monotonically increasing
 *   2. limit_name (name) — user-provided name
 *   3. enable (bool) — always true on creation
 *   4. work_node (int4) — 0=current node (default)
 *   5. max_concurrency (int4) — max concurrent queries (0=block all)
 *   6. start_time (timestamptz) — rule activation time (NULL=no start)
 *   7. end_time (timestamptz) — rule expiration time (NULL=no end)
 *   8. limit_type (text) — select/insert/update/delete/sqlId
 *   9. keyword (text) — comma-separated keywords or SQLID value
 *  10. users (text) — comma-separated user names (NULL=all users)
 *  11. hash (int8) — hash of unique_sql_id for sqlId type, 0 otherwise
 *  12. unique_sql_id (int8) — numeric SQLID for sqlId type, 0 otherwise
 *  13. rule_version (int8) — incremented on each update (starts at 1)
 * ============================================================ */

static void ValidateV2LimitParams(bool isCreate)
{
    if (!superuser()) {
        aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC, "must be system admin to execute");
    }

    if (RecoveryInProgress()) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("sql limit V2 operation is not allowed in standby.")));
    }

    if (!u_sess->attr.attr_common.enable_sql_limit) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), (errmsg("enable_sql_limit is off, please set to on"))));
    }

    if (IsTransactionBlock()) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("SQL limit V2 operations cannot be executed in explicit transactions.")));
    }
}

/*
 * gs_create_sql_limit_v2 — Create a new rate-limiting rule.
 *
 * Params: (limit_name, limit_type, work_node, max_concurrency,
 *          start_time, end_time, keyword, users)
 *
 * For sqlId type: keyword must contain a positive integer (unique_sql_id).
 *   hash is computed as hash_any(unique_sql_id) for syscache partitioning.
 * For other types: keyword is comma-separated substrings for query matching.
 *   Empty/NULL keyword matches all queries of the given type.
 *
 * rule_version is initialized to 1.  limit_id is atomically generated.
 * After insertion, the relcache callback marks the fast path dirty.
 */
Datum gs_create_sql_limit_v2(PG_FUNCTION_ARGS)
{
    ValidateV2LimitParams(true);

    /* Check rule count limit */
    Relation rel = heap_open(GsSqlLimitRuleRelationId, AccessShareLock);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
    uint32 ruleCount = 0;
    while (HeapTupleIsValid(systable_getnext(scan))) {
        ruleCount++;
    }
    systable_endscan(scan);
    heap_close(rel, AccessShareLock);

    if (ruleCount >= SQL_LIMIT_V2_RULE_MAX_COUNT) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("SQL limit V2 rule count(%u) has reached the limit(%d).",
                   ruleCount, SQL_LIMIT_V2_RULE_MAX_COUNT)));
    }

    /* Validate limit_type */
    if (PG_ARGISNULL(1)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limit_type can not be null.")));
    }
    char *sqlType = text_to_cstring(PG_GETARG_TEXT_P(1));
    if (strcasecmp(sqlType, SQLID_TYPE) != 0 && strcasecmp(sqlType, SELECT_TYPE) != 0 &&
        strcasecmp(sqlType, UPDATE_TYPE) != 0 && strcasecmp(sqlType, INSERT_TYPE) != 0 &&
        strcasecmp(sqlType, DELETE_TYPE) != 0) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("invalid limit_type: %s", sqlType)));
    }

    /* Validate required params */
    if (PG_ARGISNULL(2)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("work_node can not be null.")));
    }
    if (PG_ARGISNULL(3)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("max_concurrency can not be null.")));
    }

    /* Generate limit_id */
    uint64 limitId = pg_atomic_fetch_add_u64(&g_instance.sqlLimit_cxt.v2EntryIdSequence, 1);

    /* Compute hash and unique_sql_id for SQLID type */
    int64 hashVal = 0;
    int64 uniqueSqlId = 0;
    if (strcasecmp(sqlType, SQLID_TYPE) == 0) {
        if (PG_ARGISNULL(6)) {
            ereport(ERROR, (errmodule(MOD_WLM), errmsg("keyword must contain unique_sql_id for sqlId type.")));
        }
        char *keywordStr = text_to_cstring(PG_GETARG_TEXT_P(6));
        int64 sqlIdValue = 0;
        (void)scanint8(keywordStr, true, &sqlIdValue);
        if (sqlIdValue <= 0) {
            ereport(ERROR, (errmodule(MOD_WLM), errmsg("unique_sql_id must be positive, got: %ld", sqlIdValue)));
        }
        uniqueSqlId = sqlIdValue;
        uint32 h = DatumGetUInt32(hash_any((const unsigned char *)&uniqueSqlId, sizeof(uniqueSqlId)));
        hashVal = (int64)h;
        pfree_ext(keywordStr);
    }

    /* Build tuple */
    Datum values[Natts_gs_sql_limit_rule];
    bool nulls[Natts_gs_sql_limit_rule];
    errno_t rc = memset_s(nulls, sizeof(nulls), true, sizeof(nulls));
    securec_check(rc, "\0", "\0");
    rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "\0", "\0");

    values[Anum_gs_sql_limit_rule_limit_id - 1] = Int64GetDatum(limitId);
    nulls[Anum_gs_sql_limit_rule_limit_id - 1] = false;

    if (!PG_ARGISNULL(0)) {
        values[Anum_gs_sql_limit_rule_limit_name - 1] = PG_GETARG_DATUM(0);
    }
    nulls[Anum_gs_sql_limit_rule_limit_name - 1] = PG_ARGISNULL(0);

    values[Anum_gs_sql_limit_rule_enable - 1] = BoolGetDatum(true);
    nulls[Anum_gs_sql_limit_rule_enable - 1] = false;

    values[Anum_gs_sql_limit_rule_work_node - 1] = PG_GETARG_DATUM(2);
    nulls[Anum_gs_sql_limit_rule_work_node - 1] = false;

    values[Anum_gs_sql_limit_rule_max_concurrency - 1] = PG_GETARG_DATUM(3);
    nulls[Anum_gs_sql_limit_rule_max_concurrency - 1] = false;

    values[Anum_gs_sql_limit_rule_hash - 1] = Int64GetDatum(hashVal);
    nulls[Anum_gs_sql_limit_rule_hash - 1] = false;

    values[Anum_gs_sql_limit_rule_unique_sql_id - 1] = Int64GetDatum(uniqueSqlId);
    nulls[Anum_gs_sql_limit_rule_unique_sql_id - 1] = false;

    values[Anum_gs_sql_limit_rule_rule_version - 1] = Int64GetDatum(1);
    nulls[Anum_gs_sql_limit_rule_rule_version - 1] = false;

    if (!PG_ARGISNULL(4)) {
        values[Anum_gs_sql_limit_rule_start_time - 1] = PG_GETARG_DATUM(4);
    }
    nulls[Anum_gs_sql_limit_rule_start_time - 1] = PG_ARGISNULL(4);

    if (!PG_ARGISNULL(5)) {
        values[Anum_gs_sql_limit_rule_end_time - 1] = PG_GETARG_DATUM(5);
    }
    nulls[Anum_gs_sql_limit_rule_end_time - 1] = PG_ARGISNULL(5);

    values[Anum_gs_sql_limit_rule_limit_type - 1] = PG_GETARG_DATUM(1);
    nulls[Anum_gs_sql_limit_rule_limit_type - 1] = false;

    if (!PG_ARGISNULL(6)) {
        values[Anum_gs_sql_limit_rule_keyword - 1] = PG_GETARG_DATUM(6);
    }
    nulls[Anum_gs_sql_limit_rule_keyword - 1] = PG_ARGISNULL(6);

    if (!PG_ARGISNULL(7)) {
        values[Anum_gs_sql_limit_rule_users - 1] = PG_GETARG_DATUM(7);
    }
    nulls[Anum_gs_sql_limit_rule_users - 1] = PG_ARGISNULL(7);

    /* Insert into catalog */
    rel = heap_open(GsSqlLimitRuleRelationId, RowExclusiveLock);
    TupleDesc tupdesc = RelationGetDescr(rel);
    HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
    (void)simple_heap_insert(rel, tuple);
    CatalogUpdateIndexes(rel, tuple);
    heap_freetuple_ext(tuple);
    heap_close(rel, RowExclusiveLock);

    ereport(LOG, (errmsg("SQL limit V2: created rule, limitId=%lu, type=%s", limitId, sqlType)));
    pfree_ext(sqlType);

    PG_RETURN_INT64(limitId);
}

/*
 * gs_update_sql_limit_v2 — Update an existing rule.
 *
 * Params: (limit_id, limit_name, work_node, max_concurrency,
 *          start_time, end_time, keyword, users)
 * NULL arguments mean "no change" for that field.
 *
 * Side effects:
 *   - rule_version is always incremented (provides stats isolation)
 *   - If keyword changes and type is sqlId: hash and unique_sql_id are
 *     recalculated
 *   - Fast path is marked dirty via relcache callback
 *   - Old stats entries (previous rule_version) remain until lazy cleanup
 *
 * Version isolation: Old queries running under the previous version
 * continue to use their stats entry.  New queries use the new version's
 * entry with fresh counters (currConcurrency=0).
 */
Datum gs_update_sql_limit_v2(PG_FUNCTION_ARGS)
{
    ValidateV2LimitParams(false);

    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limit_id can not be null.")));
    }

    Datum limitIdDat = PG_GETARG_DATUM(0);
    int64 limitId = DatumGetInt64(limitIdDat);

    /* Find existing tuple */
    ScanKeyData key;
    ScanKeyInit(&key, (AttrNumber)Anum_gs_sql_limit_rule_limit_id,
        BTEqualStrategyNumber, F_INT8EQ, limitIdDat);
    Relation rel = heap_open(GsSqlLimitRuleRelationId, RowExclusiveLock);
    TupleDesc tupdesc = RelationGetDescr(rel);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
    HeapTuple oldTuple = systable_getnext(scan);

    if (!HeapTupleIsValid(oldTuple)) {
        systable_endscan(scan);
        heap_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limitId %ld does not exist.", limitId)));
    }

    /* Deform old tuple */
    Datum values[Natts_gs_sql_limit_rule];
    bool nulls[Natts_gs_sql_limit_rule];
    bool repl[Natts_gs_sql_limit_rule];
    errno_t rc = memset_s(repl, sizeof(repl), false, sizeof(repl));
    securec_check(rc, "\0", "\0");

    heap_deform_tuple(oldTuple, tupdesc, values, nulls);

    /* Update fields */
    if (!PG_ARGISNULL(1)) {
        values[Anum_gs_sql_limit_rule_limit_name - 1] = PG_GETARG_DATUM(1);
        nulls[Anum_gs_sql_limit_rule_limit_name - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_limit_name - 1] = true;

    if (!PG_ARGISNULL(2)) {
        values[Anum_gs_sql_limit_rule_work_node - 1] = PG_GETARG_DATUM(2);
        nulls[Anum_gs_sql_limit_rule_work_node - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_work_node - 1] = true;

    if (!PG_ARGISNULL(3)) {
        values[Anum_gs_sql_limit_rule_max_concurrency - 1] = PG_GETARG_DATUM(3);
        nulls[Anum_gs_sql_limit_rule_max_concurrency - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_max_concurrency - 1] = true;

    if (!PG_ARGISNULL(4)) {
        values[Anum_gs_sql_limit_rule_start_time - 1] = PG_GETARG_DATUM(4);
        nulls[Anum_gs_sql_limit_rule_start_time - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_start_time - 1] = true;

    if (!PG_ARGISNULL(5)) {
        values[Anum_gs_sql_limit_rule_end_time - 1] = PG_GETARG_DATUM(5);
        nulls[Anum_gs_sql_limit_rule_end_time - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_end_time - 1] = true;

    /* Update keyword and derived fields */
    bool keywordChanged = false;
    if (!PG_ARGISNULL(6)) {
        values[Anum_gs_sql_limit_rule_keyword - 1] = PG_GETARG_DATUM(6);
        nulls[Anum_gs_sql_limit_rule_keyword - 1] = false;
        keywordChanged = true;
    }
    repl[Anum_gs_sql_limit_rule_keyword - 1] = true;

    if (!PG_ARGISNULL(7)) {
        values[Anum_gs_sql_limit_rule_users - 1] = PG_GETARG_DATUM(7);
        nulls[Anum_gs_sql_limit_rule_users - 1] = false;
    }
    repl[Anum_gs_sql_limit_rule_users - 1] = true;

    /* Update hash and unique_sql_id if keyword changed */
    if (keywordChanged) {
        char *limitTypeStr = text_to_cstring(DatumGetTextP(values[Anum_gs_sql_limit_rule_limit_type - 1]));
        if (strcasecmp(limitTypeStr, SQLID_TYPE) == 0) {
            char *kwStr = text_to_cstring(DatumGetTextP(values[Anum_gs_sql_limit_rule_keyword - 1]));
            int64 sqlIdValue = 0;
            (void)scanint8(kwStr, true, &sqlIdValue);
            values[Anum_gs_sql_limit_rule_unique_sql_id - 1] = Int64GetDatum(sqlIdValue);
            nulls[Anum_gs_sql_limit_rule_unique_sql_id - 1] = false;
            uint32 h = DatumGetUInt32(hash_any((const unsigned char *)&sqlIdValue, sizeof(sqlIdValue)));
            values[Anum_gs_sql_limit_rule_hash - 1] = Int64GetDatum((int64)h);
            nulls[Anum_gs_sql_limit_rule_hash - 1] = false;
            pfree_ext(kwStr);
        } else {
            values[Anum_gs_sql_limit_rule_unique_sql_id - 1] = Int64GetDatum(0);
            nulls[Anum_gs_sql_limit_rule_unique_sql_id - 1] = false;
            values[Anum_gs_sql_limit_rule_hash - 1] = Int64GetDatum(0);
            nulls[Anum_gs_sql_limit_rule_hash - 1] = false;
        }
        repl[Anum_gs_sql_limit_rule_hash - 1] = true;
        repl[Anum_gs_sql_limit_rule_unique_sql_id - 1] = true;
        pfree_ext(limitTypeStr);
    }

    /* Increment rule_version */
    int64 oldVersion = DatumGetInt64(values[Anum_gs_sql_limit_rule_rule_version - 1]);
    values[Anum_gs_sql_limit_rule_rule_version - 1] = Int64GetDatum(oldVersion + 1);
    nulls[Anum_gs_sql_limit_rule_rule_version - 1] = false;
    repl[Anum_gs_sql_limit_rule_rule_version - 1] = true;

    /* Apply update */
    HeapTuple newTuple = heap_modify_tuple(oldTuple, tupdesc, values, nulls, repl);
    simple_heap_update(rel, &newTuple->t_self, newTuple);
    CatalogUpdateIndexes(rel, newTuple);
    heap_freetuple_ext(newTuple);
    systable_endscan(scan);
    heap_close(rel, RowExclusiveLock);

    ereport(LOG, (errmsg("SQL limit V2: updated rule, limitId=%ld, newVersion=%ld",
                         limitId, oldVersion + 1)));

    PG_RETURN_BOOL(true);
}

/*
 * gs_delete_sql_limit_v2 — Delete a rule by limit_id.
 *
 * Does NOT abort running queries.  Their session entries still hold
 * valid stats keys, and UnlimitCurrentQueryV2 will decrement
 * currConcurrency normally.  The stats entry is lazily cleaned up
 * when currConcurrency drops to 0 (LazyCleanupSqlLimitStatsEntry).
 *
 * After deletion, the fast path dirty flag triggers a refresh, and
 * subsequent queries skip limit checking if no other rules exist.
 */
Datum gs_delete_sql_limit_v2(PG_FUNCTION_ARGS)
{
    ValidateV2LimitParams(false);

    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limit_id can not be null.")));
    }

    Datum limitIdDat = PG_GETARG_DATUM(0);

    ScanKeyData key;
    ScanKeyInit(&key, (AttrNumber)Anum_gs_sql_limit_rule_limit_id,
        BTEqualStrategyNumber, F_INT8EQ, limitIdDat);
    Relation rel = heap_open(GsSqlLimitRuleRelationId, RowExclusiveLock);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
    HeapTuple tuple = systable_getnext(scan);

    if (!HeapTupleIsValid(tuple)) {
        systable_endscan(scan);
        heap_close(rel, RowExclusiveLock);
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limitId %ld does not exist.",
                                                   DatumGetInt64(limitIdDat))));
    }

    simple_heap_delete(rel, &tuple->t_self);
    systable_endscan(scan);
    heap_close(rel, RowExclusiveLock);

    ereport(LOG, (errmsg("SQL limit V2: deleted rule, limitId=%ld", DatumGetInt64(limitIdDat))));

    PG_RETURN_BOOL(true);
}

/*
 * gs_select_sql_limit_v2 — Query a single rule's metadata + live stats.
 *
 * Returns: (limit_id, limit_name, enable, max_concurrency, limit_type,
 *           keyword, rule_version, hit_count, reject_count, curr_concurrency)
 *
 * Stats are read atomically from the shared HTAB under LW_SHARED lock.
 * The statsKey is memset_s to 0 before use to ensure correct hash lookup
 * (padding bytes in SqlLimitStatsKey must be deterministic).
 */
Datum gs_select_sql_limit_v2(PG_FUNCTION_ARGS)
{
    if (!superuser()) {
        aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC, "must be system admin to execute");
    }

    if (!u_sess->attr.attr_common.enable_sql_limit) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), (errmsg("enable_sql_limit is off, please set to on"))));
    }

    if (PG_ARGISNULL(0)) {
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limit_id can not be null.")));
    }

    if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo)) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("set-valued function called in context that cannot accept a set")));
    }

    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
    const int SELECT_RETURN_ATTR_NUM = 10;
    int i = 1;
    MemoryContext oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    TupleDesc tupdesc = CreateTemplateTupleDesc(SELECT_RETURN_ATTR_NUM, false);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_id", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_name", NAMEOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "enable", BOOLOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "max_concurrency", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_type", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "keyword", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "rule_version", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "hit_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "reject_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "curr_concurrency", INT8OID, -1, 0);

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tuplestore_begin_heap(true, false, u_sess->attr.attr_memory.work_mem);
    rsinfo->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcontext);

    int64 limitId = DatumGetInt64(PG_GETARG_DATUM(0));

    /* Find rule from catalog */
    ScanKeyData key;
    ScanKeyInit(&key, (AttrNumber)Anum_gs_sql_limit_rule_limit_id,
        BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(limitId));
    Relation rel = heap_open(GsSqlLimitRuleRelationId, AccessShareLock);
    TupleDesc relTupdesc = RelationGetDescr(rel);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
    HeapTuple tuple = systable_getnext(scan);

    if (!HeapTupleIsValid(tuple)) {
        systable_endscan(scan);
        heap_close(rel, AccessShareLock);
        ereport(ERROR, (errmodule(MOD_WLM), errmsg("limitId %ld does not exist.", limitId)));
    }

    /* Extract metadata */
    bool isNull = false;
    Datum resultValues[SELECT_RETURN_ATTR_NUM];
    bool resultNulls[SELECT_RETURN_ATTR_NUM];
    errno_t rc = memset_s(resultNulls, sizeof(resultNulls), false, sizeof(resultNulls));
    securec_check(rc, "\0", "\0");

    resultValues[0] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_id, relTupdesc, &isNull);
    resultNulls[0] = isNull;

    resultValues[1] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_name, relTupdesc, &isNull);
    resultNulls[1] = isNull;

    resultValues[2] = heap_getattr(tuple, Anum_gs_sql_limit_rule_enable, relTupdesc, &isNull);
    resultNulls[2] = isNull;

    resultValues[3] = heap_getattr(tuple, Anum_gs_sql_limit_rule_max_concurrency, relTupdesc, &isNull);
    resultNulls[3] = isNull;

    resultValues[4] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_type, relTupdesc, &isNull);
    resultNulls[4] = isNull;

    resultValues[5] = heap_getattr(tuple, Anum_gs_sql_limit_rule_keyword, relTupdesc, &isNull);
    resultNulls[5] = isNull;

    resultValues[6] = heap_getattr(tuple, Anum_gs_sql_limit_rule_rule_version, relTupdesc, &isNull);
    resultNulls[6] = isNull;

    /* Read stats */
    uint64 ruleVersion = DatumGetUInt64(resultValues[6]);
    uint64 hitCount = 0;
    uint64 rejectCount = 0;
    uint64 currConcurrency = 0;

    if (g_instance.sqlLimit_cxt.statsHTAB != NULL) {
        LWLockAcquire(SqlLimitV2StatsLock, LW_SHARED);
        SqlLimitStatsKey statsKey;
        errno_t rc2 = memset_s(&statsKey, sizeof(statsKey), 0, sizeof(statsKey));
        securec_check(rc2, "\0", "\0");
        statsKey.pdbOid = 0;
        statsKey.limitId = (uint64)limitId;
        statsKey.ruleVersion = ruleVersion;
        SqlLimitStatsEntry *statsEntry = FindSqlLimitStatsEntry(&statsKey);
        if (statsEntry != NULL) {
            hitCount = pg_atomic_read_u64(&statsEntry->hitCount);
            rejectCount = pg_atomic_read_u64(&statsEntry->rejectCount);
            currConcurrency = pg_atomic_read_u64(&statsEntry->currConcurrency);
        }
        LWLockRelease(SqlLimitV2StatsLock);
    }

    resultValues[7] = Int64GetDatum(hitCount);
    resultValues[8] = Int64GetDatum(rejectCount);
    resultValues[9] = Int64GetDatum(currConcurrency);

    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, resultValues, resultNulls);

    systable_endscan(scan);
    heap_close(rel, AccessShareLock);
    tuplestore_donestoring(rsinfo->setResult);
    return (Datum)0;
}

/*
 * gs_select_sql_limit_all_v2 — Query all rules with their live stats.
 *
 * Same output schema as gs_select_sql_limit_v2 but returns one row per rule.
 * Scans the catalog sequentially and looks up stats for each rule.
 *
 * Note: When called with no arguments (zero-param form), the function
 * signature must match the SQL definition in the upgrade SQL file.
 * The zero-param variant is registered as a separate pg_proc entry.
 */
Datum gs_select_sql_limit_all_v2(PG_FUNCTION_ARGS)
{
    if (!superuser()) {
        aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC, "must be system admin to execute");
    }

    if (!u_sess->attr.attr_common.enable_sql_limit) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), (errmsg("enable_sql_limit is off, please set to on"))));
    }

    if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo)) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("set-valued function called in context that cannot accept a set")));
    }

    ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
    const int SELECT_RETURN_ATTR_NUM = 10;
    int i = 1;
    MemoryContext oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    TupleDesc tupdesc = CreateTemplateTupleDesc(SELECT_RETURN_ATTR_NUM, false);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_id", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_name", NAMEOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "enable", BOOLOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "max_concurrency", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "limit_type", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "keyword", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "rule_version", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "hit_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "reject_count", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber)i++, "curr_concurrency", INT8OID, -1, 0);

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tuplestore_begin_heap(true, false, u_sess->attr.attr_memory.work_mem);
    rsinfo->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcontext);

    /* Scan catalog */
    Relation rel = heap_open(GsSqlLimitRuleRelationId, AccessShareLock);
    TupleDesc relTupdesc = RelationGetDescr(rel);
    SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
    HeapTuple tuple;

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        bool isNull = false;
        Datum resultValues[SELECT_RETURN_ATTR_NUM];
        bool resultNulls[SELECT_RETURN_ATTR_NUM];
        errno_t rc = memset_s(resultNulls, sizeof(resultNulls), false, sizeof(resultNulls));
        securec_check(rc, "\0", "\0");

        resultValues[0] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_id, relTupdesc, &isNull);
        resultNulls[0] = isNull;
        resultValues[1] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_name, relTupdesc, &isNull);
        resultNulls[1] = isNull;
        resultValues[2] = heap_getattr(tuple, Anum_gs_sql_limit_rule_enable, relTupdesc, &isNull);
        resultNulls[2] = isNull;
        resultValues[3] = heap_getattr(tuple, Anum_gs_sql_limit_rule_max_concurrency, relTupdesc, &isNull);
        resultNulls[3] = isNull;
        resultValues[4] = heap_getattr(tuple, Anum_gs_sql_limit_rule_limit_type, relTupdesc, &isNull);
        resultNulls[4] = isNull;
        resultValues[5] = heap_getattr(tuple, Anum_gs_sql_limit_rule_keyword, relTupdesc, &isNull);
        resultNulls[5] = isNull;
        resultValues[6] = heap_getattr(tuple, Anum_gs_sql_limit_rule_rule_version, relTupdesc, &isNull);
        resultNulls[6] = isNull;

        /* Read stats */
        uint64 limitId = isNull ? 0 : DatumGetUInt64(resultValues[0]);
        uint64 ruleVersion = isNull ? 0 : DatumGetUInt64(resultValues[6]);
        uint64 hitCount = 0, rejectCount = 0, currConcurrency = 0;

        if (g_instance.sqlLimit_cxt.statsHTAB != NULL && !isNull) {
            LWLockAcquire(SqlLimitV2StatsLock, LW_SHARED);
            SqlLimitStatsKey statsKey;
            errno_t rc3 = memset_s(&statsKey, sizeof(statsKey), 0, sizeof(statsKey));
            securec_check(rc3, "\0", "\0");
            statsKey.pdbOid = 0;
            statsKey.limitId = limitId;
            statsKey.ruleVersion = ruleVersion;
            SqlLimitStatsEntry *statsEntry = FindSqlLimitStatsEntry(&statsKey);
            if (statsEntry != NULL) {
                hitCount = pg_atomic_read_u64(&statsEntry->hitCount);
                rejectCount = pg_atomic_read_u64(&statsEntry->rejectCount);
                currConcurrency = pg_atomic_read_u64(&statsEntry->currConcurrency);
            }
            LWLockRelease(SqlLimitV2StatsLock);
        }

        resultValues[7] = Int64GetDatum(hitCount);
        resultValues[8] = Int64GetDatum(rejectCount);
        resultValues[9] = Int64GetDatum(currConcurrency);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, resultValues, resultNulls);
    }

    systable_endscan(scan);
    heap_close(rel, AccessShareLock);
    tuplestore_donestoring(rsinfo->setResult);
    return (Datum)0;
}
