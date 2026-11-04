#!/usr/bin/env bash
set -euo pipefail

filter_words=$(realpath "$1")
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-filter-words.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

wf=$test_dir/.wf
mkdir -p "$wf/dict/done" "$wf/classified/yes" "$wf/classified/s2/yes"
printf '%s\n' apple > "$wf/dict/done/yes.words"
printf '%s\n' bread > "$wf/dict/done/no.words"
printf '%s\n' cherry,donut > "$wf/classified/yes/yes.pairs"
printf '%s\n' eagle,fig > "$wf/classified/s2/yes/yes.pairs"
printf '%s\n' grape,hazel > "$wf/classified/s2/yes/yes.pairs.old"
printf '%s\n' Apple bread cherry donut eagle fig "Gra'pe" hazel ox \
  ice-cream abcdefghijklmn > "$test_dir/words"

actual=$(WFROOT=$test_dir "$filter_words" "$test_dir/words" 2>/dev/null) ||
  fail "filter-words failed"
[[ $actual == $'grape\nhazel\nabcdefghijklmn' ]] || fail "default: $actual"

actual=$(WFROOT=$test_dir "$filter_words" -m 2 -x 13 - < "$test_dir/words" \
  2>/dev/null) || fail "filter-words -m 2 -x 13 failed"
[[ $actual == $'grape\nhazel\nox' ]] || fail "-m 2 -x 13: $actual"

rm "$wf/dict/done/no.words"
if WFROOT=$test_dir "$filter_words" "$test_dir/words" >/dev/null 2>&1; then
  fail "missing no.words accepted"
fi

echo PASS
