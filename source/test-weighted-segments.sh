#!/usr/bin/env bash
set -euo pipefail

weighted_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-weighted-segments.XXXXXX")

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

report=$test_dir/segments.txt
cat > "$report" <<'EOF'
3.000e-09  1 solo
1.000e-09  6 stay with
2.000e-09  2 zulu
2.000e-09  2 iowa
EOF

# Weighting reorders solo below the entries it outscored, and rows that weigh
# the same to the bit break on text.
expected='6.000e-09 6 stay with
4.000e-09 2 iowa
4.000e-09 2 zulu
3.000e-09 1 solo'

actual=$("$weighted_segments" "$report")
[[ $actual == "$expected" ]] ||
  fail "file weighting is wrong: $actual"

actual=$("$weighted_segments" < "$report")
[[ $actual == "$expected" ]] ||
  fail "stdin weighting is wrong: $actual"

# Repeating a file merges: counts add, scores take the maximum.
actual=$("$weighted_segments" "$report" "$report" | head -1)
[[ $actual == '1.200e-08 12 stay with' ]] ||
  fail "merging two reports is wrong: $actual"

set +e
"$weighted_segments" <<< 'not a segment row' > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 1 ]] || fail "expected exit 1 on a malformed row, got $status"

echo PASS
