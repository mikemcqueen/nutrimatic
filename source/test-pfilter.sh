#!/usr/bin/env bash
set -euo pipefail

pfilter=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pfilter.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

export WFROOT=$test_dir/root
mkdir -p "$WFROOT/.wf/classified/no"
echo act,god > "$WFROOT/.wf/classified/no/no.pairs"
cat > "$test_dir/pairs" <<'PAIRS'
cat,dog
act,god
low,ant
PAIRS

actual=$("$pfilter" --no-dict -m 0 -l catdogx "$test_dir/pairs" 2>/dev/null)
[[ $actual == 'cat,dog' ]] || fail "no.pairs aren't dropped: $actual"

actual=$("$pfilter" --no-dict -m 0 --no-nopairs -l catdogx "$test_dir/pairs")
[[ $actual == $'cat,dog\nact,god' ]] || fail "--no-nopairs is wrong: $actual"

actual=$("$pfilter" --no-dict -m 0 --no-nopairs --exact -l catdogx \
  "$test_dir/pairs")
[[ -z $actual ]] || fail "--exact keeps a pair with letters left: $actual"

actual=$("$pfilter" --no-dict -m 0 --no-nopairs --exact -l catdog \
  "$test_dir/pairs")
[[ $actual == $'cat,dog\nact,god' ]] || fail "--exact is wrong: $actual"

actual=$("$pfilter" --no-dict --no-nopairs -m 7 -l catdogx "$test_dir/pairs")
[[ -z $actual ]] || fail "-m keeps a short pair: $actual"

printf 'Dog\nlow\n' > "$test_dir/required"
actual=$("$pfilter" --no-dict -m 0 --no-nopairs --rw "$test_dir/required" \
  "$test_dir/pairs")
[[ $actual == $'cat,dog\nlow,ant' ]] || fail "--rw is wrong: $actual"

printf 'a-b\n!!\n' > "$test_dir/required"
"$pfilter" --no-dict --no-nopairs --rw "$test_dir/required" \
  "$test_dir/pairs" 2>/dev/null && fail "--rw accepts a file with no words"

echo PASS
