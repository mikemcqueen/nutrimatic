#!/usr/bin/env bash
set -euo pipefail

seed_pairs=$(realpath "$1")
make_p1_index=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-seed-pairs.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

mkdir "$test_dir/idx"
cat > "$test_dir/idx/a.jsonl" <<'JSON'
{"pair": "pound,writer", "seq": 0, "logprobs": {"fwd": [{"YES": 0.9}, {"NO": 0.1}], "rvs": [{"NO": 0.3}, {"YES": 0.7}]}}
{"pair": "author,abelard", "seq": 1, "logprobs": {"fwd": [{"NO": 0.5}, {"YES": 0.5}], "rvs": [{"YES": 0.6}, {"NO": 0.4}]}}
{"pair": "cat,dog", "seq": 2, "logprobs": {"fwd": [{"YES": 0.2}, {"NO": 0.8}], "rvs": [{"NO": 0.9}, {"YES": 0.1}]}}
JSON
"$make_p1_index" --threads 1 -d "$test_dir/idx" -o "$test_dir/p1.idx" ||
  fail "make-p1-index failed"

printf '%s\n' pound,writer author,abelard cat,dog ant,bee > "$test_dir/pairs"
printf '%s\n' pound writer author cat dog ant bee > "$test_dir/dict"

# check EXPECTED ARGS...: seed-pairs ARGS prints EXPECTED.
check() {
  local expected=$1
  shift
  local actual
  actual=$("$seed_pairs" "$@" --pairs "$test_dir/pairs") ||
    fail "seed-pairs $* failed"
  [[ $actual == "$expected" ]] || fail "seed-pairs $*: $actual"
}

p1=(--p1 "$test_dir/p1.idx")
check $'pound,writer\nabelard,author\ncat,dog' "${p1[@]}" --pm 0
check 'pound,writer' "${p1[@]}"
check $'pound,writer\nabelard,author' "${p1[@]}" --prob-min .5
check $'pound,writer\nabelard,author' "${p1[@]}" --pm 0 -m 4
check $'pound,writer\ncat,dog' "${p1[@]}" --pm 0 -x 6
check $'pound,writer\ncat,dog' "${p1[@]}" --pm 0 -d "$test_dir/dict"

if (cd "$test_dir" && "$seed_pairs" --pm 0 --pairs pairs 2> /dev/null); then
  fail "ran without --p1 or idx/p1.idx"
fi
mv "$test_dir/p1.idx" "$test_dir/idx/p1.idx"
cd "$test_dir"
check 'pound,writer' --pm .85
cp pairs idx/pairs.s3
[[ $("$seed_pairs" -s3) == pound,writer ]] ||
  fail "-s3 did not read idx/pairs.s3"

printf '%s\n' dog,cat elk,fox fox,elk > idx/s3.pairs
[[ $("$seed_pairs" -s3) == $'pound,writer\ncat,dog\nelk,fox' ]] ||
  fail "-s3 did not force in idx/s3.pairs"
forced=$("$seed_pairs" -s3 -d dict 2> "$test_dir/stderr") ||
  fail "seed-pairs -s3 -d dict failed"
[[ $forced == $'pound,writer\ncat,dog' ]] ||
  fail "-s3 -d dict kept a forced pair outside dict: $forced"
[[ $(grep -c 'forced pair word "\(elk\|fox\)"' "$test_dir/stderr") == 2 ]] ||
  fail "-s3 -d dict did not warn about elk and fox"
printf '%s\n' cat,dog > other
[[ $("$seed_pairs" -s3 --pairs other) == $'cat,dog\nelk,fox' ]] ||
  fail "-s3 --pairs other did not read other and force in idx/s3.pairs"
rm idx/s3.pairs

echo '{"pair": "ant,bee", "seq": 3}' >> idx/a.jsonl
stale=$("$seed_pairs" -s3 2>&1 > /dev/null) ||
  fail "a stale index failed the run"
[[ $stale == *"is out of date"* ]] || fail "a stale index did not warn"

echo PASS
