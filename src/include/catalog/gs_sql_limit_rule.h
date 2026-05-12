/*
 * Copyright (c) 2026 Huawei Technologies Co.,Ltd.
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
 * gs_sql_limit_rule.h
 *
 * IDENTIFICATION
 *    src/include/catalog/gs_sql_limit_rule.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef GS_SQL_LIMIT_RULE_H
#define GS_SQL_LIMIT_RULE_H

#include "postgres.h"
#include "catalog/genbki.h"

/* to make data type compatible for genbki */
#define int8 int64

#define GsSqlLimitRuleRelationId  9041
#define GsSqlLimitRuleRelationId_Rowtype_Id 9042

CATALOG(gs_sql_limit_rule,9041) BKI_SHARED_RELATION BKI_ROWTYPE_OID(9042) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    int8            limit_id;
    NameData        limit_name;
    bool            enable;
    int4            work_node;
    int4            max_concurrency;
#ifdef CATALOG_VARLEN           /* variable-length fields start here */
    timestamp       start_time;
    timestamp       end_time;
    text            limit_type;
    int8            hash;
    int8            unique_sql_id;
    text            keyword;
    int8            rule_version;
    text            users;
#endif
} FormData_gs_sql_limit_rule;

#undef int8

typedef FormData_gs_sql_limit_rule *Form_gs_sql_limit_rule;

#define Natts_gs_sql_limit_rule                      13

#define Anum_gs_sql_limit_rule_limit_id              1
#define Anum_gs_sql_limit_rule_limit_name            2
#define Anum_gs_sql_limit_rule_enable                3
#define Anum_gs_sql_limit_rule_work_node             4
#define Anum_gs_sql_limit_rule_max_concurrency       5
#define Anum_gs_sql_limit_rule_start_time            6
#define Anum_gs_sql_limit_rule_end_time              7
#define Anum_gs_sql_limit_rule_limit_type            8
#define Anum_gs_sql_limit_rule_hash                  9
#define Anum_gs_sql_limit_rule_unique_sql_id         10
#define Anum_gs_sql_limit_rule_keyword               11
#define Anum_gs_sql_limit_rule_rule_version          12
#define Anum_gs_sql_limit_rule_users                 13

#endif   /* GS_SQL_LIMIT_RULE_H */
