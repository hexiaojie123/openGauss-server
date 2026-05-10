/* -------------------------------------------------------------------------
 *
 * gs_sql_limit_rule.h
 *    definition of the SQL limit rule relation (gs_sql_limit_rule)
 *
 * -------------------------------------------------------------------------
 */
#ifndef GS_SQL_LIMIT_RULE_H
#define GS_SQL_LIMIT_RULE_H

#include "postgres.h"
#include "catalog/genbki.h"

#define int8 int64

#define GsSqlLimitRuleRelationId  9060
#define GsSqlLimitRuleRelationId_Rowtype_Id  9061

CATALOG(gs_sql_limit_rule,9060) BKI_SHARED_RELATION BKI_ROWTYPE_OID(9061) BKI_WITHOUT_OIDS BKI_SCHEMA_MACRO
{
    int8            limit_id;
    NameData        limit_name;
    bool            enable;
    int4            work_node;
    int4            max_concurrency;
    int8            hash;
    int8            unique_sql_id;
    int8            rule_version;
#ifdef CATALOG_VARLEN
    timestamptz     start_time;
    timestamptz     end_time;
    text            limit_type;
    text            keyword;
    text            users;
#endif
} FormData_gs_sql_limit_rule;

#undef int8

typedef FormData_gs_sql_limit_rule *Form_gs_sql_limit_rule;

#define Natts_gs_sql_limit_rule                         13

#define Anum_gs_sql_limit_rule_limit_id                 1
#define Anum_gs_sql_limit_rule_limit_name               2
#define Anum_gs_sql_limit_rule_enable                   3
#define Anum_gs_sql_limit_rule_work_node                4
#define Anum_gs_sql_limit_rule_max_concurrency          5
#define Anum_gs_sql_limit_rule_hash                     6
#define Anum_gs_sql_limit_rule_unique_sql_id            7
#define Anum_gs_sql_limit_rule_rule_version             8
#define Anum_gs_sql_limit_rule_start_time               9
#define Anum_gs_sql_limit_rule_end_time                 10
#define Anum_gs_sql_limit_rule_limit_type               11
#define Anum_gs_sql_limit_rule_keyword                  12
#define Anum_gs_sql_limit_rule_users                    13

#endif   /* GS_SQL_LIMIT_RULE_H */
