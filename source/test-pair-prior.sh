#!/usr/bin/env bash
set -euo pipefail

pair_prior=$1
make_p1_index=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pair-prior.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# npy FILE VALUES...: a 1-D little-endian uint32 .npy file.
npy() {
  local file=$1
  shift
  local header="{'descr': '<u4', 'fortran_order': False, 'shape': ($#,), }"
  while (( (10 + ${#header} + 1) % 64 != 0 )); do header+=' '; done
  header+=$'\n'
  {
    printf '\x93NUMPY\x01\x00'
    printf "\\x$(printf %02x $(( ${#header} & 255 )))\\x$(printf %02x $(( ${#header} >> 8 )))"
    printf '%s' "$header"
    local value
    for value in "$@"; do
      printf "\\x$(printf %02x $(( value & 255 )))\\x$(printf %02x $(( (value >> 8) & 255 )))\\x00\\x00"
    done
  } > "$file"
}

mkdir "$test_dir/p1"
cat > "$test_dir/p1/a.jsonl" <<'JSON'
{"pair": "pound,writer", "seq": 0, "logprobs": {"fwd": [{"YES": 0.9}, {"NO": 0.1}], "rvs": [{"NO": 0.3}, {"YES": 0.7}]}}
{"pair": "author,abelard", "seq": 1, "logprobs": {"fwd": [{"NO": 0.5}, {"YES": 0.5}], "rvs": [{"YES": 0.6}, {"NO": 0.4}]}}
JSON
"$make_p1_index" --threads 1 "$test_dir/p1" "$test_dir/p1.idx" ||
  fail "make-p1-index failed"

# Two answers, POUND and ABELARD; clue "American writer" -> POUND, and
# "Nine-darts code" with no answer. Its index, as cluer's build_index.py
# would write it: words american, code, darts, nine, writer.
mkdir "$test_dir/data"
printf '\x02\x00\x00\x00\x05POUND\x07ABELARD\x02\x00\x00\x00'\
'\x0fAmerican writer\x01\x00\x00\x00\x00\x00\x00\x00'\
'\x0fNine-darts code\x00\x00\x00\x00' > "$test_dir/data/cluedata"
index=$test_dir/data/index
mkdir "$index"
printf '%s\n' american code darts nine writer > "$index/tokens.txt"
printf '%s\n' POUND ABELARD > "$index/answers.txt"
npy "$index/postings.npy" 0 1 1 1 0
npy "$index/postings_start.npy" 0 1 2 3 4 5
npy "$index/clue_off.npy" 22 46
npy "$index/ref_start.npy" 0 1 1
npy "$index/refs.npy" 0
size=$(stat -c %s "$test_dir/data/cluedata")
mtime=$(stat -c %.9Y "$test_dir/data/cluedata" | tr -d .)
printf '{"version": 3, "source_size": %s, "source_mtime_ns": %s}\n' \
  "$size" "$mtime" > "$index/metadata.json"

printf '%s\n' pound,writer nine,darts abelard,author writer,pound \
  > "$test_dir/pairs"

actual=$("$pair_prior" --p1 "$test_dir/p1.idx" \
  --clues "$test_dir/data/cluedata" "$test_dir/pairs")
expected='3.800008 pound,writer
3.000000 nine,darts
1.550004 abelard,author'
[[ $actual == "$expected" ]] || fail "default scores are wrong: $actual"

actual=$("$pair_prior" --p1 "$test_dir/p1.idx" \
  --clues "$test_dir/data/cluedata" --prior-weights min=1,link=2,exact=5 \
  "$test_dir/pairs")
expected='2.700008 pound,writer
0.000000 nine,darts
0.500008 abelard,author'
[[ $actual == "$expected" ]] || fail "weighted scores are wrong: $actual"

actual=$("$pair_prior" --clues "$test_dir/data/cluedata" --features \
  "$test_dir/pairs")
expected='-1.000000 -1.000000 0 0 0 0 0 1 0 2 0 0 1 pound,writer
-1.000000 -1.000000 1 1 1 0 3 0 0 0 0 1 1 nine,darts
-1.000000 -1.000000 0 0 0 0 0 0 0 0 0 0 0 abelard,author'
[[ $actual == "$expected" ]] || fail "features are wrong: $actual"

actual=$("$pair_prior" --clues "$test_dir/data/cluedata" "$test_dir/pairs")
expected='3.000000 pound,writer
3.000000 nine,darts
1.000000 abelard,author'
[[ $actual == "$expected" ]] || fail "clue-only scores are wrong: $actual"

if "$pair_prior" --clues "$test_dir/data/cluedata" --prior-weights mean=1 \
    "$test_dir/pairs" 2> /dev/null; then
  fail "a p1 term ran without --p1"
fi
touch -d '2001-01-01' "$test_dir/data/cluedata"
if "$pair_prior" --clues "$test_dir/data/cluedata" --features \
    "$test_dir/pairs" 2> /dev/null; then
  fail "a stale clue index was read"
fi

echo PASS
