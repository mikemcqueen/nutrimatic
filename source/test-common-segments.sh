#!/usr/bin/env bash
set -euo pipefail

common_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-common-segments.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pairs=$test_dir/best.pairs
cat > "$pairs" <<'EOF'
hobbit,home
tree,home
red,fox
EOF

# Row 1 holds two pairs, so it contributes its other segments and neither
# pair. Row 2 holds three, counting "home tree" for the reversed "tree,home",
# so it contributes everything. Row 3 holds one pair and contributes nothing.
# Row 4's "tree homes" is not a complete segment match, leaving it with one
# pair, so "nearmiss seg" stays out.
input=$test_dir/results.txt
cat > "$input" <<'EOF'
9 hobbit home,red fox,extra one,solo
8 hobbit home,red fox,home tree
7 hobbit home,lonely one
6 tree homes,red fox,nearmiss seg
EOF

expected='extra,one
hobbit,home
home,tree
red,fox
solo'

actual=$("$common_segments" "$input" "$pairs" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "common segments are wrong: $actual"

actual=$("$common_segments" - "$pairs" < "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "stdin output is wrong: $actual"

# --results prints the holding rows themselves, as read and in input order.
expected_results='9 hobbit home,red fox,extra one,solo
8 hobbit home,red fox,home tree'

actual=$("$common_segments" --results "$input" "$pairs" 2>/dev/null)
[[ $actual == "$expected_results" ]] || fail "result rows are wrong: $actual"

# One pair can never make two, so every row is dropped and the empty result
# is reported rather than failed.
one_pair=$test_dir/one.pairs
cat > "$one_pair" <<'EOF'
red,fox
EOF
empty_diagnostics=$test_dir/empty-diagnostics.txt
actual=$("$common_segments" "$input" "$one_pair" 2>"$empty_diagnostics")
[[ -z $actual ]] || fail "a single pair produced output: $actual"
grep -q 'no result row holds two or more pairs' "$empty_diagnostics" ||
  fail "empty result was not reported: $(cat "$empty_diagnostics")"

malformed_input=$test_dir/malformed-results.txt
cat > "$malformed_input" <<'EOF'
9 hobbit home,red fox
noscore here
EOF
if "$common_segments" "$malformed_input" "$pairs" >/dev/null 2>&1; then
  fail "a malformed result row succeeded"
fi

malformed_pairs=$test_dir/malformed.pairs
cat > "$malformed_pairs" <<'EOF'
one,two,three
EOF
if "$common_segments" "$input" "$malformed_pairs" >/dev/null 2>&1; then
  fail "a malformed pair line succeeded"
fi

if "$common_segments" "$input" >/dev/null 2>&1; then
  fail "a missing PAIRS argument succeeded"
fi

if "$common_segments" >/dev/null 2>&1; then
  fail "missing positionals succeeded"
fi

if "$common_segments" "$input" "$pairs" "$pairs" >/dev/null 2>&1; then
  fail "a third positional succeeded"
fi

echo PASS
