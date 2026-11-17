#!/usr/bin/env bash
set -euo pipefail

bag_stats_sort=$1
bag_stats=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bag-stats-sort.XXXXXX")

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
printf '%s\n' ant tan nat owl low cat act dog god bee rat art tar \
  > "$WFROOT/.wf/dict/words.filtered"
printf '%s\n' cat,dog act,god low,ant owl,tan bee,rat tar,dog ant,bee \
  > "$test_dir/pairs"
printf '%s\n' low owl ant tan cat bee rat x dog > "$test_dir/dict"
printf '%s\n' '2.5 dog,cat' '1.0 bee,rat' '0.5 low,ant' > "$test_dir/prior"

# bag-stats' sort flags in `$1` as bag-stats-sort --sort keys.
sort_options() {
  local option out=()
  for option in $1; do
    case $option in
      --wbfrac-min) out+=(--sort wbwp-min-per-wb) ;;
      --wb|--wbwp|--tp|--mp|--wbwp-min) out+=(--sort "${option#--}") ;;
      *) out+=("$option") ;;
    esac
  done
  echo "${out[*]}"
}

letters=catdoglowanttanbeerat
"$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss pb,pb,wb,p $letters "$test_dir/pairs" "$test_dir/dict" \
  > "$test_dir/dump" 2> /dev/null

for options in "-n 5 --wbfrac-min --tp --by-pick" \
               "--score cat,dog --score ant,bee --score-detail --tp --mp --by-pick" \
               "-n 5 --tp --by-pick --best-pairs lead"; do
  expected=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss pb,pb,wb,p $options --prior "$test_dir/prior" \
    $letters "$test_dir/pairs" "$test_dir/dict" 2> /dev/null | sed 's/^  --/  /')
  actual=$("$bag_stats_sort" $(sort_options "$options") --prior "$test_dir/prior" \
    "$test_dir/dump" 2> /dev/null)
  [[ $actual == "$expected" ]] || fail "$options isn't bag-stats's: $actual"
done

printf '%s\n' dog,cat owl,tan ant,bee > "$test_dir/pp"
for options in "-n 50 --tp --mp --by-pick" \
               "--score dog,cat --score bee,rat --tp --by-pick"; do
  expected=$("$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss ppb,pb,wb,p --pp "$test_dir/pp" $options \
    --prior "$test_dir/prior" $letters "$test_dir/pairs" "$test_dir/dict" \
    2> /dev/null)
  actual=$("$bag_stats_sort" --ss ppb,pb,wb,p --pp "$test_dir/pp" $(sort_options "$options") \
    --prior "$test_dir/prior" "$test_dir/dump" 2> /dev/null)
  [[ -n $actual && $actual == "$expected" ]] ||
    fail "--ss ppb,pb $options isn't bag-stats's: $actual"
done

"$bag_stats" --min-wb-len 3 --min-pb-len 6 --ss pb,pb,wb,p $letters - "$test_dir/dict" \
  < "$test_dir/pairs" > "$test_dir/stdin-dump" 2> /dev/null
expected=$("$bag_stats_sort" -n 5 --sort tp "$test_dir/dump" 2> /dev/null)
actual=$("$bag_stats_sort" -n 5 --sort tp --pairs "$test_dir/pairs" \
  "$test_dir/stdin-dump" 2> /dev/null)
[[ -n $actual && $actual == "$expected" ]] ||
  fail "--pairs doesn't replace the dump's standard input: $actual"

best_rows() {
  sed '1,/^$/d' | awk -F, '{
    n = split($1, picks, / \+ /)
    for (i = 1; i <= n; ++i) if (picks[i] in seen) next
    for (i = 1; i <= n; ++i) seen[picks[i]] = 1
    print }'
}
expected=$("$bag_stats_sort" --sort tp "$test_dir/dump" 2> /dev/null | best_rows |
  head -n 3)
actual=$("$bag_stats_sort" --sort tp --best-rows -n 3 "$test_dir/dump" 2> /dev/null |
  sed '1,/^$/d')
[[ -n $expected && $actual == "$expected" ]] ||
  fail "--best-rows -n 3 repeats a pick: $actual"

actual=$("$bag_stats_sort" --sort tp -x dog,tar -x low,ant "$test_dir/dump" \
  2> /dev/null | sed '1,/^$/d' | cut -d, -f1)
expected='cat dog + ant bee
cat dog + bee rat'
[[ $actual == "$expected" ]] || fail "-x doesn't drop its rows: $actual"

expected=$(sed '1,/^$/d' "$test_dir/dump" |
  awk -F, '{ print ($3 ? $5 / $3 : 0), $0 }' | sort -s -k1,1gr | head -n 1 |
  cut -d' ' -f2-)
actual=$("$bag_stats_sort" -n 1 --sort mp,tp-per-wb --sort wb "$test_dir/dump" \
  2> /dev/null | sed '1,/^$/d')
[[ -n $actual ]] || fail "--sort mp,tp-per-wb --sort wb prints no row"
actual=$("$bag_stats_sort" -n 1 --sort tp-per-wb "$test_dir/dump" 2> /dev/null |
  sed '1,/^$/d')
[[ -n $expected && $actual == "$expected" ]] ||
  fail "--sort tp-per-wb doesn't rank TOTAL_PAIRS / WORDBAGS first: $actual"
"$bag_stats_sort" --sort tp-wb "$test_dir/dump" > /dev/null 2>&1 &&
  fail "--sort takes an unknown key"

printf '%s\n' cat,dog low,ant bee,rat tan,glow > "$test_dir/pairs2"
{
  sed -n '1,/^$/p' "$test_dir/dump" | sed "s|^pairs .*|pairs $test_dir/pairs2|"
  printf '%s\n' 'cat dog + low ant,1,1,1,10,1,0,1' \
    'cat dog + bee rat,1,1,1,20,1,0,1' 'low ant + bee rat,1,1,1,20,1,0,1' \
    'cat dog + tan glow,1,1,1,5,1,0,1' 'bee rat + tan glow,1,1,1,3,1,0,1'
} > "$test_dir/dump2"
actual=$("$bag_stats_sort" --sort tp --pair-score 1 --no-prior \
  "$test_dir/dump2" 2> /dev/null)
expected='SCORE PRIOR POINTS ROWS PICK BEST
2.585 0.000 2.585 3 bee,rat cat dog + bee rat
2.000 0.000 2.000 3 cat,dog cat dog + tan glow
1.585 0.000 1.585 2 low,ant low ant + bee rat
0.263 0.000 0.263 2 tan,glow cat dog + tan glow'
[[ $actual == "$expected" ]] || fail "--pair-score: $actual"
for options in --by-length --by-pick --best-rows --top-segments \
               "--score cat,dog --score low,ant" "--best-pairs appear -n 2"; do
  "$bag_stats_sort" --sort tp --pair-score 1 $options "$test_dir/dump2" \
    > /dev/null 2>&1 && fail "--pair-score takes $options"
done
actual=$("$bag_stats_sort" --sort tp --pair-score 1 --no-prior \
  --goal bee,rat --goal low,ant "$test_dir/dump2" 2> /dev/null | grep 'goal row')
expected='low,ant: the goal row is 1 of 2 rows, 0 above it and 0 tied with it
bee,rat: the goal row is 2 of 3 rows, 1 above it and 0 tied with it'
[[ $actual == "$expected" ]] || fail "--goal: $actual"
"$bag_stats_sort" --sort tp --pair-score 1 --goal bee,rat "$test_dir/dump2" \
  > /dev/null 2>&1 && fail "--goal takes one pick for two pb steps"
"$bag_stats_sort" --sort tp --pair-score -1 "$test_dir/dump2" \
  > /dev/null 2>&1 && fail "--pair-score takes a negative LAMBDA"

mkdir -p "$WFROOT/.wf/classified/no"
printf '%s\n' act,god bee,rat > "$WFROOT/.wf/classified/no/no.pairs"
actual=$("$bag_stats_sort" --sort tp "$test_dir/dump" 2> "$test_dir/err" |
  sed '1,/^$/d')
expected='cat dog + ant bee,1,3,3,21,9,0,3
low ant + ant bee,2,3,3,16,6,0,3
tar dog + ant bee,1,3,3,16,6,0,3
cat dog + low ant,2,3,3,15,9,0,3
low ant + tar dog,2,3,3,11,6,0,3'
[[ $actual == "$expected" ]] || fail "NO pairs aren't refreshed: $actual"
grep -qx 'bag-stats-sort: dropped 2 of 7 rows whose picks have no pairs left' \
  "$test_dir/err" || fail "the dropped rows aren't reported: $(cat "$test_dir/err")"

echo PASS
