#!/usr/bin/env bash
set -euo pipefail

explore_index=$1
make_index=$2

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-explore-index.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT

"$make_index" "$test_dir/test.index"

check() {
  local expected=$1
  shift
  local actual
  actual=$("$explore_index" "$@")
  [[ $actual == "$expected" ]] || {
    printf 'FAIL: %s printed:\n%s\n' "$*" "$actual" >&2
    exit 1
  }
}

check '   70 ab cd' --next-words 1 "$test_dir/test.index" ab
check $'    1 f gh' --next-words 1 "$test_dir/test.index" 'f '
check $'    1 f gh ij' --next-words 2 "$test_dir/test.index" f
check $' 1000 klmn\n   80 ab' --next-words 1 --top 2 "$test_dir/test.index" ''
check '' --next-words 1 "$test_dir/test.index" zz
