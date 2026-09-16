#!/usr/bin/env bash
set -euo pipefail

find_dupes=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-find-dupes.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pairs=$test_dir/best.pairs
cat > "$pairs" <<'EOF'
aged,well
anti,hero
well,aged
death,wish
anti,hero
hero,anti
wish,death
hero,anti
solo
EOF

expected='well,aged
hero,anti
wish,death'
actual=$("$find_dupes" "$pairs")
[[ $actual == "$expected" ]] || fail "duplicate report is wrong: $actual"

clean=$test_dir/clean.pairs
cat > "$clean" <<'EOF'
aged,well
anti,hero
EOF

actual=$("$find_dupes" "$clean")
[[ -z $actual ]] || fail "clean file produced output: $actual"

"$find_dupes" "$test_dir/missing.pairs" 2>/dev/null &&
  fail "missing file did not fail"

"$find_dupes" 2>/dev/null && fail "missing argument did not fail"

"$find_dupes" -h > /dev/null || fail "-h did not succeed"
