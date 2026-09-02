#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import tempfile

from pathlib import Path


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


def remove_words(removals: Path) -> Path:
    dictionary = DICT_DIR / DICT_FILENAME
    removed = next_removed_file()
    temporary_files: list[Path] = []

    try:
        new_removed = make_tmp_file(removed)
        temporary_files.append(new_removed)
        new_dictionary = make_tmp_file(dictionary)
        temporary_files.append(new_dictionary)
        run_to_file(["sort", "-u", str(removals)], new_removed)
        run_to_file(
            ["comm", "-23", str(dictionary), str(removals)],
            new_dictionary,
        )
        if removed.exists():
            raise FileExistsError(
                f'removed-words file already exists: "{removed}"'
            )
        new_dictionary.replace(dictionary)
        new_removed.replace(removed)
    except BaseException:
        for temporary in temporary_files:
            temporary.unlink(missing_ok=True)
        raise

    return removed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", metavar="FILE", type=Path)
    args = parser.parse_args()
    remove_words(args.file)


if __name__ == "__main__":
    main()
