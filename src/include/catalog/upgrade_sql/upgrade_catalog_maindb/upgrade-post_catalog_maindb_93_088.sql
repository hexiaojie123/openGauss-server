SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, true, true, 9060, 9061, 0, 0;

CREATE TABLE IF NOT EXISTS pg_catalog.gs_sql_limit_rule
(
   limit_id int8 NOCOMPRESS,
   limit_name name NOCOMPRESS,
   enable bool NOCOMPRESS,
   work_node int4 NOCOMPRESS,
   max_concurrency int4 NOCOMPRESS,
   hash int8 NOCOMPRESS,
   unique_sql_id int8 NOCOMPRESS,
   rule_version int8 NOCOMPRESS,
   start_time timestamptz NOCOMPRESS,
   end_time timestamptz NOCOMPRESS,
   limit_type text NOCOMPRESS,
   keyword text NOCOMPRESS,
   users text NOCOMPRESS
) WITHOUT OIDS TABLESPACE pg_global;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, true, true, 0, 0, 0, 9062;

CREATE UNIQUE INDEX IF NOT EXISTS gs_sql_limit_rule_enable_type_hash_id_index ON pg_catalog.gs_sql_limit_rule USING btree (enable bool_ops, limit_type text_ops, hash int8_ops, limit_id int8_ops);

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_CATALOG, false, true, 0, 0, 0, 0;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 9068;

CREATE OR REPLACE FUNCTION pg_catalog.gs_create_sql_limit_v2
(
    limit_name name,
    limit_type text,
    work_node int4,
    max_concurrency int4,
    start_time timestamptz,
    end_time timestamptz,
    keyword text,
    users text
)
RETURNS int8 NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_create_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 9069;

CREATE OR REPLACE FUNCTION pg_catalog.gs_update_sql_limit_v2
(
    limit_id int8,
    limit_name name,
    work_node int4,
    max_concurrency int4,
    start_time timestamptz,
    end_time timestamptz,
    keyword text,
    users text
)
RETURNS boolean NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_update_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 9070;

CREATE OR REPLACE FUNCTION pg_catalog.gs_select_sql_limit_v2
(
    IN limit_id int8,
    OUT limit_id int8,
    OUT limit_name name,
    OUT enable boolean,
    OUT max_concurrency int4,
    OUT limit_type text,
    OUT keyword text,
    OUT rule_version int8,
    OUT hit_count int8,
    OUT reject_count int8,
    OUT curr_concurrency int8
)
RETURNS SETOF record NOT FENCED NOT SHIPPABLE ROWS 1 STABLE
LANGUAGE internal AS $function$gs_select_sql_limit_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 9071;

CREATE OR REPLACE FUNCTION pg_catalog.gs_select_sql_limit_v2
(
    OUT limit_id int8,
    OUT limit_name name,
    OUT enable boolean,
    OUT max_concurrency int4,
    OUT limit_type text,
    OUT keyword text,
    OUT rule_version int8,
    OUT hit_count int8,
    OUT reject_count int8,
    OUT curr_concurrency int8
)
RETURNS SETOF record NOT FENCED NOT SHIPPABLE ROWS 1 STABLE
LANGUAGE internal AS $function$gs_select_sql_limit_all_v2$function$;

SET LOCAL inplace_upgrade_next_system_object_oids = IUO_PROC, 9072;

CREATE OR REPLACE FUNCTION pg_catalog.gs_delete_sql_limit_v2
(
    limit_id int8
)
RETURNS boolean NOT FENCED NOT SHIPPABLE STABLE
LANGUAGE internal AS $function$gs_delete_sql_limit_v2$function$;
