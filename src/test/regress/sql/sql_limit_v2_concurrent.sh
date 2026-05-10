#!/bin/bash
# ============================================================
# SQL Limit V2 Concurrent Rate-Limiting Tests
# ============================================================
# Usage: bash sql_limit_v2_concurrent.sh
# Prerequisites:
#   - Database running with enable_sql_limit=on
#   - gsql in PATH
#   - Run as the database superuser (peer/trust auth via local socket)
# ============================================================

set -o pipefail

DBPORT="${DBPORT:-5432}"
DBNAME="${DBNAME:-postgres}"

# Test user - created by this script
TEST_USER="sl_v2_ct"
TEST_PASS="Gauss@123"

TMPDIR="/tmp/sql_limit_v2_ct_$$"
mkdir -p "$TMPDIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

pass() { echo -e "${GREEN}PASS${NC}: $1"; ((PASS++)); }
fail() { echo -e "${RED}FAIL${NC}: $1"; ((FAIL++)); }
skip() { echo -e "${YELLOW}SKIP${NC}: $1"; ((SKIP++)); }
section() { echo ""; echo -e "${YELLOW}=== $1 ===${NC}"; }

# Helper: run SQL as admin via local socket (no -h, peer/trust auth)
asql() { gsql -p "$DBPORT" -d "$DBNAME" -t -A -c "$1" 2>&1; }

# Helper: run SQL as test user via local socket with -W password
tsql() { gsql -p "$DBPORT" -d "$DBNAME" -U "$TEST_USER" -W "$TEST_PASS" -t -A -c "$1" 2>&1; }

# Helper: run SQL as test user, capture full output to file (for background)
tsql_bg() {
    gsql -p "$DBPORT" -d "$DBNAME" -U "$TEST_USER" -W "$TEST_PASS" \
        -c "$1" > "$2" 2>&1
}

# Helper: run gsql as test user with full output, return exit code
tsql_full() {
    gsql -p "$DBPORT" -d "$DBNAME" -U "$TEST_USER" -W "$TEST_PASS" \
        -c "$1" 2>&1
}

# ============================================================
# Setup
# ============================================================
section "Setup"

# Clean up any leftover rules to make the test reentrant
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;" >/dev/null 2>&1

asql "DROP USER IF EXISTS $TEST_USER;" >/dev/null 2>&1
asql "CREATE USER $TEST_USER WITH PASSWORD '$TEST_PASS';" >/dev/null 2>&1
asql "DROP TABLE IF EXISTS sl_v2_ct_tbl;" >/dev/null 2>&1
asql "CREATE TABLE sl_v2_ct_tbl (id int, val text);" >/dev/null 2>&1
asql "INSERT INTO sl_v2_ct_tbl SELECT generate_series(1, 100), 'data';" >/dev/null 2>&1
asql "GRANT SELECT, INSERT, UPDATE, DELETE ON sl_v2_ct_tbl TO $TEST_USER;" >/dev/null 2>&1

# Warm up: let test user establish baseline (gsql version queries consume slots)
tsql "SELECT 0 AS warmup;" >/dev/null 2>&1
sleep 1

# ============================================================
# Test 1: max_concurrency=1 — one passes, second rejected
# ============================================================
section "Test 1: max_concurrency=1 — reject second concurrent query"

asql "SELECT gs_create_sql_limit_v2('t1_rule', 'select', 0, 1, NULL, NULL, NULL, NULL);" >/dev/null
LID=$(asql "SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t1_rule';" | tr -d ' \n')

# Session 1: hold slot with pg_sleep
tsql_bg "SELECT pg_sleep(4) AS s1;" "$TMPDIR/t1_s1.out" &
S1=$!
sleep 2  # ensure session 1 has acquired the slot

# Session 2: should be rejected
OUT=$(tsql_full "SELECT 1 AS s2;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "Session 2 correctly rejected (max_concurrency=1)"
else
    fail "Session 2 should be rejected, got: $OUT"
fi

wait $S1
sleep 1

# Session 3: after release, should succeed
OUT=$(tsql_full "SELECT 2 AS s3;")
if echo "$OUT" | grep -q "2"; then
    pass "Session 3 succeeded after release"
else
    fail "Session 3 should succeed, got: $OUT"
fi

# Check stats: curr_concurrency should be 0
STATS=$(asql "SELECT hit_count || ',' || reject_count || ',' || curr_concurrency FROM gs_select_sql_limit_v2($LID);" | tr -d ' \n')
CURR=$(echo "$STATS" | cut -d',' -f3)
if [ "$CURR" = "0" ]; then
    pass "curr_concurrency is 0 after all queries done"
else
    fail "curr_concurrency should be 0, got: $CURR (stats: $STATS)"
fi

asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t1_rule';" >/dev/null
sleep 1

# ============================================================
# Test 2: max_concurrency=2 — two pass, third rejected
# ============================================================
section "Test 2: max_concurrency=2 — two pass, third rejected"

asql "SELECT gs_create_sql_limit_v2('t2_rule', 'select', 0, 2, NULL, NULL, NULL, NULL);" >/dev/null

tsql_bg "SELECT pg_sleep(4) AS c2s1;" "$TMPDIR/t2_s1.out" &
S1=$!
sleep 1
tsql_bg "SELECT pg_sleep(4) AS c2s2;" "$TMPDIR/t2_s2.out" &
S2=$!
sleep 2

# Third should be rejected
OUT=$(tsql_full "SELECT 1 AS c2s3;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "Third session correctly rejected (max_concurrency=2)"
else
    fail "Third session should be rejected, got: $OUT"
fi

wait $S1; wait $S2
sleep 1

# After both done, should succeed
OUT=$(tsql_full "SELECT 2 AS c2s4;")
if echo "$OUT" | grep -q "2"; then
    pass "Session after release succeeded"
else
    fail "Session after release should succeed, got: $OUT"
fi

asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t2_rule';" >/dev/null
sleep 1

# ============================================================
# Test 3: Keyword matching
# ============================================================
section "Test 3: Keyword matching — only matching queries limited"

asql "SELECT gs_create_sql_limit_v2('t3_rule', 'select', 0, 1, NULL, NULL, 'sl_v2_ct_tbl', NULL);" >/dev/null

# Hold slot with matching query
tsql_bg "SELECT pg_sleep(3) FROM sl_v2_ct_tbl LIMIT 1;" "$TMPDIR/t3_s1.out" &
S1=$!
sleep 2

# Non-matching query should NOT be limited
OUT=$(tsql_full "SELECT 1 AS no_match;")
if echo "$OUT" | grep -q "1"; then
    pass "Non-matching keyword query bypassed limit"
else
    fail "Non-matching query should succeed, got: $OUT"
fi

# Another matching query should be rejected
OUT=$(tsql_full "SELECT count(*) FROM sl_v2_ct_tbl;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "Matching keyword query correctly rejected"
else
    fail "Matching keyword query should be rejected, got: $OUT"
fi

wait $S1
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t3_rule';" >/dev/null
sleep 1

# ============================================================
# Test 4: Empty keyword matches all SELECT
# ============================================================
section "Test 4: Empty keyword matches all SELECT queries"

asql "SELECT gs_create_sql_limit_v2('t4_rule', 'select', 0, 1, NULL, NULL, NULL, NULL);" >/dev/null

tsql_bg "SELECT pg_sleep(3);" "$TMPDIR/t4_s1.out" &
S1=$!
sleep 2

OUT=$(tsql_full "SELECT 99 AS any_select;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "Empty keyword rule rejects all SELECT queries"
else
    fail "Empty keyword should reject any SELECT, got: $OUT"
fi

# INSERT should not be affected by SELECT rule
# Note: wait for session 1 to finish first, to avoid gsql version-query
# being caught by the SELECT rule
wait $S1
sleep 1
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t4_rule';" >/dev/null
sleep 1

OUT=$(tsql_full "INSERT INTO sl_v2_ct_tbl VALUES(999, 'test');")
if ! echo "$OUT" | grep -q "over max concurrency"; then
    pass "INSERT not affected by SELECT rule"
else
    fail "INSERT should not be affected, got: $OUT"
fi

asql "DELETE FROM sl_v2_ct_tbl WHERE id=999;" >/dev/null 2>&1

# ============================================================
# Test 5: INSERT rule
# ============================================================
section "Test 5: INSERT rule rate limiting"

asql "SELECT gs_create_sql_limit_v2('t5_rule', 'insert', 0, 1, NULL, NULL, NULL, NULL);" >/dev/null

# Use pg_sleep in CASE expression to make INSERT slow (pg_sleep returns void,
# so we use it inside CASE WHEN ... IS NULL which works with any return type)
tsql_bg "INSERT INTO sl_v2_ct_tbl VALUES(1, CASE WHEN pg_sleep(5) IS NULL THEN 'a' ELSE 'b' END);" "$TMPDIR/t5_s1.out" &
S1=$!
sleep 3

OUT=$(tsql_full "INSERT INTO sl_v2_ct_tbl VALUES(888, 'test');")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "INSERT rule correctly rejects concurrent INSERT"
else
    fail "Concurrent INSERT should be rejected, got: $OUT"
fi

# SELECT should not be affected
OUT=$(tsql_full "SELECT 1 AS sel_ok;")
if echo "$OUT" | grep -q "1"; then
    pass "SELECT not affected by INSERT rule"
else
    fail "SELECT should not be affected, got: $OUT"
fi

wait $S1
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t5_rule';" >/dev/null
sleep 1

# ============================================================
# Test 6: User-scoped rule
# ============================================================
section "Test 6: User-scoped rule"

asql "SELECT gs_create_sql_limit_v2('t6_rule', 'select', 0, 1, NULL, NULL, NULL, '$TEST_USER');" >/dev/null

tsql_bg "SELECT pg_sleep(3) AS u_hold;" "$TMPDIR/t6_s1.out" &
S1=$!
sleep 2

OUT=$(tsql_full "SELECT 1 AS u_reject;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "User-scoped rule correctly limits target user"
else
    fail "Target user should be limited, got: $OUT"
fi

wait $S1
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t6_rule';" >/dev/null
sleep 1

# ============================================================
# Test 7: Rule update — version isolation
# ============================================================
section "Test 7: Rule update — version isolation"

asql "SELECT gs_create_sql_limit_v2('t7_rule', 'select', 0, 2, NULL, NULL, NULL, NULL);" >/dev/null
RID=$(asql "SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t7_rule';" | tr -d ' \n')

# Session 1 holds slot
tsql_bg "SELECT pg_sleep(4) AS ver_s1;" "$TMPDIR/t7_s1.out" &
S1=$!
sleep 2

# Update rule: max_concurrency=1, version becomes 2
asql "SELECT gs_update_sql_limit_v2($RID, NULL, NULL, 1, NULL, NULL, NULL, NULL);" >/dev/null

# New version stats start fresh, first query should succeed (curr=0 < max=1)
OUT=$(tsql_full "SELECT 1 AS ver_s2;")
if echo "$OUT" | grep -q "1"; then
    pass "After update, new version allows first query (fresh counter)"
else
    fail "After update, new version query got: $OUT"
fi

wait $S1

VER=$(asql "SELECT rule_version FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t7_rule';" | tr -d ' \n')
if [ "$VER" = "2" ]; then
    pass "Rule version correctly incremented to 2"
else
    fail "Rule version should be 2, got: $VER"
fi

asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t7_rule';" >/dev/null
sleep 1

# ============================================================
# Test 8: Rule deletion — active queries complete gracefully
# ============================================================
section "Test 8: Rule deletion — active queries complete gracefully"

asql "SELECT gs_create_sql_limit_v2('t8_rule', 'select', 0, 1, NULL, NULL, NULL, NULL);" >/dev/null
RID=$(asql "SELECT limit_id FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t8_rule';" | tr -d ' \n')

tsql_bg "SELECT pg_sleep(4) AS del_s1;" "$TMPDIR/t8_s1.out" &
S1=$!
sleep 2

# Delete rule while session 1 is active
asql "SELECT gs_delete_sql_limit_v2($RID);" >/dev/null

wait $S1

# Session 1 should have completed without error
if grep -q "del_s1" "$TMPDIR/t8_s1.out"; then
    pass "Active query completed gracefully after rule deletion"
else
    fail "Active query should complete, got: $(cat $TMPDIR/t8_s1.out)"
fi

# After deletion, queries should not be limited
sleep 1
OUT=$(tsql_full "SELECT 1 AS after_del;")
if echo "$OUT" | grep -q "1"; then
    pass "Query succeeds after rule deletion"
else
    fail "Query should succeed after deletion, got: $OUT"
fi

# ============================================================
# Test 9: Fast path — no rules → skip → add → limit → delete → skip
# ============================================================
section "Test 9: Fast path verification"

# Ensure no rules
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;" >/dev/null 2>&1
sleep 2

OUT=$(tsql_full "SELECT 1 AS no_rules;")
if echo "$OUT" | grep -q "1"; then
    pass "No rules: query succeeds"
else
    fail "No rules: query should succeed, got: $OUT"
fi

# Add rule with max_concurrency=0 (block all)
asql "SELECT gs_create_sql_limit_v2('fp_block', 'select', 0, 0, NULL, NULL, NULL, NULL);" >/dev/null
sleep 2

OUT=$(tsql_full "SELECT 2 AS fp_blocked;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "Fast path: rule added, query rejected (max_concurrency=0)"
else
    fail "Fast path: query should be rejected, got: $OUT"
fi

# Delete rule
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='fp_block';" >/dev/null
sleep 2

OUT=$(tsql_full "SELECT 3 AS fp_restored;")
if echo "$OUT" | grep -q "3"; then
    pass "Fast path: rule deleted, query succeeds again"
else
    fail "Fast path: query should succeed after delete, got: $OUT"
fi

# ============================================================
# Test 10: UPDATE rule
# ============================================================
section "Test 10: UPDATE rule rate limiting"

asql "SELECT gs_create_sql_limit_v2('t10_rule', 'update', 0, 1, NULL, NULL, NULL, NULL);" >/dev/null

tsql_bg "UPDATE sl_v2_ct_tbl SET val = pg_sleep(3)::text WHERE id = 1;" "$TMPDIR/t10_s1.out" &
S1=$!
sleep 2

OUT=$(tsql_full "UPDATE sl_v2_ct_tbl SET val = 'x' WHERE id = 2;")
if echo "$OUT" | grep -q "over max concurrency"; then
    pass "UPDATE rule correctly rejects concurrent UPDATE"
else
    fail "Concurrent UPDATE should be rejected, got: $OUT"
fi

wait $S1
asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule WHERE limit_name='t10_rule';" >/dev/null
sleep 1

# ============================================================
# Cleanup
# ============================================================
section "Cleanup"

asql "SELECT gs_delete_sql_limit_v2(limit_id) FROM pg_catalog.gs_sql_limit_rule;" >/dev/null 2>&1
asql "DROP TABLE IF EXISTS sl_v2_ct_tbl;" >/dev/null 2>&1
asql "DROP USER IF EXISTS $TEST_USER;" >/dev/null 2>&1
rm -rf "$TMPDIR"

# ============================================================
# Summary
# ============================================================
section "Summary"
TOTAL=$((PASS + FAIL + SKIP))
echo "Total: $TOTAL  Passed: $PASS  Failed: $FAIL  Skipped: $SKIP"

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}$FAIL TESTS FAILED${NC}"
    exit 1
fi
