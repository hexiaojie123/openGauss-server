#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
export GAUSSHOME="${GAUSSHOME:-$ROOT_DIR/mppdb_temp_install}"
export PATH="$GAUSSHOME/bin:$PATH"
export LD_LIBRARY_PATH="$GAUSSHOME/lib:${LD_LIBRARY_PATH:-}"

DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-postgres}"
ADMIN_USER="${ADMIN_USER:-shangshanfei}"
TEST_PASS="${TEST_PASS:-Gauss@123}"
USQL_BG_PID=

GSQL_BASE=(gsql -d "$DB_NAME" -p "$DB_PORT" -q -t -A -X)
TMP_DIR=$(mktemp -d)

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT

asql() {
    "${GSQL_BASE[@]}" -U "$ADMIN_USER" -c "$1"
}

usql() {
    local user=$1
    local pass=$2
    local sql=$3
    "${GSQL_BASE[@]}" -U "$user" -W "$pass" -c "$sql"
}

usql_bg() {
    local user=$1
    local pass=$2
    local name=$3
    local sql_file="$TMP_DIR/$name.sql"
    cat >"$sql_file"
    (
        "${GSQL_BASE[@]}" -U "$user" -W "$pass" -f "$sql_file"
    ) >"$TMP_DIR/${name}_bg.log" 2>&1 &
    USQL_BG_PID=$!
}

setup() {
    asql "ALTER SYSTEM SET enable_sql_limit = 'on';"
    asql "SELECT pg_reload_conf();"
    asql "DO \$\$ DECLARE r record; BEGIN FOR r IN SELECT limit_id FROM pg_catalog.gs_sql_limit_rule LOOP PERFORM gs_delete_sql_limit_v2(r.limit_id); END LOOP; END; \$\$;"
    asql "DROP TABLE IF EXISTS slv2_tbl;"
    asql "DROP USER IF EXISTS slv2_user_a;"
    asql "DROP USER IF EXISTS slv2_user_b;"
    asql "CREATE TABLE slv2_tbl(i int, v text);"
    asql "INSERT INTO slv2_tbl VALUES (1, 'a'), (2, 'b'), (3, 'c');"
    asql "CREATE USER slv2_user_a WITH PASSWORD '$TEST_PASS';"
    asql "CREATE USER slv2_user_b WITH PASSWORD '$TEST_PASS';"
    asql "GRANT SELECT, INSERT, UPDATE, DELETE ON slv2_tbl TO slv2_user_a, slv2_user_b;"
}

teardown() {
    asql "DO \$\$ DECLARE r record; BEGIN FOR r IN SELECT limit_id FROM pg_catalog.gs_sql_limit_rule LOOP PERFORM gs_delete_sql_limit_v2(r.limit_id); END LOOP; END; \$\$;"
    asql "DROP TABLE IF EXISTS slv2_tbl;"
    asql "DROP USER IF EXISTS slv2_user_a;"
    asql "DROP USER IF EXISTS slv2_user_b;"
    asql "ALTER SYSTEM SET enable_sql_limit = 'off';"
    asql "SELECT pg_reload_conf();"
}

require_line() {
    local file=$1
    local needle=$2
    if ! grep -qx "$needle" "$file"; then
        echo "missing expected line: $needle" >&2
        echo "--- $file ---" >&2
        cat "$file" >&2
        exit 1
    fi
}

require_contains() {
    local text=$1
    local needle=$2
    if [[ "$text" != *"$needle"* ]]; then
        echo "missing expected text: $needle" >&2
        echo "--- text ---" >&2
        printf '%s\n' "$text" >&2
        exit 1
    fi
}

scenario_11() {
    echo "1.1 max_concurrency=2 competition"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_11', 'select', 0, 2, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,pg_sleep}', 'slv2_user_a'); END; \$\$;"

    local bg1_log="$TMP_DIR/11a_bg.log"
    local bg2_log="$TMP_DIR/11b_bg.log"
    local bg1_pid
    local bg2_pid
    usql_bg slv2_user_a "$TEST_PASS" 11a <<'SQL'
SELECT pg_sleep(3);
SQL
    bg1_pid=$USQL_BG_PID
    usql_bg slv2_user_a "$TEST_PASS" 11b <<'SQL'
SELECT pg_sleep(3);
SQL
    bg2_pid=$USQL_BG_PID

    sleep 1

    local rejected
    set +e
    rejected=$(usql slv2_user_a "$TEST_PASS" "SELECT pg_sleep(0);" 2>&1)
    set -e
    require_contains "$rejected" "over max concurrency of sql limit"

    wait "$bg1_pid"
    wait "$bg2_pid"
    require_line "$bg1_log" ""
    require_line "$bg2_log" ""

    local fresh_result
    fresh_result=$(usql slv2_user_a "$TEST_PASS" "SELECT pg_sleep(0);")
    require_line <(printf '%s\n' "$fresh_result") ""

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_11'));")
    require_contains "$stats" "rule_11|4|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_11')); END; \$\$;"
}

scenario_12() {
    echo "1.2 keyword match and miss"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_12', 'select', 0, 0, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,count,from,slv2_tbl}', 'slv2_user_a'); END; \$\$;"

    local rejected
    set +e
    rejected=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl;" 2>&1)
    set -e
    require_contains "$rejected" "over max concurrency of sql limit"

    local missed
    missed=$(usql slv2_user_a "$TEST_PASS" "SELECT 1;")
    require_line <(printf '%s\n' "$missed") "1"

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_12'));")
    require_contains "$stats" "rule_12|1|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_12')); END; \$\$;"
}

scenario_13() {
    echo "1.3 empty keyword matches all select only"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_13', 'select', 0, 0, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{}', 'slv2_user_a'); END; \$\$;"

    local rejected
    set +e
    rejected=$(asql "SET role slv2_user_a password '$TEST_PASS'; SELECT 1; RESET role;" 2>&1)
    set -e
    require_contains "$rejected" "over max concurrency of sql limit"

    asql "SET role slv2_user_a password '$TEST_PASS'; INSERT INTO slv2_tbl VALUES (101, 'empty_keyword_insert'); RESET role;"
    local inserted
    inserted=$(asql "SELECT count(*) FROM slv2_tbl WHERE i = 101;")
    require_line <(printf '%s\n' "$inserted") "1"

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_13'));")
    require_contains "$stats" "rule_13|1|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_13')); END; \$\$;"
}

scenario_14() {
    echo "1.4 insert rule concurrency"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_14', 'insert', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{insert,into,slv2_tbl,values}', 'slv2_user_a'); END; \$\$;"

    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 14 <<'SQL'
INSERT INTO slv2_tbl VALUES (201, CASE WHEN pg_sleep(3) IS NULL THEN 'slow' ELSE 'slow' END);
SQL
    bg_pid=$USQL_BG_PID

    sleep 1

    local rejected
    set +e
    rejected=$(usql slv2_user_a "$TEST_PASS" "INSERT INTO slv2_tbl VALUES (202, 'rejected');" 2>&1)
    set -e
    require_contains "$rejected" "over max concurrency of sql limit"

    local selected
    selected=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i = 2;")
    require_line <(printf '%s\n' "$selected") "1"

    wait "$bg_pid"
    local inserted
    inserted=$(asql "SELECT count(*) FROM slv2_tbl WHERE i = 201;")
    require_line <(printf '%s\n' "$inserted") "1"

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_14'));")
    require_contains "$stats" "rule_14|2|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_14')); END; \$\$;"
}

scenario_15() {
    echo "1.5 update rule concurrency"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_15', 'update', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{update,slv2_tbl,set,where}', 'slv2_user_a'); END; \$\$;"

    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 15 <<'SQL'
UPDATE slv2_tbl SET v = pg_sleep(3)::text WHERE i = 2;
SQL
    bg_pid=$USQL_BG_PID

    sleep 1

    local rejected
    set +e
    rejected=$(usql slv2_user_a "$TEST_PASS" "UPDATE slv2_tbl SET v = 'rejected' WHERE i = 3;" 2>&1)
    set -e
    require_contains "$rejected" "over max concurrency of sql limit"

    wait "$bg_pid"

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_15'));")
    require_contains "$stats" "rule_15|2|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_15')); END; \$\$;"
}

scenario_71() {
    echo "7.1 cross-session invalidation"

    local bg_log="$TMP_DIR/71_bg.log"
    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 71 <<'SQL'
DO $$ BEGIN PERFORM pg_sleep(2); END $$;
SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);
SQL
    bg_pid=$USQL_BG_PID

    sleep 1
    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_71', 'select', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,from,slv2_tbl,where}', NULL); END; \$\$;"
    wait "$bg_pid"

    require_line "$bg_log" "2"
    echo "session-b-result=$(tail -n 1 "$bg_log")"
    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_71')); END; \$\$;"
}

scenario_72() {
    echo "7.2 update with active sql"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_72', 'select', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,from,slv2_tbl,where}', NULL); END; \$\$;"

    local bg_log="$TMP_DIR/72_bg.log"
    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 72 <<'SQL'
DO $$ BEGIN PERFORM pg_sleep(2); END $$;
SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);
SQL
    bg_pid=$USQL_BG_PID

    sleep 1
    asql "DO \$\$ BEGIN PERFORM gs_update_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_72'), 'rule_72', 0, 1, timestamp '2000-01-02 00:00:00', NULL::timestamp, '{select,from,slv2_tbl,where,i}', NULL); END; \$\$;"
    wait "$bg_pid"

    require_line "$bg_log" "2"

    local fresh_result
    fresh_result=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);")
    require_line <(printf '%s\n' "$fresh_result") "2"

    local stats
    stats=$(asql "SELECT limit_name, rule_version, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_72'));")
    require_contains "$stats" "rule_72|2|2|0|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_72')); END; \$\$;"
}

scenario_73() {
    echo "7.3 delete with active sql"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_73', 'select', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,from,slv2_tbl,where}', NULL); END; \$\$;"

    local bg_log="$TMP_DIR/73_bg.log"
    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 73 <<'SQL'
DO $$ BEGIN PERFORM pg_sleep(2); END $$;
SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);
SQL
    bg_pid=$USQL_BG_PID

    sleep 1
    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_73')); END; \$\$;"
    wait "$bg_pid"

    require_line "$bg_log" "2"

    local fresh_result
    fresh_result=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);")
    require_line <(printf '%s\n' "$fresh_result") "2"

    local rule_count
    rule_count=$(asql "SELECT count(*) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_73';")
    require_line <(printf '%s\n' "$rule_count") "0"
}

scenario_74() {
    echo "7.4 time window"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_74_expired', 'select', 0, 0, timestamp '2000-01-01 00:00:00', timestamp '2000-01-02 00:00:00', '{select,from,slv2_tbl,where}', NULL); END; \$\$;"
    local expired_result
    expired_result=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);")
    require_line <(printf '%s\n' "$expired_result") "2"
    local expired_stats
    expired_stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_expired'));")
    require_contains "$expired_stats" "rule_74_expired|0|0|0"
    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_expired')); END; \$\$;"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_74_future', 'select', 0, 0, timestamp '2100-01-01 00:00:00', timestamp '2100-01-02 00:00:00', '{select,from,slv2_tbl,where}', NULL); END; \$\$;"
    local future_result
    future_result=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);")
    require_line <(printf '%s\n' "$future_result") "2"
    local future_stats
    future_stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_future'));")
    require_contains "$future_stats" "rule_74_future|0|0|0"
    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_future')); END; \$\$;"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_74_current', 'select', 0, 1, timestamp '2000-01-01 00:00:00', timestamp '2100-01-01 00:00:00', '{select,from,slv2_tbl,where}', NULL); END; \$\$;"
    local current_result
    current_result=$(usql slv2_user_a "$TEST_PASS" "SELECT count(*) FROM slv2_tbl WHERE i IN (2, 3);")
    require_line <(printf '%s\n' "$current_result") "2"
    local current_stats
    current_stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_current'));")
    require_contains "$current_stats" "rule_74_current|1|0|0"
    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_74_current')); END; \$\$;"
}

scenario_75() {
    echo "7.5 user filter"

    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_75', 'select', 0, 1, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,pg_sleep}', 'slv2_user_a'); END; \$\$;"

    local bg_log="$TMP_DIR/75_bg.log"
    local bg_pid
    usql_bg slv2_user_a "$TEST_PASS" 75 <<'SQL'
SELECT pg_sleep(3);
SQL
    bg_pid=$USQL_BG_PID

    sleep 1

    local user_b_result
    user_b_result=$(usql slv2_user_b "$TEST_PASS" "SELECT pg_sleep(0);")
    require_line <(printf '%s\n' "$user_b_result") ""

    local user_a_result
    set +e
    user_a_result=$(usql slv2_user_a "$TEST_PASS" "SELECT pg_sleep(0);" 2>&1)
    set -e
    require_contains "$user_a_result" "over max concurrency of sql limit"

    wait "$bg_pid"
    require_line "$bg_log" ""

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_75'));")
    require_contains "$stats" "rule_75|2|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_75')); END; \$\$;"
}

scenario_76() {
    echo "7.6 new thread first refresh"

    asql "SELECT 1;"
    asql "DO \$\$ BEGIN PERFORM gs_create_sql_limit_v2('rule_76', 'select', 0, 0, timestamp '2000-01-01 00:00:00', NULL::timestamp, '{select,42}', 'slv2_user_a'); END; \$\$;"

    local first_result
    set +e
    first_result=$(usql slv2_user_a "$TEST_PASS" "SELECT 42;" 2>&1)
    set -e
    require_contains "$first_result" "over max concurrency of sql limit"

    local stats
    stats=$(asql "SELECT limit_name, hit_count, reject_count, curr_concurrency FROM gs_select_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_76'));")
    require_contains "$stats" "rule_76|1|1|0"

    asql "DO \$\$ BEGIN PERFORM gs_delete_sql_limit_v2((SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name = 'rule_76')); END; \$\$;"
}

setup
scenario_11
scenario_12
scenario_13
scenario_14
scenario_15
scenario_71
scenario_72
scenario_73
scenario_74
scenario_75
scenario_76
teardown
