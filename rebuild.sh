#!/bin/bash
# usage: rebuild.sh [-f|--force] [universe|wiki|cluer|combine|subsets|all]...
#        sN|SN|sNsM
# requires: source ./setup.sh
#
# rebuild.mk as a script: the same targets, files and commands. A target
# builds the targets it depends on first; the pair files depend on universe
# order-only, so universe is never a reason to rebuild one. -f rebuilds
# every named target and what it depends on, as make -B does. subsets
# writes pairs.sN.NAME for each word list words.sN.NAME, keeping the pairs of
# pairs.sN whose words are both in it. sNsM, N less than M, is a combined
# sentence: the letters of $SN and $SM, written to idx.2.sNsM, cluer.sNsM and
# pairs.sNsM, with the hand-added pairs of sN.pairs and sM.pairs.

# braces make bash parse the whole script before running it, so editing the
# file during a run doesn't change what the run does
{
set -euo pipefail

die() {
    echo "rebuild: $*" >&2
    exit 1
}

usage() {
    die "usage: $0 [-f|--force] [universe|wiki|cluer|combine|subsets|all]... sN|SN|sNsM (N and M are 1-9, N less than M)"
}

force=
seed=
targets=()
for arg; do
    case $arg in
        -f|--force) force=1 ;;
        -*) usage ;;
        universe|wiki|cluer|combine|subsets|all) targets+=("$arg") ;;
        *) [ -z "$seed" ] || usage; seed=$arg ;;
    esac
done
if [[ $seed =~ ^[sS]([1-9])$ ]]; then
    nums=("${BASH_REMATCH[1]}")
elif [[ $seed =~ ^[sS]([1-9])[sS]([1-9])$ ]]; then
    nums=("${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}")
    [ "${nums[0]}" -lt "${nums[1]}" ] || usage
else
    usage
fi
[ ${#targets[@]} -gt 0 ] || targets=(all)
id=
letters=
for num in "${nums[@]}"; do
    S=S$num
    [ -n "${!S:-}" ] || die "\$$S is unset or empty; source ./setup.sh"
    id+=s$num
    letters+=${!S}
done
[ -n "${IDX_DIR:-}" ] || die "\$IDX_DIR is unset; source ./setup.sh"
[ -d "$IDX_DIR" ] || die "IDX_DIR $IDX_DIR is not a directory"
[ -n "${BIG:-}" ] || die "\$BIG is unset; source ./setup.sh"
[ -n "${DICT:-}" ] || die "\$DICT is unset; source ./setup.sh"

here=$(cd "$(dirname "$0")" && pwd)
wf=$here/wf
cluer=$(dirname "$(readlink -f "$wf")")/cluer
tokens=$here/cluer/data/index/tokens.txt
bigrams=$here/cluer/data/index/bigrams.npy
words=$IDX_DIR/words
universe=$IDX_DIR/universe
idx_out=$IDX_DIR/idx.2.$id
cluer_out=$IDX_DIR/cluer.$id
pairs_out=$IDX_DIR/pairs.$id
extra_pairs=()
for num in "${nums[@]}"; do
    [ ! -f "$IDX_DIR/s$num.pairs" ] || extra_pairs+=("$IDX_DIR/s$num.pairs")
done

for tool in query-index pcomm python; do
    command -v "$tool" > /dev/null || die "$tool not found on PATH; source ./setup.sh"
done
[ -x "$wf" ] || die "$wf not found"
[ -f "$cluer/query_index.py" ] || die "$cluer/query_index.py not found"
for file in "$tokens" "$bigrams" "$words" "$BIG" "$DICT"; do
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
# succeeds
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
    msg "Generating $what"
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

# universe word pairs adjacent in some cluer clue that fit in the letters
cluer_pairs() {
    python "$cluer/query_index.py" --letters "$letters" --dict "$universe" -m3
}

# usage: add_pairs FILE...
# stdin's pairs with each FILE's folded in
add_pairs() {
    if [ $# -eq 0 ]; then
        cat
    else
        pcomm - "$1" | add_pairs "${@:2}"
    fi
}

# adds $extra_pairs
all_pairs() {
    pcomm "$cluer_out" "$idx_out" | add_pairs "${extra_pairs[@]}" | sort -u
}

# usage: subset_pairs WORDS
# the pairs of pairs.sN whose words are both in WORDS
subset_pairs() {
    LC_ALL=C awk -F, 'NR == FNR { w[$1]; next } ($1 in w) && ($2 in w)' \
        "$1" "$pairs_out"
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
            run_to "cluer pairs" "$cluer_out" "$tokens" "$bigrams" -- \
                cluer_pairs
            ;;
        combine)
            build wiki
            build cluer
            local srcs=("$cluer_out" "$idx_out")
            srcs+=("${extra_pairs[@]}")
            run_to "combined pairs" "$pairs_out" "${srcs[@]}" -- all_pairs
            ;;
        subsets)
            build combine
            local list
            for list in "$IDX_DIR"/words."$id".*; do
                [ -f "$list" ] || continue
                local out=$IDX_DIR/pairs.${list#"$IDX_DIR"/words.}
                run_to "$(basename "$out")" "$out" "$list" "$pairs_out" -- \
                    subset_pairs "$list"
            done
            ;;
        all)
            build combine
            build subsets
            ;;
    esac
}

for target in "${targets[@]}"; do
    build "$target"
done

exit
}
