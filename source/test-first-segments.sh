#!/usr/bin/env bash
set -euo pipefail

first_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-first-segments.XXXXXX")

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
9 alpha beta,beta gamma,alpha beta
8 delta epsilon,beta gamma,zeta eta
7 theta iota,kappa lambda,mu nu
this trailing row is not parsed after the limit
EOF

exclude1=$test_dir/exclude1.pairs
cat > "$exclude1" <<'EOF'
gamma,beta
EOF

exclude2=$test_dir/exclude2.pairs
cat > "$exclude2" <<'EOF'
eta,zeta
iota,theta
EOF

expected='alpha,beta
delta,epsilon
kappa,lambda'
diagnostics=$test_dir/diagnostics.txt
actual=$("$first_segments" -n 3 -x "$exclude1" --exclude "$exclude2" \
  "$input" 2> "$diagnostics")
[[ $actual == "$expected" ]] || fail "first segments are wrong: $actual"

expected_diagnostics='found segment 3 on line 3'
actual=$(< "$diagnostics")
[[ $actual == "$expected_diagnostics" ]] ||
  fail "found-segment diagnostics are wrong: $actual"

expected='alpha,beta
beta,gamma'
actual=$("$first_segments" -n 2 "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "duplicate handling is wrong: $actual"

actual=$("$first_segments" -n 0 "$input" 2>/dev/null)
[[ -z $actual ]] || fail "-n 0 produced output: $actual"

expected='alpha,beta
delta,epsilon
kappa,lambda
mu,nu'
actual=$(head -n 3 "$input" |
  "$first_segments" -n 10 -x "$exclude1" --exclude "$exclude2" - \
  2>/dev/null)
[[ $actual == "$expected" ]] || fail "stdin or short result is wrong: $actual"

if "$first_segments" "$input" >/dev/null 2>&1; then
  fail "missing -n succeeded"
fi

if "$first_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

echo PASS
