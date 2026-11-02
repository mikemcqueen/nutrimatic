#!/usr/bin/env bash
set -euo pipefail

sampler=$(realpath "$1")
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-anagram-sampler.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT
cd "$test_dir"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

cat > prior <<'ROWS'
6.5 salt,pepper
3.0 pepper,tar
2.0 harbor,harbor
1.0 zebra,quartz
ROWS
cat > solo <<'ROWS'
1000 harbor
500 arbor
40 slat
ROWS
printf 'salt pepper\nharbor,salt\nslat\n' > watch

run() {
  "$sampler" --pairs prior --solo solo --threads 2 --burn 100 \
    --moves 1000 --progress 0 "$@" 2>stderr
}

run saltpepperharbor > out || fail "sampler failed: $(cat stderr)"
printf '1.000000 salt,pepper\n1.000000 harbor\n' | cmp - out ||
  fail "unexpected shares: $(cat out)"
grep -q '^anagram-sampler: 2000 draws' stderr || fail "draws: $(cat stderr)"

run --watch watch saltpepperharbor > out || fail "watch failed"
sed -n 's/^anagram-sampler: watch //p' stderr > watched
printf '1.000000 salt pepper\n1.000000 harbor,salt\n0.000000 slat\n' |
  cmp - watched || fail "unexpected watch shares: $(cat watched)"

if run saltpepperharbo > out; then fail "accepted a bag with no anagram"; fi
grep -q 'no full anagram' stderr || fail "no-anagram message: $(cat stderr)"
echo ok
