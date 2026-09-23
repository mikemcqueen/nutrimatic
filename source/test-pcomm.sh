#!/usr/bin/env bash
set -euo pipefail

pcomm=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-pcomm.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

first=$test_dir/first
second=$test_dir/second
printf '%s\n' b,a x,y a,b x,y c,d > "$first"
printf '%s\n' q,r d,c q,r a,b d,c r,q c,d > "$second"

expected=$(printf '%s\n' b,a x,y x,y c,d q,r q,r r,q)
expected_23=$(printf '%s\n' x,y x,y)

for mode in files stdin-first stdin-second pipe-first pipe-second; do
  for flags in "" -23; do
    case $mode in
      files) actual=$("$pcomm" $flags "$first" "$second") ;;
      stdin-first) actual=$("$pcomm" $flags - "$second" < "$first") ;;
      stdin-second) actual=$("$pcomm" $flags "$first" - < "$second") ;;
      pipe-first) actual=$("$pcomm" $flags <(cat "$first") "$second") ;;
      pipe-second) actual=$("$pcomm" $flags "$first" <(cat "$second")) ;;
    esac
    want=$expected
    [[ -n $flags ]] && want=$expected_23
    [[ $actual == "$want" ]] ||
      fail "$mode $flags output is wrong: $actual"
  done
done
