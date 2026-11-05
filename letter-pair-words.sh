#!/bin/bash
# usage: letter-pair-words.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
#        letter-pair-words.sh -s N [--pm P] -l BAG -u USED -U NEW_USED
#          LETTERS...
# requires: source ./setup.sh
#
# For each letter, lists the words of seed pairs that fit NEW-BAG but not
# OLD-BAG and hold the letter in both words, less words with a workflow
# verdict (see filter-words). With -l, OLD-BAG is BAG less USED and NEW-BAG
# is BAG less NEW_USED; a USED or NEW_USED that doesn't fit BAG, or uses all
# of it, is an error. LETTERS may be given as one argument
# ("hwdy", "h w d y") or several (h w d y). With more than one letter, each
# letter's words follow a "== L ==" header. --pm P is seed-pairs' p1 YES
# band, [P, 1] (default: seed-pairs' own).

set -euo pipefail

usage() {
  local name
  name=$(basename "$0")
  cat >&2 <<EOF
usage: $name -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
       $name -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS...
  -s N         sentence number (required)
  --pm P       seed-pairs' p1 YES band [P, 1] (default: seed-pairs', .85)
  -l BAG       letter bag
  -u USED      letters used from BAG for OLD-BAG
  -U NEW_USED  letters used from BAG for NEW-BAG
EOF
  exit 2
}

sentence=
seed_args=()
unset bag used new_used
while [[ ${1-} == -* ]]; do
  [[ $# -ge 2 ]] || usage
  case $1 in
    -s) sentence=$2 ;;
    --pm) seed_args+=(--pm "$2") ;;
    -l) bag=$2 ;;
    -u) used=$2 ;;
    -U) new_used=$2 ;;
    *) usage ;;
  esac
  shift 2
done
[[ -n $sentence ]] || usage
if [[ -n ${bag+x} ]]; then
  [[ -n ${used+x} && -n ${new_used+x} && $# -ge 1 ]] || usage
  old_bag=$bag
  new_bag=$bag
else
  [[ -z ${used+x} && -z ${new_used+x} && $# -ge 3 ]] || usage
  old_bag=$1
  new_bag=$2
  used=
  new_used=
  shift 2
fi
letters=$(echo "$*" | tr -d ' ' | tr '[:upper:]' '[:lower:]')
[[ $letters =~ ^[a-z]+$ ]] || usage

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pairs() {
  seed-pairs -s "$sentence" "${seed_args[@]}" |
    pfilter -s "$sentence" -d "$DICT" -m 0 -x 0 -l "$1" -u "$2" -
}

words() {
  grep "$1.*,.*$1" "$2" | tr , '\n' | sort -u || true
}

pairs "$old_bag" "$used" > "$tmp/old"
pairs "$new_bag" "$new_used" > "$tmp/new"

for ((i = 0; i < ${#letters}; i++)); do
  l=${letters:i:1}
  [[ ${#letters} -eq 1 ]] || echo "== $l =="
  comm -13 <(words "$l" "$tmp/old") <(words "$l" "$tmp/new") |
    filter-words -m 0 -
done
