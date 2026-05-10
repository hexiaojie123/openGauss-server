-- ============================================================
-- SQL Limit V2 Comprehensive Regression Tests
-- ============================================================

\c postgres
SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;

DROP TABLE IF EXISTS sql_limit_v2_tbl;
DROP USER IF EXISTS sl_v2_user;
DROP USER IF EXISTS sl_v2_other;

CREATE TABLE sql_limit_v2_tbl (id int, val text);
INSERT INTO sql_limit_v2_tbl SELECT generate_series(1, 100), 'data';

CREATE USER sl_v2_user WITH PASSWORD 'Gauss@123';
CREATE USER sl_v2_other WITH PASSWORD 'Gauss@123';
GRANT SELECT, INSERT, UPDATE, DELETE ON sql_limit_v2_tbl TO sl_v2_user;
GRANT SELECT, INSERT, UPDATE, DELETE ON sql_limit_v2_tbl TO sl_v2_other;

-- ============================================================
-- 1. Catalog verification
-- ============================================================

SELECT count(*) > 0 AS table_has_columns
FROM pg_attribute
WHERE attrelid = 'pg_catalog.gs_sql_limit_rule'::regclass
  AND attnum > 0 AND NOT attisdropped;

SELECT count(*) = 1 AS has_unique_index
FROM pg_index
WHERE indrelid = 'pg_catalog.gs_sql_limit_rule'::regclass
  AND indisunique;

-- ============================================================
-- 2. Basic CRUD operations
-- ============================================================

SELECT gs_create_sql_limit_v2('crud_select', 'select', 0, 2, NULL, NULL, NULL, NULL);

SELECT limit_name, enable, max_concurrency, limit_type, keyword, rule_version,
       users, hash, unique_sql_id
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select';

SELECT * FROM gs_select_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select'));

SELECT gs_update_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select'),
    'crud_select_renamed', 0, 5, NULL, NULL, NULL, NULL);

SELECT limit_name, max_concurrency, rule_version
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select_renamed';

SELECT gs_update_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select_renamed'),
    'crud_select_v3', 0, 3, NULL, NULL, NULL, NULL);
SELECT limit_name, max_concurrency, rule_version
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select_v3';

SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'crud_select_v3'));

SELECT count(*) AS rules_after_delete FROM pg_catalog.gs_sql_limit_rule;

-- ============================================================
-- 3. All rule types (select, insert, update, delete, sqlId)
-- ============================================================

SELECT gs_create_sql_limit_v2('type_select', 'select', 0, 10, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('type_insert', 'insert', 0, 10, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('type_update', 'update', 0, 10, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('type_delete', 'delete', 0, 10, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('type_sqlid', 'sqlId', 0, 10, NULL, NULL, '98765', NULL);

SELECT limit_name, limit_type, max_concurrency
FROM pg_catalog.gs_sql_limit_rule
ORDER BY limit_name;

SELECT limit_name, hash != 0 AS has_hash, unique_sql_id
FROM pg_catalog.gs_sql_limit_rule WHERE limit_type = 'sqlId';

SELECT limit_name, hash, unique_sql_id
FROM pg_catalog.gs_sql_limit_rule WHERE limit_type != 'sqlId'
ORDER BY limit_name;

SET role sl_v2_user password 'Gauss@123';
SELECT count(*) FROM sql_limit_v2_tbl WHERE id < 10;
INSERT INTO sql_limit_v2_tbl VALUES(101, 'new');
UPDATE sql_limit_v2_tbl SET val = 'updated' WHERE id = 101;
DELETE FROM sql_limit_v2_tbl WHERE id = 101;
RESET role;

SELECT r.limit_name, r.limit_type, s.hit_count, s.reject_count, s.curr_concurrency
FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s
ORDER BY r.limit_name;

SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;

-- ============================================================
-- 4. Keyword rules
-- ============================================================

SELECT gs_create_sql_limit_v2('kw_empty', 'select', 0, 10, NULL, NULL, NULL, NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT 1 AS kw_empty_test;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'kw_empty';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_empty'));

SELECT gs_create_sql_limit_v2('kw_single', 'select', 0, 10, NULL, NULL, 'sql_limit_v2_tbl', NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT count(*) FROM sql_limit_v2_tbl WHERE id = 1;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'kw_single';
SET role sl_v2_user password 'Gauss@123';
SELECT 2 AS no_keyword_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'kw_single';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_single'));

SELECT gs_create_sql_limit_v2('kw_multi', 'select', 0, 10, NULL, NULL, 'sql_limit_v2_tbl, pg_class', NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT count(*) FROM sql_limit_v2_tbl WHERE id = 1;
SELECT count(*) FROM pg_class;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'kw_multi';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_multi'));

-- ============================================================
-- 5. SQLID rules
-- ============================================================

SELECT gs_create_sql_limit_v2('sqlid_111', 'sqlId', 0, 5, NULL, NULL, '111', NULL);

SELECT limit_name, hash != 0 AS has_hash, unique_sql_id
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'sqlid_111';

SET role sl_v2_user password 'Gauss@123';
SELECT 3 AS no_sqlid_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'sqlid_111';

SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'sqlid_111'));

-- ============================================================
-- 6. Time window
-- ============================================================

SELECT gs_create_sql_limit_v2('tw_expired', 'select', 0, 1,
    '2020-01-01 00:00:00', '2020-01-02 00:00:00', NULL, NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT 4 AS expired_rule_no_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'tw_expired';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'tw_expired'));

SELECT gs_create_sql_limit_v2('tw_future', 'select', 0, 1,
    '2099-01-01 00:00:00', NULL, NULL, NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT 5 AS future_rule_no_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'tw_future';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'tw_future'));

SELECT gs_create_sql_limit_v2('tw_open_end', 'select', 0, 10,
    NULL, '2099-12-31 23:59:59', NULL, NULL);
SET role sl_v2_user password 'Gauss@123';
SELECT 6 AS open_end_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'tw_open_end';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'tw_open_end'));

-- ============================================================
-- 7. User scoping
-- ============================================================

SELECT gs_create_sql_limit_v2('u_scope_user', 'select', 0, 10,
    NULL, NULL, NULL, 'sl_v2_user');
SET role sl_v2_user password 'Gauss@123';
SELECT 7 AS user_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'u_scope_user';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'u_scope_user'));

SELECT gs_create_sql_limit_v2('u_scope_other', 'select', 0, 10,
    NULL, NULL, NULL, 'sl_v2_other');
SET role sl_v2_user password 'Gauss@123';
SELECT 8 AS other_user_no_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'u_scope_other';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'u_scope_other'));

SELECT gs_create_sql_limit_v2('u_scope_multi', 'select', 0, 10,
    NULL, NULL, NULL, 'sl_v2_user, sl_v2_other');
SET role sl_v2_user password 'Gauss@123';
SELECT 9 AS multi_user_match;
RESET role;
SELECT s.hit_count FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s WHERE r.limit_name = 'u_scope_multi';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'u_scope_multi'));

-- ============================================================
-- 8. Superuser exemption
-- ============================================================

SELECT gs_create_sql_limit_v2('su_test', 'select', 0, 1, NULL, NULL, NULL, NULL);
SELECT 10 AS superuser_bypass;
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'su_test'));

-- ============================================================
-- 9. Transaction rejection
-- ============================================================

BEGIN;
SELECT gs_create_sql_limit_v2('txn_create', 'select', 0, 1, NULL, NULL, NULL, NULL);
ROLLBACK;

SELECT gs_create_sql_limit_v2('txn_rule', 'select', 0, 1, NULL, NULL, NULL, NULL);
BEGIN;
SELECT gs_update_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_rule'),
    'txn_rule_updated', 0, 2, NULL, NULL, NULL, NULL);
ROLLBACK;
SELECT limit_name, max_concurrency FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_rule';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_rule'));

SELECT gs_create_sql_limit_v2('txn_del', 'select', 0, 1, NULL, NULL, NULL, NULL);
BEGIN;
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_del'));
ROLLBACK;
SELECT count(*) AS txn_del_exists FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_del';
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'txn_del'));

-- ============================================================
-- 10. Parameter validation
-- ============================================================

SELECT gs_create_sql_limit_v2('bad_type', 'invalid', 0, 1, NULL, NULL, NULL, NULL);

SELECT gs_create_sql_limit_v2('case_test', 'SELECT', 0, 1, NULL, NULL, NULL, NULL);
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'case_test'));
SELECT gs_create_sql_limit_v2('case_test2', 'Select', 0, 1, NULL, NULL, NULL, NULL);
SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'case_test2'));

SELECT gs_create_sql_limit_v2('null_type', NULL, 0, 1, NULL, NULL, NULL, NULL);

SELECT gs_create_sql_limit_v2('null_node', 'select', NULL, 1, NULL, NULL, NULL, NULL);

SELECT gs_create_sql_limit_v2('null_conc', 'select', 0, NULL, NULL, NULL, NULL, NULL);

SELECT gs_create_sql_limit_v2('sqlid_null_kw', 'sqlId', 0, 1, NULL, NULL, NULL, NULL);

SELECT gs_create_sql_limit_v2('sqlid_zero', 'sqlId', 0, 1, NULL, NULL, '0', NULL);
SELECT gs_create_sql_limit_v2('sqlid_neg', 'sqlId', 0, 1, NULL, NULL, '-1', NULL);

SELECT gs_delete_sql_limit_v2(999999);

SELECT gs_update_sql_limit_v2(999999, 'ghost', 0, 1, NULL, NULL, NULL, NULL);

SELECT * FROM gs_select_sql_limit_v2(999999);

-- ============================================================
-- 11. gs_select_sql_limit_all_v2
-- ============================================================

SELECT gs_create_sql_limit_v2('all_1', 'select', 0, 5, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('all_2', 'insert', 0, 3, NULL, NULL, 'sql_limit_v2_tbl', NULL);
SELECT gs_create_sql_limit_v2('all_3', 'sqlId', 0, 7, NULL, NULL, '55555', NULL);

SELECT limit_name, enable, max_concurrency, limit_type, keyword, rule_version
FROM gs_select_sql_limit_v2()
ORDER BY limit_name;

SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;

-- ============================================================
-- 12. Multiple rules coexistence
-- ============================================================

SELECT gs_create_sql_limit_v2('multi_empty', 'select', 0, 10, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('multi_kw', 'select', 0, 10, NULL, NULL, 'pg_class', NULL);
SELECT gs_create_sql_limit_v2('multi_ins', 'insert', 0, 10, NULL, NULL, NULL, NULL);

SET role sl_v2_user password 'Gauss@123';
SELECT count(*) FROM sql_limit_v2_tbl WHERE id = 1;
SELECT count(*) FROM pg_class;
INSERT INTO sql_limit_v2_tbl VALUES(102, 'test');
DELETE FROM sql_limit_v2_tbl WHERE id = 102;
RESET role;

SELECT r.limit_name, s.hit_count
FROM pg_catalog.gs_sql_limit_rule r,
     LATERAL gs_select_sql_limit_v2(r.limit_id) s
ORDER BY r.limit_name;

SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;

-- ============================================================
-- 13. Update keyword triggers hash recalculation (sqlId type)
-- ============================================================

SELECT gs_create_sql_limit_v2('kw_recalc', 'sqlId', 0, 5, NULL, NULL, '11111', NULL);
SELECT limit_name, hash, unique_sql_id
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_recalc';

SELECT gs_update_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_recalc'),
    NULL, NULL, NULL, NULL, NULL, '22222', NULL);
SELECT limit_name, hash, unique_sql_id, rule_version
FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_recalc';

SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'kw_recalc'));

-- ============================================================
-- 14. Rule limit_id monotonicity
-- ============================================================

SELECT gs_create_sql_limit_v2('mono_1', 'select', 0, 1, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('mono_2', 'select', 0, 1, NULL, NULL, NULL, NULL);
SELECT gs_create_sql_limit_v2('mono_3', 'select', 0, 1, NULL, NULL, NULL, NULL);

SELECT limit_name, limit_id FROM pg_catalog.gs_sql_limit_rule
WHERE limit_name IN ('mono_1','mono_2','mono_3') ORDER BY limit_name;

SELECT gs_delete_sql_limit_v2(
    (SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'mono_2'));
SELECT gs_create_sql_limit_v2('mono_4', 'select', 0, 1, NULL, NULL, NULL, NULL);

SELECT limit_name, limit_id FROM pg_catalog.gs_sql_limit_rule
WHERE limit_name LIKE 'mono_%' ORDER BY limit_id;

SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;

-- ============================================================
-- 15. Final cleanup
-- ============================================================

SELECT count(*) AS final_remaining_rules FROM pg_catalog.gs_sql_limit_rule;
DROP TABLE IF EXISTS sql_limit_v2_tbl;
DROP USER IF EXISTS sl_v2_user;
DROP USER IF EXISTS sl_v2_other;
