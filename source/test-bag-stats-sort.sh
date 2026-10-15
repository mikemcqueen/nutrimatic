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

letters=catdoglowanttanbeerat
"$bag_stats" --min-wb-len 3 --ss pb,pb,wb,p $letters "$test_dir/pairs" "$test_dir/dict" \
  > "$test_dir/dump" 2> /dev/null

for options in "-n 5 --wbfrac-min --tp --by-pick" \
               "--score cat,dog --score ant,bee --score-detail --tp --mp --by-pick" \
               "-n 5 --tp --by-pick --best-pairs lead"; do
  expected=$("$bag_stats" --min-wb-len 3 --ss pb,pb,wb,p $options --prior "$test_dir/prior" \
    $letters "$test_dir/pairs" "$test_dir/dict" 2> /dev/null)
  actual=$("$bag_stats_sort" $options --prior "$test_dir/prior" \
    "$test_dir/dump" 2> /dev/null)
  [[ $actual == "$expected" ]] || fail "$options isn't bag-stats's: $actual"
done

printf '%s\n' dog,cat owl,tan ant,bee > "$test_dir/pp"
for options in "-n 50 --tp --mp --by-pick" \
               "--score dog,cat --score bee,rat --tp --by-pick"; do
  expected=$("$bag_stats" --min-wb-len 3 --ss ppb,pb,wb,p --pp "$test_dir/pp" $options \
    --prior "$test_dir/prior" $letters "$test_dir/pairs" "$test_dir/dict" \
    2> /dev/null)
  actual=$("$bag_stats_sort" --ss ppb,pb,wb,p --pp "$test_dir/pp" $options \
    --prior "$test_dir/prior" "$test_dir/dump" 2> /dev/null)
  [[ -n $actual && $actual == "$expected" ]] ||
    fail "--ss ppb,pb $options isn't bag-stats's: $actual"
done

mkdir -p "$WFROOT/.wf/classified/no"
printf '%s\n' act,god bee,rat > "$WFROOT/.wf/classified/no/no.pairs"
actual=$("$bag_stats_sort" --tp "$test_dir/dump" 2> "$test_dir/err" |
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
