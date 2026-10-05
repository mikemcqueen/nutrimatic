#!/usr/bin/env bash
set -euo pipefail

dump_index=$1
make_index=$2

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-dump-index.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT

"$make_index" "$test_dir/test.index"

check() {
  local expected=$1
  shift
  local actual
  actual=$("$dump_index" "$@" "$test_dir/test.index")
  [[ $actual == "$expected" ]] || {
    printf 'FAIL: %s printed:\n%s\n' "$*" "$actual" >&2
    exit 1
  }
}

check $'klmn\nab\nf' --top-words 3
check $' 1000 klmn\n   80 ab\n   11 f' --top-words 3 --score
