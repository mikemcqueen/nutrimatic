#!/usr/bin/env bash
set -euo pipefail

dfs_anagrams=$1
make_index=$2
query_index=$3
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-dfs-cli.XXXXXX")
index_file=$test_dir/test.index
diagnostic_prefix='^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] '

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

expect_status() {
  expected=$1
  shift
  set +e
  "$@" > "$test_dir/status.stdout" 2> "$test_dir/status.stderr"
  actual=$?
  set -e
  [[ $actual -eq $expected ]] ||
    fail "expected exit $expected, got $actual from: $*"
}

assert_close() {
  local actual=$1
  local expected=$2
  local description=$3
  awk -v actual="$actual" -v expected="$expected" '
    BEGIN {
      difference = actual - expected
      if (difference < 0) difference = -difference
      scale = expected < 0 ? -expected : expected
      if (scale == 0) scale = 1
      exit difference <= scale * 0.0006 ? 0 : 1
    }
  ' || fail "$description: expected $expected, got $actual"
}

"$make_index" "$index_file"

expect_status 2 "$dfs_anagrams" "$index_file" abcd
grep -q '^usage: .* \[-i INDEX\] letters' "$test_dir/status.stderr" ||
  fail "positional index rejection did not show the new synopsis"
expect_status 2 "$dfs_anagrams" abcd

"$dfs_anagrams" --idx "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  > "$test_dir/all.stdout" 2> "$test_dir/all.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 -P 1000000 --word-bonus 0 \
  > "$test_dir/explicit-default.stdout" \
  2> "$test_dir/explicit-default.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  --cache-size 0 \
  --allow-cache-fallback \
  > "$test_dir/uncached.stdout" 2> "$test_dir/uncached.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  --preprocess-threads 1 \
  > "$test_dir/thread-one.stdout" 2> "$test_dir/thread-one.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  --preprocess-threads 4 \
  > "$test_dir/threaded.stdout" 2> "$test_dir/threaded.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  -d 0 \
  > "$test_dir/depth-zero.stdout" 2> "$test_dir/depth-zero.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  -S 0 \
  > "$test_dir/search-thread-auto.stdout" \
  2> "$test_dir/search-thread-auto.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  -S 1 \
  > "$test_dir/search-thread-one.stdout" \
  2> "$test_dir/search-thread-one.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  -p 2147483648 \
  > "$test_dir/wide-progress-factor.stdout" \
  2> "$test_dir/wide-progress-factor.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 0 --word-bonus 0 \
  > "$test_dir/unlimited.stdout" 2> "$test_dir/unlimited.stderr"
cmp "$test_dir/all.stdout" "$test_dir/explicit-default.stdout" ||
  fail "explicit default segment penalty changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/uncached.stdout" ||
  fail "score cache changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/thread-one.stdout" ||
  fail "--preprocess-threads 1 changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/threaded.stdout" ||
  fail "threaded preprocessing changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/depth-zero.stdout" ||
  fail "--projection-depth changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/search-thread-auto.stdout" ||
  fail "-S 0 changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/search-thread-one.stdout" ||
  fail "-S 1 changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/wide-progress-factor.stdout" ||
  fail "64-bit --progress-factor changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/unlimited.stdout" ||
  fail "-n 0 did not return all results"
grep -Eq \
  "${diagnostic_prefix}phase 2: using up to [2-4] threads to calculate projected score bounds bottom-up$" \
  "$test_dir/threaded.stderr" ||
  fail "threaded preprocessing diagnostic is missing"
[[ $(wc -l < "$test_dir/all.stdout") -eq 4 ]] ||
  fail "full synthetic search did not print four word sets"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/all.stdout") -eq 1 ]] ||
  fail "the contiguous phrase did not win and deduplicate its split form"
if grep -Eq "$diagnostic_prefix" "$test_dir/all.stdout"; then
  fail "progress leaked onto stdout"
fi
grep -Eq "${diagnostic_prefix}"'4 letters "abcd", words of 2\+, at most 2 words$' \
  "$test_dir/all.stderr" ||
  fail "search header is missing from stderr"
default_search_threads=$(sed -n -E \
  's/.*search threads ([1-9][0-9]*) cache 64 segment penalty 1000000$/\1/p' \
  "$test_dir/all.stderr")
auto_search_threads=$(sed -n -E \
  's/.*search threads ([1-9][0-9]*) cache 64 segment penalty 1000000$/\1/p' \
  "$test_dir/search-thread-auto.stderr")
[[ -n $default_search_threads && "$default_search_threads" == "$auto_search_threads" ]] ||
  fail "default and -S 0 did not resolve to the same positive thread count"
grep -Eq "${diagnostic_prefix}phase 1 complete:" "$test_dir/all.stderr" ||
  fail "phase-1 statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: score-bound mode projected dense \\(4-byte values, capacity [0-9]+, complete effective coverage\\)$" \
  "$test_dir/all.stderr" ||
  fail "default projected cache mode diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: projected score table keeps 4 rarest letters exact, merges 0 wildcard letters;" \
  "$test_dir/all.stderr" ||
  fail "automatic projected depth diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: projected score table keeps 0 rarest letters exact, merges 4 wildcard letters;" \
  "$test_dir/depth-zero.stderr" ||
  fail "--projection-depth diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: score-bound mode off$" \
  "$test_dir/unlimited.stderr" ||
  fail "-n 0 unexpectedly enabled score-bound pruning"
grep -Eq "${diagnostic_prefix}phase 2: precomputed [0-9]+ bounded states in [0-9.]+s$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 precompute timing is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 complete:" "$test_dir/all.stderr" ||
  fail "phase-2 statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 timing: [0-9.]+s setup, [0-9.]+s search, [0-9]+ successful bound transitions, [0-9]+ nextafter calls$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 timing or transition statistics are missing from stderr"
if grep -Eq "${diagnostic_prefix}phase 2 length certificate: .* prepare," \
    "$test_dir/all.stderr"; then
  fail "length-certificate preparation timing was printed"
fi
grep -Eq "${diagnostic_prefix}phase 2 length certificate: active, [0-9]+ table bytes$" \
  "$test_dir/all.stderr" ||
  fail "length-certificate summary is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2   [0-9]+ group tests, [0-9]+ rejected, [0-9]+ class scans kept, [0-9]+ skipped$" \
  "$test_dir/all.stderr" ||
  fail "length-certificate counters are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 score cache: [0-9]+ bound entries, [0-9]+ bound bytes$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 cache statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 complete: .* [0-9]+ retained$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 completion statistics have the wrong format"

"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 2 --word-bonus 0 \
  > "$test_dir/top.stdout" 2> "$test_dir/top.stderr"
head -n 2 "$test_dir/all.stdout" > "$test_dir/expected-top.stdout"
cmp "$test_dir/expected-top.stdout" "$test_dir/top.stdout" ||
  fail "--top did not retain the two highest-scoring word sets"
[[ $(wc -l < "$test_dir/unlimited.stdout") -gt \
   $(wc -l < "$test_dir/top.stdout") ]] ||
  fail "-n 0 did not return more results than -n 2"

"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 -P 1 --word-bonus 0 \
  > "$test_dir/penalty-one.stdout" \
  2> "$test_dir/penalty-one.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 \
  --segment-penalty 1 --cache-size 0 --allow-cache-fallback \
  > "$test_dir/penalty-one-uncached.stdout" \
  2> "$test_dir/penalty-one-uncached.stderr"
cmp "$test_dir/penalty-one.stdout" \
    "$test_dir/penalty-one-uncached.stdout" ||
  fail "cache mode changed stdout at a non-default segment penalty"
default_split_score=$(awk '$2 == "ab,dc" { print $1 }' \
  "$test_dir/all.stdout")
penalty_one_split_score=$(awk '$2 == "ab,dc" { print $1 }' \
  "$test_dir/penalty-one.stdout")
[[ -n $default_split_score && -n $penalty_one_split_score ]] ||
  fail "known two-segment spelling was not retained"
assert_close "$penalty_one_split_score" \
  "$(awk -v score="$default_split_score" \
      'BEGIN { print score * 1000000 }')" \
  "P=1 should remove one million-fold penalty from two segments"
assert_close "$(awk '$2 == "ab" && $3 == "cd" { print $1 }' \
    "$test_dir/penalty-one.stdout")" 70 \
  "one-segment phrase should be invariant"

"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 --max-extract-words 2 \
  > "$test_dir/extract-two.stdout" 2> "$test_dir/extract-two.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 -x 1 \
  > "$test_dir/extract-one.stdout" 2> "$test_dir/extract-one.stderr"
cmp "$test_dir/all.stdout" "$test_dir/extract-two.stdout" ||
  fail "-x 2 changed stdout where no entry holds three words"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/extract-one.stdout") -eq 0 ]] ||
  fail "-x 1 kept the two-word index entry"
grep -Eq "${diagnostic_prefix}at most 1 word per index entry$" \
  "$test_dir/extract-one.stderr" ||
  fail "--max-extract-words diagnostic is missing from stderr"

"$dfs_anagrams" -i "$index_file" fghij -m 1 -n 10 \
  > "$test_dir/extract-default.stdout" 2> "$test_dir/extract-default.stderr"
"$dfs_anagrams" -i "$index_file" fghij -m 1 -n 10 -x 2 \
  > "$test_dir/extract-default-two.stdout" \
  2> "$test_dir/extract-default-two.stderr"
"$dfs_anagrams" -i "$index_file" fghij -m 1 -n 10 -x 0 \
  > "$test_dir/extract-unlimited.stdout" \
  2> "$test_dir/extract-unlimited.stderr"
cmp "$test_dir/extract-default.stdout" "$test_dir/extract-default-two.stdout" ||
  fail "default max extraction differs from -x 2"
grep -q ' f gh ij$' "$test_dir/extract-unlimited.stdout" ||
  fail "-x 0 did not retain the three-word index entry"
if grep -q ' f gh ij$' "$test_dir/extract-default.stdout"; then
  fail "default max extraction retained the three-word index entry"
fi

# The second line is the first one reversed, so its two insertions are the two
# the first line already made: four insertions, two keys. Both reversal and
# dedup show up in the reported key count. An explicit zero pair bonus makes
# loading the scoring input alone leave output unchanged.
printf 'ab,cd\ncd,ab\n' > "$test_dir/pairs.txt"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --pairs "$test_dir/pairs.txt" --word-bonus 0 --pair-bonus 0 \
  > "$test_dir/pair-list.stdout" 2> "$test_dir/pair-list.stderr"
grep -Eq "${diagnostic_prefix}pair list: 2 pairs, 2 keys$" \
  "$test_dir/pair-list.stderr" ||
  fail "the pair-list diagnostic did not report reversal and dedup"
cmp "$test_dir/all.stdout" "$test_dir/pair-list.stdout" ||
  fail "a loaded pair list changed stdout at --pair-bonus 0"

# A '-' line is skipped rather than counted, but still advances the line
# number the next error reports.
printf 'ab,cd\ne-f,gh\nij,kl,mn\n' > "$test_dir/bad-pairs.txt"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --pairs "$test_dir/bad-pairs.txt"
grep -q "^error: pair list \"$test_dir/bad-pairs.txt\" line 3: expected one word or two comma-separated words$" \
  "$test_dir/status.stderr" ||
  fail "the malformed pair-line diagnostic did not name the right line"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --pairs "$test_dir/missing-pairs.txt"
grep -q "^error: can't open pair list \"$test_dir/missing-pairs.txt\"$" \
  "$test_dir/status.stderr" ||
  fail "the missing pair-list diagnostic is unclear"
expect_status 2 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --pairs

# A short component may bypass -m only as part of the exact pair in its
# written orientation. The digit fixture is disjoint from the legacy output
# cases above, and its total normalized length is at least four.
printf '1,2345\n' > "$test_dir/short-first.pairs"
printf '2345,1\n' > "$test_dir/short-last.pairs"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 \
  > "$test_dir/short-none.stdout" 2> "$test_dir/short-none.stderr"
[[ ! -s "$test_dir/short-none.stdout" ]] ||
  fail "a short pair was extracted without --pairs"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" --pair-bonus 0 \
  > "$test_dir/short-first.stdout" 2> "$test_dir/short-first.stderr"
grep -q ' 1 2345$' "$test_dir/short-first.stdout" ||
  fail "a listed short-first pair was not extracted"
! grep -q ' 2345 1$' "$test_dir/short-first.stdout" ||
  fail "a short-first pair was matched in reverse"
! grep -q ' 1$' "$test_dir/short-first.stdout" ||
  fail "a short first word was emitted as a standalone entry"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 -g 1 \
  --pairs "$test_dir/short-first.pairs" --pair-bonus 0 \
  > "$test_dir/short-one-segment.stdout" \
  2> "$test_dir/short-one-segment.stderr"
grep -q ' 1 2345$' "$test_dir/short-one-segment.stdout" ||
  fail "-g 1 did not retain a short-pair entry as one segment"
grep -Eq "${diagnostic_prefix}"'5 letters "12345", entries of 4[+] letters, at most 1 segment, exactly 1 segment$' \
  "$test_dir/short-one-segment.stderr" ||
  fail "short-pair search header did not preserve the entry bound"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-last.pairs" --pair-bonus 0 \
  > "$test_dir/short-last.stdout" 2> "$test_dir/short-last.stderr"
grep -q ' 2345 1$' "$test_dir/short-last.stdout" ||
  fail "a listed long-first short-last pair was not extracted"
! grep -q ' 1 2345$' "$test_dir/short-last.stdout" ||
  fail "a long-first short-last pair was matched in reverse"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 -x 1 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-extract-one.stdout" \
  2> "$test_dir/short-extract-one.stderr"
[[ ! -s "$test_dir/short-extract-one.stdout" ]] ||
  fail "-x 1 kept a short-pair exception"
"$dfs_anagrams" -i "$index_file" 23456789 -m 4 -n 10 \
  --pairs "$test_dir/short-last.pairs" --pair-bonus 0 \
  > "$test_dir/short-shared-prefix.stdout" \
  2> "$test_dir/short-shared-prefix.stderr"
grep -q ' 2345 6789$' "$test_dir/short-shared-prefix.stdout" ||
  fail "an ordinary phrase sharing an exception prefix was rejected"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" \
  --exclude-pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-excluded.stdout" \
  2> "$test_dir/short-excluded.stderr"
[[ ! -s "$test_dir/short-excluded.stdout" ]] ||
  fail "--exclude-pairs did not reject a short-pair exception"
printf '2345\n' > "$test_dir/short-dictionary"
"$dfs_anagrams" -i "$index_file" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" --dict "$test_dir/short-dictionary" \
  > "$test_dir/short-dictionary.stdout" \
  2> "$test_dir/short-dictionary.stderr"
[[ ! -s "$test_dir/short-dictionary.stdout" ]] ||
  fail "dictionary filtering did not reject a short-pair component"

# Pair membership remains directional for asserted solo-word edges. No index
# phrase joins 6789 to 1, so only the oriented pair-file key can make the edge.
printf '6789,1\n' > "$test_dir/solo-short-forward.pairs"
printf '1,6789\n' > "$test_dir/solo-short-reverse.pairs"
"$dfs_anagrams" -i "$index_file" 6789 -m 4 -n 10 \
  --solo-words 1 --word-bonus 0 \
  --pairs "$test_dir/solo-short-forward.pairs" \
  > "$test_dir/solo-short-forward.stdout" \
  2> "$test_dir/solo-short-forward.stderr"
grep -q ' 6789 (1)$' "$test_dir/solo-short-forward.stdout" ||
  fail "the oriented short-pair solo edge was not applied"
"$dfs_anagrams" -i "$index_file" 6789 -m 4 -n 10 \
  --solo-words 1 --word-bonus 0 \
  --pairs "$test_dir/solo-short-reverse.pairs" \
  > "$test_dir/solo-short-reverse.stdout" \
  2> "$test_dir/solo-short-reverse.stderr"
! grep -q ' 6789 (1)$' "$test_dir/solo-short-reverse.stdout" ||
  fail "a reversed short-pair solo edge was applied"

# Validation measures cleaned field characters, not the comma or formatting.
printf '!b!e!, a!n!\n' > "$test_dir/normalized-valid.pairs"
"$dfs_anagrams" -i "$index_file" abcd -m 4 -n 10 \
  --pairs "$test_dir/normalized-valid.pairs" \
  > /dev/null 2> "$test_dir/normalized-valid.stderr"
printf '!b!e!, a!\n' > "$test_dir/normalized-short-pair.pairs"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 4 -n 10 \
  --pairs "$test_dir/normalized-short-pair.pairs"
grep -q "^error: pair list \"$test_dir/normalized-short-pair.pairs\" line 1: normalized entry has 3 non-space characters, fewer than -m 4$" \
  "$test_dir/status.stderr" ||
  fail "short normalized pair diagnostic is wrong"
printf 'a!n!\n' > "$test_dir/normalized-short-word.pairs"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 4 -n 10 \
  --pairs "$test_dir/normalized-short-word.pairs"
grep -q "^error: pair list \"$test_dir/normalized-short-word.pairs\" line 1: normalized entry has 2 non-space characters, fewer than -m 4$" \
  "$test_dir/status.stderr" ||
  fail "short normalized word diagnostic is wrong"

# --exclude-pairs drops the whole "ab cd" index entry, in either written
# order. The two-entry "ab,cd" answer survives: the exclusion is over index
# entries, not over adjacency in a result.
printf 'cd,ab\n' > "$test_dir/exclude.pairs"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --word-bonus 0 --exclude-pairs "$test_dir/exclude.pairs" \
  > "$test_dir/excluded.stdout" 2> "$test_dir/excluded.stderr"
grep -Eq "${diagnostic_prefix}exclude list: 1 pairs, 2 keys$" \
  "$test_dir/excluded.stderr" ||
  fail "the exclude-list diagnostic is missing from stderr"
grep -q '^70\.00 ab cd$' "$test_dir/all.stdout" ||
  fail "the unexcluded run should rank the \"ab cd\" entry first"
[[ $(grep -c ' ab cd$' "$test_dir/excluded.stdout") -eq 0 ]] ||
  fail "--exclude-pairs kept the excluded index entry"
grep -q ' ab,cd$' "$test_dir/excluded.stdout" ||
  fail "--exclude-pairs dropped a result built from two separate entries"

# Repeated files are unioned rather than replacing an earlier option.
printf 'gh,ij\n' > "$test_dir/exclude-ghij.pairs"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/exclude.pairs" \
  --exclude-pairs "$test_dir/exclude-ghij.pairs" \
  > "$test_dir/excluded-multiple.stdout" \
  2> "$test_dir/excluded-multiple.stderr"
cmp "$test_dir/excluded.stdout" "$test_dir/excluded-multiple.stdout" ||
  fail "repeated --exclude-pairs did not combine both files"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/exclude-ghij.pairs" \
  --exclude-pairs "$test_dir/exclude.pairs" \
  > "$test_dir/excluded-multiple-reversed.stdout" \
  2> "$test_dir/excluded-multiple-reversed.stderr"
cmp "$test_dir/excluded.stdout" \
    "$test_dir/excluded-multiple-reversed.stdout" ||
  fail "reversed --exclude-pairs options did not combine both files"

# The test is whole-entry equality, so a longer entry holding the excluded
# pair -- here at its end -- is a different spelling and is kept.
"$dfs_anagrams" -i "$index_file" fghij -m 1 -n 10 -x 0 \
  --exclude-pairs "$test_dir/exclude-ghij.pairs" \
  > "$test_dir/exclude-prefix.stdout" 2> "$test_dir/exclude-prefix.stderr"
grep -q ' f gh ij$' "$test_dir/exclude-prefix.stdout" ||
  fail "--exclude-pairs dropped a longer entry containing the excluded pair"

# A workflow root resolves to its hard-NO aggregate; a directory with no .wf
# is an error rather than an empty exclusion set.
mkdir -p "$test_dir/wf/.wf/classified/no"
cp "$test_dir/exclude.pairs" "$test_dir/wf/.wf/classified/no/no.pairs"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/exclude-ghij.pairs" \
  --exclude-pairs "$test_dir/wf" \
  > "$test_dir/exclude-wf.stdout" 2> "$test_dir/exclude-wf.stderr"
cmp "$test_dir/excluded.stdout" "$test_dir/exclude-wf.stdout" ||
  fail "a file and workflow root did not combine their exclusion sets"
mkdir -p "$test_dir/wf2/.wf/classified/no"
cp "$test_dir/exclude.pairs" "$test_dir/wf2/.wf/classified/no/no.pairs"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/wf" --exclude-pairs "$test_dir/wf2"
grep -q '^error: only one --exclude-pairs argument may be a directory$' \
  "$test_dir/status.stderr" ||
  fail "multiple --exclude-pairs directories were not rejected clearly"
mkdir -p "$test_dir/not-wf"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/not-wf"
grep -q "^error: --exclude-pairs directory \"$test_dir/not-wf\" has no workflow metadata \"$test_dir/not-wf/.wf\"$" \
  "$test_dir/status.stderr" ||
  fail "the missing-workflow diagnostic did not name both paths"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/missing-exclude.pairs"
grep -q "^error: can't open exclude list \"$test_dir/missing-exclude.pairs\"$" \
  "$test_dir/status.stderr" ||
  fail "the missing exclude-list diagnostic is unclear"

# A '-' line is a skipped bonus but a dropped exclusion, so only the exclusion
# list rejects it rather than quietly enforcing less than it was given.
printf 'a-b,cd\n' > "$test_dir/hyphen.pairs"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --pairs "$test_dir/hyphen.pairs" --pair-bonus 0 \
  > /dev/null 2> "$test_dir/hyphen-bonus.stderr"
grep -Eq "${diagnostic_prefix}pair list: 0 pairs, 0 keys$" \
  "$test_dir/hyphen-bonus.stderr" ||
  fail "a '-' line should still be skipped in a bonus list"
expect_status 1 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --exclude-pairs "$test_dir/hyphen.pairs"
grep -q "^error: exclude list \"$test_dir/hyphen.pairs\" line 1: '-' would silently skip this entry$" \
  "$test_dir/status.stderr" ||
  fail "a '-' line in an exclusion list was not rejected"

# "klmn" (1000) and "kl mn" (5) are one anagram class, so the bonus has to
# reorder within a class to promote the phrase, and the single word's own
# score must not move with a bonus it never earned.
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 5 --word-bonus 0 \
  > "$test_dir/bonus-zero.stdout" 2> "$test_dir/bonus-zero.stderr"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 5 --word-bonus 1 \
  > "$test_dir/bonus-one.stdout" 2> "$test_dir/bonus-one.stderr"
[[ $(awk 'NR == 1 { print $2 }' "$test_dir/bonus-zero.stdout") == klmn ]] ||
  fail "--word-bonus 0 did not rank the more frequent single word first"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/bonus-zero.stdout")" \
  1000 "--word-bonus 0 should score the single word by its raw count"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/bonus-one.stdout") \
   == "kl mn" ]] ||
  fail "--word-bonus 1 did not promote the phrase within its class"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/bonus-one.stdout")" \
  5000000 "--word-bonus 1 should multiply the phrase by one million"
[[ $(awk 'NR == 2 { print $2 }' "$test_dir/bonus-one.stdout") == klmn ]] ||
  fail "--word-bonus 1 dropped the single word from the class"
assert_close "$(awk 'NR == 2 { print $1 }' "$test_dir/bonus-one.stdout")" \
  1000 "--word-bonus 1 should leave the single word's score alone"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 5 \
  > "$test_dir/bonus-default.stdout" 2> "$test_dir/bonus-default.stderr"
cmp "$test_dir/bonus-one.stdout" "$test_dir/bonus-default.stdout" ||
  fail "omitted --word-bonus did not match --word-bonus 1"

# The same within-class promotion must work when only the selected pair earns
# the bonus. This exercises pair lookup, score ordering, phase-2 bounds, and
# phase-3 spelling deltas together.
printf 'kl,mn\n' > "$test_dir/klmn-pairs.txt"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 5 \
  --word-bonus 0 --pairs "$test_dir/klmn-pairs.txt" \
  > "$test_dir/pair-bonus.stdout" 2> "$test_dir/pair-bonus.stderr"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/pair-bonus.stdout") \
   == "kl mn" ]] ||
  fail "the default pair bonus did not promote the listed pair within its class"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/pair-bonus.stdout")" \
  5000000 "the default pair bonus should multiply the listed pair by one million"
[[ $(awk 'NR == 2 { print $2 }' "$test_dir/pair-bonus.stdout") == klmn ]] ||
  fail "--pair-bonus dropped the unlisted single word from the class"
assert_close "$(awk 'NR == 2 { print $1 }' "$test_dir/pair-bonus.stdout")" \
  1000 "--pair-bonus should leave the unlisted single word's score alone"

# Fixed positive sources are repeatable and choose one maximum tier for both
# orientations; exercise the DFS score path as well as query-index --score.
printf 'kl,mn\n' > "$test_dir/weighted-seed.pairs"
printf 'mn,kl\n' > "$test_dir/weighted-seed-reversed.pairs"
printf 'mn,kl\n' > "$test_dir/weighted-yes.pairs"
printf 'kl,mn\n' > "$test_dir/weighted-yes-reversed.pairs"
printf 'kl,mn\n' > "$test_dir/weighted-best.pairs"
printf 'mn,kl\n' > "$test_dir/weighted-best-reversed.pairs"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 --word-bonus 0 \
  --seed-pairs "$test_dir/weighted-seed.pairs" \
  --seed-pairs "$test_dir/weighted-seed-reversed.pairs" \
  --yes-pairs "$test_dir/weighted-yes.pairs" \
  --yes-pairs "$test_dir/weighted-yes-reversed.pairs" \
  --best-pairs "$test_dir/weighted-best.pairs" \
  --best-pairs "$test_dir/weighted-best-reversed.pairs" \
  > "$test_dir/weighted-pairs.stdout" \
  2> "$test_dir/weighted-pairs.stderr"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/weighted-pairs.stdout") \
   == "kl mn" ]] ||
  fail "weighted pair tiers did not promote the paired DFS spelling"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/weighted-pairs.stdout")" \
  "$(awk 'BEGIN { print 5 * exp(log(1000000) * 1.10) }')" \
  "repeated weighted pair sources did not retain the BEST tier"
expect_status 2 "$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 \
  --pairs "$test_dir/klmn-pairs.txt" \
  --seed-pairs "$test_dir/weighted-seed.pairs"
grep -q '^error: --pairs cannot be combined with --seed-pairs, --yes-pairs, or --best-pairs$' \
  "$test_dir/status.stderr" ||
  fail "legacy and fixed DFS pair inputs were not rejected together"

# Workflow defaults use the filtered dictionary and layer sentence seed,
# global YES, and a complete target's optional BEST pair file.
workflow_root="$test_dir/workflow-positive"
mkdir -p "$workflow_root/.wf/dict" \
  "$workflow_root/.wf/classified/yes" \
  "$workflow_root/.wf/best/idx" \
  "$workflow_root/.wf/best/s1/o-klmn/m2/g1"
cp "$index_file" "$workflow_root/.wf/best/idx/wiki-merged.2.index"
printf 'kl\nmn\n' > "$workflow_root/.wf/dict/words.filtered"
printf 'kl,mn\n' > "$workflow_root/.wf/classified/yes/yes.pairs"
printf 'mn,kl\n' > "$workflow_root/.wf/best/s1/seed.m2.pairs"
printf 'kl,mn\n' > "$workflow_root/.wf/best/s1/o-klmn/m2/g1/best.pairs"
"$dfs_anagrams" klmn -m 2 -n 2 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-klmn/m2/g1' \
  > "$test_dir/workflow-positive.stdout" \
  2> "$test_dir/workflow-positive.stderr"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/workflow-positive.stdout") \
   == "kl mn" ]] ||
  fail "workflow default dictionary did not retain its allowed phrase"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/workflow-positive.stdout")" \
  "$(awk 'BEGIN { print 5 * exp(log(1000000) * 1.10) }')" \
  "workflow target did not auto-load BEST pairs above seed and YES"
WFROOT="$workflow_root" "$dfs_anagrams" klmn -m 2 -n 2 \
  --word-bonus 0 --wf -t 'S1/o-klmn/m2/g1' \
  > "$test_dir/workflow-alias.stdout" \
  2> "$test_dir/workflow-alias.stderr"
cmp "$test_dir/workflow-positive.stdout" "$test_dir/workflow-alias.stdout" ||
  fail "--wf did not use WFROOT like --wfroot"
rm "$workflow_root/.wf/best/idx/wiki-merged.2.index"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-klmn/m2/g1' \
  > "$test_dir/workflow-index-override.stdout" \
  2> "$test_dir/workflow-index-override.stderr"
cmp "$test_dir/workflow-positive.stdout" \
    "$test_dir/workflow-index-override.stdout" ||
  fail "explicit -i did not override a missing workflow index"
expect_status 2 "$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 \
  --wfroot "$workflow_root"
grep -q 'workflow mode requires --target beginning with sN or an explicit --seed-pairs' \
  "$test_dir/status.stderr" ||
  fail "workflow seed requirement diagnostic is unclear"

# Workflow mode excludes its own classified NO pairs and a complete selected
# target's no.pairs without an explicit --exclude-pairs.
mkdir -p "$workflow_root/.wf/classified/no"
printf 'kl,mn\n' > "$workflow_root/.wf/best/s1/no.pairs"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 --word-bonus 0 \
  --wfroot "$workflow_root" -t s1 \
  > "$test_dir/workflow-abbreviated-negative.stdout" \
  2> "$test_dir/workflow-abbreviated-negative.stderr"
grep -q 'kl mn' "$test_dir/workflow-abbreviated-negative.stdout" ||
  fail "workflow mode applied no.pairs from an abbreviated target"
rm "$workflow_root/.wf/best/s1/no.pairs"
printf 'kl,mn\n' > "$workflow_root/.wf/classified/no/no.pairs"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-klmn/m2/g1' \
  > "$test_dir/workflow-negative.stdout" \
  2> "$test_dir/workflow-negative.stderr"
! grep -q 'kl mn' "$test_dir/workflow-negative.stdout" ||
  fail "workflow mode kept a classified NO pair"
rm "$workflow_root/.wf/classified/no/no.pairs"
printf 'kl,mn\n' > "$workflow_root/.wf/best/s1/o-klmn/m2/g1/no.pairs"
"$dfs_anagrams" -i "$index_file" klmn -m 2 -n 2 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-klmn/m2/g1' \
  > "$test_dir/workflow-target-negative.stdout" \
  2> "$test_dir/workflow-target-negative.stderr"
! grep -q 'kl mn' "$test_dir/workflow-target-negative.stdout" ||
  fail "workflow mode kept the target's own NO pair"
rm "$workflow_root/.wf/best/s1/o-klmn/m2/g1/no.pairs"

"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --solo-words cd --word-bonus 0 --pair-bonus 0 \
  > "$test_dir/solo-inert.stdout" 2> "$test_dir/solo-inert.stderr"
cmp "$test_dir/all.stdout" "$test_dir/solo-inert.stdout" ||
  fail "score-inert --solo-words changed DFS output"

# A pairs-only external edge promotes the split spelling above the contiguous
# phrase even with a bounded top-N queue.
printf 'ab,zz\n' > "$test_dir/solo-top-pairs.txt"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 1 -P 1 \
  --solo-words zz --word-bonus 0 \
  --pairs "$test_dir/solo-top-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-top.stdout" 2> "$test_dir/solo-top.stderr"
[[ $(sed -n '1s/^[^ ]* //p' "$test_dir/solo-top.stdout") \
   == "ab (zz),cd" ]] ||
  fail "solo-word upper bounds did not retain the bounded winner"
solo_top_score=$(awk 'NR == 1 { print $1 }' "$test_dir/solo-top.stdout")
solo_top_round_trip=$("$query_index" -i "$index_file" ab,cd --score -P 1 \
  --solo-words zz --word-bonus 0 \
  --pairs "$test_dir/solo-top-pairs.txt" --pair-bonus 1 | awk '{ print $1 }')
assert_close "$solo_top_score" "$solo_top_round_trip" \
  "DFS solo-word score did not round-trip through query-index --score"

printf 'ba,cd\nwx,ab\nxy,ab\n' > "$test_dir/solo-pairs.txt"
"$dfs_anagrams" -i "$index_file" abba -m 2 -n 10 -P 1 \
  --solo-words cd --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 0 \
  > "$test_dir/solo-scarcity.stdout" 2> "$test_dir/solo-scarcity.stderr"
scarce_score=$(awk '$0 ~ / ab,ba \(cd\)$/ { print $1 }' \
  "$test_dir/solo-scarcity.stdout")
[[ -n $scarce_score ]] || fail "solo-word scarcity spelling is missing"
scarce_round_trip=$("$query_index" -i "$index_file" ab,ba --score -P 1 \
  --solo-words cd --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 0 | awk '{ print $1 }')
assert_close "$scarce_score" "$scarce_round_trip" \
  "DFS spent one solo word more than once"

# wx can reroute from its asserted ab edge to its aggregate yz edge, leaving
# ab for xy. This is the non-greedy maximum-score assignment.
"$dfs_anagrams" -i "$index_file" wxxy -m 2 -n 1 -P 1 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-reroute.stdout" 2> "$test_dir/solo-reroute.stderr"
[[ $(sed -n '1s/^[^ ]* //p' "$test_dir/solo-reroute.stdout") \
   == "wx (yz),xy (ab)" ]] ||
  fail "solo-word assignment reroute lost the bounded DFS winner"
reroute_score=$(awk 'NR == 1 { print $1 }' \
  "$test_dir/solo-reroute.stdout")
reroute_round_trip=$("$query_index" -i "$index_file" wx,xy --score -P 1 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 | awk '{ print $1 }')
assert_close "$reroute_score" "$reroute_round_trip" \
  "rerouted DFS score did not round-trip through query-index --score"
"$dfs_anagrams" -i "$index_file" wxxy -m 2 -n 1 -P 1 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  --hide-solo-words \
  > "$test_dir/solo-reroute-hidden.stdout" \
  2> "$test_dir/solo-reroute-hidden.stderr"
[[ $(awk 'NR == 1 { print $2 }' \
      "$test_dir/solo-reroute-hidden.stdout") == wx,xy ]] ||
  fail "--hide-solo-words did not restore the unannotated DFS spelling"
assert_close "$(awk 'NR == 1 { print $1 }' \
    "$test_dir/solo-reroute-hidden.stdout")" "$reroute_score" \
  "--hide-solo-words changed the DFS score"
grep -Eq "${diagnostic_prefix}solo words: 2 profiles, 3 word edges, 2 pair edges$" \
  "$test_dir/solo-reroute.stderr" ||
  fail "solo profile and edge diagnostics are missing"

# Every result line's entry list must be pasteable into "query-index --score"
# and reproduce that line's own score.
while read -r result_score result_entries; do
  round_trip=$("$query_index" -i "$index_file" "$result_entries" --score \
    --word-bonus 0 |
    awk '{ print $1 }')
  assert_close "$round_trip" "$result_score" \
    "query-index --score disagrees on \"$result_entries\""
done < "$test_dir/all.stdout"

# -g N keeps only N-entry results. "ab cd" wins the unconstrained search as one
# contiguous phrase and absorbs its split form, so -g 2 is what exposes "ab,cd".
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 -g 1 \
  > "$test_dir/one-segment.stdout" 2> "$test_dir/one-segment.stderr"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 -g 2 \
  > "$test_dir/two-segment.stdout" 2> "$test_dir/two-segment.stderr"
if grep -q ',' "$test_dir/one-segment.stdout"; then
  fail "-g 1 returned a multi-entry result"
fi
[[ $(grep -c '^[^ ]* [^,]*,[^,]*$' "$test_dir/two-segment.stdout") \
   -eq $(wc -l < "$test_dir/two-segment.stdout") ]] ||
  fail "-g 2 returned a result that does not use exactly two entries"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/one-segment.stdout") -eq 1 ]] ||
  fail "-g 1 lost the contiguous phrase"
grep -Eq "${diagnostic_prefix}"'4 letters "abcd", words of 2\+, at most 2 words, exactly 1 segment$' \
  "$test_dir/one-segment.stderr" ||
  fail "the search header did not report the segment constraint"
grep -Eq "${diagnostic_prefix}"'.*, exactly 2 segments$' \
  "$test_dir/two-segment.stderr" ||
  fail "the search header did not pluralize the segment constraint"
grep -q ' ab,cd$' "$test_dir/two-segment.stdout" ||
  fail "-g 2 did not expose the split form of the contiguous phrase"
"$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 --word-bonus 0 -g 9 \
  > "$test_dir/unreachable-segment.stdout" \
  2> "$test_dir/unreachable-segment.stderr"
[[ ! -s $test_dir/unreachable-segment.stdout ]] ||
  fail "-g beyond the letters' segment limit returned results"

"$dfs_anagrams" -i "$index_file" abcd -u ab -m 2 -n 10 \
  > "$test_dir/used.stdout" 2> "$test_dir/used.stderr"
"$dfs_anagrams" -i "$index_file" cd -m 2 -n 10 \
  > "$test_dir/left.stdout" 2> "$test_dir/left.stderr"
cmp "$test_dir/used.stdout" "$test_dir/left.stdout" ||
  fail "--used-letters differs from searching the remaining bag"

expect_status 2 "$dfs_anagrams" -i "$index_file" 'ab!'
expect_status 2 "$dfs_anagrams" -i "$index_file" abc -p 0
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --cache-size nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --preprocess-threads nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --search-threads nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --projection-depth nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --max-extract-words nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc -P 0
grep -q '^error: --segment-penalty must be at least 1$' \
  "$test_dir/status.stderr" ||
  fail "zero segment-penalty diagnostic is unclear"
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --segment-penalty 0.5
expect_status 2 "$dfs_anagrams" -i "$index_file" abc -P nope
expect_status 2 "$dfs_anagrams" -i "$index_file" abc -P inf
expect_status 2 "$dfs_anagrams" -i "$index_file" abc \
  --solo-words ab --word-bonus -1
grep -q '^error: --word-bonus must be non-negative with --solo-words$' \
  "$test_dir/status.stderr" ||
  fail "negative solo-word bonus diagnostic is unclear"
expect_status 2 "$dfs_anagrams" -i "$index_file" abcd -m 2 -n 10 \
  --cache-size 0
grep -q '^error: projected dense score table requires at least 1 MiB; supplied cache is 0 MiB$' \
  "$test_dir/status.stderr" ||
  fail "undersized projected-cache diagnostic is missing"
grep -q '^       use -C 1 or --allow-cache-fallback$' \
  "$test_dir/status.stderr" ||
  fail "projected-cache recovery diagnostic is missing"
expect_status 1 "$dfs_anagrams" -i "$test_dir/missing.index" abcd
