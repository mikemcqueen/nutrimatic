#!/usr/bin/env python3
"""Builds the new-pairs workflow's intermediate files under tmp/sN.

See plans/rebuild-pairs.md. A path is any number of u-LETTERS directories,
then alternating steps and cuts (Nk, pp-NAME-Nk or union-Nk), starting with
a step: u-abc/pb3/2k/pb3/pp-1sq-5k/pb2wb. An Nk cut's N may have up to three
decimals: 1.5k. LETTERS may be several hyphen-separated
groups: u-abc-def. An all-uppercase group is an alias from tmp/sN/aliases.json,
added with --add-alias: u-OT-def.
"""

import argparse
import collections
import fcntl
import json
import os
import re
import shlex
import subprocess
import sys

from dataclasses import dataclass, field
from pathlib import Path

PROGRAM = "rebuild-pairs"
MANIFEST = ".manifest.json"
ALIASES = "aliases.json"
GROUP = r"(?:[a-z]+|[A-Z]+)"
LETTERS = rf"{GROUP}(?:-{GROUP})*"
STEP_RE = re.compile(r"pb([1-9][0-9]*)(wb)?(?:-(dict|big))?"
                     rf"(?:-x([1-9][0-9]*))?(?:-({LETTERS}))?")
PPB_RE = re.compile(r"ppb([1-9][0-9]*)?\.pb([1-9][0-9]*)?")
USED_RE = re.compile(rf"u-({LETTERS})")
ALIAS_RE = re.compile(r"([A-Z]+)=([a-z]+)")
THOUSANDS = r"([1-9][0-9]*(?:\.[0-9]{0,2}[1-9])?|0\.[0-9]{0,2}[1-9])k"
CUT_RE = re.compile(THOUSANDS)
UNION_RE = re.compile(rf"union-{THOUSANDS}")
PP_CUT_RE = re.compile(r"pp-([0-9a-z]+)-([1-9][0-9]*)k")
WB_ROWS = 5000000
Segs = tuple[str, ...]


def die(message: str) -> None:
    print(f"{PROGRAM}: {message}", file=sys.stderr)
    sys.exit(1)


@dataclass
class Sentence:
    n: str
    letters: str
    dict: str
    big: str
    aliases: dict[str, str] = field(default_factory=dict)

    @property
    def root(self) -> Path:
        return Path("tmp") / f"s{self.n}"

    @property
    def composite(self) -> bool:
        return "s" in self.n

    @property
    def no_pairs_flag(self) -> str:
        return "--no-nopairs" if self.composite else f"-s{self.n}"

    @property
    def seed_flag(self) -> str:
        return f"--pairs idx/pairs.s{self.n}" if self.composite else f"-s{self.n}"


@dataclass
class Step:
    segs: Segs
    outputs: list[str]
    deps: list[tuple[Segs, str]]
    command: str
    external: list[str]
    scratch: list[str] = field(default_factory=list)
    gpu: bool = False


def split_segs(segs: Segs) -> tuple[Segs, Segs]:
    count = 0
    while count < len(segs) and USED_RE.fullmatch(segs[count]):
        count += 1
    return segs[:count], segs[count:]


def is_root(segs: Segs) -> bool:
    return len(split_segs(segs)[1]) == 1


def pool_of(rest: Segs) -> int | None:
    for index in range(len(rest) - 3, -1, -2):
        if STEP_RE.fullmatch(rest[index]):
            return index
    return None


def cut_words(thousands: str) -> int:
    whole, _, fraction = thousands.partition(".")
    return int(whole) * 1000 + int(fraction.ljust(3, "0"))


def resolve(sentence: Sentence, letters: str) -> str:
    return "-".join(sentence.aliases.get(group, group)
                    for group in letters.split("-"))


def used_letters(sentence: Sentence, segs: Segs) -> str:
    used_dirs, rest = split_segs(segs)
    letters = [USED_RE.fullmatch(seg)[1] for seg in used_dirs]
    if rest:
        letters.append(STEP_RE.fullmatch(rest[0])[5] or "")
    return "".join(resolve(sentence, part) for part in letters)


def canonical(seg: str) -> str:
    match = PPB_RE.fullmatch(seg)
    if not match:
        return seg
    ppb, pb = ("" if count in (None, "1") else count
               for count in match.groups())
    return f"ppb{ppb}.pb{pb}"


def parse_path(sentence: Sentence, path: str) -> Segs:
    segs = tuple(canonical(part) for part in path.split("/") if part)
    error = path_error(sentence, segs)
    if error:
        die(f"bad path {path}: {error}")
    for seg in segs:
        match = PP_CUT_RE.fullmatch(seg)
        if match and not Path(f"idx/pp.{match[1]}").exists():
            die(f"bad path {path}: idx/pp.{match[1]} doesn't exist")
    return segs


def path_error(sentence: Sentence, segs: Segs) -> str | None:
    for seg in segs:
        for group in seg.split("-"):
            if re.fullmatch(r"[A-Za-z]+", group) \
                    and not re.fullmatch(GROUP, group):
                return f"{group} mixes upper and lower case"
    used_dirs, rest = split_segs(segs)
    for index, seg in enumerate(rest):
        patterns = (CUT_RE, PP_CUT_RE, UNION_RE) if index % 2 \
            else (STEP_RE, PPB_RE)
        if not any(pattern.fullmatch(seg) for pattern in patterns):
            rest = ()
    if not rest:
        return ("want any u-LETTERS directories, then a step (pbN, pbNwb, "
                "optionally -dict or -big, then -xN, then -LETTERS; or "
                "ppb[N].pb[M]), then alternating cuts (Nk, pp-NAME-Nk, union-Nk) "
                "and steps")
    for index in range(1, len(rest), 2):
        if UNION_RE.fullmatch(rest[index]) and not (
                index > 1 and PPB_RE.fullmatch(rest[index - 1])
                and PP_CUT_RE.fullmatch(rest[index - 2])):
            return (f"{rest[index]} goes below a ppb step that is below a "
                    "pp-NAME-Nk cut")
    for index, seg in enumerate(rest[::2]):
        match = STEP_RE.fullmatch(seg)
        if not match:
            at = pool_of(rest[:2 * index + 1])
            pool = None if at is None else STEP_RE.fullmatch(rest[at])
            if not pool or pool[2]:
                return (f"{seg} goes below a cut of a pbN step or of a ppb "
                        "step below one")
            if seg != canonical(seg):
                return f"{seg} is spelled {canonical(seg)}"
            ppb = PPB_RE.fullmatch(seg)
            if int(ppb[1] or 1) + int(ppb[2] or 1) < int(pool[1]):
                return (f"{seg} picks fewer pairs than {rest[at]}, whose "
                        "pairs file would be missing pairs it could pick")
            if CUT_RE.fullmatch(rest[2 * index - 1]):
                if pool[4]:
                    return (f"{seg} can't go below an Nk cut of a "
                            f"-x{pool[4]} root: the cut's pairs can be "
                            "longer than the cap")
                if at and (PP_CUT_RE.fullmatch(rest[at - 1])
                           or UNION_RE.fullmatch(rest[at - 1])):
                    return (f"{seg} can't go below an Nk cut when "
                            f"{rest[at]} is below {rest[at - 1]}: the cut's "
                            f"pairs needn't be among {rest[at - 1]}'s")
            continue
        used = match[5]
        if match[3] and index:
            return f"-{match[3]} is only for a root step"
        if match[4] and (index or match[2]):
            return f"-x{match[4]} is only for a root pbN step"
        for group in ("dict", "big"):
            if used and group in used.split("-"):
                return f"-{group} must come right after pbN or pbNwb"
        if used and (index or used_dirs):
            return (f"-{used} is only for a root step directly below "
                    f"tmp/s{sentence.n}")
    used = used_letters(sentence, segs).replace("-", "")
    for group in re.findall(r"[A-Z]+", used):
        return (f"s{sentence.n} has no alias {group}; add it with "
                f"--add-alias {group}=LETTERS")
    if collections.Counter(used) - collections.Counter(sentence.letters):
        return f"{used} is not within s{sentence.n}"
    return None


def node_dir(sentence: Sentence, segs: Segs) -> Path:
    return sentence.root.joinpath(*segs)


def step_input(sentence: Sentence, segs: Segs) -> tuple[str, list, list[str]]:
    match = STEP_RE.fullmatch(segs[-1])
    used_dirs, rest = split_segs(segs)
    if len(rest) == 1 and (len(used_dirs) < 2 or match[2]):
        source = f"seed-pairs {sentence.seed_flag} -m4"
        deps = []
        external = [f"idx/pairs.s{sentence.n}"]
        if match[3]:
            seed_dict = sentence.big if match[3] == "big" else sentence.dict
            source += f" -d {shlex.quote(seed_dict)}"
            external.append(seed_dict)
    else:
        parent = segs[:-1] if len(rest) > 1 else (*used_dirs[:-1], rest[0])
        path = shlex.quote(str(node_dir(sentence, parent) / "pairs"))
        source = f"cat {path}"
        deps = [(parent, "pairs")]
        external = []
    cap = match[4]
    if cap:
        source += f" | pfilter --no-nopairs --no-dict -m0 -x{cap} -"
    return source, deps, external


def used_flag(sentence: Sentence, segs: Segs) -> str:
    used = used_letters(sentence, segs)
    return f"-u {used} " if used else ""


def words_step(sentence: Sentence, segs: Segs) -> Step:
    d = node_dir(sentence, segs)
    return Step(segs, ["words"], [(segs, "words.ranked")],
                f"awk '{{print $2}}' {d / 'words.ranked'} > {d / 'words.tmp'}",
                [])


def pair_step(sentence: Sentence, segs: Segs, count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    source, deps, external = step_input(sentence, segs)
    ss = ",".join(["pb"] * count)
    letters = shlex.quote(sentence.letters)
    dictionary = shlex.quote(sentence.dict)
    rank = Step(
        segs, ["pairs.ranked"], deps,
        f"{source} | bag-stats-cuda --threads 0 {sentence.no_pairs_flag} "
        f"{used_flag(sentence, segs)}--ss {ss} --uc "
        f"{letters} - {dictionary} --progress 15 > {d / 'pairs.ranked.tmp'}",
        [sentence.dict, *external], gpu=True)
    pairs = Step(
        segs, ["pairs"], [(segs, "pairs.ranked")],
        f"awk '{{print $2\",\"$3}}' {d / 'pairs.ranked'} > {d / 'pairs.tmp'}",
        [])
    return [rank, pairs, counted_step(sentence, segs),
            words_step(sentence, segs)]


def counted_step(sentence: Sentence, segs: Segs) -> Step:
    d = node_dir(sentence, segs)
    return Step(
        segs, ["words.ranked"], [(segs, "pairs.ranked")],
        f"top-segments -n0 --pair-words --by-count < {d / 'pairs.ranked'} "
        f"> {d / 'words.ranked.tmp'}", [])


def prospect_step(sentence: Sentence, segs: Segs, prospects: int,
                  count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    cut = segs[:-1]
    used_dirs, rest = split_segs(segs)
    pool = (*used_dirs, *rest[:pool_of(rest) + 1])
    ss = ",".join(["ppb"] * prospects + ["pb"] * count)
    letters = shlex.quote(sentence.letters)
    dictionary = shlex.quote(sentence.dict)
    rank = Step(
        segs, ["pairs.ranked"], [(cut, "pairs"), (pool, "pairs")],
        f"bag-stats-cuda --threads 0 {sentence.no_pairs_flag} "
        f"{used_flag(sentence, segs)}--ss {ss} --uc "
        f"--pp {node_dir(sentence, cut) / 'pairs'} "
        f"{letters} {node_dir(sentence, pool) / 'pairs'} {dictionary} "
        f"--progress 15 > {d / 'pairs.ranked.tmp'}",
        [sentence.dict], gpu=True)
    return [rank, counted_step(sentence, segs), words_step(sentence, segs)]


def word_bag_step(sentence: Sentence, segs: Segs, count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    source, deps, external = step_input(sentence, segs)
    ss = ",".join(["pb"] * count + ["wb", "p"])
    letters = shlex.quote(sentence.letters)
    dictionary = shlex.quote(sentence.dict)
    dump = Step(
        segs, ["dump"], deps,
        f"{source} | bag-stats-cuda -n {WB_ROWS} --wb --wbfrac-min --wbwp-min "
        f"--threads 0 {sentence.no_pairs_flag} {used_flag(sentence, segs)}--ss {ss} "
        f"{letters} - {dictionary} "
        f"--progress 15 --cache-size 5000 > {d / 'dump.tmp'}",
        [sentence.dict, *external], gpu=True)
    top = d / "top.tmp"
    digest = Step(
        segs, ["pairs.ranked", "words.ranked"], [(segs, "dump")],
        f"./bs-to-top.sh -o {top} {d / 'dump'}\n"
        f"top-segments -n0 --pairs -c < {top} | tr , ' ' "
        f"> {d / 'pairs.ranked.tmp'}\n"
        f"top-segments -n0 --pair-words -c < {top} "
        f"> {d / 'words.ranked.tmp'}\n"
        f"rm {top}", [], ["top.tmp"])
    return [dump, digest, words_step(sentence, segs)]


def cut_step(sentence: Sentence, segs: Segs, count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    parent = segs[:-1]
    words = node_dir(sentence, parent) / "words"
    n = sentence.n
    return [Step(
        segs, ["pairs"], [(parent, "words")],
        f"head -n {count} {words} | seed-pairs {sentence.seed_flag} "
        f"-m4 -d /dev/stdin | pfilter {sentence.no_pairs_flag} --no-dict -x0 "
        f"-l {shlex.quote(sentence.letters)} {used_flag(sentence, segs)}- "
        f"> {d / 'pairs.tmp'}",
        [f"idx/pairs.s{n}"])]


def pp_cut_step(sentence: Sentence, segs: Segs, name: str,
                thousands: int) -> list[Step]:
    d = node_dir(sentence, segs)
    parent = segs[:-1]
    ranked = node_dir(sentence, parent) / "pairs.ranked"
    manifest = f"idx/pp.{name}"
    return [Step(
        segs, ["pairs"], [(parent, "pairs.ranked")],
        f"pfilter {sentence.no_pairs_flag} --no-dict -x0 --ranked {ranked} | "
        f"pair-prior --ranked --manifest {manifest} --as-pairs "
        f"-n {thousands * 1000} - > {d / 'pairs.tmp'}",
        [manifest, "idx/p1.idx",
         "cluer/data/cluedata"])]


def union_step(sentence: Sentence, segs: Segs, count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    ppb = segs[:-1]
    cut = segs[:-2]
    top = d / "cut.tmp"
    return [Step(
        segs, ["pairs"], [(cut, "pairs"), (ppb, "pairs.ranked")],
        f"head -n {count} {node_dir(sentence, cut) / 'pairs'} > {top}\n"
        f"head -n {count} {node_dir(sentence, ppb) / 'pairs.ranked'} | "
        f"awk '{{print $2\",\"$3}}' | pcomm {top} - > {d / 'pairs.tmp'}\n"
        f"rm {top}", [], ["cut.tmp"])]


def steps_of(sentence: Sentence, segs: Segs) -> list[Step]:
    last = segs[-1]
    union = UNION_RE.fullmatch(last)
    if union:
        return union_step(sentence, segs, cut_words(union[1]))
    pp_cut = PP_CUT_RE.fullmatch(last)
    if pp_cut:
        return pp_cut_step(sentence, segs, pp_cut[1], int(pp_cut[2]))
    cut = CUT_RE.fullmatch(last)
    if cut:
        return cut_step(sentence, segs, cut_words(cut[1]))
    ppb = PPB_RE.fullmatch(last)
    if ppb:
        return prospect_step(sentence, segs, int(ppb[1] or 1),
                             int(ppb[2] or 1))
    match = STEP_RE.fullmatch(last)
    count = int(match[1])
    if match[2]:
        return word_bag_step(sentence, segs, count)
    return pair_step(sentence, segs, count)


def producer(sentence: Sentence, segs: Segs, output: str) -> Step:
    for step in steps_of(sentence, segs):
        if output in step.outputs:
            return step
    die(f"{'/'.join(segs)} has no {output}")


def stat(path: Path | str) -> list[int] | None:
    try:
        info = os.stat(path)
    except OSError:
        return None
    return [info.st_size, info.st_mtime_ns]


def read_manifest(directory: Path) -> dict:
    try:
        return json.loads((directory / MANIFEST).read_text())
    except FileNotFoundError:
        return {}


def write_manifest(directory: Path, manifest: dict) -> None:
    path = directory / MANIFEST
    temporary = directory / (MANIFEST + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    os.replace(temporary, path)


def changed_since(record: dict) -> list[str]:
    return [Path(path).name for path, info in record.get("external", {}).items()
            if stat(path) != info]


def current_inputs(sentence: Sentence, step: Step) -> dict:
    return {
        str(node_dir(sentence, segs) / output):
            stat(node_dir(sentence, segs) / output)
        for segs, output in step.deps
    }


def stale_reason(sentence: Sentence, step: Step) -> str | None:
    directory = node_dir(sentence, step.segs)
    manifest = read_manifest(directory)
    if any(not (directory / output).exists() for output in step.outputs):
        return "missing"
    for output in step.outputs:
        record = manifest.get(output)
        if record is None:
            return "no record"
        if not record.get("adopted") and record["command"] != step.command:
            return "command changed"
        if record["inputs"] != current_inputs(sentence, step):
            return "input changed"
    return None


class Planner:
    def __init__(self, sentence: Sentence, forced: set[Segs]):
        self.sentence = sentence
        self.forced = forced
        self.memo: dict[tuple[Segs, str], bool] = {}
        self.order: list[tuple[Step, str]] = []
        self.force_required: list[tuple[Step, str | None, list[str]]] = []

    def mark_force_required(self, step: Step, reason: str | None) -> None:
        directory = node_dir(self.sentence, step.segs)
        record = read_manifest(directory).get(step.outputs[0], {})
        changed = changed_since(record)
        if reason or changed:
            self.force_required.append((step, reason, changed))

    def visit(self, step: Step) -> bool:
        key = (step.segs, step.outputs[0])
        if key in self.memo:
            return self.memo[key]
        upstream = False
        for segs, output in step.deps:
            if self.visit(producer(self.sentence, segs, output)):
                upstream = True
        reason = ("forced" if step.segs in self.forced else None) \
            or stale_reason(self.sentence, step) \
            or ("upstream rebuilt" if upstream else None)
        if step.gpu and is_root(step.segs) \
                and reason not in ("forced", "missing"):
            self.mark_force_required(step, reason)
            reason = None
        if reason:
            self.order.append((step, reason))
        self.memo[key] = bool(reason)
        return self.memo[key]


def run_step(sentence: Sentence, step: Step) -> None:
    directory = node_dir(sentence, step.segs)
    directory.mkdir(parents=True, exist_ok=True)
    temporaries = [directory / f"{output}.tmp" for output in step.outputs]
    temporaries += [directory / name for name in step.scratch]
    try:
        with open("tmp/.rebuild-pairs.gpu.lock", "w") as gpu_lock:
            if step.gpu:
                fcntl.flock(gpu_lock, fcntl.LOCK_EX)
                print(f"Executing {step.command}", flush=True)
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", step.command])
        if result.returncode != 0:
            die(f"{'/'.join(step.segs)} {' '.join(step.outputs)} failed "
                f"(exit {result.returncode})")
        manifest = read_manifest(directory)
        for output in step.outputs:
            manifest.pop(output, None)
        write_manifest(directory, manifest)
        for output in step.outputs:
            os.replace(directory / f"{output}.tmp", directory / output)
        record = {
            "command": step.command,
            "inputs": current_inputs(sentence, step),
            "external": {path: stat(path) for path in step.external},
        }
        for output in step.outputs:
            manifest[output] = record
        write_manifest(directory, manifest)
    finally:
        for path in temporaries:
            path.unlink(missing_ok=True)


def nodes_of(sentence: Sentence) -> list[Segs]:
    found = []

    def walk(directory: Path, segs: Segs) -> None:
        if split_segs(segs)[1] and path_error(sentence, segs):
            return
        if segs and (directory / MANIFEST).exists():
            found.append(segs)
        children = sorted(c for c in directory.iterdir() if c.is_dir())
        for child in children:
            walk(child, (*segs, child.name))

    if sentence.root.is_dir():
        walk(sentence.root, ())
    return found


def line_count(path: Path) -> str:
    if not path.exists():
        return "-"
    result = subprocess.run(["wc", "-l", str(path)], capture_output=True,
                            text=True, check=True)
    return result.stdout.split()[0]


def natural(seg: str) -> list:
    return [int(part) if part.isdigit() else part
            for part in re.split(r"(\d+)", seg)]


def status(sentence: Sentence, used: str | None, paths: list[Segs]) -> None:
    planner = Planner(sentence, set())
    nodes = nodes_of(sentence)
    for segs in nodes:
        for step in steps_of(sentence, segs):
            planner.visit(step)
    stale = {(step.segs, step.outputs[0]): why for step, why in planner.order}
    stale |= {(step.segs, step.outputs[0]): reason
              for step, reason, _ in planner.force_required if reason}

    tree = {segs[:i] for segs in nodes for i in range(1, len(segs) + 1)}
    shown = [node for node in tree if not split_segs(node)[1]
             and used_letters(sentence, node).replace("-", "") == used]
    if used is not None and not shown:
        die(f"no u-LETTERS directories with used letters {used}")
    for segs in paths:
        if segs not in tree:
            die(f"{'/'.join(segs)} has nothing built")
    shown += paths
    rows: list[tuple] = []

    def visible(segs: Segs) -> bool:
        return not shown or any(
            segs[:len(node)] == node or node[:len(segs)] == segs
            for node in shown)

    def collect(segs: Segs) -> None:
        depth = len(segs)
        rows.append(("dir", "  " * (depth - 1) + segs[-1] + "/"))
        directory = node_dir(sentence, segs)
        manifest = read_manifest(directory)
        steps = steps_of(sentence, segs) if split_segs(segs)[1] else []
        for step in steps:
            reason = stale.get((segs, step.outputs[0]))
            for output in step.outputs:
                path = directory / output
                if not path.exists():
                    state = "missing"
                elif reason == "missing":
                    state = "partial"
                else:
                    state = reason or "ok"
                changed = changed_since(manifest.get(output, {}))
                note = f"  (changed since: {', '.join(changed)})" \
                    if changed else ""
                rows.append(("file", "  " * depth + output, state,
                             line_count(path), note))
        children = [node for node in tree if len(node) == depth + 1
                    and node[:depth] == segs and visible(node)]
        for child in sorted(children, key=lambda node: natural(node[-1])):
            collect(child)

    for root in sorted((node for node in tree
                        if len(node) == 1 and visible(node)),
                       key=lambda node: natural(node[0])):
        collect(root)

    files = [row for row in rows if row[0] == "file"]
    if not files:
        return
    name_width = max(27, max(len(row[1]) for row in files) + 2)
    state_width = max(7, max(len(row[2]) for row in files) + 2)
    color = sys.stdout.isatty()

    def paint(text: str, code: str) -> str:
        return f"\033[{code}m{text}\033[0m" if color else text

    codes = {"ok": "32", "missing": "31"}
    for row in rows:
        if row[0] == "dir":
            print(paint(row[1], "36"))
            continue
        _, name, state, count, note = row
        padded = state.ljust(state_width)
        if state in codes:
            padded = paint(state, codes[state]) \
                + " " * (state_width - len(state))
        print(f"{name.ljust(name_width)}{padded}{count}{note}")


def report(sentence: Sentence) -> None:
    targets_path = sentence.root / "targets"
    if not targets_path.exists():
        die(f"{targets_path} not found: one word per line")
    targets = targets_path.read_text().split()
    rows = []
    for segs in nodes_of(sentence):
        path = node_dir(sentence, segs) / "words.ranked"
        if not path.exists():
            continue
        ranks = {}
        with path.open() as words:
            for rank, line in enumerate(words, 1):
                ranks[line.split()[1]] = rank
        rows.append(("/".join(segs), str(len(ranks)),
                     *(str(ranks.get(word, "-")) for word in targets)))
    header = ("node", "words", *targets)
    widths = [max(len(row[i]) for row in [header, *rows])
              for i in range(len(header))]
    for row in [header, *rows]:
        cells = [row[0].ljust(widths[0])]
        cells += [cell.rjust(width) for cell, width in zip(row[1:], widths[1:])]
        print("  ".join(cells))


ADOPT = [
    (r"pairs\.(pb\d+)\.ranked", r"\1/pairs.ranked"),
    (r"pairs\.(pb\d+)\.ranked\.words", r"\1/words.ranked"),
    (r"pairs\.(pb\d+)\.words\.ranked", r"\1/words.ranked"),
    (r"pairs\.(pb\d+)\.words", r"\1/words"),
    (r"pairs\.from\.(pb\d+)\.(\d+k)", r"\1/\2/pairs"),
    (r"pairs\.(pb\d+)\.from\.pairs\.(pb\d+)\.(\d+k)", r"\2/\3/\1/pairs.ranked"),
    (r"bs\.(pb\d+)\.wb\.from\.pairs\.(pb\d+)\.(\d+k)", r"\2/\3/\1wb/dump"),
]


def adopt(sentence: Sentence) -> None:
    root = sentence.root
    for entry in sorted(p for p in root.iterdir() if p.is_file()):
        for pattern, replacement in ADOPT:
            if re.fullmatch(pattern, entry.name):
                target = root / re.sub(pattern, replacement, entry.name)
                if target.exists():
                    die(f"{target} already exists")
                target.parent.mkdir(parents=True, exist_ok=True)
                os.rename(entry, target)
                print(f"{entry} -> {target}")
                break
    for directory, _, _ in os.walk(root):
        relative = Path(directory).relative_to(root).parts
        if not split_segs(relative)[1] or path_error(sentence, relative):
            continue
        manifest = read_manifest(Path(directory))
        for step in steps_of(sentence, relative):
            if all((Path(directory) / out).exists() for out in step.outputs):
                for output in step.outputs:
                    manifest.setdefault(output, {
                        "adopted": True,
                        "inputs": current_inputs(sentence, step),
                        "external": {},
                    })
        if manifest:
            write_manifest(Path(directory), manifest)


def write_aliases(sentence: Sentence, aliases: dict[str, str]) -> None:
    sentence.root.mkdir(parents=True, exist_ok=True)
    temporary = sentence.root / (ALIASES + ".tmp")
    temporary.write_text(json.dumps(aliases, indent=1, sort_keys=True) + "\n")
    os.replace(temporary, sentence.root / ALIASES)


def remove_aliases(sentence: Sentence, names: list[str]) -> None:
    aliases = dict(sentence.aliases)
    removed = []
    for name in names:
        if name not in aliases:
            die(f"s{sentence.n} has no alias {name}")
        removed.append(f"removed alias {name}={aliases.pop(name)}")
    write_aliases(sentence, aliases)
    print("\n".join(removed))


def add_aliases(sentence: Sentence, specs: list[str]) -> None:
    aliases = dict(sentence.aliases)
    bag = collections.Counter(sentence.letters)
    added = []
    for spec in specs:
        match = ALIAS_RE.fullmatch(spec)
        if not match:
            die(f"bad alias {spec}: want NAME=letters, NAME all uppercase "
                "and letters all lowercase")
        name, letters = match.groups()
        if aliases.get(name, letters) != letters:
            die(f"alias {name} is already {aliases[name]}; edit "
                f"{sentence.root / ALIASES} to change it")
        if collections.Counter(letters) - bag:
            die(f"bad alias {spec}: {letters} is not within s{sentence.n}")
        aliases[name] = letters
        added.append(f"added alias {name}={letters}")
    write_aliases(sentence, aliases)
    print("\n".join(added))


def load_sentence(arg: str) -> Sentence:
    match = re.fullmatch(r"[sS]?([1-9])(?:[sS]([1-9]))?", arg)
    if not match or (match[2] and match[1] >= match[2]):
        die(f"bad sentence {arg}: want sN or sNsM, N less than M")
    nums = [num for num in match.groups() if num]
    values = [os.environ.get(f"S{num}") for num in nums]
    dictionary = os.environ.get("DICT")
    big = os.environ.get("BIG")
    if not all(values) or not dictionary or not big:
        names = ", ".join(f"$S{num}" for num in nums)
        die(f"{names}, $DICT or $BIG is unset; source ./setup.sh")
    sentence = Sentence("s".join(nums), "".join(values), dictionary, big)
    try:
        sentence.aliases = json.loads((sentence.root / ALIASES).read_text())
    except FileNotFoundError:
        pass
    return sentence


def main() -> int:
    parser = argparse.ArgumentParser(
        prog=PROGRAM, description="Builds new-pairs workflow files under "
        "tmp/sN; a path such as pb3/2k/pb3 or u-abc/pb3 is the build "
        "request.")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="print what would run")
    parser.add_argument("-f", "--force", action="store_true",
                        help="also rerun every step of the named path; "
                        "the only way an out-of-date root bag-stats-cuda "
                        "step (e.g. pb3) is rebuilt")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--status", action="store_true",
                      help="show the tree and what is stale; with "
                      "PATH, only the subtree at PATH; with u-LETTERS, only "
                      "the subtrees with those used letters")
    mode.add_argument("--report", action="store_true",
                      help="show tmp/sN/targets word ranks per node")
    mode.add_argument("--adopt", action="store_true",
                      help="move the old flat file names into the tree")
    mode.add_argument("--add-alias", action="append", metavar="NAME=letters",
                      help="name a group of used letters, kept in "
                      "tmp/sN/aliases.json: --add-alias OT=onetwo makes "
                      "u-OT use the letters onetwo; may be repeated")
    mode.add_argument("--remove-alias", action="append", metavar="NAME",
                      help="drop an alias from tmp/sN/aliases.json; may be "
                      "repeated")
    parser.add_argument("sentence", metavar="sN|sNsM")
    parser.add_argument("paths", metavar="PATH", nargs="*")
    args = parser.parse_args()

    os.chdir(Path(__file__).resolve().parent)
    sentence = load_sentence(args.sentence)
    if args.status:
        used = [USED_RE.fullmatch(path) for path in args.paths]
        if sum(map(bool, used)) > 1:
            parser.error("--status takes at most one u-LETTERS")
        letters = next((resolve(sentence, match[1]).replace("-", "")
                        for match in used if match), None)
        for group in re.findall(r"[A-Z]+", letters or ""):
            die(f"s{sentence.n} has no alias {group}")
        paths = [parse_path(sentence, path)
                 for path, match in zip(args.paths, used) if not match]
        status(sentence, letters, paths)
        return 0
    if args.report:
        report(sentence)
        return 0
    if args.adopt and args.paths:
        parser.error("--adopt takes no PATH")
    alias_mode = args.add_alias or args.remove_alias
    if alias_mode and args.paths:
        parser.error("--add-alias and --remove-alias take no PATH")
    if not args.adopt and not alias_mode and len(args.paths) != 1:
        parser.error("give exactly one PATH")
    with open(f"tmp/.rebuild-pairs.s{sentence.n}.lock", "w") as lock:
        if not args.dry_run:
            fcntl.flock(lock, fcntl.LOCK_EX)
        if args.adopt:
            adopt(sentence)
            return 0
        if args.add_alias:
            add_aliases(sentence, args.add_alias)
            return 0
        if args.remove_alias:
            remove_aliases(sentence, args.remove_alias)
            return 0
        segs = parse_path(sentence, args.paths[0])
        planner = Planner(sentence, {segs} if args.force else set())
        for step in steps_of(sentence, segs):
            planner.visit(step)
        needed = {segs[:i] for i in range(1, len(segs) + 1)}
        for step, reason in planner.order:
            if step.gpu and is_root(step.segs) and step.segs not in needed:
                die(f"parent GPU results missing; run {PROGRAM}.py "
                    f"s{sentence.n} {'/'.join(step.segs)} to generate it")
        for step, reason, changed in planner.force_required:
            since = f"changed since: {', '.join(changed)}" if changed else None
            why = ", ".join(part for part in (reason, since) if part)
            root = "/".join(step.segs)
            print(f"{PROGRAM}: warning: s{sentence.n}/{root} "
                  f"{' '.join(step.outputs)} is out of date ({why}); "
                  f"rebuild it on its own with: {PROGRAM} -f s{sentence.n} "
                  f"{root}", file=sys.stderr)
        total = len(planner.order)
        if total == 0 and not planner.force_required:
            print(f"{PROGRAM}: up to date")
        for index, (step, reason) in enumerate(planner.order, 1):
            print(f"{PROGRAM}: [{index}/{total}] s{sentence.n}/"
                  f"{'/'.join(step.segs)} {' '.join(step.outputs)} ({reason})", flush=True)
            if args.dry_run:
                print(step.command)
            else:
                run_step(sentence, step)
    return 0


if __name__ == "__main__":
    sys.exit(main())
