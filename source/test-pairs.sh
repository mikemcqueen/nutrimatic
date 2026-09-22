#!/usr/bin/env bash
set -euo pipefail

pairs=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pairs.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

check_pairs() {
  local name=$1
  local dictionary=$2
  local letters=$3
  local expected_count=$4
  local expected_rows=$5
  local output=$test_dir/$name.out

  "$pairs" -m 0 -d "$dictionary" "$letters" > "$output"

  local actual_count
  actual_count=$(tail -n 1 "$output")
  [[ $actual_count == "$expected_count" ]] ||
    fail "$name count is wrong: $actual_count"

  local actual_rows
  actual_rows=$(sed '$d' "$output" | sort)
  local sorted_expected
  sorted_expected=$(printf '%s\n' "$expected_rows" | sed '/^$/d' | sort)
  [[ $actual_rows == "$sorted_expected" ]] ||
    fail "$name rows are wrong: $actual_rows"
}

cross_groups=$test_dir/cross-groups.txt
cat > "$cross_groups" <<'EOF'
ab
cd
ba
dc
aa
EOF
check_pairs cross-groups "$cross_groups" abcd 4 'ab,cd
ab,dc
cd,ba
ba,dc'

same_group=$test_dir/same-group.txt
cat > "$same_group" <<'EOF'
abc
acb
bac
EOF
check_pairs same-group "$same_group" aabbcc 3 'abc,acb
abc,bac
acb,bac'
check_pairs same-group-once "$same_group" abc 0 ''

duplicate=$test_dir/duplicate.txt
cat > "$duplicate" <<'EOF'
ab
ab
cd
EOF
check_pairs duplicate "$duplicate" abcd 1 'ab,cd'
