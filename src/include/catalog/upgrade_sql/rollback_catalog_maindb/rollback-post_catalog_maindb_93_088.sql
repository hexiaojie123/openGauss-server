DROP FUNCTION IF EXISTS pg_catalog.gs_delete_sql_limit_v2(int8) CASCADE;
DROP FUNCTION IF EXISTS pg_catalog.gs_select_sql_limit_v2(int8) CASCADE;
DROP FUNCTION IF EXISTS pg_catalog.gs_select_sql_limit_v2() CASCADE;
DROP FUNCTION IF EXISTS pg_catalog.gs_update_sql_limit_v2(int8, name, int4, int4, timestamptz, timestamptz, text, text) CASCADE;
DROP FUNCTION IF EXISTS pg_catalog.gs_create_sql_limit_v2(name, text, int4, int4, timestamptz, timestamptz, text, text) CASCADE;

DROP INDEX IF EXISTS pg_catalog.gs_sql_limit_rule_enable_type_hash_id_index;

DROP TABLE IF EXISTS pg_catalog.gs_sql_limit_rule;
