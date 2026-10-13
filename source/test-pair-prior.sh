#!/usr/bin/env bash
set -euo pipefail

pair_prior=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pair-prior.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir "$test_dir/p1"
cat > "$test_dir/p1/a.jsonl" <<'JSON'
{"pair": "pound,writer", "seq": 0, "logprobs": {"fwd": [{"YES": 0.9}, {"NO": 0.1}], "rvs": [{"NO": 0.3}, {"YES": 0.7}]}}
{"pair": "author,abelard", "seq": 1, "logprobs": {"fwd": [{"NO": 0.5}, {"YES": 0.5}], "rvs": [{"YES": 0.6}, {"NO": 0.4}]}}
JSON

# Two answers, POUND and ABELARD; clue "American writer" -> POUND, and
# "Nine-darts code" with no answer.
printf '\x02\x00\x00\x00\x05POUND\x07ABELARD\x02\x00\x00\x00'\
'\x0fAmerican writer\x01\x00\x00\x00\x00\x00\x00\x00'\
'\x0fNine-darts code\x00\x00\x00\x00' > "$test_dir/cluedata"

printf '%s\n' pound,writer nine,darts abelard,author writer,pound \
  > "$test_dir/pairs"

actual=$("$pair_prior" --p1 "$test_dir/p1" --clues "$test_dir/cluedata" \
  "$test_dir/pairs")
expected='1.700000 pound,writer
1.000000 nine,darts
0.500000 abelard,author'
[[ $actual == "$expected" ]] || fail "scores are wrong: $actual"

echo PASS
