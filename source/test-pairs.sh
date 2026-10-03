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
  shift 5
  local output=$test_dir/$name.out

  "$pairs" "$@" -m 0 -d "$dictionary" "$letters" > "$output"

  local actual_count
  actual_count=$(wc -l < "$output")
  [[ $actual_count == "$expected_count" ]] ||
    fail "$name count is wrong: $actual_count"

  local actual_rows
  actual_rows=$(sort "$output")
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
ba,cd
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

exact=$test_dir/exact.txt
cat > "$exact" <<'EOF'
ab

1234
cd
c
abc
bca
EOF
check_pairs exact "$exact" abcd 1 'ab,cd' -x
check_pairs exact-same-group "$exact" aabbcc 1 'abc,bca' -x
check_pairs exact-solo "$exact" abc 3 'abc
bca
ab,c' -x --mx 1
check_pairs exact-solo-only "$exact" abc 2 'abc
bca' -x --min-max-words 1,1
check_pairs solo-fits "$exact" abc 4 'ab
c
abc
bca' --mx 1,1
if "$pairs" --mx 2,3 -d "$exact" abc 2>/dev/null; then
  fail "--mx 2,3 was accepted"
fi
