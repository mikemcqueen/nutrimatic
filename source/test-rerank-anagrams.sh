#!/usr/bin/env bash
set -euo pipefail

rerank=$1
dfs_anagrams=$2
make_index=$3
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-rerank.XXXXXX")

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

workflow=$test_dir/workflow
target_name=s1/o-abcd/m2/g2
target=$workflow/.wf/best/$target_name
used_target_name=s1/u-efgh/m2/g2
used_target=$workflow/.wf/best/$used_target_name
mkdir -p "$workflow/.wf/best/idx" \
  "$workflow/.wf/dict" \
  "$workflow/.wf/classified/yes" \
  "$target" "$used_target"

index=$workflow/.wf/best/idx/wiki-merged.2.index
"$make_index" "$index"
printf 'abcdefgh\n' > "$workflow/.wf/best/s1/letters"
printf 'ab\nba\ncd\ndc\n' > "$workflow/.wf/dict/words.filtered"
printf 'ab\n' > "$workflow/.wf/best/s1/seed.m2.pairs"
printf 'cd\n' > "$workflow/.wf/classified/yes/yes.pairs"
printf 'ba\n' > "$target/best.pairs"

"$dfs_anagrams" abcd --wfroot "$workflow" -t "$target_name" \
  -m 2 -g 2 -n 0 \
  > "$test_dir/dfs.stdout" 2> "$test_dir/dfs.stderr"
"$dfs_anagrams" abcd --wfroot "$workflow" -t "$target_name" \
  -m 2 -g 2 -n 0 --show-bonus \
  > "$test_dir/dfs-bonus.stdout" 2> "$test_dir/dfs-bonus.stderr"

"$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/dfs.stdout" \
  > "$test_dir/rerank.stdout" 2> "$test_dir/rerank.stderr"
cmp "$test_dir/dfs.stdout" "$test_dir/rerank.stdout" ||
  fail "unchanged rerank output differs from dfs-anagrams"
env WFROOT="$workflow" "$rerank" --wf -t "$target_name" \
  "$test_dir/dfs.stdout" > "$test_dir/rerank-wf.stdout" \
  2> "$test_dir/rerank-wf.stderr"
cmp "$test_dir/dfs.stdout" "$test_dir/rerank-wf.stdout" ||
  fail "--wf did not use WFROOT like --wfroot"

"$rerank" --wfroot "$workflow" -t "$target_name" --show-bonus \
  < "$test_dir/dfs.stdout" \
  > "$test_dir/rerank-bonus.stdout" 2> "$test_dir/rerank-bonus.stderr"
cmp "$test_dir/dfs-bonus.stdout" "$test_dir/rerank-bonus.stdout" ||
  fail "bonus rerank output differs from dfs-anagrams --show-bonus"

"$dfs_anagrams" abcd --wfroot "$workflow" -t "$target_name" \
  -m 2 -g 2 -n 0 --ptm \
  > "$test_dir/dfs-ptm.stdout" 2> "$test_dir/dfs-ptm.stderr"
"$rerank" --wfroot "$workflow" -t "$target_name" --ptm \
  "$test_dir/dfs-ptm.stdout" \
  > "$test_dir/rerank-ptm.stdout" 2> "$test_dir/rerank-ptm.stderr"
cmp "$test_dir/dfs-ptm.stdout" "$test_dir/rerank-ptm.stdout" ||
  fail "--ptm rerank output differs from dfs-anagrams --ptm"
grep -q 'ptm: tail rate ' "$test_dir/rerank-ptm.stderr" ||
  fail "--ptm did not report the fitted tail"

"$dfs_anagrams" abcdefgh --used-letters efgh \
  --wfroot "$workflow" -t "$used_target_name" -m 2 -g 2 -n 0 \
  > "$test_dir/dfs-used.stdout" 2> "$test_dir/dfs-used.stderr"
"$rerank" --wfroot "$workflow" -t "$used_target_name" \
  "$test_dir/dfs-used.stdout" \
  > "$test_dir/rerank-used.stdout" 2> "$test_dir/rerank-used.stderr"
cmp "$test_dir/dfs-used.stdout" "$test_dir/rerank-used.stdout" ||
  fail "u- target did not derive the generator's subtracted bag"

printf 'dc\n' > "$test_dir/replacement-best.pairs"
printf 'ab\n' > "$test_dir/reject.pairs"
"$rerank" --wfroot "$workflow" -t "$target_name" \
  --best-pairs "$test_dir/replacement-best.pairs" \
  -r "$test_dir/reject.pairs" --show-bonus "$test_dir/dfs.stdout" \
  > "$test_dir/changed.stdout" 2> "$test_dir/changed.stderr"

[[ $(wc -l < "$test_dir/changed.stdout") -eq 2 ]] ||
  fail "replacement and rejection did not retain two saved candidates"
[[ $(sed -n '1s/^[^ ]* //p' "$test_dir/changed.stdout") == '-,B ba,dc' ]] ||
  fail "replacement BEST did not promote and mark ba,dc"
[[ $(sed -n '2s/^[^ ]* //p' "$test_dir/changed.stdout") == '-,Y ba,cd' ]] ||
  fail "changed pool has the wrong marker or ordering"
if grep -q 'B,- ba,' "$test_dir/changed.stdout"; then
  fail "explicit BEST augmented rather than replaced target best.pairs"
fi

printf 'ba,dc\n' > "$test_dir/one-best.pairs"
"$rerank" --wfroot "$workflow" -t "$target_name" \
  --best-pairs "$test_dir/one-best.pairs" \
  -r "$test_dir/reject.pairs" --show-bonus "$test_dir/dfs.stdout" \
  > "$test_dir/one-best-file.stdout" 2> "$test_dir/one-best-file.stderr"
"$rerank" --wfroot "$workflow" -t "$target_name" \
  --one-best-pair ba,dc \
  -r "$test_dir/reject.pairs" --show-bonus "$test_dir/dfs.stdout" \
  > "$test_dir/one-best.stdout" 2> "$test_dir/one-best.stderr"
cmp "$test_dir/one-best-file.stdout" "$test_dir/one-best.stdout" ||
  fail "--one-best-pair differs from the equivalent --best-pairs file"

expect_status 2 "$rerank" --wfroot "$workflow" -t "$target_name" \
  --one-best-pair ba "$test_dir/dfs.stdout"
expect_status 2 "$rerank" --wfroot "$workflow" -t "$target_name" \
  --one-best-pair ba,dc --best-pairs "$test_dir/one-best.pairs" \
  "$test_dir/dfs.stdout"
old_score=$(awk '$2 == "ba,cd" { print $1 }' "$test_dir/dfs.stdout")
new_score=$(awk 'NR == 2 { print $1 }' "$test_dir/changed.stdout")
[[ -n $old_score && -n $new_score && $old_score != "$new_score" ]] ||
  fail "replacement BEST did not change the saved candidate score"

"$rerank" --wfroot "$workflow" -t "$target_name" -n 1 \
  "$test_dir/dfs.stdout" \
  > "$test_dir/rerank-top.stdout" 2> "$test_dir/rerank-top.stderr"
[[ $(cat "$test_dir/rerank-top.stdout") == $(head -n 1 "$test_dir/dfs.stdout") ]] ||
  fail "-n 1 did not print only the best row"

"$rerank" --wfroot "$workflow" -t "$target_name" --no-score \
  "$test_dir/dfs.stdout" \
  > "$test_dir/rerank-no-score.stdout" 2> "$test_dir/rerank-no-score.stderr"
[[ $(cat "$test_dir/rerank-no-score.stdout") == \
   $(cut -d' ' -f2- "$test_dir/dfs.stdout") ]] ||
  fail "--no-score did not drop only the score column"

expect_status 2 "$rerank" -t "$target_name" "$test_dir/dfs.stdout"
expect_status 2 "$rerank" --wfroot "$workflow" \
  "$test_dir/dfs.stdout"
expect_status 2 "$rerank" --wfroot "$workflow" -t s1/o-abcd \
  "$test_dir/dfs.stdout"
expect_status 2 "$rerank" --wfroot "$workflow" -t s9/o-abcd/m2/g2 \
  "$test_dir/dfs.stdout"
expect_status 2 env WFROOT="$workflow" "$rerank" --wf \
  --wfroot "$workflow" -t "$target_name" "$test_dir/dfs.stdout"
expect_status 2 "$rerank" --wfroot "$workflow" -t "$target_name" \
  -m 2 "$test_dir/dfs.stdout"

mkdir "$target/no.pairs"
expect_status 2 "$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/dfs.stdout"
grep -q 'target NO pair file .* is not a regular file' \
  "$test_dir/status.stderr" ||
  fail "target NO pair directory diagnostic is missing"
rmdir "$target/no.pairs"

printf 'not-a-score ab,cd\n' > "$test_dir/malformed.rows"
expect_status 1 "$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/malformed.rows"
printf '1.000 abcd\n' > "$test_dir/wrong-count.rows"
expect_status 1 "$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/wrong-count.rows"
printf '1.000 ab,ce\n' > "$test_dir/wrong-bag.rows"
expect_status 1 "$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/wrong-bag.rows"
expect_status 1 "$rerank" --wfroot "$workflow" -t "$target_name" \
  "$test_dir/dfs-bonus.stdout"
grep -q 'annotated input is not supported' "$test_dir/status.stderr" ||
  fail "annotated input diagnostic is missing"

printf 'ab,zz\n' > "$test_dir/solo.pairs"
"$dfs_anagrams" abcd --wfroot "$workflow" -t "$target_name" \
  -m 2 -g 2 -n 0 --more-best-pairs "$test_dir/solo.pairs" --solo-words zz \
  > "$test_dir/dfs-solo.stdout" 2> "$test_dir/dfs-solo.stderr"
grep -q ' (zz)' "$test_dir/dfs-solo.stdout" ||
  fail "solo fixture produced no partner annotation"
"$rerank" --wfroot "$workflow" -t "$target_name" \
  --more-best-pairs "$test_dir/solo.pairs" --solo-words zz \
  "$test_dir/dfs-solo.stdout" \
  > "$test_dir/rerank-solo.stdout" 2> "$test_dir/rerank-solo.stderr"
cmp "$test_dir/dfs-solo.stdout" "$test_dir/rerank-solo.stdout" ||
  fail "solo rerank output differs from dfs-anagrams --solo-words"
"$rerank" --wfroot "$workflow" -t "$target_name" \
  --more-best-pairs "$test_dir/solo.pairs" --solo-words zz --hide-solo-words \
  "$test_dir/dfs-solo.stdout" \
  > "$test_dir/rerank-solo-hidden.stdout" 2> "$test_dir/rerank-solo-hidden.stderr"
if grep -q ' (zz)' "$test_dir/rerank-solo-hidden.stdout"; then
  fail "--hide-solo-words kept partner annotations"
fi

echo PASS
