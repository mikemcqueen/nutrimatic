#!/usr/bin/env bash
set -euo pipefail

top_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-top-segments.XXXXXX")

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

input=$test_dir/results.txt
cat > "$input" <<'EOF'
9 alpha,beta gamma,alpha
8 beta gamma,delta
7 alpha,delta,epsilon zeta,one two three
EOF

expected='3 alpha
2 beta gamma
2 delta
1 epsilon zeta
1 one two three'

actual=$("$top_segments" "$input")
[[ $actual == "$expected" ]] || fail "file counts are wrong: $actual"

actual=$("$top_segments" < "$input")
[[ $actual == "$expected" ]] || fail "stdin counts are wrong: $actual"

expected_pairs='beta,gamma
epsilon,zeta
one,two,three'

actual=$("$top_segments" --pairs "$input")
[[ $actual == "$expected_pairs" ]] || fail "pair output is wrong: $actual"

echo PASS
