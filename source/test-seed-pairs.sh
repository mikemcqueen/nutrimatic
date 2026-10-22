#!/usr/bin/env bash
set -euo pipefail

seed_pairs=$1
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

mkdir "$test_dir/p1"
cat > "$test_dir/p1/a.jsonl" <<'JSON'
{"pair": "pound,writer", "seq": 0, "logprobs": {"fwd": [{"YES": 0.9}, {"NO": 0.1}], "rvs": [{"NO": 0.3}, {"YES": 0.7}]}}
{"pair": "author,abelard", "seq": 1, "logprobs": {"fwd": [{"NO": 0.5}, {"YES": 0.5}], "rvs": [{"YES": 0.6}, {"NO": 0.4}]}}
{"pair": "cat,dog", "seq": 2, "logprobs": {"fwd": [{"YES": 0.2}, {"NO": 0.8}], "rvs": [{"NO": 0.9}, {"YES": 0.1}]}}
JSON
"$make_p1_index" --threads 1 -d "$test_dir/p1" -o "$test_dir/p1.idx" ||
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

mv "$test_dir/p1.idx" "$test_dir/p1/p1.idx"
IDX_DIR=$test_dir/p1 check 'pound,writer' --pm .85
if IDX_DIR= "$seed_pairs" --pm 0 --pairs "$test_dir/pairs" 2> /dev/null; then
  fail "ran without --p1 or IDX_DIR"
fi
cp "$test_dir/pairs" "$test_dir/p1/pairs.s3"
[[ $(IDX_DIR=$test_dir/p1 "$seed_pairs" -s3) == pound,writer ]] ||
  fail "-s3 did not read \$IDX_DIR/pairs.s3"
if IDX_DIR=$test_dir/p1 "$seed_pairs" -s3 --pairs "$test_dir/pairs" \
    2> /dev/null; then
  fail "ran with both -s and --pairs"
fi

echo PASS
