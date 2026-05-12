\c postgres
\pset format unaligned
\pset tuples_only on
\pset pager off
\set ON_ERROR_STOP on
SET client_min_messages = error;

ALTER SYSTEM SET enable_sql_limit = 'on';
SELECT pg_reload_conf();

DROP TABLE IF EXISTS slv2_tbl;
DROP USER IF EXISTS slv2_user_a;
DROP USER IF EXISTS slv2_user_b;
DO $$
DECLARE
    r record;
BEGIN
    FOR r IN SELECT limit_id FROM pg_catalog.gs_sql_limit_rule LOOP
        PERFORM gs_delete_sql_limit_v2(r.limit_id);
    END LOOP;
END;
$$;

CREATE TABLE slv2_tbl(i int, v text);
INSERT INTO slv2_tbl VALUES (1, 'a'), (2, 'b'), (3, 'c');
CREATE USER slv2_user_a WITH PASSWORD 'Gauss@123';
CREATE USER slv2_user_b WITH PASSWORD 'Gauss@123';
GRANT SELECT, INSERT, UPDATE, DELETE ON slv2_tbl TO slv2_user_a, slv2_user_b;

SELECT 'catalog';
SELECT count(*) FROM pg_class WHERE relname = 'gs_sql_limit_rule';
SELECT count(*) FROM pg_indexes WHERE tablename = 'gs_sql_limit_rule';
SELECT count(*) FROM pg_proc WHERE proname IN (
    'gs_create_sql_limit_v2',
    'gs_update_sql_limit_v2',
    'gs_delete_sql_limit_v2',
    'gs_select_sql_limit_v2',
    'gs_select_sql_limit_all_v2'
);

SELECT 'create';
DO $$
BEGIN
    PERFORM gs_create_sql_limit_v2('rule_select', 'select', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp,
        '{select,count,from,slv2_tbl,where}', NULL);
    PERFORM gs_create_sql_limit_v2('rule_insert', 'insert', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp,
        '{insert,into,slv2_tbl,values}', NULL);
    PERFORM gs_create_sql_limit_v2('rule_update', 'update', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp,
        '{update,slv2_tbl,set,where}', NULL);
    PERFORM gs_create_sql_limit_v2('rule_delete', 'delete', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp,
        '{delete,from,slv2_tbl,where}', NULL);
    PERFORM gs_create_sql_limit_v2('rule_sqlid', 'sqlId', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp,
        '123456789', NULL);
END;
$$;
SELECT hash <> unique_sql_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_sqlid';

SELECT 'run';
SET role slv2_user_a password 'Gauss@123';
SELECT count(*) FROM slv2_tbl WHERE i > 1;
INSERT INTO slv2_tbl VALUES (4, 'd');
UPDATE slv2_tbl SET v = 'x' WHERE i = 1;
DELETE FROM slv2_tbl WHERE i = 1;
RESET role;

SELECT 'stats';
SELECT count(*), sum(hit_count), sum(reject_count), sum(curr_concurrency) FROM gs_select_sql_limit_all_v2();
SELECT limit_name, enable, work_node, max_concurrency, limit_type, keyword, rule_version, hit_count, reject_count, curr_concurrency
FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_select'));

SELECT 'update';
DO $$
BEGIN
    PERFORM gs_update_sql_limit_v2(
        (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_select'),
        'rule_select',
        0,
        1,
        timestamp '2000-01-02 00:00:00',
        NULL::timestamp,
        '{select,count,from,slv2_tbl,where,i}',
        NULL
    );
END;
$$;

SET role slv2_user_a password 'Gauss@123';
SELECT count(*) FROM slv2_tbl WHERE i > 1;
RESET role;

SELECT limit_name, enable, work_node, max_concurrency, limit_type, keyword, rule_version, hit_count, reject_count, curr_concurrency
FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_select'));

SELECT 'delete';
DO $$
BEGIN
    PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_select'));
    PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_insert'));
    PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_update'));
    PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_delete'));
    PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_sqlid'));
END;
$$;
SELECT count(*) FROM pg_catalog.gs_sql_limit_rule;

SELECT 'validation';
CREATE TEMP TABLE slv2_checks(name text, ok bool);
DO $$
DECLARE
    i int;
    r record;
BEGIN
    BEGIN
        PERFORM gs_create_sql_limit_v2('bad_sqlid', 'sqlId', 0, 1, timestamp '2000-01-01 00:00:00',
            NULL::timestamp, 'abc', NULL);
        INSERT INTO slv2_checks VALUES ('invalid_sqlid', false);
    EXCEPTION WHEN others THEN
        INSERT INTO slv2_checks VALUES ('invalid_sqlid', SQLERRM LIKE '%positive integer%');
    END;

    BEGIN
        PERFORM gs_create_sql_limit_v2('bad_concurrency', 'select', 0, -1, timestamp '2000-01-01 00:00:00',
            NULL::timestamp, '{select}', NULL);
        INSERT INTO slv2_checks VALUES ('negative_concurrency', false);
    EXCEPTION WHEN others THEN
        INSERT INTO slv2_checks VALUES ('negative_concurrency', SQLERRM LIKE '%negative%');
    END;

    BEGIN
        PERFORM gs_update_sql_limit_v2(9223372036854775807, 'missing_rule', 0, 1,
            timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select}', NULL);
        INSERT INTO slv2_checks VALUES ('update_missing_rule', false);
    EXCEPTION WHEN others THEN
        INSERT INTO slv2_checks VALUES ('update_missing_rule', SQLERRM LIKE '%not exist%');
    END;

    BEGIN
        PERFORM gs_delete_sql_limit_v2(9223372036854775807);
        INSERT INTO slv2_checks VALUES ('delete_missing_rule', false);
    EXCEPTION WHEN others THEN
        INSERT INTO slv2_checks VALUES ('delete_missing_rule', SQLERRM LIKE '%not exist%');
    END;

    FOR i IN 1..1000 LOOP
        PERFORM gs_create_sql_limit_v2(('bulk_' || i)::name, 'select', 0, 1,
            timestamp '2000-01-01 00:00:00', NULL::timestamp, ('{bulk_' || i || '}')::text, NULL);
    END LOOP;

    BEGIN
        PERFORM gs_create_sql_limit_v2('bulk_overflow', 'select', 0, 1, timestamp '2000-01-01 00:00:00',
            NULL::timestamp, '{bulk_overflow}', NULL);
        INSERT INTO slv2_checks VALUES ('rule_count_limit', false);
    EXCEPTION WHEN others THEN
        INSERT INTO slv2_checks
        SELECT 'rule_count_limit', SQLERRM LIKE '%over the limit%' AND count(*) = 1000
        FROM pg_catalog.gs_sql_limit_rule;
    END;

    FOR r IN SELECT limit_id FROM pg_catalog.gs_sql_limit_rule LOOP
        PERFORM gs_delete_sql_limit_v2(r.limit_id);
    END LOOP;
END;
$$;
SELECT name, ok FROM slv2_checks ORDER BY name;
SELECT count(*) FROM pg_catalog.gs_sql_limit_rule;

DROP TABLE slv2_tbl;
DROP USER slv2_user_a;
DROP USER slv2_user_b;
ALTER SYSTEM SET enable_sql_limit = 'off';
SELECT pg_reload_conf();
