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

header="bag-stats 2
letters aaacdglnnootttw
min 3
min-wb 5
pairs p
dict d
columns PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,MIN_WB,MIN_WB_WITH_PAIRS
"
cat > "$test_dir/stats" <<END
$header
cat dog,acdgot,2,3,2,9,6,1,0,0
low ant,alnotw,1,1,1,4,4,0,0,0
tan owl,alnotw,1,5,1,9,9,0,0,0
END

actual=$("$bag_stats_sort" --tp "$test_dir/stats")
expected="$header
cat dog,acdgot,2,3,2,9,6,1,0,0
tan owl,alnotw,1,5,1,9,9,0,0,0
low ant,alnotw,1,1,1,4,4,0,0,0"
[[ $actual == "$expected" ]] || fail "--tp rows are wrong: $actual"

actual=$("$bag_stats_sort" --wb < "$test_dir/stats" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'tan owl\ncat dog\nlow ant' ]] || fail "--wb order is wrong: $actual"

cat > "$test_dir/min-wb" <<END
$header
aa bb,aabb,1,0,0,0,0,0,8,2
eee fff,eeefff,1,0,0,0,0,0,4,2
cc dd,ccdd,1,0,0,0,0,0,4,3
END
actual=$("$bag_stats_sort" --min-wb "$test_dir/min-wb" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'aa bb\neee fff\ncc dd' ]] || fail "--min-wb order is wrong: $actual"

actual=$("$bag_stats_sort" --min-wb-frac "$test_dir/min-wb" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'cc dd\neee fff\naa bb' ]] || fail "--min-wb-frac order is wrong: $actual"

sed -i 's/^eee fff,eeefff,1,0,0,0,0,0,4,2$/eee fff,eeefff,1,0,0,0,0,0,4,0/' "$test_dir/min-wb"
actual=$("$bag_stats_sort" --min-wb-frac --by-length "$test_dir/min-wb" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'cc dd\neee fff\naa bb' ]] || fail "--by-length order is wrong: $actual"

actual=$("$bag_stats_sort" --min-wb-frac --pb-len 4 "$test_dir/min-wb" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'cc dd\naa bb' ]] || fail "--pb-len rows are wrong: $actual"

cat > "$test_dir/merge" <<END
$header
z z,zz,1,0,0,0,5,0,2,1
x x,xx,1,0,0,0,1,0,2,2
y y,yy,1,0,0,0,9,0,2,0
END
actual=$("$bag_stats_sort" --min-wb-frac --mp "$test_dir/merge" | sed '1,/^$/d' | cut -d, -f1)
[[ $actual == $'x x\ny y\nz z' ]] || fail "merged order is wrong: $actual"

echo PASS
