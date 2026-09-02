#!/usr/bin/env bash
set -euo pipefail

first_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-first-segments.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
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

ignore1=$test_dir/ignore1.pairs
cat > "$ignore1" <<'EOF'
gamma,beta
EOF

ignore2=$test_dir/ignore2.pairs
cat > "$ignore2" <<'EOF'
eta,zeta
iota,theta
EOF

expected='alpha,beta
delta,epsilon
kappa,lambda'
diagnostics=$test_dir/diagnostics.txt
actual=$("$first_segments" -n 3 -i "$ignore1" --ignore "$ignore2" \
  "$input" 2> "$diagnostics")
[[ $actual == "$expected" ]] || fail "first segments are wrong: $actual"

expected_diagnostics='found segment 3 on line 3'
actual=$(< "$diagnostics")
[[ $actual == "$expected_diagnostics" ]] ||
  fail "found-segment diagnostics are wrong: $actual"

expected='theta,iota
kappa,lambda
mu,nu'
actual=$("$first_segments" -n 3 -r "$ignore1" "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "rejected row was not discarded: $actual"

expected='alpha,beta
beta,gamma'
actual=$("$first_segments" -n 2 "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "duplicate handling is wrong: $actual"

actual=$("$first_segments" -n 0 "$input" 2>/dev/null)
[[ -z $actual ]] || fail "-n 0 produced output: $actual"

pairs_input=$test_dir/pair-results.txt
cat > "$pairs_input" <<'EOF'
9 solo,alpha beta
8 another,beta gamma,alpha beta
7 final,kappa lambda
EOF

expected='alpha,beta
beta,gamma'
actual=$("$first_segments" --pairs -n 2 "$pairs_input" 2> "$diagnostics")
[[ $actual == "$expected" ]] || fail "--pairs output is wrong: $actual"
expected_diagnostics='found segment 2 on line 2'
actual=$(< "$diagnostics")
[[ $actual == "$expected_diagnostics" ]] ||
  fail "--pairs diagnostics are wrong: $actual"

expected='solo
another'
actual=$("$first_segments" --solo-words -n 2 "$pairs_input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "--solo-words output is wrong: $actual"

expected='solo
alpha
beta
another'
actual=$("$first_segments" --all-words -n 4 "$pairs_input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "--all-words output is wrong: $actual"

expected='alpha,beta
beta,gamma
another
solo'
actual=$("$first_segments" --by-length -n 4 "$pairs_input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "--by-length output is wrong: $actual"

actual=$("$first_segments" -l -n 4 "$pairs_input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "-l output is wrong: $actual"

if "$first_segments" --solo-words --all-words "$pairs_input" \
    >/dev/null 2>&1; then
  fail "--solo-words with --all-words succeeded"
fi

expected='alpha,beta
delta,epsilon
kappa,lambda
mu,nu'
actual=$(head -n 3 "$input" |
  "$first_segments" -n 10 -i "$ignore1" --ignore "$ignore2" - \
  2>/dev/null)
[[ $actual == "$expected" ]] || fail "explicit stdin is wrong: $actual"

actual=$(head -n 3 "$input" |
  "$first_segments" -i "$ignore1" --ignore "$ignore2" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "implicit stdin is wrong: $actual"

default_input=$test_dir/default-results.txt
for i in $(seq 1 1001); do
  echo "1 pair $i"
done > "$default_input"
actual=$("$first_segments" "$default_input" 2>/dev/null | wc -l)
[[ $actual == 1000 ]] || fail "default limit is wrong: $actual"

wfroot=$test_dir/wf
mkdir -p "$wfroot/.wf/classified/yes" "$wfroot/.wf/classified/no"
cat > "$wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
eta,zeta
EOF
cat > "$wfroot/.wf/classified/no/no.pairs" <<'EOF'
beta,alpha
EOF

expected='delta,epsilon
beta,gamma
zeta,eta'
actual=$(WFROOT=$wfroot "$first_segments" -n 3 --wf "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "--wf rejections are wrong: $actual"

expected='delta,epsilon
beta,gamma
theta,iota'
actual=$(WFROOT=$wfroot "$first_segments" -n 3 --wf --yes "$input" \
  2>/dev/null)
[[ $actual == "$expected" ]] || fail "--wf --yes filtering is wrong: $actual"

actual=$(WFROOT=$wfroot "$first_segments" -n 3 --wf -y "$input" \
  2>/dev/null)
[[ $actual == "$expected" ]] || fail "--wf -y filtering is wrong: $actual"

actual=$(env -u WFROOT "$first_segments" -n 3 --wfroot "$wfroot" -y \
  "$input" 2>/dev/null)
[[ $actual == "$expected" ]] || fail "--wfroot -y filtering is wrong: $actual"

if "$first_segments" --wfroot "$wfroot" --wf "$input" \
    >/dev/null 2>&1; then
  fail "--wfroot with --wf succeeded"
fi

partial_wfroot=$test_dir/partial-wf
mkdir -p "$partial_wfroot/.wf/classified/yes"
cat > "$partial_wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
beta,alpha
EOF
diagnostics=$test_dir/wf-diagnostics.txt
actual=$(WFROOT=$partial_wfroot "$first_segments" -n 2 --wf "$input" \
  2> "$diagnostics")
expected='alpha,beta
beta,gamma'
[[ $actual == "$expected" ]] || fail "--wf loaded YES pairs: $actual"
expected_diagnostics="first-segments: WARNING: classified pair file \"$partial_wfroot/.wf/classified/no/no.pairs\" is not present
found segment 2 on line 1"
actual=$(< "$diagnostics")
[[ $actual == "$expected_diagnostics" ]] ||
  fail "missing workflow file diagnostics are wrong: $actual"

if env -u WFROOT "$first_segments" -n 1 --wf "$input" \
    >/dev/null 2> "$diagnostics"; then
  fail "--wf with unset WFROOT succeeded"
fi
expected_diagnostics="first-segments: --wf requires WFROOT to be set and nonempty"
actual=$(< "$diagnostics")
[[ $actual == "$expected_diagnostics" ]] ||
  fail "unset WFROOT diagnostics are wrong: $actual"

if WFROOT= "$first_segments" -n 1 --wf "$input" >/dev/null 2>&1; then
  fail "--wf with empty WFROOT succeeded"
fi

if "$first_segments" --yes "$input" >/dev/null 2> "$diagnostics"; then
  fail "--yes without --wf succeeded"
fi
if ! grep -q -- '--yes requires --wf or --wfroot' "$diagnostics"; then
  fail "--yes without --wf diagnostic is wrong"
fi

if "$first_segments" -x "$ignore1" "$input" >/dev/null 2>&1; then
  fail "removed -x option succeeded"
fi

if "$first_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

echo PASS
