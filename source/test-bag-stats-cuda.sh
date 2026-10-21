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
WORDS

cat > "$test_dir/pairs" <<'PAIRS'
cat,dog
dog,cat
act,god
low,ant
zzz,yyy
PAIRS

cat > "$test_dir/dict" <<'DICT'
low
owl
ant
antlowtan
x
DICT

run_both() {
  local expected actual
  expected=$("$bag_stats" "$@" catdoglowanttan "$test_dir/pairs" \
    "$test_dir/dict")
  actual=$("$bag_stats_cuda" "$@" catdoglowanttan "$test_dir/pairs" \
    "$test_dir/dict")
  [[ $actual == "$expected" ]] ||
    fail "bag-stats-cuda $* differs: $actual"$'\n'"bag-stats: $expected"
}

run_both
run_both --min-pb-len 6
run_both --min-pb-len 6 --min-wb-len 3
run_both --min-pb-len 6 --ss pb,pb,wb,wb,p -n 5 --wb --mp
run_both --min-pb-len 6 --ss pb,pb,wb,wb,p -n 5 --wb --mp --cache-size 0
