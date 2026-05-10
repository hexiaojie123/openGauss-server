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
