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
expected="bag-stats 1
letters aaacdglnnootttw
min 3
pairs $test_dir/pairs
dict $test_dir/dict
columns PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO

cat dog,acdgot,2,3,2,9,6,1
low ant,alnotw,1,1,1,4,4,0"
[[ $actual == "$expected" ]] || fail "rows are wrong: $actual"

sum=$(( $("$pairs" -m3 -x -c aanntt) + $("$pairs" -m3 -x -c lowant) ))
[[ $sum == 9 ]] || fail "pairs -m3 -x -c sum is $sum, not 9"

actual=$("$bag_stats" -u cat catdoglowanttan "$test_dir/pairs" - \
  < "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'low ant,alnotw,1,1,0,0,0,0' ]] || fail "-u rows are wrong: $actual"

echo PASS
