#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import sys
import tempfile

from pathlib import Path

from nutrimatic.counts import strip_count_prefix
from nutrimatic.fs import line_count


DICT_DIR = Path("~/code/nutrimatic/idx").expanduser()
DICT_FILENAME = "words.big"
REMOVED_FILENAME = DICT_FILENAME + ".removed.{n}"
REMOVED_FILENAME_RE = re.compile(
    rf"{re.escape(DICT_FILENAME)}\.removed\.([1-9][0-9]*)"
)


def next_removed_file() -> Path:
    numbers = []
    for path in DICT_DIR.iterdir():
        match = REMOVED_FILENAME_RE.fullmatch(path.name)
        if match is not None:
            numbers.append(int(match.group(1)))
    next_number = max(numbers, default=0) + 1
    return DICT_DIR / REMOVED_FILENAME.format(n=next_number)


def make_tmp_file(destination: Path) -> Path:
    temporary = tempfile.NamedTemporaryFile(
        dir=destination.parent,
        prefix=f".{destination.name}.",
        suffix=".tmp",
        delete=False,
    )
    temporary.close()
    return Path(temporary.name)


def c_env() -> dict[str, str]:
    return {**os.environ, "LC_ALL": "C"}


def run_to_file(argv: list[str], destination: Path) -> None:
    with destination.open("wb") as output:
        subprocess.run(argv, stdout=output, env=c_env(), check=True)


def write_stripped(source: Path, destination: Path) -> None:
    with source.open(encoding="utf-8") as input_file:
        with destination.open("w", encoding="utf-8") as output_file:
            for line in input_file:
                output_file.write(
                    strip_count_prefix(line.rstrip("\r\n")) + "\n"
                )


def remove_words(removals: Path, *, dry_run: bool = False) -> Path | None:
    dictionary = DICT_DIR / DICT_FILENAME
    removed = next_removed_file()
    temporary_files: list[Path] = []

    try:
        stripped = make_tmp_file(removed)
        temporary_files.append(stripped)
        new_removed = make_tmp_file(removed)
        temporary_files.append(new_removed)
        new_dictionary = make_tmp_file(dictionary)
        temporary_files.append(new_dictionary)

        write_stripped(removals, stripped)
        run_to_file(["sort", "-u", str(stripped)], new_removed)
        run_to_file(
            ["comm", "-23", str(dictionary), str(new_removed)],
            new_dictionary,
        )

        old_lines = line_count(dictionary)
        new_lines = line_count(new_dictionary)
        removed_lines = old_lines - new_lines
        print(
            f"dictionary has {old_lines} lines {dictionary}",
            file=sys.stderr,
        )
        print(
            f"removing {line_count(new_removed)} lines in {removals}",
            file=sys.stderr,
        )

        if removed_lines == 0:
            print("No dictionary lines removed", file=sys.stderr)
            return None

        if removed.exists():
            raise FileExistsError(
                f'removed-words file already exists: "{removed}"'
            )

        if dry_run:
            print(
                f"would remove {removed_lines} lines from dictionary",
                file=sys.stderr,
            )
            print(
                f"new dictionary would have {new_lines} lines",
                file=sys.stderr,
            )
            return None

        new_dictionary.replace(dictionary)
        new_removed.replace(removed)
        print(
            f"removed {removed_lines} lines from dictionary",
            file=sys.stderr,
        )
        print(f"new dictionary has {new_lines} lines", file=sys.stderr)
    finally:
        for temporary in temporary_files:
            temporary.unlink(missing_ok=True)

    return removed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", metavar="FILE", type=Path)
    parser.add_argument(
        "-n", "--dry-run", action="store_true",
        help="report what would change without modifying any file",
    )
    args = parser.parse_args()
    remove_words(args.file, dry_run=args.dry_run)


if __name__ == "__main__":
    main()
