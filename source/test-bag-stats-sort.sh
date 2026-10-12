#!/usr/bin/env bash
set -euo pipefail

bag_stats_sort=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-bag-stats-sort.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

header="bag-stats 1
letters aaacdglnnootttw
min 3
pairs p
dict d
columns PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO
"
cat > "$test_dir/stats" <<END
$header
cat dog,acdgot,2,3,2,9,6,1
low ant,alnotw,1,1,1,4,4,0
tan owl,alnotw,1,5,1,9,9,0
END

actual=$("$bag_stats_sort" --tp "$test_dir/stats")
expected="$header
cat dog,acdgot,2,3,2,9,6,1
tan owl,alnotw,1,5,1,9,9,0
low ant,alnotw,1,1,1,4,4,0"
[[ $actual == "$expected" ]] || fail "--tp rows are wrong: $actual"

actual=$("$bag_stats_sort" --wb < "$test_dir/stats" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'tan owl\ncat dog\nlow ant' ]] || fail "--wb order is wrong: $actual"

if "$bag_stats_sort" --wb --mp "$test_dir/stats" 2>/dev/null; then
  fail "two sort options were accepted"
fi

echo PASS
