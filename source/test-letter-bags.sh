#!/usr/bin/env bash
set -euo pipefail

letter_bags=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-letter-bags.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

check() {
  local name=$1 expected=$2
  local actual
  actual=$("$letter_bags" "$test_dir/$name") || fail "$name exited nonzero"
  [[ $actual == "$expected" ]] || fail "$name output is wrong: $actual"
}

printf '%s\n' cat Listen silent enlist > "$test_dir/words"
check words 'lines: 4 bags: 2 diff: 2
longest (6): Listen (3)
largest (3): Listen'

printf '%s\n' ab,c xyz,w ca,b > "$test_dir/pairs"
check pairs 'lines: 3 bags: 2 diff: 1
longest (4): xyz,w (1)
largest (2): ab,c'

printf '%s\n' '9 tea' '4 eat' '2 at' > "$test_dir/ranked-words"
check ranked-words 'lines: 3 bags: 2 diff: 1
longest (3): tea (2)
largest (2): tea'

printf '%s\n' '  5 ab c' '3 ca b' > "$test_dir/ranked-pairs"
check ranked-pairs 'lines: 2 bags: 1 diff: 1
longest (3): ab c (2)
largest (2): ab c'

actual=$("$letter_bags" --match 'S-i,L E n t' "$test_dir/words" \
    "$test_dir/ranked-words") || fail "--match exited nonzero"
[[ $actual == $'Listen\nsilent\nenlist' ]] ||
  fail "--match output is wrong: $actual"
actual=$("$letter_bags" --match tae "$test_dir/ranked-words")
[[ $actual == $'tea\neat' ]] || fail "--match ranked output is wrong: $actual"

printf '%s\n' white whitney whites whitey yetiwh whit > "$test_dir/near"
actual=$("$letter_bags" --near whitey "$test_dir/near")
[[ $actual == $'whitey\nwhite\nwhitney' ]] ||
  fail "--near output is wrong: $actual"
actual=$("$letter_bags" --near whitey --by 2 "$test_dir/near" | tail -1)
[[ $actual == whit ]] || fail "--near --by 2 output is wrong: $actual"
actual=$("$letter_bags" --match whitez "$test_dir/near")
[[ $actual == 'bag not found' ]] || fail "--match not found is wrong: $actual"
actual=$("$letter_bags" --near whitez "$test_dir/near")
[[ $actual == white ]] || fail "--near not found is wrong: $actual"
actual=$("$letter_bags" --near whitez --relaxed "$test_dir/near")
[[ $actual == $'white\nwhites\nwhitey' ]] ||
  fail "--near --relaxed is wrong: $actual"
actual=$("$letter_bags" --near zzzz "$test_dir/near")
[[ $actual == 'no nearby bags found' ]] ||
  fail "--near no results is wrong: $actual"
printf '%s\n' white tawny wanty whitey yetiwh zebra zebras \
    > "$test_dir/outliers"
actual=$("$letter_bags" --outliers "$test_dir/outliers")
[[ $actual == $'lines: 7 outliers: 1\ntawny (2)' ]] ||
  fail "--outliers output is wrong: $actual"
actual=$("$letter_bags" --outliers --all "$test_dir/outliers")
[[ $actual == $'tawny\nwanty' ]] || fail "--outliers --all is wrong: $actual"
actual=$("$letter_bags" --outliers --by 3 "$test_dir/near")
[[ $actual == 'lines: 6 outliers: 0' ]] ||
  fail "--outliers --by 3 is wrong: $actual"
{
  printf '%s\n' uuuuuuvvvvvvwwwwww xxxxxxyyyyyyzzzzzz uuuxxxvvvyyywwwzzz
  awk 'BEGIN {
    srand(1)
    for (i = 0; i < 12000; ++i) {
      w = ""
      for (j = 0; j < 20; ++j)
        w = w substr("abcdefghijklmnopqrst", int(rand() * 20) + 1, 1)
      print w
    }
  }'
  printf '%s\n' uuuuvvvvvvwwwwwwxx xxxyyyyyyzzzzzzuuu uxvvvvvyyywwwwwzzz
} > "$test_dir/chunks"
actual=$("$letter_bags" --outliers --by 3 --relaxed "$test_dir/chunks" | grep '^[u-z]')
[[ $actual == $'uuuxxxvvvyyywwwzzz (1)\nuxvvvvvyyywwwwwzzz (1)' ]] ||
  fail "--outliers across stages and chunks is wrong: $actual"
if "$letter_bags" --match a --near a "$test_dir/near" > /dev/null 2>&1; then
  fail "--match with --near accepted"
fi

for mixed in 'a,b\nc\n' 'a\nb,c\n' '3 ab\n2 a b\n' '3 a b\na,b\n'; do
  printf "$mixed" > "$test_dir/mixed"
  if "$letter_bags" "$test_dir/mixed" > /dev/null 2>&1; then
    fail "mixed formats accepted: $mixed"
  fi
done

echo PASS
