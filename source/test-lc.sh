#!/usr/bin/env bash
set -euo pipefail

lc=$1

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ $("$lc" -r "Hello, World" low) == dehllor ]] || fail "-r output is wrong"
[[ $("$lc" --cv "Hello, World" low) == 2.50 ]] || fail "--cv output is wrong"
[[ $("$lc" --cv "rhythm") == inf ]] || fail "--cv without vowels is wrong"

table=$("$lc" "Hello, World" low)
grep -qx "remain: 7, dehllor" <<< "$table" || fail "remain line is missing"
grep -qx "removed: 1, w" <<< "$table" || fail "removed line is missing"
grep -q "^'h'  1    6.09     14.29    2.34 +++$" <<< "$table" ||
  fail "h row is wrong"

if "$lc" "abc" xyz 2> /dev/null; then fail "missing letters accepted"; fi
