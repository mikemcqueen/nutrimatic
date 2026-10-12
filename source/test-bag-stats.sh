#!/usr/bin/env bash
set -euo pipefail

bag_stats=$1
pairs=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bag-stats.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

export WFROOT=$test_dir/root
mkdir -p "$WFROOT/.wf/dict"
cat > "$WFROOT/.wf/dict/words.filtered" <<'EOF'
ant
tan
nat
owl
low
cat
act
dog
god
EOF

cat > "$test_dir/pairs" <<'EOF'
cat,dog
dog,cat
act,god
low,ant
zzz,yyy
EOF

cat > "$test_dir/dict" <<'EOF'
low
owl
ant
antlowtan
x
EOF

actual=$("$bag_stats" catdoglowanttan "$test_dir/pairs" "$test_dir/dict")
expected="bag-stats 3
letters aaacdglnnootttw
min 3
min-wb 5
ss pb,wb,p
pp
pairs $test_dir/pairs
dict $test_dir/dict
columns PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,MIN_WB,MIN_WB_WITH_PAIRS

cat dog,2,3,2,9,6,1,1,0
low ant,1,1,1,4,4,0,0,0"
[[ $actual == "$expected" ]] || fail "rows are wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed -n '/^min-wb /p; /^cat dog,/p')
[[ $actual == $'min-wb 3\ncat dog,2,3,2,9,6,1,3,2' ]] ||
  fail "--min-wb-len 3 rows are wrong: $actual"

sum=$(( $("$pairs" -m3 -x -c aanntt) + $("$pairs" -m3 -x -c lowant) ))
[[ $sum == 9 ]] || fail "pairs -m3 -x -c sum is $sum, not 9"

actual=$("$bag_stats" -u cat catdoglowanttan "$test_dir/pairs" - \
  < "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'low ant,1,1,0,0,0,0,0,0' ]] || fail "-u rows are wrong: $actual"

actual=$("$bag_stats" --ss pb,pb,wb,p catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'cat dog + low ant,2,1,0,0,0,1,0,0' ]] ||
  fail "pb,pb rows are wrong: $actual"

actual=$("$bag_stats" --ss wb,pb,wb,p --threads 2 catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict" | sed -n '/^ss /p; /^cat dog,/p')
[[ $actual == $'ss pb,wb,wb,p\ncat dog,2,2,0,0,0,0,0,0' ]] ||
  fail "wb,wb rows are wrong: $actual"

echo low,ant > "$test_dir/prospects"
actual=$("$bag_stats" --ss ppb,pb,wb,p --pp "$test_dir/prospects" \
  catdoglowanttan "$test_dir/pairs" "$test_dir/dict" | sed '1,/^$/d')
expected=$("$bag_stats" -u lowant catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d; s/^/low ant + /')
[[ $actual == "$expected" ]] || fail "ppb rows aren't the -u rows: $actual"

actual=$("$bag_stats" -n 1 --tp --by-length catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'cat dog,2,3,2,9,6,1,1,0' ]] || fail "-n rows are wrong: $actual"

actual=$("$bag_stats" --score ant,low --tp catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict")
[[ $actual == $'low ant,1,1,1,4,4,0,0,0\nrank 2 of 2, tied 2-2' ]] ||
  fail "--score is wrong: $actual"

echo PASS
