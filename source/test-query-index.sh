#!/usr/bin/env bash
set -euo pipefail

query_index=$1
make_index=$2

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-query-index.XXXXXX")

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

synthetic_index="$test_dir/test.index"
"$make_index" "$synthetic_index"

# "wx" and "yz" exactly tile each other's remainder of the "wxyz" bag; "xy"
# leaves "wz" behind, which nothing in the synthetic index can complete.
"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  > "$test_dir/completable-off.stdout" 2> "$test_dir/completable-off.stderr"
[[ $(wc -l < "$test_dir/completable-off.stdout") -eq 4 ]] ||
  fail "expected all four wxyz-bag entries without --require-completable"

"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -d 0 -S 2 \
  > "$test_dir/completable-on.stdout" 2> "$test_dir/completable-on.stderr"
[[ $(wc -l < "$test_dir/completable-on.stdout") -eq 3 ]] ||
  fail "--require-completable should drop the one dead-end class"
grep -q ' xy$' "$test_dir/completable-on.stdout" &&
  fail "--require-completable kept the dead-end 'xy' class"
grep -q ' wx$' "$test_dir/completable-on.stdout" ||
  fail "--require-completable dropped the completable 'wx' class"
grep -q ' yz$' "$test_dir/completable-on.stdout" ||
  fail "--require-completable dropped the completable 'yz' class"
grep -Eq 'phase 2 completability: 4 classes checked, .* exact validations$' \
  "$test_dir/completable-on.stderr" ||
  fail "batch completability diagnostics are missing"
grep -Eq 'phase 2 exact memo: [0-9]+ states computed, [0-9]+ hits$' \
  "$test_dir/completable-on.stderr" ||
  fail "exact memo diagnostics are missing"
grep -q 'phase 2 exact validation parallelism: 2 requested, 2 used' \
  "$test_dir/completable-on.stderr" ||
  fail "-S did not enable parallel exact validation"

# Filtering is score-independent. In particular, an extreme negative bonus
# must not make the phrase-only completion of "f" look unreachable.
"$query_index" "$synthetic_index" fghij -m 1 -n 10 \
  --words-only --require-completable --word-bonus -100 \
  > "$test_dir/underflow-bonus.stdout" \
  2> "$test_dir/underflow-bonus.stderr"
grep -q ' f$' "$test_dir/underflow-bonus.stdout" ||
  fail "--word-bonus underflow changed exact completability"

"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -D \
  > "$test_dir/completable-dense.stdout" \
  2> "$test_dir/completable-dense.stderr"
cmp "$test_dir/completable-on.stdout" "$test_dir/completable-dense.stdout" ||
  fail "projected and dense completability filtering disagree"

"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -d 0 -C 0 -F \
  > "$test_dir/completable-fallback.stdout" \
  2> "$test_dir/completable-fallback.stderr"
cmp "$test_dir/completable-on.stdout" \
    "$test_dir/completable-fallback.stdout" ||
  fail "cache fallback changed exact completability filtering"

"$query_index" "$synthetic_index" wxyz -m 2 -n 1 \
  --require-completable --word-bonus 1 \
  > "$test_dir/bonus.stdout" 2> "$test_dir/bonus.stderr"
grep -q ' wx yz$' "$test_dir/bonus.stdout" ||
  fail "--word-bonus did not rank a low-frequency phrase above the -n floor"

# Phrases remain available as completion classes under --words-only, but are
# filtered from the displayed members.
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable \
  > "$test_dir/words-completed-by-phrase.stdout" \
  2> "$test_dir/words-completed-by-phrase.stderr"
[[ $(wc -l < "$test_dir/words-completed-by-phrase.stdout") -eq 1 ]] ||
  fail "--words-only should print only the word completed by a phrase"
grep -q ' uv$' "$test_dir/words-completed-by-phrase.stdout" ||
  fail "a phrase was not retained as a completion path under --words-only"

printf 'qr\nst\nuv\n' > "$test_dir/dictionary-all"
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable --dict "$test_dir/dictionary-all" \
  > "$test_dir/dictionary-all.stdout" \
  2> "$test_dir/dictionary-all.stderr"
grep -q ' uv$' "$test_dir/dictionary-all.stdout" ||
  fail "dictionary filtering dropped an allowed candidate or completion"

printf 'qr\nuv\n' > "$test_dir/dictionary-no-st"
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable --dict "$test_dir/dictionary-no-st" \
  > "$test_dir/dictionary-no-st.stdout" \
  2> "$test_dir/dictionary-no-st.stderr"
[[ ! -s "$test_dir/dictionary-no-st.stdout" ]] ||
  fail "dictionary filtering did not remove a disallowed completion path"

if [[ -z ${IDX:-} ]]; then
  echo "SKIP: export IDX to run the rest of the query-index CLI test" >&2
  exit 77
fi

"$query_index" "$IDX" penbuilt -n 5 \
  > "$test_dir/top5.stdout" 2> "$test_dir/top5.stderr"

[[ $(wc -l < "$test_dir/top5.stdout") -eq 5 ]] ||
  fail "expected 5 result lines"

if grep -Ev '^[0-9]+ [a-z]+( [a-z]+)*$' "$test_dir/top5.stdout"; then
  fail "unexpected output line format"
fi

awk '{ print $1 }' "$test_dir/top5.stdout" > "$test_dir/counts"
sort -rn -C "$test_dir/counts" ||
  fail "results are not sorted by descending count"

"$query_index" "$IDX" penbuilt -n 2 \
  > "$test_dir/top2.stdout" 2> "$test_dir/top2.stderr"
head -n 2 "$test_dir/top5.stdout" > "$test_dir/expected-top2.stdout"
cmp "$test_dir/expected-top2.stdout" "$test_dir/top2.stdout" ||
  fail "--top did not retain the two highest-frequency entries"

"$query_index" "$IDX" penbuilt -n 5 -w \
  > "$test_dir/words-only.stdout" 2> "$test_dir/words-only.stderr"
if awk 'NF > 2 { exit 1 }' "$test_dir/words-only.stdout"; then :; else
  fail "--words-only emitted a multi-word phrase"
fi

set +e
"$query_index" "$IDX" 'ab!' > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad letters should exit 2, got $status"

set +e
"$query_index" "$IDX" penbuilt -m nope > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad -m should exit 2, got $status"
