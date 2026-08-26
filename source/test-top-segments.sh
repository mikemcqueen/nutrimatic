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

expected_top='3 alpha
2 beta gamma'
actual=$("$top_segments" -n 2 "$input")
[[ $actual == "$expected_top" ]] || fail "-n output is wrong: $actual"

expected_top_pairs='beta,gamma
epsilon,zeta'
actual=$("$top_segments" --pairs -n 2 "$input")
[[ $actual == "$expected_top_pairs" ]] || fail "pair -n output is wrong: $actual"

limit_input=$test_dir/limit.txt
for i in $(seq -w 0 1000); do
  printf '1 segment%s\n' "$i"
done > "$limit_input"
actual=$("$top_segments" "$limit_input" | wc -l)
[[ $actual == 1000 ]] || fail "default -n is not 1000: $actual"

if "$top_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

echo PASS
