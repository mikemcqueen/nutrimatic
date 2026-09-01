#!/usr/bin/env bash
set -euo pipefail

top_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-top-segments.XXXXXX")

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

ignore1=$test_dir/ignore1.pairs
cat > "$ignore1" <<'EOF'
gamma,beta
EOF
ignore2=$test_dir/ignore2.pairs
cat > "$ignore2" <<'EOF'
zeta,epsilon
EOF

expected_ignored='3 alpha
2 delta
1 one two three'
actual=$("$top_segments" -i "$ignore1" --ignore "$ignore2" "$input")
[[ $actual == "$expected_ignored" ]] || fail "ignored counts are wrong: $actual"

expected_rejected='1 alpha
1 delta
1 epsilon zeta
1 one two three'
actual=$("$top_segments" -r "$ignore1" "$input")
[[ $actual == "$expected_rejected" ]] || fail "rejected rows were counted: $actual"

wfroot=$test_dir/wf
mkdir -p "$wfroot/.wf/classified/yes" "$wfroot/.wf/classified/no"
cat > "$wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
gamma,beta
EOF
cat > "$wfroot/.wf/classified/no/no.pairs" <<'EOF'
zeta,epsilon
EOF

expected_wf='2 alpha
2 beta gamma
1 delta'
actual=$(WFROOT=$wfroot "$top_segments" --wf "$input")
[[ $actual == "$expected_wf" ]] || fail "--wf counts are wrong: $actual"

expected_wf_yes='2 alpha
1 delta'
actual=$(WFROOT=$wfroot "$top_segments" --wf --yes "$input")
[[ $actual == "$expected_wf_yes" ]] || fail "--wf --yes counts are wrong: $actual"

actual=$(WFROOT=$wfroot "$top_segments" --wf -y "$input")
[[ $actual == "$expected_wf_yes" ]] || fail "--wf -y counts are wrong: $actual"

limit_input=$test_dir/limit.txt
for i in $(seq -w 0 1000); do
  printf '1 segment%s\n' "$i"
done > "$limit_input"
actual=$("$top_segments" "$limit_input" | wc -l)
[[ $actual == 1000 ]] || fail "default -n is not 1000: $actual"

if "$top_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

if "$top_segments" --yes "$input" >/dev/null 2>&1; then
  fail "--yes without --wf succeeded"
fi

if "$top_segments" -x "$ignore1" "$input" >/dev/null 2>&1; then
  fail "removed -x option succeeded"
fi

echo PASS
