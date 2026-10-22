#!/bin/bash
# usage: rebuild.sh [-f|--force] [universe|wiki|cluer|combine|all]... sN|SN
# requires: source ./setup.sh
#
# rebuild.mk as a script: the same targets, files and commands. A target
# builds the targets it depends on first; the pair files depend on universe
# order-only, so universe is never a reason to rebuild one. -f rebuilds
# every named target and what it depends on, as make -B does.

# braces make bash parse the whole script before running it, so editing the
# file during a run doesn't change what the run does
{
set -euo pipefail

die() {
    echo "rebuild: $*" >&2
    exit 1
}

usage() {
    die "usage: $0 [-f|--force] [universe|wiki|cluer|combine|all]... sN|SN (N is 1-9)"
}

force=
seed=
targets=()
for arg; do
    case $arg in
        -f|--force) force=1 ;;
        -*) usage ;;
        universe|wiki|cluer|combine|all) targets+=("$arg") ;;
        *) [ -z "$seed" ] || usage; seed=$arg ;;
    esac
done
[[ $seed =~ ^[sS]([1-9])$ ]] || usage
snum=${BASH_REMATCH[1]}
[ ${#targets[@]} -gt 0 ] || targets=(all)
S=S${snum}
letters=${!S:-}
[ -n "$letters" ] || die "\$$S is unset or empty; source ./setup.sh"
[ -n "${IDX_DIR:-}" ] || die "\$IDX_DIR is unset; source ./setup.sh"
[ -d "$IDX_DIR" ] || die "IDX_DIR $IDX_DIR is not a directory"
[ -n "${BIG:-}" ] || die "\$BIG is unset; source ./setup.sh"
[ -n "${DICT:-}" ] || die "\$DICT is unset; source ./setup.sh"

here=$(cd "$(dirname "$0")" && pwd)
wf=$here/wf
cluer=$(dirname "$(readlink -f "$wf")")/cluer
tokens=$here/cluer/data/index/tokens.txt
words=$IDX_DIR/words
universe=$IDX_DIR/universe
idx_out=$IDX_DIR/idx.2.s${snum}
cluer_out=$IDX_DIR/cluer.s${snum}
pairs_out=$IDX_DIR/pairs.s${snum}
extra_pairs=$IDX_DIR/s${snum}.pairs

for tool in query-index pairs pcomm python; do
    command -v "$tool" > /dev/null || die "$tool not found on PATH; source ./setup.sh"
done
[ -x "$wf" ] || die "$wf not found"
[ -f "$cluer/query_index.py" ] || die "$cluer/query_index.py not found"
for file in "$tokens" "$words" "$BIG" "$DICT"; do
    [ -f "$file" ] || die "$file not found"
done

msg() {
    echo "$*" >&2
}

# true if $1 exists, is nonempty, and is no older than any of the remaining
# files
up_to_date() {
    local out=$1 src
    shift
    [ -s "$out" ] || return 1
    for src; do
        if [ "$src" -nt "$out" ]; then
            return 1
        fi
    done
}

# usage: run_to WHAT OUT [SRC...] -- CMD...
# skip if OUT is up to date with the SRC files and --force is not set;
# otherwise print WHAT and write the stdout of CMD to OUT, only if CMD
# succeeds; set slow=1 to warn that CMD takes a while
run_to() {
    local what=$1 out=$2
    local srcs=()
    shift 2
    while [ $# -gt 0 ] && [ "$1" != -- ]; do
        srcs+=("$1")
        shift
    done
    [ $# -gt 1 ] || die "usage: run_to WHAT OUT [SRC...] -- CMD..."
    shift
    if [ -z "$force" ] && up_to_date "$out" ${srcs[@]+"${srcs[@]}"}; then
        msg "Skipping $what, $(basename "$out") is up to date"
        return
    fi
    msg "Generating $what${slow:+, this might take a while...}"
    "$@" > "$out.tmp" || { rm -f "$out.tmp"; die "failed writing $out"; }
    mv "$out.tmp" "$out"
}

universe_words() {
    LC_ALL=C sort -u "$words" "$BIG" "$DICT" |
        LC_ALL=C awk 'length >= 3 && length <= 13'
}

idx_pairs() {
    query-index "$letters" --wf -w2 -n0 --csv -m3 --dict "$universe"
}

# cluer pairs, filtering out non-cluer-index dict words before sending to pairs
cluer_pairs() {
    LC_ALL=C comm -12 "$tokens" "$universe" |
        pairs "$letters" -d - -m3 |
        python "$cluer/query_index.py" -f - -j --threads 2
}

# adds $extra_pairs when it exists
all_pairs() {
    if [ -f "$extra_pairs" ]; then
        pcomm "$cluer_out" "$idx_out" | pcomm - "$extra_pairs"
    else
        pcomm "$cluer_out" "$idx_out"
    fi | sort -u
}

declare -A built=()

# builds target $1 once, after the targets it depends on
build() {
    [ -z "${built[$1]:-}" ] || return 0
    built[$1]=1
    case $1 in
        universe)
            run_to universe "$universe" "$words" "$BIG" "$DICT" -- \
                universe_words
            ;;
        wiki)
            build universe
            run_to "wiki-index pairs" "$idx_out" -- idx_pairs
            ;;
        cluer)
            build universe
            slow=1 run_to "cluer pairs" "$cluer_out" "$tokens" -- cluer_pairs
            ;;
        combine)
            build wiki
            build cluer
            local srcs=("$cluer_out" "$idx_out")
            [ ! -f "$extra_pairs" ] || srcs+=("$extra_pairs")
            run_to "combined pairs" "$pairs_out" "${srcs[@]}" -- all_pairs
            ;;
        all)
            build combine
            ;;
    esac
}

for target in "${targets[@]}"; do
    build "$target"
done

exit
}
