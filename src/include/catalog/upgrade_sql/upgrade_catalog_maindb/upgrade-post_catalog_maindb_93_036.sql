SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, true, false, 9041, 9042, 0, 0;

CREATE TABLE IF NOT EXISTS pg_catalog.gs_sql_limit_rule
(
   limit_id int8 NOCOMPRESS,
   limit_name name NOCOMPRESS,
   enable bool NOCOMPRESS,
   work_node int4 NOCOMPRESS,
   max_concurrency int4 NOCOMPRESS,
   start_time timestamp NOCOMPRESS,
   end_time timestamp NOCOMPRESS,
   limit_type text NOCOMPRESS,
   hash int8 NOCOMPRESS,
   unique_sql_id int8 NOCOMPRESS,
   keyword text NOCOMPRESS,
   rule_version int8 NOCOMPRESS,
   users text NOCOMPRESS
) WITHOUT OIDS TABLESPACE pg_global;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, true, false, 0, 0, 0, 9043;

CREATE UNIQUE INDEX IF NOT EXISTS gs_sql_limit_enable_type_hash_id_index
ON pg_catalog.gs_sql_limit_rule USING btree(enable bool_ops, limit_type text_ops, hash int8_ops, limit_id int8_ops);

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_class c
        JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname = 'pg_catalog'
          AND c.relname = 'gs_sql_limit_rule_id_seq'
          AND c.relkind = 'S'
    ) THEN
        CREATE SEQUENCE pg_catalog.gs_sql_limit_rule_id_seq START WITH 1 INCREMENT BY 1 MINVALUE 1 NO MAXVALUE CACHE 1;
    END IF;
END
$$;

DO $$
DECLARE
    max_limit_id int8;
BEGIN
    SELECT COALESCE(MAX(limit_id), 0) INTO max_limit_id FROM pg_catalog.gs_sql_limit_rule;
    IF max_limit_id > 0 THEN
        PERFORM pg_catalog.setval('pg_catalog.gs_sql_limit_rule_id_seq'::regclass, max_limit_id::numeric, true);
    END IF;
END
$$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, false, true, 0, 0, 0, 0;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 8245;

CREATE OR REPLACE FUNCTION pg_catalog.gs_create_sql_limit_v2
(
    limit_name name,
    limit_type text,
    work_node int4,
    max_concurrency int4,
    start_time timestamp,
    end_time timestamp,
    keyword text,
    users text
)
RETURNS int8 NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_create_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 8246;

CREATE OR REPLACE FUNCTION pg_catalog.gs_update_sql_limit_v2
(
    limit_id int8,
    limit_name name,
    work_node int4,
    max_concurrency int4,
    start_time timestamp,
    end_time timestamp,
    keyword text,
    users text
)
RETURNS boolean NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_update_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 8247;

CREATE OR REPLACE FUNCTION pg_catalog.gs_select_sql_limit_v2
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
)
RETURNS SETOF record NOT FENCED NOT SHIPPABLE ROWS 1 STABLE
LANGUAGE internal AS $function$gs_select_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 8248;

CREATE OR REPLACE FUNCTION pg_catalog.gs_select_sql_limit_all_v2
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
)
RETURNS SETOF record NOT FENCED NOT SHIPPABLE ROWS 1 STABLE
LANGUAGE internal AS $function$gs_select_sql_limit_all_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 8249;

CREATE OR REPLACE FUNCTION pg_catalog.gs_delete_sql_limit_v2
(
    limit_id int8
)
RETURNS boolean NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_delete_sql_limit_v2$function$;
