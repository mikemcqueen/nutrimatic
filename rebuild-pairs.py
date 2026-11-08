#!/usr/bin/env python3
"""Builds the new-pairs workflow's intermediate files under tmp/sN.

See plans/rebuild-pairs.md. A path alternates steps and word cuts, starting
with a step: pb3/2k/pb3/3k/pb2wb.
"""

import argparse
import fcntl
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

from dataclasses import dataclass, field
from pathlib import Path

PROGRAM = "rebuild-pairs"
MANIFEST = ".manifest.json"
STEP_RE = re.compile(r"pb([1-9][0-9]*)(wb)?")
CUT_RE = re.compile(r"([1-9][0-9]*)k")
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


def parse_path(path: str) -> Segs:
    segs = tuple(part for part in path.split("/") if part)
    if not valid_segs(segs):
        die(f"bad path {path}: want a step (pbN, pbNwb), then alternating "
            "word cuts (Nk) and steps")
    return segs


def valid_segs(segs: Segs) -> bool:
    if not segs:
        return False
    for index, seg in enumerate(segs):
        pattern = CUT_RE if index % 2 else STEP_RE
        if not pattern.fullmatch(seg):
            return False
    return True


def node_dir(sentence: Sentence, segs: Segs) -> Path:
    return sentence.root.joinpath(*segs)


def tool_paths(*tools: str) -> list[str]:
    return [shutil.which(tool) or tool for tool in tools]


def step_input(sentence: Sentence, segs: Segs) -> tuple[str, list, list[str]]:
    if len(segs) == 1:
        pairs = f"idx/pairs.s{sentence.n}"
        return f"seed-pairs {sentence.seed_flag} -m4", [], [
            pairs, *tool_paths("seed-pairs")]
    parent = segs[:-1]
    path = shlex.quote(str(node_dir(sentence, parent) / "pairs"))
    return f"cat {path}", [(parent, "pairs")], []


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
        f"--ss {ss} --uc "
        f"{letters} - {dictionary} --progress 15 > {d / 'pairs.ranked.tmp'}",
        [sentence.dict, *external, *tool_paths("bag-stats-cuda")])
    counted = Step(
        segs, ["words.ranked"], [(segs, "pairs.ranked")],
        f"top-segments -n0 --pair-words --by-count < {d / 'pairs.ranked'} "
        f"> {d / 'words.ranked.tmp'}", tool_paths("top-segments"))
    return [rank, counted, words_step(sentence, segs)]


def word_bag_step(sentence: Sentence, segs: Segs, count: int) -> list[Step]:
    d = node_dir(sentence, segs)
    source, deps, external = step_input(sentence, segs)
    ss = ",".join(["pb"] * count + ["wb", "p"])
    letters = shlex.quote(sentence.letters)
    dictionary = shlex.quote(sentence.dict)
    dump = Step(
        segs, ["dump"], deps,
        f"{source} | bag-stats-cuda -n {WB_ROWS} --wb --wbfrac-min --wbwp-min "
        f"--threads 0 {sentence.no_pairs_flag} --ss {ss} {letters} - {dictionary} "
        f"--progress 15 --cache-size 5000 > {d / 'dump.tmp'}",
        [sentence.dict, *external, *tool_paths("bag-stats-cuda")])
    top = d / "top.tmp"
    digest = Step(
        segs, ["pairs.ranked", "words.ranked"], [(segs, "dump")],
        f"./bs-to-top.sh -o {top} {d / 'dump'}\n"
        f"top-segments -n0 --pairs -c < {top} | tr , ' ' "
        f"> {d / 'pairs.ranked.tmp'}\n"
        f"top-segments -n0 --pair-words -c < {top} "
        f"> {d / 'words.ranked.tmp'}\n"
        f"rm {top}", tool_paths("top-segments"), ["top.tmp"])
    return [dump, digest, words_step(sentence, segs)]


def cut_step(sentence: Sentence, segs: Segs, thousands: int) -> list[Step]:
    d = node_dir(sentence, segs)
    parent = segs[:-1]
    words = node_dir(sentence, parent) / "words"
    n = sentence.n
    return [Step(
        segs, ["pairs"], [(parent, "words")],
        f"head -n {thousands * 1000} {words} | seed-pairs {sentence.seed_flag} "
        f"-m4 -d /dev/stdin | pfilter {sentence.no_pairs_flag} "
        f"-l {shlex.quote(sentence.letters)} - "
        f"> {d / 'pairs.tmp'}",
        [f"idx/pairs.s{n}", *tool_paths("seed-pairs", "pfilter")])]


def steps_of(sentence: Sentence, segs: Segs) -> list[Step]:
    last = segs[-1]
    cut = CUT_RE.fullmatch(last)
    if cut:
        return cut_step(sentence, segs, int(cut[1]))
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
        result = subprocess.run(["bash", "-euo", "pipefail", "-c", step.command])
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
        if segs and not valid_segs(segs):
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


def status(sentence: Sentence) -> None:
    planner = Planner(sentence, set())
    nodes = nodes_of(sentence)
    for segs in nodes:
        for step in steps_of(sentence, segs):
            planner.visit(step)
    stale = {(step.segs, step.outputs[0]): why for step, why in planner.order}

    tree = {segs[:i] for segs in nodes for i in range(1, len(segs) + 1)}
    rows: list[tuple] = []

    def collect(segs: Segs) -> None:
        depth = len(segs)
        rows.append(("dir", "  " * (depth - 1) + segs[-1] + "/"))
        directory = node_dir(sentence, segs)
        manifest = read_manifest(directory)
        for step in steps_of(sentence, segs):
            reason = stale.get((segs, step.outputs[0]))
            for output in step.outputs:
                path = directory / output
                if not path.exists():
                    state = "missing"
                elif reason == "missing":
                    state = "partial"
                else:
                    state = reason or "ok"
                changed = [Path(p).name for p, info in
                           manifest.get(output, {}).get("external", {}).items()
                           if stat(p) != info]
                note = f"  (changed since: {', '.join(changed)})" \
                    if changed else ""
                rows.append(("file", "  " * depth + output, state,
                             line_count(path), note))
        children = [node for node in tree
                    if len(node) == depth + 1 and node[:depth] == segs]
        for child in sorted(children, key=lambda node: natural(node[-1])):
            collect(child)

    for root in sorted((node for node in tree if len(node) == 1),
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
        if not valid_segs(relative):
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


def load_sentence(arg: str) -> Sentence:
    match = re.fullmatch(r"[sS]?([1-9])(?:[sS]([1-9]))?", arg)
    if not match or (match[2] and match[1] >= match[2]):
        die(f"bad sentence {arg}: want sN or sNsM, N less than M")
    nums = [num for num in match.groups() if num]
    values = [os.environ.get(f"S{num}") for num in nums]
    dictionary = os.environ.get("DICT")
    if not all(values) or not dictionary:
        names = ", ".join(f"$S{num}" for num in nums)
        die(f"{names} or $DICT is unset; source ./setup.sh")
    return Sentence("s".join(nums), "".join(values), dictionary)


def main() -> int:
    parser = argparse.ArgumentParser(
        prog=PROGRAM, description="Builds new-pairs workflow files under "
        "tmp/sN; a path such as pb3/2k/pb3 is the build request.")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="print what would run")
    parser.add_argument("-f", "--force", action="store_true",
                        help="also rerun every step of each named path")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--status", action="store_true",
                      help="show the tree and what is stale")
    mode.add_argument("--report", action="store_true",
                      help="show tmp/sN/targets word ranks per node")
    mode.add_argument("--adopt", action="store_true",
                      help="move the old flat file names into the tree")
    parser.add_argument("sentence", metavar="sN|sNsM")
    parser.add_argument("paths", metavar="PATH", nargs="*")
    args = parser.parse_args()

    os.chdir(Path(__file__).resolve().parent)
    sentence = load_sentence(args.sentence)
    if args.status or args.report:
        (status if args.status else report)(sentence)
        return 0
    if not args.adopt and not args.paths:
        parser.error("give at least one PATH")
    with open("tmp/.rebuild-pairs.lock", "w") as lock:
        if not args.dry_run:
            fcntl.flock(lock, fcntl.LOCK_EX)
        if args.adopt:
            adopt(sentence)
            return 0
        paths = [parse_path(path) for path in args.paths]
        planner = Planner(sentence, set(paths) if args.force else set())
        for segs in paths:
            for step in steps_of(sentence, segs):
                planner.visit(step)
        total = len(planner.order)
        if total == 0:
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
