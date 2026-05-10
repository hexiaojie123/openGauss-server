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
 * sql_limit_v2.h
 *
 * V2 syscache-based SQL limit: stats HTAB, fast path, runtime matching,
 * and management function declarations.
 *
 * IDENTIFICATION
 *    src/include/workload/sql_limit_v2.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef SQL_LIMIT_V2_H
#define SQL_LIMIT_V2_H

#include "postgres.h"
#include "workload/sql_limit_base.h"

/* ---- Stats HTAB functions ---- */

/*
 * Initialize the V2 stats HTAB in shared memory.
 * Called from CreateSharedMemoryAndSemaphores.
 */
extern void InitSqlLimitV2StatsHTAB(void);

/*
 * Try to atomically reserve a concurrency slot using CAS.
 * Returns true if reservation succeeded (currConcurrency < maxConcurrency).
 */
extern bool TryReserveSqlLimit(SqlLimitStatsEntry *entry, uint64 maxConcurrency);

/*
 * Find or create a stats entry for the given key.
 * Must be called with SqlLimitV2StatsLock held.
 */
extern SqlLimitStatsEntry *FindOrCreateSqlLimitStatsEntry(const SqlLimitStatsKey *key);

/*
 * Atomically release one concurrency slot for the given stats entry.
 * If currConcurrency drops to 0, performs lazy cleanup check.
 */
extern void ReleaseSqlLimitStatsConcurrency(const SqlLimitStatsKey *key);

/*
 * Lazy cleanup: if currConcurrency == 0 and no rule exists in syscache
 * for this (limitId, ruleVersion), delete the stats entry.
 * Must be called with SqlLimitV2StatsLock held.
 */
extern void LazyCleanupSqlLimitStatsEntry(SqlLimitStatsEntry *entry);

/*
 * Initialize the V2 limit_id sequence from existing catalog data.
 */
extern void InitSqlLimitV2EntryIdSequence(void);

/* ---- Fast Path functions ---- */

/*
 * Initialize the fast path HTAB in shared memory.
 */
extern void InitSqlLimitV2FastPathHTAB(void);

/*
 * Mark the fast path entry dirty for the given PDB (or all PDBs if InvalidOid).
 */
extern void MarkSqlLimitFastPathDirty(Oid pdbOid);

/*
 * Relcache callback for gs_sql_limit_rule invalidation.
 * Marks fast path entries dirty.
 */
extern void SqlLimitRelcacheCallback(Datum arg, Oid relid);

/*
 * Refresh fast path summary for the given PDB by scanning gs_sql_limit_rule.
 */
extern void RefreshSqlLimitFastPath(Oid pdbOid);

/*
 * Entry point check: returns true if SQL limit checking is needed.
 * Handles fast path check, callback registration, and first-use verification.
 */
extern bool SqlLimitNeedCheck(const char *commandTag);

/* ---- Runtime matching V2 functions ---- */

/*
 * V2 LimitCurrentQuery: matches SQL against rules from syscache,
 * performs CAS reservation, records matched key in session.
 */
extern void LimitCurrentQueryV2(const char *commandTag, const char *queryString);

/*
 * V2 UnlimitCurrentQuery: releases concurrency by session-recorded keys,
 * performs lazy cleanup of stats entries.
 */
extern void UnlimitCurrentQueryV2(void);

/* ---- Management function V2 declarations ---- */

extern Datum gs_create_sql_limit_v2(PG_FUNCTION_ARGS);
extern Datum gs_update_sql_limit_v2(PG_FUNCTION_ARGS);
extern Datum gs_delete_sql_limit_v2(PG_FUNCTION_ARGS);
extern Datum gs_select_sql_limit_v2(PG_FUNCTION_ARGS);
extern Datum gs_select_sql_limit_all_v2(PG_FUNCTION_ARGS);

#endif /* SQL_LIMIT_V2_H */
