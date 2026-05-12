DROP FUNCTION IF EXISTS pg_catalog.gs_create_sql_limit_v2
(
    limit_name name,
    limit_type text,
    work_node int4,
    max_concurrency int4,
    start_time timestamp,
    end_time timestamp,
    keyword text,
    users text
) CASCADE;

DROP FUNCTION IF EXISTS pg_catalog.gs_update_sql_limit_v2
(
    limit_id int8,
    limit_name name,
    work_node int4,
    max_concurrency int4,
    start_time timestamp,
    end_time timestamp,
    keyword text,
    users text
) CASCADE;

DROP FUNCTION IF EXISTS pg_catalog.gs_select_sql_limit_v2
(
    IN limit_id int8,
    OUT limit_id int8,
    OUT limit_name name,
    OUT enable boolean,
    OUT work_node int4,
    OUT max_concurrency int4,
    OUT start_time timestamp,
    OUT end_time timestamp,
    OUT limit_type text,
    OUT hash int8,
    OUT unique_sql_id int8,
    OUT keyword text,
    OUT rule_version int8,
    OUT users text,
    OUT hit_count int8,
    OUT reject_count int8,
    OUT curr_concurrency int8
) CASCADE;

DROP FUNCTION IF EXISTS pg_catalog.gs_select_sql_limit_all_v2
(
    OUT limit_id int8,
    OUT limit_name name,
    OUT enable boolean,
    OUT work_node int4,
    OUT max_concurrency int4,
    OUT start_time timestamp,
    OUT end_time timestamp,
    OUT limit_type text,
    OUT hash int8,
    OUT unique_sql_id int8,
    OUT keyword text,
    OUT rule_version int8,
    OUT users text,
    OUT hit_count int8,
    OUT reject_count int8,
    OUT curr_concurrency int8
) CASCADE;

DROP FUNCTION IF EXISTS pg_catalog.gs_delete_sql_limit_v2
(
    limit_id int8
) CASCADE;

DROP INDEX IF EXISTS pg_catalog.gs_sql_limit_enable_type_hash_id_index;
DROP SEQUENCE IF EXISTS pg_catalog.gs_sql_limit_rule_id_seq;
DROP TYPE IF EXISTS pg_catalog.gs_sql_limit_rule;
DROP TABLE IF EXISTS pg_catalog.gs_sql_limit_rule;
