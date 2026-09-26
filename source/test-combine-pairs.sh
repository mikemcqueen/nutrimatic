#!/usr/bin/env bash
set -euo pipefail

combine_pairs=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-combine-pairs.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

printf '%s\n' red,fox fox,red red,hat fox,hat ox zzz > "$test_dir/input"

actual=$("$combine_pairs" -m 1 redfoxredhat "$test_dir/input")
expected='red fox,red hat
red hat,ox      defr
fox hat         ddeerr'
[[ $actual == "$expected" ]] || fail "output is wrong: $actual"

actual=$("$combine_pairs" redfoxredhat "$test_dir/input")
expected='red fox,red hat
red hat,ox      defr'
[[ $actual == "$expected" ]] || fail "default output is wrong: $actual"

mkdir -p "$test_dir/.wf/classified/s3/yes"
cp "$test_dir/input" "$test_dir/.wf/classified/s3/yes/yes.pairs"
actual=$(WFROOT=$test_dir "$combine_pairs" -s 3 redfoxredhat)
[[ $actual == "$expected" ]] || fail "sentence output is wrong: $actual"

actual=$("$combine_pairs" -m 1 --cv redfoxredhat "$test_dir/input")
expected='red fox,red hat        0.00
fox hat         ddeerr 2.00
red hat,ox      defr   3.00'
[[ $actual == "$expected" ]] || fail "cv output is wrong: $actual"
