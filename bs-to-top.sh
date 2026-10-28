#!/usr/bin/env bash
# Converts a bag-stats dump into top-segments input: "0 PAIR,PAIR" per row.
set -euo pipefail

usage() {
  echo "usage: ${0##*/} [-o FILE] INPUT" >&2
  echo "  write to FILE, or to INPUT.top when -o isn't given" >&2
  exit 2
}

output=
while getopts o:h opt; do
  case $opt in
    o) output=$OPTARG ;;
    *) usage ;;
  esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || usage
input=$1
output=${output:-$input.top}

if ! head -n 1 "$input" | grep -q '^bag-stats '; then
  echo "${0##*/}: $input is not a bag-stats dump" >&2
  exit 1
fi

tmp=$(mktemp "$output.XXXXXX")
trap 'rm -f "$tmp"' EXIT
awk -F, 'body {p=$1; gsub(/ \+ /, ",", p); print "0 " p; next} /^$/ {body=1}' \
    "$input" > "$tmp"
mv "$tmp" "$output"
