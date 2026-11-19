#!/usr/bin/env bash
set -euo pipefail

pair_answers=$(realpath "$1")
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pair-answers.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
cd "$test_dir"

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

# Answers DEN, MARK and NOPE; clue "New York" -> DEN, and
# "Big new york city" -> MARK, NOPE.
mkdir data
printf '\x03\x00\x00\x00\x03DEN\x04MARK\x04NOPE\x02\x00\x00\x00'\
'\x08New York\x01\x00\x00\x00\x00\x00\x00\x00'\
'\x11Big new york city\x02\x00\x00\x00\x02\x00\x00\x00\x04\x00\x00\x00' \
  > data/cluedata
mkdir data/index
printf '%s\n' big city new york > data/index/tokens.txt
printf '%s\n' DEN MARK NOPE > data/index/answers.txt
npy data/index/postings.npy 1 1 0 1 0 1
npy data/index/postings_start.npy 0 1 2 4 6
npy data/index/clue_off.npy 22 39
npy data/index/ref_start.npy 0 1 3
npy data/index/refs.npy 0 2 4
printf '{"version": 4, "source_size": %s, "source_mtime_ns": %s}\n' \
  "$(stat -c %s data/cluedata)" "$(date -r data/cluedata +%s%N)" \
  > data/index/metadata.json

printf '%s\n' den mark denmark markup up > dict
printf '%s\n' new,york "York,  New" "new york,city" big,york york\ new,city > pairs

actual=$("$pair_answers" -e -d dict --clues data/cluedata pairs)
expected='new,york
DEN: MP

york,new
DEN: MP'
[[ $actual == "$expected" ]] || fail "-e output is wrong: $actual"

actual=$(DICT=dict "$pair_answers" -j --clues data/cluedata pairs)
expected='new,york
DEN: MP
MARK: MPS
NOPE: X

york,new
DEN: MP
MARK: MPS
NOPE: X

new york,city
MARK: MPS
NOPE: X'
[[ $actual == "$expected" ]] || fail "-j output is wrong: $actual"

actual=$("$pair_answers" -d dict --clues data/cluedata pairs)
expected='new,york
DEN: MP
MARK: MPS
NOPE: X

york,new
DEN: MP
MARK: MPS
NOPE: X

new york,city
MARK: MPS
NOPE: X

big,york
MARK: MPS
NOPE: X

york new,city
MARK: MPS
NOPE: X'
[[ $actual == "$expected" ]] || fail "default output is wrong: $actual"

actual=$(env -u DICT "$pair_answers" -e -p --clues data/cluedata pairs)
expected='new,york
york,new'
[[ $actual == "$expected" ]] || fail "-e -p output is wrong: $actual"

: > empty
if "$pair_answers" -e -d empty --clues data/cluedata pairs 2>/dev/null; then
  fail "an empty DICT should fail"
fi
if "$pair_answers" -e -d missing --clues data/cluedata pairs 2>/dev/null; then
  fail "a missing DICT should fail"
fi
