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

actual=$("$bag_stats" catdoglowanttan "$test_dir/pairs" "$test_dir/dict" |
  sed -n '/^min-pb /p; /^$/,$p')
[[ $actual == 'min-pb 9' ]] || fail "default --min-pb-len keeps rows: $actual"

actual=$("$bag_stats" --min-pb-len 6 catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict")
expected="bag-stats 6
letters aaacdglnnootttw
min 3
min-wb 5
min-pb 6
ss pb,wb,p
pp
sentence
pairs $test_dir/pairs
dict $test_dir/dict
columns PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,WB_WITH_MIN_PAIRS

cat dog,2,1,0,0,0,1,0
low ant,1,0,0,0,0,0,0"
[[ $actual == "$expected" ]] || fail "rows are wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed -n '/^min-wb /p; /^cat dog,/p')
[[ $actual == $'min-wb 3\ncat dog,2,3,2,9,6,1,2' ]] ||
  fail "--min-wb-len 3 --min-pb-len 6 rows are wrong: $actual"

sum=$(( $("$pairs" -m3 -x -c aanntt) + $("$pairs" -m3 -x -c lowant) ))
[[ $sum == 9 ]] || fail "pairs -m3 -x -c sum is $sum, not 9"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -u cat catdoglowanttan "$test_dir/pairs" - \
  < "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'low ant,1,1,0,0,0,0,0' ]] || fail "-u rows are wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss pb,pb,wb,p catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'cat dog + low ant,2,1,0,0,0,1,0' ]] ||
  fail "pb,pb rows are wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss wb,pb,wb,p --threads 2 catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict" | sed -n '/^ss /p; /^cat dog,/p')
[[ $actual == $'ss pb,wb,wb,p\ncat dog,2,2,0,0,0,0,0' ]] ||
  fail "wb,wb rows are wrong: $actual"

echo low,ant > "$test_dir/prospects"
actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss ppb,pb,wb,p --pp "$test_dir/prospects" \
  catdoglowanttan "$test_dir/pairs" "$test_dir/dict" | sed '1,/^$/d')
expected=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -u lowant catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d; s/^/low ant + /')
[[ $actual == "$expected" ]] || fail "ppb rows aren't the -u rows: $actual"

actual=$("$bag_stats" --min-wb-len 3 --ss ppb,wb,p --pp "$test_dir/prospects" \
  -n 1 --tp --best-pairs appear catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict")
[[ $actual == 'low,ant' ]] || fail "ppb,wb,p --best-pairs is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -n 1 --tp --by-length catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == 'cat dog,2,3,2,9,6,1,2' ]] || fail "-n rows are wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --score ant,low --tp catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict")
[[ $actual == $'low ant,1,1,1,4,4,0,1\nrank 2 of 2' ]] ||
  fail "--score is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --score cat,dog --tp --completions 2 catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict" | sed 1,2d)
[[ $actual == $'completions 2 of 3 by PC\nPC 6      ant\nPC 3      low/owl' ]] ||
  fail "--completions is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --score cat,dog --score low,ant --ss pb,pb,wb,p \
  --completions 1 catdoglowanttan "$test_dir/pairs" "$test_dir/dict")
[[ $actual == $'cat dog + low ant,2,1,0,0,0,1,0\ncompletions 1 of 1 by PC\nPC 0 SOLO ant' ]] ||
  fail "--score without SORT options is wrong: $actual"

echo '2.5 dog,cat' > "$test_dir/prior"
actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --score cat,dog --score low,ant --wbfrac-min --wbwp-min \
  --by-pick --prior "$test_dir/prior" --ss pb,pb,wb,p catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict")
[[ $actual == $'cat dog + low ant,2,1,0,0,0,1,0\nrank 1 of 1' ]] ||
  fail "--by-pick --score is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --score cat,dog --score low,ant --score-detail \
  --wbfrac-min --by-pick --prior "$test_dir/prior" --ss pb,pb,wb,p \
  catdoglowanttan "$test_dir/pairs" "$test_dir/dict" | sed -n 3,4p)
[[ $actual == $'KEY 0.0910297 SUM 0.248698\nPAIR cat dog PRIOR 2.5 P 0.25000 ROWS 1 TERM 0.0910297 Q 0.50000' ]] ||
  fail "--score-detail is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -n 1 --best-pairs appear --wbfrac-min --by-pick \
  --prior "$test_dir/prior" --ss pb,pb,wb,p catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict")
[[ $actual == $'cat,dog\nlow,ant' ]] ||
  fail "--best-pairs appear is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -n 1 --best-pairs lead --wbfrac-min --by-pick \
  --prior "$test_dir/prior" --ss pb,pb,wb,p catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict")
[[ $actual == 'cat,dog' ]] || fail "--best-pairs lead is wrong: $actual"

cached=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -n 2 --wbfrac-min --by-pick --prior "$test_dir/prior" \
  --ss pb,pb,wb,p catdoglowanttan "$test_dir/pairs" "$test_dir/dict")
uncached=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -n 2 --wbfrac-min --by-pick --prior "$test_dir/prior" \
  --ss pb,pb,wb,p --cache-size 0 catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict")
[[ -n $cached && $cached == "$uncached" ]] ||
  fail "--cache-size 0 rows differ: $uncached"

mkdir -p "$WFROOT/.wf/classified/no" "$WFROOT/.wf/classified/s1/no"
echo dog,cat > "$WFROOT/.wf/classified/no/no.pairs"
echo ant,low > "$WFROOT/.wf/classified/s1/no/no.pairs"
actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 -s 1 catdoglowanttan "$test_dir/pairs" \
  "$test_dir/dict" | sed '1,/^$/d' | cut -d, -f1,2)
[[ $actual == 'act god,1' ]] || fail "-s NO pairs aren't dropped: $actual"

echo act,god >> "$WFROOT/.wf/classified/no/no.pairs"
actual=$("$bag_stats" --min-wb-len 3 --ss wb,pf --no-nopairs --exact --all \
  catdoglow "$test_dir/pairs" "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == $',1,1,1,2,2,0,1\ncompletions 1 of 1 by PC\nPC 2      low/owl' ]] ||
  fail "--ss wb,pf is wrong: $actual"
"$bag_stats" --ss wb,pf --no-nopairs --all catdoglow "$test_dir/pairs" \
  "$test_dir/dict" >/dev/null 2>&1 && fail "--ss wb,pf runs without --exact"
"$bag_stats" --min-pb-len 6 --ss pb,pf --no-nopairs catdoglow "$test_dir/pairs" \
  "$test_dir/dict" >/dev/null 2>&1 && fail "--ss pb,pf runs without --exact"

actual=$("$bag_stats" --min-wb-len 3 --ss pf --no-nopairs --all --threads 2 \
  catdoglowant "$test_dir/pairs" "$test_dir/dict" | sed '1,/^$/d')
[[ $actual == $',1,1,1,3,3,0,1\ncompletions 2 of 2 by PC\nPC 2      cat,dog/act,god\nPC 1      low,ant' ]] ||
  fail "--ss pf is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --ss pf --all catdoglowant \
  "$test_dir/pairs" "$test_dir/dict" 2>/dev/null | tail -n 1)
[[ $actual == 'PC 1      low,ant' ]] || fail "--ss pf keeps no.pairs: $actual"

actual=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss pb,wb,pf --no-nopairs \
  --exact -n 2 --tp catdoglowantlow "$test_dir/pairs" "$test_dir/dict" |
  sed '1,/^$/d')
[[ $actual == $'low ant,1,1,1,2,2,0,1\ncat dog,2,2,1,1,1,0,0' ]] ||
  fail "--ss pb,wb,pf is wrong: $actual"

actual=$("$bag_stats" --min-wb-len 3 --ss wb,p catdoglow "$test_dir/pairs" \
  "$test_dir/dict" 2>/dev/null | tail -n 1)
[[ $actual == ',1,1,1,4,4,0,1' ]] || fail "--ss wb,p is wrong: $actual"

exact=$("$bag_stats" --leftovers --min-pb-len 6 --ss pb,pb catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict")
hll=$("$bag_stats" --leftovers --hll --min-pb-len 6 --ss pb,pb \
  catdoglowanttan "$test_dir/pairs" "$test_dir/dict")
[[ -n $exact && ${hll/LEFTOVERS_HLL/LEFTOVERS} == "$exact" ]] ||
  fail "--leftovers --hll differs: $hll"

echo PASS
