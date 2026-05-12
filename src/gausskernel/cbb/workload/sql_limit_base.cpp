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
 * sql_limit_base.cpp
 *
 * The file is used to provide the interfaces of sql limit management.
 *
 * IDENTIFICATION
 *	  src/gausskernel/cbb/workload/sql_limit_base.cpp
 *
 * -------------------------------------------------------------------------
 */

#include "workload/sql_limit_base.h"
#include "catalog/gs_sql_limit.h"
#include "catalog/gs_sql_limit_rule.h"
#include "catalog/indexing.h"
#include "storage/lock/lwlock.h"
#include "storage/shmem.h"
#include "utils/atomic.h"
#include "utils/hsearch.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/array.h"
#include "utils/mem_snapshot.h"
#include "utils/fmgroids.h"
#include "knl/knl_variable.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "access/xact.h"

static const char* const g_sqlLimitRuleTypes[] = {SQLID_TYPE, SELECT_TYPE, INSERT_TYPE, UPDATE_TYPE, DELETE_TYPE};

static bool SqlLimitStatsRuleExists(const SqlLimitStatsKey* key);
static void SqlLimitStatsEntryInit(SqlLimitStatsEntry* entry, const SqlLimitStatsKey* key);
static void SqlLimitStatsCleanupStaleEntries(Oid pdbOid);
static SqlLimitFastPathEntry *SqlLimitFastPathLookupOrCreate(Oid pdbOid, bool *found);
static void SqlLimitFastPathEntryInit(SqlLimitFastPathEntry *entry, Oid pdbOid);
static void SqlLimitSyscacheCallback(Datum arg, int cacheid, uint32 hashvalue);

Size SqlLimitStatsShmemSize(void)
{
    return hash_estimate_size(SQL_LIMIT_STATS_HASH_SIZE, sizeof(SqlLimitStatsEntry));
}

Size SqlLimitFastPathShmemSize(void)
{
    return hash_estimate_size(SQL_LIMIT_FAST_PATH_HASH_SIZE, sizeof(SqlLimitFastPathEntry));
}

void SqlLimitStatsKeyInit(SqlLimitStatsKey *key, Oid pdbOid, uint64 limitId, uint64 ruleVersion)
{
    if (key == NULL) {
        return;
    }

    MemSet(key, 0, sizeof(*key));
    key->pdbOid = pdbOid;
    key->limitId = limitId;
    key->ruleVersion = ruleVersion;
}

static void SqlLimitStatsEntryInit(SqlLimitStatsEntry *entry, const SqlLimitStatsKey *key)
{
    if (entry == NULL || key == NULL) {
        return;
    }

    entry->key = *key;
    pg_atomic_init_u64(&entry->hitCount, 0);
    pg_atomic_init_u64(&entry->rejectCount, 0);
    pg_atomic_init_u64(&entry->currConcurrency, 0);
}

void SqlLimitStatsShmemInit(void)
{
    if (g_instance.sqlLimit_cxt.statsHash != NULL) {
        return;
    }

    if (g_instance.sqlLimit_cxt.statsLock == NULL) {
        g_instance.sqlLimit_cxt.statsLock = LWLockAssign(LWTRANCHE_EXTEND);
    }

    HASHCTL hashCtl;
    errno_t rc = memset_s(&hashCtl, sizeof(hashCtl), 0, sizeof(hashCtl));
    securec_check(rc, "\0", "\0");
    hashCtl.keysize = sizeof(SqlLimitStatsKey);
    hashCtl.entrysize = sizeof(SqlLimitStatsEntry);
    g_instance.sqlLimit_cxt.statsHash = ShmemInitHash("SQL Limit Stats Hash",
        SQL_LIMIT_STATS_HASH_SIZE,
        SQL_LIMIT_STATS_HASH_SIZE,
        &hashCtl,
        HASH_ELEM | HASH_BLOBS);
}

static void SqlLimitFastPathEntryInit(SqlLimitFastPathEntry *entry, Oid pdbOid)
{
    if (entry == NULL) {
        return;
    }

    entry->pdbOid = pdbOid;
    pg_atomic_init_u32(&entry->dirty, 1);
    pg_atomic_init_u32(&entry->activeRuleCount, 0);
}

void SqlLimitFastPathShmemInit(void)
{
    if (g_instance.sqlLimit_cxt.fastPathHash != NULL) {
        return;
    }

    if (g_instance.sqlLimit_cxt.fastPathLock == NULL) {
        g_instance.sqlLimit_cxt.fastPathLock = LWLockAssign(LWTRANCHE_EXTEND);
    }

    HASHCTL hashCtl;
    errno_t rc = memset_s(&hashCtl, sizeof(hashCtl), 0, sizeof(hashCtl));
    securec_check(rc, "\0", "\0");
    hashCtl.keysize = sizeof(Oid);
    hashCtl.entrysize = sizeof(SqlLimitFastPathEntry);
    g_instance.sqlLimit_cxt.fastPathHash = ShmemInitHash("SQL Limit Fast Path Hash",
        SQL_LIMIT_FAST_PATH_HASH_SIZE,
        SQL_LIMIT_FAST_PATH_HASH_SIZE,
        &hashCtl,
        HASH_ELEM | HASH_BLOBS);
}

static SqlLimitFastPathEntry *SqlLimitFastPathLookupOrCreate(Oid pdbOid, bool *found)
{
    if (g_instance.sqlLimit_cxt.fastPathHash == NULL || g_instance.sqlLimit_cxt.fastPathLock == NULL) {
        if (found != NULL) {
            *found = false;
        }
        return NULL;
    }

    bool localFound = false;
    LWLockAcquire(g_instance.sqlLimit_cxt.fastPathLock, LW_EXCLUSIVE);
    SqlLimitFastPathEntry *entry = (SqlLimitFastPathEntry *)hash_search(g_instance.sqlLimit_cxt.fastPathHash,
        (void *)&pdbOid, HASH_ENTER, &localFound);
    if (entry != NULL && !localFound) {
        SqlLimitFastPathEntryInit(entry, pdbOid);
    }
    LWLockRelease(g_instance.sqlLimit_cxt.fastPathLock);

    if (found != NULL) {
        *found = localFound;
    }
    return entry;
}

void MarkSqlLimitFastPathDirty(Oid pdbOid)
{
    bool found = false;
    SqlLimitFastPathEntry *entry = SqlLimitFastPathLookupOrCreate(pdbOid, &found);
    if (entry == NULL) {
        return;
    }

    pg_atomic_write_u32(&entry->dirty, 1);
}

static void SqlLimitSyscacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
    MarkSqlLimitFastPathDirty((Oid)DatumGetUInt64(arg));
}

bool RefreshSqlLimitFastPath(Oid pdbOid)
{
    bool found = false;
    SqlLimitFastPathEntry *entry = SqlLimitFastPathLookupOrCreate(pdbOid, &found);
    if (entry == NULL) {
        return false;
    }

    CatCList *catlist = SearchSysCacheList1(GSSQLLIMIT, BoolGetDatum(true));
    uint32 activeRuleCount = (uint32)catlist->n_members;
    ReleaseSysCacheList(catlist);

    SqlLimitStatsCleanupStaleEntries(pdbOid);

    pg_atomic_write_u32(&entry->dirty, 0);
    pg_atomic_write_u32(&entry->activeRuleCount, activeRuleCount);
    return true;
}

bool SqlLimitNeedCheck(const char *commandTag)
{
    if (commandTag == NULL || strlen(commandTag) == 0 || !u_sess->attr.attr_common.enable_sql_limit) {
        return false;
    }

    if (!t_thrd.sql_limit_cxt.syscacheCallbackRegistered) {
        CacheRegisterThreadSyscacheCallback(GSSQLLIMIT, SqlLimitSyscacheCallback, UInt64GetDatum(0));
        t_thrd.sql_limit_cxt.syscacheCallbackRegistered = true;
    }

    AcceptInvalidationMessages();

    Oid pdbOid = 0;
    bool found = false;
    SqlLimitFastPathEntry *entry = SqlLimitFastPathLookupOrCreate(pdbOid, &found);
    if (entry == NULL) {
        return true;
    }

    if (t_thrd.sql_limit_cxt.verifiedPdbSet == NULL ||
        !bms_is_member((int)pdbOid, t_thrd.sql_limit_cxt.verifiedPdbSet)) {
        (void)RefreshSqlLimitFastPath(pdbOid);
        MemoryContext oldCxt = MemoryContextSwitchTo(THREAD_GET_MEM_CXT_GROUP(MEMORY_CONTEXT_CBB));
        t_thrd.sql_limit_cxt.verifiedPdbSet = bms_add_member(t_thrd.sql_limit_cxt.verifiedPdbSet, (int)pdbOid);
        MemoryContextSwitchTo(oldCxt);
    }

    if (pg_atomic_read_u32(&entry->dirty) != 0) {
        (void)RefreshSqlLimitFastPath(pdbOid);
    }

    if (pg_atomic_read_u32(&entry->dirty) == 0 && pg_atomic_read_u32(&entry->activeRuleCount) == 0) {
        return false;
    }

    return true;
}

bool SqlLimitStatsReserve(const SqlLimitStatsKey *key, uint64 maxConcurrency, uint64 *currConcurrency)
{
    if (key == NULL || g_instance.sqlLimit_cxt.statsHash == NULL || g_instance.sqlLimit_cxt.statsLock == NULL) {
        if (currConcurrency != NULL) {
            *currConcurrency = 0;
        }
        return false;
    }

    bool localFound = false;
    LWLockAcquire(g_instance.sqlLimit_cxt.statsLock, LW_EXCLUSIVE);
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(g_instance.sqlLimit_cxt.statsHash,
        (void *)key, HASH_ENTER, &localFound);
    if (entry != NULL && !localFound) {
        SqlLimitStatsEntryInit(entry, key);
    }
    if (entry == NULL) {
        LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
        if (currConcurrency != NULL) {
            *currConcurrency = 0;
        }
        return false;
    }

    pg_atomic_fetch_add_u64(&entry->hitCount, 1);

    uint64 oldValue;
    uint64 newValue;
    do {
        oldValue = pg_atomic_read_u64(&entry->currConcurrency);
        if (oldValue >= maxConcurrency) {
            pg_atomic_fetch_add_u64(&entry->rejectCount, 1);
            if (currConcurrency != NULL) {
                *currConcurrency = oldValue;
            }
            LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
            ereport(DEBUG1,
                (errmodule(MOD_WLM),
                    errmsg("sql limit stats reserve rejected, limitId: %lu, currConcurrency: %lu, "
                        "maxConcurrency: %lu",
                        key->limitId, oldValue, maxConcurrency)));
            return false;
        }
        newValue = oldValue + 1;
    } while (!pg_atomic_compare_exchange_u64(&entry->currConcurrency, &oldValue, newValue));

    if (currConcurrency != NULL) {
        *currConcurrency = newValue;
    }
    LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
    return true;
}

bool SqlLimitStatsSnapshot(const SqlLimitStatsKey *key, uint64 *hitCount, uint64 *rejectCount, uint64 *currConcurrency)
{
    if (hitCount != NULL) {
        *hitCount = 0;
    }
    if (rejectCount != NULL) {
        *rejectCount = 0;
    }
    if (currConcurrency != NULL) {
        *currConcurrency = 0;
    }
    if (key == NULL || g_instance.sqlLimit_cxt.statsHash == NULL || g_instance.sqlLimit_cxt.statsLock == NULL) {
        return false;
    }

    bool found = false;
    LWLockAcquire(g_instance.sqlLimit_cxt.statsLock, LW_SHARED);
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(g_instance.sqlLimit_cxt.statsHash,
        (void *)key, HASH_FIND, &found);
    if (found && entry != NULL) {
        if (hitCount != NULL) {
            *hitCount = pg_atomic_read_u64(&entry->hitCount);
        }
        if (rejectCount != NULL) {
            *rejectCount = pg_atomic_read_u64(&entry->rejectCount);
        }
        if (currConcurrency != NULL) {
            *currConcurrency = pg_atomic_read_u64(&entry->currConcurrency);
        }
    }
    LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
    return found && entry != NULL;
}

uint64 SqlLimitStatsRelease(const SqlLimitStatsKey *key)
{
    if (key == NULL || g_instance.sqlLimit_cxt.statsHash == NULL || g_instance.sqlLimit_cxt.statsLock == NULL) {
        return 0;
    }

    bool found = false;
    uint64 newValue = 0;

    LWLockAcquire(g_instance.sqlLimit_cxt.statsLock, LW_EXCLUSIVE);
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(g_instance.sqlLimit_cxt.statsHash,
        (void *)key, HASH_FIND, &found);
    if (!found || entry == NULL) {
        LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
        return 0;
    }

    uint64 oldValue;
    do {
        oldValue = pg_atomic_read_u64(&entry->currConcurrency);
        if (oldValue == 0) {
            break;
        }
        newValue = oldValue - 1;
    } while (!pg_atomic_compare_exchange_u64(&entry->currConcurrency, &oldValue, newValue));
    LWLockRelease(g_instance.sqlLimit_cxt.statsLock);

    if (newValue == 0) {
        (void)SqlLimitStatsCleanupIfUnused(key);
    }

    return newValue;
}

static bool SqlLimitStatsRuleExistsByEnable(bool enable)
{
    CatCList *catlist = SearchSysCacheList1(GSSQLLIMIT, BoolGetDatum(enable));
    for (int j = 0; j < catlist->n_members; ++j) {
        HeapTuple tuple = t_thrd.lsc_cxt.FetchTupleFromCatCList(catlist, j);
        bool isNull = false;
        uint64 limitId = DatumGetInt64(SysCacheGetAttr(GSSQLLIMIT, tuple, Anum_gs_sql_limit_rule_limit_id,
            &isNull));
        if (isNull || limitId != key->limitId) {
            continue;
        }
        uint64 ruleVersion = DatumGetInt64(SysCacheGetAttr(GSSQLLIMIT, tuple,
            Anum_gs_sql_limit_rule_rule_version, &isNull));
        if (!isNull && ruleVersion == key->ruleVersion) {
            ReleaseSysCacheList(catlist);
            return true;
        }
    }
    ReleaseSysCacheList(catlist);
}

static bool SqlLimitStatsRuleExists(const SqlLimitStatsKey *key)
{
    if (key == NULL) {
        return false;
    }

    if (SqlLimitStatsRuleExistsByEnable(true) || SqlLimitStatsRuleExistsByEnable(false)) {
        return true;
    }

    return false;
}

bool SqlLimitStatsCleanupIfUnused(const SqlLimitStatsKey *key)
{
    if (key == NULL || g_instance.sqlLimit_cxt.statsHash == NULL || g_instance.sqlLimit_cxt.statsLock == NULL) {
        return false;
    }

    if (SqlLimitStatsRuleExists(key)) {
        return false;
    }

    bool found = false;
    LWLockAcquire(g_instance.sqlLimit_cxt.statsLock, LW_EXCLUSIVE);
    SqlLimitStatsEntry *entry = (SqlLimitStatsEntry *)hash_search(g_instance.sqlLimit_cxt.statsHash,
        (void *)key, HASH_FIND, &found);
    if (!found || entry == NULL || pg_atomic_read_u64(&entry->currConcurrency) != 0) {
        LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
        return false;
    }

    (void)hash_search(g_instance.sqlLimit_cxt.statsHash, (void *)key, HASH_REMOVE, NULL);
    LWLockRelease(g_instance.sqlLimit_cxt.statsLock);
    return true;
}

static void SqlLimitStatsCleanupStaleEntries(Oid pdbOid)
{
    if (g_instance.sqlLimit_cxt.statsHash == NULL || g_instance.sqlLimit_cxt.statsLock == NULL) {
        return;
    }

    List *staleKeys = NIL;
    HASH_SEQ_STATUS status;
    LWLockAcquire(g_instance.sqlLimit_cxt.statsLock, LW_SHARED);
    hash_seq_init(&status, g_instance.sqlLimit_cxt.statsHash);
    SqlLimitStatsEntry *entry = NULL;
    while ((entry = (SqlLimitStatsEntry *)hash_seq_search(&status)) != NULL) {
        if (entry->key.pdbOid != pdbOid || pg_atomic_read_u64(&entry->currConcurrency) != 0) {
            continue;
        }

        SqlLimitStatsKey *key = (SqlLimitStatsKey *)palloc(sizeof(SqlLimitStatsKey));
        *key = entry->key;
        staleKeys = lappend(staleKeys, key);
    }
    LWLockRelease(g_instance.sqlLimit_cxt.statsLock);

    foreach_cell(cell, staleKeys) {
        SqlLimitStatsKey *key = (SqlLimitStatsKey *)lfirst(cell);
        (void)SqlLimitStatsCleanupIfUnused(key);
    }
    list_free_deep(staleKeys);
}

void TimeWindowInit(TimeWindow *window)
{
    if (window == NULL) {
        return;
    }

    window->startTime = 0;
    window->endTime = 0;
}

void TimeWindowSet(TimeWindow *window, TimestampTz start, TimestampTz end)
{
    if (window == NULL) {
        return;
    }

    window->startTime = start;
    window->endTime = end;
}

bool TimeWindowContainsTime(const TimeWindow *window)
{
    return TimeWindowContainsTimestamp(window, GetCurrentTimestamp());
}

bool TimeWindowContainsTimestamp(const TimeWindow *window, TimestampTz ts)
{
    if (window == NULL) {
        return false;
    }

    if (window->startTime == 0 || ts >= window->startTime) {
        if (window->endTime == 0 || ts <= window->endTime) {
            return true;
        }
    }

    return false;
}

void LimitStatsInit(LimitStats *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->hitCount = 0;
    stats->rejectCount = 0;
    stats->currConcurrency = 0;
}

void LimitStatsUpdateHit(LimitStats *stats)
{
    if (stats == NULL) {
        return;
    }

    pg_atomic_fetch_add_u64((pg_atomic_uint64 *)&stats->hitCount, 1);
}

void LimitStatsUpdateReject(LimitStats *stats)
{
    if (stats == NULL) {
        return;
    }

    pg_atomic_fetch_add_u64((pg_atomic_uint64 *)&stats->rejectCount, 1);
}

void LimitStatsUpdateConcurrency(LimitStats *stats, bool increase)
{
    if (stats == NULL) {
        return;
    }

    if (increase) {
        pg_atomic_fetch_add_u64((pg_atomic_uint64 *)&stats->currConcurrency, 1);
        return;
    }

    uint64 oldValue;
    uint64 newValue;
    do {
        oldValue = stats->currConcurrency;
        if (oldValue <= 0) {
            break;
        }
        newValue = oldValue - 1;
    } while (!pg_atomic_compare_exchange_u64((pg_atomic_uint64 *)&stats->currConcurrency, &oldValue, newValue));
}

bool LimitStatsTryIncreaseConcurrency(LimitStats *stats, uint64 maxConcurrency)
{
    if (stats == NULL) {
        return false;
    }

    uint64 oldValue;
    uint64 newValue;
    do {
        oldValue = stats->currConcurrency;
        if (oldValue >= maxConcurrency) {
            return false;
        }
        newValue = oldValue + 1;
    } while (!pg_atomic_compare_exchange_u64((pg_atomic_uint64 *)&stats->currConcurrency, &oldValue, newValue));
    return true;
}

void LimitStatsDecreaseConcurrency(LimitStats *stats)
{
    if (stats == NULL) {
        return;
    }

    uint64 oldValue;
    uint64 newValue;
    do {
        oldValue = stats->currConcurrency;
        if (oldValue <= 0) {
            break;
        }
        newValue = oldValue - 1;
    } while (!pg_atomic_compare_exchange_u64((pg_atomic_uint64 *)&stats->currConcurrency, &oldValue, newValue));
}

void LimitStatsReset(LimitStats *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->hitCount = 0;
    stats->rejectCount = 0;
    stats->currConcurrency = 0;
}


SqlLimit *SqlLimitCreate(uint64 limitId, SqlType sqlType)
{
    SqlLimit *limit = (SqlLimit *)palloc0(sizeof(SqlLimit));

    limit->limitId = limitId;
    limit->sqlType = sqlType;
    limit->databases = NIL;
    limit->users = NIL;
    limit->maxConcurrency = 0;
    limit->workNode = 0;
    limit->isValid = true;
    limit->xmin = InvalidTransactionId;
    TimeWindowInit(&limit->timeWindow);
    LimitStatsInit(&limit->stats);
    limit->typeData.keyword.keywords = NIL;
    limit->typeData.uniqueSql.uniqueSqlId = 0;

    return limit;
}

void SqlLimitClear(SqlLimit *limit)
{
    if (limit == NULL) {
        return;
    }

    SqlType sqlType = limit->sqlType;

    limit->sqlType = SQL_TYPE_OTHER;
    limit->maxConcurrency = 0;
    limit->isValid = false;
    limit->workNode = 0;
    TimeWindowInit(&limit->timeWindow);
    LimitStatsInit(&limit->stats);

    list_free_ext(limit->databases);
    list_free_ext(limit->users);

    if (IsKeywordsLimit(sqlType)) {
        list_free_deep(limit->typeData.keyword.keywords);
    } else if (sqlType == SQL_TYPE_UNIQUE_SQLID) {
        limit->typeData.uniqueSql.uniqueSqlId = 0;
    }
}

void SqlLimitDestroy(SqlLimit *limit)
{
    if (limit == NULL) {
        return;
    }

    if (limit->databases != NIL) {
        list_free_ext(limit->databases);
    }

    if (limit->users != NIL) {
        list_free_ext(limit->users);
    }

    if (IsKeywordsLimit(limit->sqlType)) {
        if (limit->typeData.keyword.keywords != NIL) {
            list_free_deep(limit->typeData.keyword.keywords);
        }
    } else if (limit->sqlType == SQL_TYPE_UNIQUE_SQLID) {
        limit->typeData.uniqueSql.uniqueSqlId = 0;
    }

    pfree(limit);
}

void SqlLimitSetDatabases(SqlLimit *limit, List *databases)
{
    if (limit == NULL) {
        return;
    }

    if (limit->databases != NIL) {
        list_free_ext(limit->databases);
    }

    limit->databases = databases;
}

void SqlLimitSetUsers(SqlLimit *limit, List *users)
{
    if (limit == NULL) {
        return;
    }

    if (limit->users != NIL) {
        list_free_ext(limit->users);
    }

    limit->users = users;
}

bool SqlLimitIsValidTime(const SqlLimit *limit)
{
    if (limit == NULL) {
        return false;
    }

    return TimeWindowContainsTime(&limit->timeWindow);
}

bool SqlLimitIsValidNode(const SqlLimit *limit)
{
    if (limit == NULL) {
        return false;
    }

    if (limit->workNode == 0) {
        return true;
    } else if (limit->workNode == 1) {
        return (!RecoveryInProgress()); // master node
    } else {
        return (RecoveryInProgress()); // standby node
    }
}

bool SqlLimitIsValidDatabases(const SqlLimit *limit)
{
    if (limit == NULL) {
        return false;
    }

    if (list_length(limit->databases) == 0) {
        return true;
    }

    foreach_cell(cell, limit->databases) {
        Oid dbOid = lfirst_oid(cell);
        if (u_sess->proc_cxt.MyDatabaseId == dbOid) {
            return true;
        }
    }

    return false;
}

bool SqlLimitIsValidUsers(const SqlLimit *limit)
{
    if (limit == NULL) {
        return false;
    }

    // if no users, means all users need to be limited.
    if (list_length(limit->users) == 0) {
        return true;
    }

    // otherwise, limit the user in the list.
    foreach_cell(cell, limit->users) {
        Oid userOid = lfirst_oid(cell);
        if (GetCurrentUserId() == userOid) {
            return true;
        }
    }
    return false;
}

bool SqlLimitIsExceedMaxConcurrency(const SqlLimit *limit)
{
    if (limit == NULL) {
        return false;
    }

    return limit->stats.currConcurrency >= limit->maxConcurrency;
}

bool SqlLimitIsHit(const SqlLimit *limit, const char *queryString, uint64 queryId)
{
    if (limit == NULL || !limit->isValid) {
        return false;
    }

    if (!SqlLimitIsValidTime(limit) || !SqlLimitIsValidNode(limit) || !SqlLimitIsValidUsers(limit)) {
        return false;
    }

    switch (limit->sqlType) {
        case SQL_TYPE_UNIQUE_SQLID:
            return (queryId == limit->typeData.uniqueSql.uniqueSqlId);

        case SQL_TYPE_SELECT:
        case SQL_TYPE_INSERT:
        case SQL_TYPE_UPDATE:
        case SQL_TYPE_DELETE: {
            if (queryString == NULL || limit->typeData.keyword.keywords == NIL) {
                return false;
            }

            if (!SqlLimitIsValidDatabases(limit)) {
                return false;
            }

            const char* currentPos = queryString;
            foreach_cell(cell, limit->typeData.keyword.keywords) {
                const char* keyword = (const char*)lfirst(cell);
                // find keyword in current position and after
                const char* foundPos = strcasestr(currentPos, keyword);
                if (foundPos == NULL) {
                    return false;
                }

                // update current position to found keyword position
                currentPos = foundPos + strlen(keyword);
            }
            return true;
        }
        case SQL_TYPE_OTHER:
            return false;
        default:
            return false;
    }
}
