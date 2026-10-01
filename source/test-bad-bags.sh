#!/usr/bin/env bash
set -euo pipefail

bad_bags=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bad-bags.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir -p "$test_dir/.wf/dict"
printf '%s\n' at cat act tac dog god cats > "$test_dir/.wf/dict/words.filtered"

actual=$(WFROOT=$test_dir "$bad_bags" -m 2 actt)
expected='tt
ct
ctt
att
ac
actt'
[[ $actual == "$expected" ]] || fail "output is wrong: $actual"

actual=$("$bad_bags" --dict "$test_dir/.wf/dict/words.filtered" -x 3 actt)
expected='ctt
att'
[[ $actual == "$expected" ]] || fail "--dict -x output is wrong: $actual"

WFROOT=$test_dir "$bad_bags" -m 2 --bitmap actt > "$test_dir/bitmap"
expected='bad-bags-bitmap 1
letters actt
min 2
max 4
bits 12'
[[ $(head -6 "$test_dir/bitmap") == "$expected" ]] ||
  fail "--bitmap header is wrong: $(head -6 "$test_dir/bitmap")"
size=$(( $(head -6 "$test_dir/bitmap" | wc -c) + 8 ))
[[ $(wc -c < "$test_dir/bitmap") -eq $size ]] || fail "--bitmap size is wrong"
