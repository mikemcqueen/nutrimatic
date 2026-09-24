#!/usr/bin/env bash
set -euo pipefail

bag_subset=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bag-subset.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir -p "$test_dir/.wf/dict" "$test_dir/.wf/classified/no"
printf '%s\n' a ab b c > "$test_dir/.wf/dict/words.filtered"
printf 'a,b\n' > "$test_dir/.wf/classified/no/no.pairs"
printf '%s\n' a b ab bc a,b a,c ab,c > "$test_dir/input"

actual=$(WFROOT=$test_dir "$bag_subset" -l 2 abc "$test_dir/input" 2>/dev/null)
expected='bc 1
c  2
ab 2
ac 2
b  3
a  4'
[[ $actual == "$expected" ]] || fail "output is wrong: $actual"

actual=$(WFROOT=$test_dir "$bag_subset" -r -l 2 abc "$test_dir/input" 2>/dev/null)
expected='ab 0
a  1
ac 1
bc 1
b  2
c  3'
[[ $actual == "$expected" ]] || fail "reverse output is wrong: $actual"
