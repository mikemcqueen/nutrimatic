# Plan: letter sets in the BEST PAIRS tree

## Status

Complete. All code is in `words`. Nutrimatic needed no source change;
`docs/best-pairs-workflow-v2.md` lives here and is updated alongside this
plan.

Both milestones landed together in one `words` commit. Milestone 2 was
separable only to keep milestone 1 from waiting on it -- until it landed the
operator would have made `<letter-set>/` and `m<N>/` by hand -- and that
transitional value does not exist when the two arrive at once.

It is motivated by `findings/best-letters.md`.

Status values are `[ ]` pending, `[-]` in progress, and `[x]` complete.

| Milestone | Deliverable | Status | Commit(s) |
|---:|---|:---:|---|
| 1 | Address grammar, `Target`, seed relocation, `status`, the old-shape diagnostic, and the letter set in the `dfs-anagrams` invocation, the rendered DFS names, and the review bundle name | [x] | `words` 3d48f73 |
| 2 | Validation, the duplicate check, `-f` creation | [x] | `words` 3d48f73 |

Milestone 1 is one change and does not divide. Land the level without the
rendered names and two letter sets at one `s`/`m`/`g` write the same DFS output
path, each `gen` replacing the other's hours of results. Land the names without
the invocation and every letter set runs the identical full bag while its
output claims a restriction that was never applied -- results that look
legitimate and are not. Land either without the review bundle name and the two
sets mint colliding P2 bundles that `review_locations` finds for each other.

Milestone 2 does separate. Until it lands, `gen dfs.seed -f` creates `g<N>/` as
it does today and the operator makes `<letter-set>/` and `m<N>/` by hand, which
is the rule the tree already runs under one level up.

Smoke tests only, per `CLAUDE.md`. They join the existing
`words/tests/test_workflow_best.py` and `test_workflow_best_e2e.py`, building
their trees with `wf_fixture`.

## Outcome

A full sentence has too many letters for `dfs-anagrams` and `top-segments` to
produce a useful candidate sample. In practice the workflow runs against a
**subset** of the sentence's letters, and an entire run — every `m`, every `g`,
every review round — belongs to one such subset. The tree keys on it.

```text
wf best gen s2 -u thisandthat -g 4 dfs.seed
```

The subset is a first-class level in `.wf/best`, above `m<N>`, so `wf best
status` shows which letter sets have been worked and how far each got.

## The letter set

### Two forms, one bag

`dfs-anagrams` takes the bag two ways, and both are used:

| Form | Meaning | Invocation |
|---|---|---|
| `o-LETTERS` | use **only** these letters | positional letters = `LETTERS` |
| `u-LETTERS` | the sentence **less** these letters | positional letters = `letters`, plus `-u LETTERS` |

Both exist for legibility, not for expressiveness: they describe the same
working bag from opposite ends, and which one reads better depends on whether
more letters were kept or removed. `LETTERS` is whatever string the operator
finds comprehensible — typically the words run together, `u-thisandthat` — and
it is **passed to `dfs-anagrams` verbatim**. The label is the value. There is
no lookup, no stored mapping, and no way for the label to describe a run other
than the one it produced.

It is therefore not canonical, and two labels can name one bag. That is what
the duplicate check is for.

`LETTERS` is `[a-z]+`. A hyphen would break the level's shape, and the
restriction keeps the label safe to drop into a filename glob without quoting.
`clean_letters` (`dfs-cli-args.cpp:39`) in fact admits `0-9` alongside `a-z`,
so a sentence carrying a digit could not be named by any letter set. None does,
and this plan does not provide for one.

### The duplicate check

The **working bag** is the canonical form, derived and never written down.
Write `bag(s)` for the multiset of characters of `s` with whitespace removed:
`letters` holds the sentence as written, spaces and all, and `dfs-anagrams`
drops them itself in `clean_letters`, so the reduction drops them too.

| Form | Working bag | Valid when |
|---|---|---|
| `o-X` | `bag(X)` | `bag(X)` is a *proper* multiset subset of `bag(letters)` |
| `u-Y` | `bag(letters) - bag(Y)` | `bag(Y)` is a *proper* multiset subset of `bag(letters)` |

The canonical form is the sorted string of the working bag, and a letter set is
a duplicate when its bag equals the bag of an existing sibling. Stripping is
one function applied to both sides even though only `letters` needs it, since
`X` and `Y` are `[a-z]+` by the grammar.

| Situation | Response |
|---|---|
| Not a multiset subset of `letters` | Error; `-f` does not help |
| Names every letter of `letters` | Error; `-f` does not help |
| Bag equals an existing sibling's | Error; `-f` does not help |
| New and valid | Refused without `-f`; created with it |

*Proper* is what rejects a label naming every letter of the sentence, and it
rejects one on each side. Under `u-` that label leaves the **empty** bag:
`dfs-anagrams` refuses it — `subtract_letters` errors with `no letters left
after removing used letters` (`dfs-cli-args.cpp:71`) — but it refuses at run
time, and by then `-f` has built `<letter-set>/m<N>/g<N>` for a letter set that
can never produce anything, which `status` would go on listing while offering a
`gen` that always fails. Under `o-` it leaves the **full** bag, which runs
perfectly well and is exactly the unrestricted search this plan exists to
avoid; nothing downstream would call it an error, so this is the only place it
can be refused.

With both ends closed and `[a-z]+` nonempty, the working bag is always a
nonempty proper subset of the sentence, whichever form named it.

`dfs-anagrams` already rejects a `u-` set that is not a subset
(`subtract_letters`, `dfs-cli-args.cpp:51`), so that half of the validation
only moves an existing error earlier. The `o-` half is new: letters absent from
the sentence are accepted today, and nothing else would catch them.

The check runs at creation only, and *creation* is one condition tested in one
place: `letter_set_dir` does not exist. `check_letter_set(target)` holds the
whole of it — the validation table, the reduction, and the sibling comparison —
and returns having done nothing when the directory is already there. Both
callers invoke it identically: `gen dfs.seed` before it creates anything, and
`status` before it derives the state of a synthesized target. `one_target` does
not call it, so `exclude`, `review`, and `complete` neither validate nor
re-check.

That gate is also what stops the check firing on itself. An existing letter set
is one of its own siblings and its bag necessarily equals its bag, so a check
that ran unconditionally would report every established letter set as a
duplicate of itself on every `gen dfs.seed`.

A duplicate directory created by hand is not detected, deliberately: the check
exists to catch a mistake at the moment it is about to cost hours of DFS, not to
police the tree.

Its real target is the **typo**, and the bag check takes the near half of it. A
transposition is an anagram: `u-thisandtaht` reduces to exactly `u-thisandthat`'s
bag and is refused as a duplicate. What escapes is a typo that *changes* the bag
— a dropped or doubled letter, `u-thisandtht` — which is a valid letter string, a
valid subset, and collides with nothing. It passes every check that can be
written, and would silently open a fresh letter set. The `-f` refusal is what
catches it, which is also why a smarter check cannot replace the flag.

**Typo first, correct spelling second is a non-issue.** Once `u-thisandtaht`
exists, `u-thisandthat` is refused as its duplicate, and nothing in this tool
removes a directory; the operator deletes the wrong one by hand and proceeds. No
command is added for it, and the refusal is not softened to accommodate it. The
check has done its job at that point — it declined to open a second letter set
for a bag that already has one, which is the whole of what it promises.

Reducing `u-` needs `letters`, which `gen dfs.seed` already requires
(`generate.py:_dfs_inputs`) and which `derive_state` already reports first
(`state.py:derive_state`). No new ordering.

## State layout

```text
.wf/best/
  idx/
  dict/
  s2/
    letters
    seed.m4.idx2.85.15.pairs
    u-thisandthat/
      m4/
        g4/
          dfs.seed -> RESULTS/dfs.s2.idx2.85.15.m4.x2.g4.1000000.u-thisandthat
          top.segments
          .top.segments.gen
          best.pairs
          .best.pairs.gen
          dfs.best -> RESULTS/dfs.s2.idx2.85.15.m4.x2.g4.best.1000000.u-thisandthat
        g5/
      m5/
    o-someotherletters/
      m4/
        g4/
```

`best.pairs` stays where it is, per leaf, accumulating independently of every
other leaf. The `classified/yes` and `classified/no` aggregates remain global
and continue to reach every leaf through `gen best.pairs`, whose intersection is
against the global YES set — so a pair confirmed under one letter set enters
another's `best.pairs` without being *decided* again.

That is not the same as skipping the leaf's review round, and the two must not
be read together. `_review_state` gates on an archived bundle for *this* leaf,
so a new letter set still runs one round before `status` offers `best.pairs`,
and that round bundles its whole top-N because `wf best review` passes
`--no-filter` always — `plans/archive/integrate-best-pairs.md`, *`review` does not
filter*, which already settles that re-presentation is not re-review. The
verdict carries across letter sets; the reading of a new ranked set does not.
The gate is per leaf today for the same reason, `g4` and `g5` each needing their
own round, and this plan only adds leaves to it.

### The seed moves up

The seed is a property of the sentence and `-m` only. A restricted bag's
candidate set is a **subset** of the full bag's, so a seed extracted from the
whole sentence is a valid superset for every letter set beneath it — pairs the
sub-bag cannot spell simply never match. One P1 cycle, the expensive stage,
serves every letter set at that `-m`.

Since `m<N>/` now sits *below* the letter set, there is no sentence-and-`m`
directory for the seed to live in, so it moves to the sentence level and takes
`m<N>` into its name:

```text
s2/m4/seed.idx2.85.15.pairs   ->   s2/seed.m4.idx2.85.15.pairs
```

Four places spell that path, and all four move together:

| Site | Today | After |
|---|---|---|
| `Target.seed()` | globs `universe_dir/"seed*.pairs"` | globs `sentence_dir/f"seed.m{N}.*.pairs"` |
| `_dfs_inputs` (`generate.py:83`) | `seed missing:` names `universe_dir` | names `sentence_dir` |
| `derive_state` (`state.py:236`) | `place:` hint names `universe_dir` | names `sentence_dir` |
| `_seed_annotation` (`generate.py:34`) | chops a literal `"seed"` | chops `seed.m<N>` |

`Target.seed()` still requires exactly one match. Miss either of the two
message sites and `status` directs the operator to put the seed in a directory
the tool no longer reads.

`_seed_annotation` needs `<N>` to know how much to chop, so it takes the
target: `_seed_annotation(target, seed)`. With that, the rendered DFS names are
unchanged by the move — `m4` appears once, where it always did.

The `m<N>` component is a prefix the tool *constructs* from a key it already
holds; matching it is not parsing a name for a dimension, and the opaque
annotation between it and `.pairs` is still never taken apart.

The seed sits directly under `s2/` beside the letter-set directories and does
not match their shape, so the walk passes over it — the same mechanism that
already keeps `idx/` and `dict/` out of a bare `wf best status`. No reserved
name list.

### What `m<N>` means now

It stops being the candidate universe. With the seed gone it holds nothing of
its own and survives purely as a key on `-m`. `Target.universe` keeps its name
to avoid churn, but the level's justification in
`plans/archive/integrate-best-pairs.md` — "the seed and anything else determined by
`-m`" — no longer applies to it.

### Migrating the tree that exists

The grammar change strands `s2/m4/g4` permanently. Every address now carries a
letter set in second position, so no milestone reaches the old shape and none
restores it; the leaf keeps its `top.segments`, its `best.pairs`, and its
archived review rounds, and nothing can address them.

The tool does not move it. `letters` and the seed were always the operator's to
place, and the two directories holding them are placed the same way, once:

```sh
cd .wf/best/s2
mkdir u-thisandthat
mv m4 u-thisandthat/m4
mv u-thisandthat/m4/seed.idx2.85.15.pairs seed.m4.idx2.85.15.pairs
```

Two consequences follow the move and neither needs a command. The `dfs.seed`
and `dfs.best` symlinks still resolve, to rendered names carrying no letter-set
suffix, so the leaf reports up to date and keeps its results until something
goes stale and the next `gen` re-renders under the new name. And archived P2
bundles under the old prefix `top.s2.m4.g4.` no longer match `review_prefix`,
so round ordinals restart at `r1`; the old bundles keep their names and nothing
collides with them.

**The unmigrated tree says so.** Left alone, `s2/` has no child matching
`[ou]-[a-z]+` and the walk finds nothing, so `wf best status` prints `no BEST
PAIRS targets` — precisely the reading the plan rejects elsewhere, work not yet
done, about work that exists. `targets` raises instead when a sentence
directory has no letter-set child and at least one child matching `m[1-9]\d*`,
naming the pre-letter-set shape and the move above. It is the same diagnostic
the address components already get, at the one level where an absent match is
ambiguous.

## Address grammar

```text
<sentence>[/<letter-set>[/m<N>[/g<N>]]]      s[1-9] / [ou]-[a-z]+ / m\d+ / g\d+
```

A fourth entry joins `SHAPES` (`state.py:9`) in second position. Everything
else about the walk is unchanged: shapes validate, nothing enumerates, and a
component that does not match its level is a diagnostic rather than an empty
result. `m4` does not match the letter-set level, so the walk knows exactly one
shape and `status` needs no compatibility branch.

`Target` gains a `letter_set` field holding the directory name (`u-thisandthat`)
and a `letter_set_dir` between `sentence_dir` and `universe_dir`. `address`
becomes `s2/u-thisandthat/m4/g4`.

## Command surface

```text
wf best status   [ADDRESS]
wf best gen      SENTENCE (-o|-u) LETTERS -g N [-m N] STAGE [-r DIR] [-n N]
wf best exclude  SENTENCE (-o|-u) LETTERS -g N [-m N] FILE
wf best review   SENTENCE (-o|-u) LETTERS -g N [-m N]
wf best complete SENTENCE (-o|-u) LETTERS -g N [-m N]
```

Exactly one of `-o` / `-u` is required on every command that names a single
target: neither is an error, and both together are an error. Both letters are
free: the global parser takes only `-d`, `-f`, and `-h` (`usage.py:9-17`). They
are named for the `dfs-anagrams` behavior they select, in keeping with `-g` and
`-m` — the flag you type is the flag that runs, except for `-o`, which selects
the positional argument instead.

Both are declared as ordinary optional flags and checked by hand, beside the
checks `-g` and `-m` already get: neither given is
`usage.missing_argument(self.format_help(command_text))`, both given is a
`ValueError`. Argparse's `required=True` mutually exclusive group is not used,
for the reason `-g` is not declared `required` either. `Action.parse`
(`command.py:71`) runs `parse_known_args` on a bare
`ArgumentParser(add_help=False)` carrying no `prog`, and `wf` runs as `python -m
workflow.wf`, so argparse would report the failure as

```text
usage: wf.py [-g COUNT] [-m LENGTH] [-r DIR] [-n COUNT] (-o LETTERS | -u LETTERS)
wf.py: error: one of the arguments -o -u is required
```

— naming the module file, dropping `best gen`, dropping `SENTENCE` and `STAGE`,
and exiting `SystemExit(2)` around `log.error`. Every other required argument in
this package is checked by hand precisely so the operator gets the command's own
help.

`Target.command()` emits the selecting flag, so every `next:` line is runnable.
It also takes `force: bool = False` and appends `-f`, which `_missing_artifact`
passes as `not target.target_dir.exists()`. `dfs.seed` is the first artifact
`derive_state` reaches, so it is the only stage that can be reported while the
directory is absent, and it is the only stage `-f` is valid on.

On `exclude` the letter set selects nothing but the report. The hard-NO verdict
`classify` records is global and reaches every letter set, exactly as `-g` and
`-m` already do there.

### Creating a letter set

`gen dfs.seed -f` already creates a missing `g<N>/`. It now creates any missing
part of `<letter-set>/m<N>/g<N>`, having first called `check_letter_set` — the
same call `status` makes, gated on the same condition, so the label `status`
refuses is the label `gen` refuses.

`s<N>/` remains an error. It holds the two hand-placed files — `letters` and
the seed — that `gen dfs.seed` requires and that `-f` cannot conjure, which is
the same rule as before, applied to the level that still holds them.

The existence checks move to match. `one_target` requires `sentence_dir` and
nothing below it, since every command calls it and only `gen dfs.seed` may
create: each of `letter_set_dir`, `universe_dir`, and `target_dir` must be a
directory if it exists, and is otherwise left alone. `Gen` keeps the `-f` gate
and names the shallowest missing level rather than only `target_dir`, and
`_gen_dfs` creates with `parents=True`. `exclude`, `review`, and `complete`
still require `target_dir` through `_action_target`, unchanged.

Everything else is unchanged. `-f` carries no overwrite meaning on `gen`, it is
rejected on the other three stages, and `gen` remains imperative.

### `status` on a target that does not exist

A **fully-qualified** address whose directories are absent is answered rather
than rejected: `status` synthesizes the target and reports its state. `letters`
comes first, as it does for every other target — there is nothing to validate a
letter set against without it — and where `letters` is present
`check_letter_set` runs before the state is derived.

Synthesis starts below `sentence_dir`, which must exist. `s<N>/` is the level
the tool never creates and the one holding both hand-placed files, so an absent
one is a typo rather than work not yet done, and it gets the same diagnostic a
partial prefix gets. The three levels below it are exactly the levels `gen
dfs.seed -f` would create, which is why they are the three that can be
synthesized.

```text
$ wf best status s2/u-thisandthat/m4/g4
s2/u-thisandthat/m4/g4: dfs.seed missing
  next: wf best gen s2 -u thisandthat -g 4 dfs.seed -f
```

so the command it offers is the one that works. This is what makes `status` the
place to find out a label is a duplicate before committing an hour of DFS to
it: an invalid or duplicate letter set is reported as the error it is, through
the per-target handler `Status` already has.

A **partial** prefix keeps the behavior it has. It names a subtree, and a
subtree that does not exist is an error rather than an empty listing — the
diagnostic that answers `wf best status s2/u-thisandtaht`, where "no BEST PAIRS
targets" would read as work not yet done. Only a complete address names one
target, and only a complete address is synthesized.

## Rendered names

The letter set reaches every name that leaves the tree, in the position each
kind of name wants it.

```text
dfs.s2.idx2.85.15.m4.x2.g4.1000000.u-thisandthat
dfs.s2.idx2.85.15.m4.x2.g4.best.1000000.u-thisandthat
top.s2.m4.g4.u-thisandthat.1000.r1.pairs
```

**The DFS names take it as a trailing suffix.** They are read by eye in
`results/`, nothing matches them by prefix, and last is where the key the
operator scans for stands out. `_dfs_name` (`generate.py:38`) renders them;
without the suffix two letter sets at one `s`/`m`/`g` render the same path and
each `gen` overwrites the other's results.

**The review bundle takes it as a prefix component instead.** `review_prefix`
(`state.py:65`) names the P2 queue entry, the eval directory, and the archived
input, all of which live outside the tree in the shared p2 phase directories.
Two letter sets at one `s`/`m`/`g` would otherwise mint colliding bundle names,
and `review_locations` globs by that prefix, so each would find the other's
bundles — reporting a review in flight that belongs to a different letter set,
and counting the wrong round ordinal.

Placing it before the cutoff and the round ordinal is what `names.py` asks for:
invariant dimensions come first, so the bundle name stays a true prefix of every
artifact derived under it and can be hoisted into a directory name. It is also
the cheaper change. `review_locations` needs no edit at all — its existing
`{prefix}*.pairs` and `{prefix}*` globs go on working — and the final dot that
already stops `g4` from matching `g45` stops `u-that` from matching
`u-thatandmore`.

The bundle still ends in `.pairs`, which is what `submit p2` requires
(`names.QUEUE_SUFFIXES`), and round ordinals stay per letter set because
`archived` is now scoped to one.

## Invoking `dfs-anagrams`

`_gen_dfs` (`generate.py:89`) builds the letters argument from the letter set:

| Letter set | argv |
|---|---|
| `o-X` | positional `X` |
| `u-Y` | positional `<contents of letters>`, plus `-u Y` |

`_display_dfs` substitutes `"$(cat .../letters)"` for the positional argument
so the echoed command stays copy-pasteable. Under `o-` the positional is the
label itself and is displayed literally; the substitution applies only to `u-`.

## Documentation

`docs/best-pairs-workflow-v2.md` is in the Nutrimatic repository, not in
`words`. Its update is a Nutrimatic commit carrying no source change.

It gains the letter set as a dimension of the recipe. Its Setup block already
binds `LETTERS=$S2` — the whole sentence, spelled `"$LETTERS"` as the
`dfs-anagrams` positional in stages 3, 5, and 7 — so the subset takes a name of
its own rather than redefining a variable those three stages consume:

```sh
LETTER_SET=u-thisandthat
DFS_LETTERS=("$LETTERS" -u thisandthat)   # u- form
# DFS_LETTERS=(thisandthat)               # o- form
```

`LETTER_SET` is the label as it appears in the tree and in artifact names.
`DFS_LETTERS` is the argv fragment, an array because the two forms differ in
argument count, which keeps stages 3, 5, and 7 to one substitution —
`"${DFS_LETTERS[@]}"` in place of `"$LETTERS"` — and free of a branch. The
Artifact names table gains the suffix.

## Prerequisites

None. `dfs-anagrams` already accepts the working bag both ways —
`-u`/`--used-letters` subtracts (`dfs-anagrams.cpp:72`,
`dfs-cli-args.cpp:341`), and the positional letters argument is the "use only
these" form. No Nutrimatic source change is needed for any milestone.

## Not in this plan

- **The whole sentence as a letter set.** Every target names a proper subset,
  so the full bag has no spelling under either form. A `u-none` for it is a
  later question and nothing here anticipates it.
- **Detecting hand-made duplicates.** The check runs at creation only.
- **Moving the tree into the new shape.** No command creates `letters` or the
  seed, and nothing in this plan removes or moves a file. The migration is the
  operator's, as those two files already were; the steps are under *Migrating
  the tree that exists*, and an unmigrated sentence is diagnosed rather than
  reported empty.
- **Removing a letter set.** The duplicate check can refuse a label whose bag
  is already taken by a mistyped sibling, and the sibling goes away by hand.
- **Per-letter-set `best.pairs` isolation.** `best.pairs` is per leaf, but the
  `classified/yes` and `classified/no` aggregates it is built from are global,
  so a verdict recorded under one letter set reaches every other. That is
  intended, and the review bundle already passes `--no-filter`
  (`commands.py:195`) so a pair confirmed elsewhere is still re-presented for
  review here.
- **The collective operation**, which `plans/archive/integrate-best-pairs.md` already
  places out of scope. Its gate on an existing target directory extends to the
  letter-set level unchanged: it never creates one, and `gen dfs.seed -f`
  remains the only path to directory creation in the tool.
