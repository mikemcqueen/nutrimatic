#!/usr/bin/env bash
set -euo pipefail

bag_stats_cuda=$1
bag_stats=$2

nvidia-smi -L 2>/dev/null | grep -q '^GPU' || exit 77

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bag-stats-cuda.XXXXXX")

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
cat > "$WFROOT/.wf/dict/words.filtered" <<'WORDS'
ant
tan
nat
owl
low
cat
act
dog
god
abcdefghijklmnopqrstuabcdefghijklmnopqrstu
bcdefghijklmopqrsu
abcdefghijkmnpqrstuvxyzabcdefghijklmnopqrstuvwxyz
befhijklmnpqrsuvwxyz
WORDS

cat > "$test_dir/pairs" <<'PAIRS'
cat,dog
dog,cat
act,god
low,ant
zzz,yyy
PAIRS

cat > "$test_dir/wide-pairs" <<'PAIRS'
vwxyzvwxyz,vwxyz
cat,dog
PAIRS

cat > "$test_dir/dict" <<'DICT'
low
owl
ant
antlowtan
x
DICT

run_both() {
  local letters=$1 pairs=$2 expected actual
  shift 2
  expected=$("$bag_stats" "$@" "$letters" "$pairs" "$test_dir/dict")
  actual=$("$bag_stats_cuda" "$@" "$letters" "$pairs" "$test_dir/dict")
  [[ $actual == "$expected" ]] ||
    fail "bag-stats-cuda $* differs: $actual"$'\n'"bag-stats: $expected"
}

run_both catdoglowanttan "$test_dir/pairs"
run_both catdoglowanttan "$test_dir/pairs" --min-pb-len 6
run_both catdoglowanttan "$test_dir/pairs" --min-pb-len 6 --min-wb-len 3
run_both catdoglowanttan "$test_dir/pairs" --min-pb-len 6 \
  --ss pb,pb,wb,wb,p -n 5 --wb --mp
run_both catdoglowanttan "$test_dir/pairs" --min-pb-len 6 \
  --ss pb,pb,wb,wb,p -n 5 --wb --mp --cache-size 0

expected=$("$bag_stats" --min-pb-len 6 --ss pb,pb,wb,wb,p catdoglowanttan \
  "$test_dir/pairs" "$test_dir/dict")
actual=$("$bag_stats_cuda" --streams 3 --min-pb-len 6 --ss pb,pb,wb,wb,p \
  catdoglowanttan "$test_dir/pairs" "$test_dir/dict")
[[ $actual == "$expected" ]] || fail "--streams 3 differs: $actual"
"$bag_stats_cuda" --streams 0 x "$test_dir/pairs" "$test_dir/dict" \
  >/dev/null 2>&1 && fail "--streams 0 accepted"

alphabet=abcdefghijklmnopqrstuvwxyz
run_both "$alphabet$alphabet$alphabet" "$test_dir/wide-pairs" \
  --min-pb-len 6 --min-wb-len 3

letters=catdoglowanttan
for x in c a t d o g l w n; do
  for y in c a t d o g l w n; do echo "$x,$y"; done
done > "$test_dir/letter-pairs"
for ss in pb pb,pb pb,pb,pb; do
  expected=$("$bag_stats" --leftovers --hll --min-pb-len 2 --ss $ss \
    "$letters" "$test_dir/letter-pairs" "$test_dir/dict")
  actual=$("$bag_stats_cuda" --leftovers --hll --min-pb-len 2 --ss $ss \
    "$letters" "$test_dir/letter-pairs" "$test_dir/dict")
  [[ $actual == "$expected" ]] ||
    fail "--leftovers --ss $ss differs: $actual"$'\n'"bag-stats: $expected"
done

for x in s t u v w x y z; do
  for y in s t u v w x y z; do echo "$x,$y"; done
done > "$test_dir/wide-letter-pairs"
for test in "catdoglwncatdoglwncatdoglwn:letter-pairs" \
  "$alphabet$alphabet$alphabet:wide-letter-pairs"; do
  bag=${test%:*}
  pairs=$test_dir/${test#*:}
  for ss in pb,pb,pb pb,pb,pb,pb; do
    expected=$("$bag_stats" --leftovers --hll --min-pb-len 2 --ss $ss \
      "$bag" "$pairs" "$test_dir/dict")
    actual=$("$bag_stats_cuda" --leftovers --hll --min-pb-len 2 --ss $ss \
      "$bag" "$pairs" "$test_dir/dict")
    [[ $actual == "$expected" ]] ||
      fail "--leftovers $bag --ss $ss differs: $actual"$'\n'"bag-stats: $expected"
  done
done
