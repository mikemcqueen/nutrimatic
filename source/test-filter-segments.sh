#!/usr/bin/env bash
set -euo pipefail

filter_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-filter-segments.XXXXXX")

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
9 alpha beta,beta gamma
8 delta epsilon,zeta eta
7 theta iota,kappa lambda
6 mu nu
EOF

reject=$test_dir/reject.pairs
cat > "$reject" <<'EOF'
gamma,beta
iota,theta
EOF

expected='8 delta epsilon,zeta eta
6 mu nu'
actual=$("$filter_segments" -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "file filtering is wrong: $actual"

expected='8 delta epsilon,zeta eta'
actual=$("$filter_segments" -n 1 --reject "$reject" "$input")
[[ $actual == "$expected" ]] || fail "output limit is wrong: $actual"

actual=$("$filter_segments" -n 0 --reject "$reject" "$input")
[[ -z $actual ]] || fail "-n 0 produced output: $actual"

expected='9 alpha beta,beta gamma
8 delta epsilon,zeta eta
7 theta iota,kappa lambda
6 mu nu'
actual=$("$filter_segments" < "$input")
[[ $actual == "$expected" ]] || fail "default stdin is wrong: $actual"

expected='8 delta epsilon,zeta eta
6 mu nu'
actual=$("$filter_segments" -r "$reject" - < "$input")
[[ $actual == "$expected" ]] || fail "explicit stdin is wrong: $actual"

wfroot=$test_dir/wf
mkdir -p "$wfroot/.wf/classified/yes" "$wfroot/.wf/classified/no"
cat > "$wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
epsilon,delta
EOF
cat > "$wfroot/.wf/classified/no/no.pairs" <<'EOF'
nu,mu
EOF

expected='8 delta epsilon,zeta eta'
actual=$(WFROOT=$wfroot "$filter_segments" --wf -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "--wf rejections are wrong: $actual"

actual=$(WFROOT=$test_dir/not-the-root \
  "$filter_segments" --wfroot "$wfroot" -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "--wfroot rejections are wrong: $actual"

if "$filter_segments" --wf --wfroot "$wfroot" "$input" \
    >/dev/null 2>&1; then
  fail "--wf with --wfroot succeeded"
fi

if "$filter_segments" -i "$reject" "$input" >/dev/null 2>&1; then
  fail "unsupported -i option succeeded"
fi

if "$filter_segments" --yes "$input" >/dev/null 2>&1; then
  fail "unsupported --yes option succeeded"
fi

if "$filter_segments" -x "$reject" "$input" >/dev/null 2>&1; then
  fail "removed -x option succeeded"
fi

if "$filter_segments" "$input" "$input" >/dev/null 2>&1; then
  fail "multiple input files succeeded"
fi

if "$filter_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

echo PASS
