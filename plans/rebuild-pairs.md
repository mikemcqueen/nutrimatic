# rebuild-pairs

`rebuild-pairs.py` builds the intermediate files of
docs/new-pairs-workflow.md. A file's directory under `tmp/sN` says how it was
made, and a path given on the command line is the build request.

## Paths

A path is any number of `u-LETTERS` directories, then alternating steps and
cuts, starting with a step:

    pb3/2k/pb3/pp-1sq-5k/pb2wb
    u-abc/u-def/pb3-dict/pp-1sq-5k/ppb.pb3

* `u-LETTERS`: letters removed from the sentence for everything below it. It
  holds no files of its own. LETTERS may be several hyphen-separated groups:
  `u-abc-def`
* `pbN`: bag-stats-cuda `--ss pb,...,pb --uc`
* `pbNwb`: bag-stats-cuda `--ss pb,...,pb,wb,p`, sorted as in step 5a
* `ppb[N].pb[M]`: bag-stats-cuda `--ss ppb,...,ppb,pb,...,pb --uc --pp FILE`,
  N `ppb` and M `pb`, a missing number being 1: `ppb.pb3` is `ppb,pb,pb,pb`,
  `ppb2.pb` is `ppb,ppb,pb` and `ppb2.pb2` is `ppb,ppb,pb,pb`. A 1 may be
  written out on the command line and is dropped: `ppb1.pb3` builds `ppb.pb3`
  and `ppb2.pb1` builds `ppb2.pb`
* `Nk`: seed pairs from the top N thousand words of the step above it. N may
  have up to three decimals, `1.5k` being the top 1,500 words and `0.25k` the
  top 250, with no trailing zero: `1.50k` and `1.0k` are rejected
* `pp-NAME-Nk`: the top N thousand pairs of the step above it, by the
  `pair-prior` score under the manifest `idx/pp.NAME`

A root step is the first step of a path: the one directly below `tmp/sN` or
a `u-LETTERS` directory.

### Used letters

A path's used letters are its `u-LETTERS` directories' letters, concatenated
in path order: `u-abc/u-def` is `abcdef`. LETTERS is one or more groups of
letters joined by hyphens, `u-abc-def`, using the letters of every group:
`abcdef` again. A group's letters are in any order, `u-dec-ind`, and the
concatenation is within the sentence. Every bag-stats-cuda step below gets
`-u` with them, and every `Nk` cut's `pfilter` does too, so the whole subtree
works on one bag. `-u` gets the hyphens as written, `-u abc-def`: both tools
drop every character of it that isn't a letter or digit.

A root step directly below `tmp/sN` may instead carry a `-LETTERS` suffix,
`pb3-dict-abc` or `pb3-dict-abc-def`, with the same meaning as
`u-abc/pb3-dict` or `u-abc-def/pb3-dict`: the letters are used by every step
and `Nk` cut below it. `-LETTERS` is rejected on any other
step, and on a step below a `u-LETTERS` directory.

One bag can be spelled several ways: `u-abcdef`, `u-abc-def`, `u-abc/u-def`,
`u-def/u-abc` and the root suffix `pb3-abcdef` all use the letters `abcdef`.
Each spelling is its own directory with its own root runs. Nothing merges
them or warns.

### Aliases

A group that is all uppercase is an alias: a name for a group of letters,
kept per sentence in `tmp/sN/aliases.json`. After `--add-alias OT=onetwo
--add-alias TF=threefour`, `u-OT-TF` uses the letters `onetwo-threefour`, and
that is what `-u` gets. The directory keeps the alias in its name:
`tmp/sN/u-OT-TF`. Aliases work wherever LETTERS does: `u-OT/u-TF`, the root
suffix `pb3-dict-OT`, and `--status sN u-OT`.

A name is uppercase letters only and its letters are one lowercase group
within the sentence. Groups of a LETTERS may be aliases or letters in any
mix, `u-OT-threefour`, but one group is all uppercase or all lowercase: `u-Ot`
is rejected, and so is an alias the sentence doesn't have. `--add-alias`
prints `added alias NAME=letters`. It rejects a name that already stands for
other letters; changing one is an edit of `aliases.json`, and the steps below
a directory that uses it are then stale by their changed command.

`--remove-alias NAME` drops an alias and prints `removed alias NAME=letters`.
Directories that use the name stay on disk; their paths are rejected, and
`--status` and `--report` leave them out, until the alias is added again.

Exact counts for a different set of used letters need their own root
bag-stats-cuda run, which is what a nested `u-LETTERS` directory is: each one
holds its own root steps.

### Step suffixes

A `pbN` or `pbNwb` step takes three optional suffixes, in this order:
`pb3-big-x15-glllpu`.

* `-dict`, `-big`: `seed-pairs -d` with `$DICT` or `$BIG`, keeping only seeds
  with both words in it; without it seeds have no dictionary filter. Accepted
  on a root `pbN` or `pbNwb` only. bag-stats-cuda's `DICT` argument stays
  `$DICT` either way. A forced pair with a word outside the dictionary is
  dropped, with a warning
* `-xN`: the step's input seeds go through `pfilter -xN` first, a cap of N
  letters per pair; without it they have no cap. Accepted on a root `pbN` only
* `-LETTERS`: as under Used letters

`-xN` applies to that step alone: a cut below it isn't filtered by it. `-dict`
and `-big` carry down: a cut's words come from the step's pairs, so they are
all in the dictionary.

### Step inputs

* A root step directly below `tmp/sN` or a top-level `u-LETTERS` directory:
  `seed-pairs -sN -m4`
* A root `pbN` step below a nested `u-LETTERS` directory: the `pairs` file of
  the same-named step in the `u-LETTERS` directory above. Every pair in a
  combination under the smaller bag is in that same combination under the
  larger one, so the counts are exact. When that step's `pairs.ranked` is
  missing the build fails without running anything: `parent GPU results
  missing; run rebuild-pairs.py sN u-abc/pb4-dict to generate it`, naming the
  parent step
* A root `pbNwb` step below a nested `u-LETTERS` directory: `seed-pairs -sN
  -m4`. Its parent's `pairs.ranked` holds only the pairs of the top dump
  rows, so it can't be the input
* A later `pbN` or `pbNwb` step: the `pairs` file of the cut above it
* A `ppb[N].pb[M]` step: `--pp` is the `pairs` file of the cut above it, and
  PAIRS is the `pairs` file of its pool step

### ppb steps

A `ppb[N].pb[M]` step sits below a cut of a `pbN` step, or below a cut of a
`ppb` step that itself qualifies, and takes no suffixes:

    pb3-dict/pp-1sq-3k/ppb3.pb/pp-1sq-3k/ppb.pb3
    pb3-dict/pp-1sq-3k/ppb3.pb/1.5k/pb3/pp-1sq-3k/ppb3.pb

Its pool step is the nearest `pbN` step above it, root or not, with only
`ppb` steps between. In the second path the first `ppb3.pb`'s pool step is
`pb3-dict` and the last one's is the `pb3` below `1.5k`, which ran on the
pairs of the top 1,500 words: a second round on a smaller pool. A `pbNwb`
step is not a pool step, and a `ppb` step whose nearest step above is one is
rejected.

Every `ppb` step uses its pool step's `pairs` as PAIRS. A `ppb` step's
`pairs.ranked` holds only pool pairs and its prospects, and those prospects
are themselves a cut of the pool step or of a `ppb` step above, so a cut of
it stands to the pool step as a cut of the pool step does.

The rule every `ppb` step must meet: PAIRS holds every pair that can be in a
combination with one of its prospects. The pool step's `pairs` holds every
pair of its input that is in one of its combinations, so the rule holds when
the step picks at least as many pairs as the pool step and the pool step's
input holds every pair that can be in a combination under its bag. Pairs that
are in no combination don't matter, as prospects or as partners.

A `pp-NAME-Nk` cut takes its pairs from the `pairs.ranked` of the step above,
so its pairs are already pool pairs or prospects of a `ppb` step above. An
`Nk` cut instead holds every seed pair of its words, so below one the pool
step's input must hold every such pair that can be in a combination. By pool
step:

* A root step directly below `tmp/sN` or a top-level `u-LETTERS` directory:
  its input is every seed pair, so it holds them
* A root step below a nested `u-LETTERS` directory: its input is the `pairs`
  of the same-named step above, which holds every pair that can be in a
  combination under the smaller bag (see Step inputs), so it holds them
* A step below an `Nk` cut: its input is every seed pair of that cut's words,
  and an `Nk` cut further down takes its words from that cut's pairs, so it
  holds them
* An `-xN` root: the cap drops pairs longer than N letters, and those can be
  in a combination, so it doesn't hold them
* A step below a `pp-NAME-Nk` cut: the `pp` cut keeps only its top pairs by
  `pair-prior` score, and the pairs it drops can be in a combination, so it
  doesn't hold them

So a `ppb` step is rejected:

* when N + M is less than the pool step's pick count: the pool step's `pairs`
  would be missing pairs the step could pick
* below an `Nk` cut when the pool step is an `-xN` root
* below an `Nk` cut when the pool step is below a `pp-NAME-Nk` cut

## Files

    pbN           pairs.ranked  pairs  words.ranked  words
    pbNwb         dump  pairs.ranked  words.ranked  words
    ppb[N].pb[M]  pairs.ranked  words.ranked  words
    Nk            pairs
    pp-NAME-Nk    pairs

`pairs.ranked` is `COUNT word word`, `words.ranked` is `COUNT word`, `pairs` is
`word,word`, and `words` is one word per line. A `pbN` step's `pairs` is its
`pairs.ranked` in the same order without the counts, `awk '{print $2","$3}'`.
Any `pbN`, `pbNwb` or `ppb[N].pb[M]` step can be cut and fed forward: an `Nk`
cut reads the step's `words`, a `pp-NAME-Nk` cut its `pairs.ranked`.

## Commands

    rebuild-pairs.py sN PATH            build PATH and whatever it needs
    rebuild-pairs.py -n sN PATH         print what would run
    rebuild-pairs.py -f sN PATH         also rerun the steps of PATH
    rebuild-pairs.py --status sN        the tree: files, line counts, stale steps
    rebuild-pairs.py --status sN u-L    only the subtrees with used letters L
    rebuild-pairs.py --report sN        each tmp/sN/targets word's rank per node
    rebuild-pairs.py --adopt sN         move the old flat names into the tree
    rebuild-pairs.py sN --add-alias NAME=letters    name a group of used letters
    rebuild-pairs.py sN --remove-alias NAME         drop an alias

Each build call takes one PATH.

`--status sN u-abcdef` shows the subtree of every `u-LETTERS` directory whose
used letters are exactly `abcdef`: `u-abcdef`, `u-abc-def` and `u-abc/u-def`
when they exist. Each comes with the directory lines of the `u-LETTERS`
directories above it and none of their steps. The match is on the
concatenated letters as written, aliases replaced by their letters and
hyphens dropped, not sorted.

## Rebuilds

Each node's `.manifest.json` records, per file, its command and the size and
mtime of its inputs. A step reruns when an output is missing, its command or an
input changed, or a step it depends on reruns. `$DICT`, the seed pair file, and
a `pp` cut's `idx/pp.NAME`, `idx/p1.idx` and `cluer/data/cluedata` are
recorded and flagged by `--status` when they change, and don't cause a rebuild.
Adopted steps have no recorded command, so only their inputs count.

A root step's `bag-stats-cuda` run is the exception: without `-f` naming that
root, it runs only when its output is missing. If it is otherwise out of date,
by a changed command, a missing record, a changed recorded file or a rebuilt
`pairs` file in the `u-LETTERS` directory above, the run warns with the reason
and leaves it alone, and the steps after it don't rerun because of it.

A root step fed by the `u-LETTERS` directory above is only as current as that
parent step: seed pairs added after the parent ran reach neither of them until
the parent is rebuilt with `-f`, and `--status` flags the changed seed file on
the parent alone.

Outputs are written to `FILE.tmp` and renamed. Each run holds its sentence's
lock, so a second run on that sentence waits while runs on other sentences
proceed. `bag-stats-cuda` steps also hold a global GPU lock for the step's
duration, so GPU jobs never overlap.

## Previous steps:

An `Nk` cut's N was a whole number of thousands.

A `ppb[N].pb[M]` step sat below a cut of a root `pbN` step, or below a cut of
a `ppb` step that itself qualified, with only `ppb` steps between it and the
root. Every step in such a chain used the root's `pairs` as PAIRS, and a `ppb`
step anywhere below a later `pbN` step was rejected.

A `ppb[N].pb[M]` step had to sit below a cut directly below a root `pbN` step,
and nothing could go below it.

Each `u-LETTERS` directory's letters, and a root step's `-LETTERS` suffix, had
to be in sorted order: `u-cba` was rejected.

Before `u-LETTERS` directories, used letters were a step suffix only, accepted
on any step:

* `-LETTERS`: bag-stats-cuda `-u LETTERS`, in sorted order and within the
  sentence

`-xN` and `-LETTERS` apply to that step alone: a cut below it isn't filtered by
them, so it can feed steps with different suffixes.

The first step's seeds are `seed-pairs -sN -m4`; a later step's are the `pairs`
file of the cut above it.

    pbN     pairs.ranked  words.ranked  words
