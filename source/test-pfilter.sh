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

actual=$(printf '  7 cat dog\n3 act god\n' |
  "$pfilter" --no-dict -m 0 --ranked - 2>/dev/null)
[[ $actual == '  7 cat dog' ]] || fail "--ranked is wrong: $actual"

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

printf 'cat,dog\nlow,ant\n' > "$test_dir/new-pairs"
actual=$("$pfilter" --no-dict -m 0 --no-nopairs -l catdoglowantx -u lowant \
  -U x "$test_dir/new-pairs")
[[ $actual == 'low,ant' ]] || fail "-U keeps a pair that fits -u: $actual"

mkdir -p "$WFROOT/.wf/classified/yes"
echo dog,cat > "$WFROOT/.wf/classified/yes/yes.pairs"
actual=$("$pfilter" --no-dict -m 0 --no-nopairs -l catdoglowantx -u lowant \
  -U x --keep-yes "$test_dir/new-pairs" 2>/dev/null)
[[ $actual == $'cat,dog\nlow,ant' ]] || fail "--keep-yes is wrong: $actual"

actual=$("$pfilter" --no-dict -m 0 --no-nopairs -l catdoglowantxy -u x -U xy \
  "$test_dir/new-pairs" 2>"$test_dir/err")
[[ -z $actual ]] || fail "-U inside -u prints pairs: $actual"
grep -q 'no pair can be new' "$test_dir/err" || fail "-U inside -u: no warning"

"$pfilter" --no-dict -l catdogx -U x --keep-yes -y "$test_dir/pairs" \
  2>/dev/null && fail "--keep-yes accepts -y"
"$pfilter" --no-dict -U x "$test_dir/pairs" 2>/dev/null &&
  fail "-U accepts no -l"

echo PASS
