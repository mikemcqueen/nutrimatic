#!/usr/bin/env python3

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile

from collections import Counter
from pathlib import Path
from typing import Any, IO


PROGRAM = "select-segment"
SENTENCE_RE = re.compile(r"(?:^|\.)s([1-9]\d*)(?=\.|$)")
LETTER_SET_RE = re.compile(r"(?:^|\.)([ou])-([a-z]+)(?=\.|$)")
SEGMENT_RE = re.compile(r"[a-z]+(?:[ ,][a-z]+)?")


class DiagnosticError(Exception):
    """An error that should be printed without a traceback."""


def parse_nonnegative_integer(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "must be a non-negative integer"
        ) from error
    if value < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return value


def parse_segment(text: str) -> str:
    if SEGMENT_RE.fullmatch(text) is None:
        raise argparse.ArgumentTypeError(
            "must be one alphabetic sequence, or two alphabetic sequences "
            "separated by one space or comma"
        )
    return text


def one_filename_match(
    pattern: re.Pattern[str], filename: str, description: str
) -> re.Match[str]:
    matches = list(pattern.finditer(filename))
    if len(matches) != 1:
        raise DiagnosticError(
            f'cannot parse {description} from result filename "{filename}"'
        )
    return matches[0]


def parse_result_filename(path: Path) -> tuple[str, str, str]:
    filename = path.name
    sentence_match = one_filename_match(
        SENTENCE_RE, filename, "sentence number"
    )
    letter_set_match = one_filename_match(
        LETTER_SET_RE, filename, "used/only letters"
    )
    return (
        f"s{sentence_match.group(1)}",
        letter_set_match.group(1),
        letter_set_match.group(2),
    )


def workflow_root() -> Path:
    value = os.environ.get("WFROOT")
    if value is None:
        raise DiagnosticError("WFROOT is not set")
    if not value:
        raise DiagnosticError("WFROOT is empty")
    root = Path(value)
    if not root.is_dir():
        raise DiagnosticError(f'WFROOT is not a directory: "{root}"')
    return root


def sentence_letters(root: Path, sentence: str) -> str:
    path = root / ".wf" / "best" / sentence / "letters"
    try:
        status = path.stat()
    except OSError as error:
        detail = error.strerror or str(error)
        raise DiagnosticError(f'cannot stat letters file "{path}": {detail}') \
            from error
    if not path.is_file():
        raise DiagnosticError(f'letters file is not a plain file: "{path}"')
    if status.st_size == 0:
        raise DiagnosticError(f'letters file is empty: "{path}"')
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise DiagnosticError(f'cannot read letters file "{path}": {error}') \
            from error
    return "".join(character for character in text if character.isalpha())


def subtract_letters(letters: str, used: str) -> str:
    remaining = Counter(letters)
    remaining.subtract(used)
    if any(count < 0 for count in remaining.values()):
        raise DiagnosticError(
            "used letters in result filename are not a subset of "
            "the sentence letters"
        )
    return "".join(sorted(remaining.elements()))


def segment_letters(segment: str) -> str:
    return "".join(character for character in segment if character.isalpha())


def remove_segment(
    segment_list: list[str], selected_segment: str
) -> list[str]:
    normalized_segment = selected_segment.replace(" ", ",")
    try:
        index = segment_list.index(normalized_segment)
    except ValueError as error:
        raise DiagnosticError(
            f'selected segment not found: "{selected_segment}"'
        ) from error
    return segment_list[:index] + segment_list[index + 1:]


def build_segment_letter_map(
    segment_list: list[str], remaining_letters: str, *, quiet: bool = True
) -> dict[str, str]:
    result = {}
    for segment in segment_list:
        try:
            result[segment] = subtract_letters(
                remaining_letters, segment_letters(segment)
            )
        except DiagnosticError:
            if not quiet:
                print(
                    f'{PROGRAM}: segment cannot be made from remaining '
                    f'letters: "{segment}"',
                    file=sys.stderr,
                )
            continue
    return result


def _display_segment_hierarchy(
    selected_segment: str,
    segment_list: list[str],
    remaining_letters: str,
    total: int,
    depth: int,
    *,
    quiet: bool,
) -> None:
    segment_letter_map = build_segment_letter_map(
        segment_list, remaining_letters, quiet=quiet
    )
    print(
        f'{"  " * depth}{selected_segment} '
        f'({len(segment_letter_map)}/{total})',
        file=sys.stderr,
    )

    compatible_segments = list(segment_letter_map)
    for segment, child_remaining_letters in segment_letter_map.items():
        child_segment_list = remove_segment(
            compatible_segments, segment
        )
        _display_segment_hierarchy(
            segment,
            child_segment_list,
            child_remaining_letters,
            len(compatible_segments),
            depth + 1,
            quiet=quiet,
        )


def display_segment_hierarchy(
    segment_list: list[str],
    selected_segment: str,
    remaining_letters: str,
    *,
    quiet: bool = True,
) -> None:
    segment_list = remove_segment(segment_list, selected_segment)
    try:
        remaining_letters = subtract_letters(
            remaining_letters, segment_letters(selected_segment)
        )
    except DiagnosticError as error:
        raise DiagnosticError(
            f'selected segment is not contained in remaining letters: '
            f'"{selected_segment}"'
        ) from error
    _display_segment_hierarchy(
        selected_segment,
        segment_list,
        remaining_letters,
        len(segment_list),
        0,
        quiet=quiet,
    )


def grep_segment_results(
    dfs_results: str, segment: str
) -> tuple[str, int]:
    result_segment = segment.replace(",", " ")
    pattern = (
        rf"(^[^ ]+ |,){re.escape(result_segment)}(,|$)"
    )
    path_segment = segment.replace(" ", "_").replace(",", "_")
    filtered_path = Path(f"{dfs_results}.{path_segment}")
    created = False

    try:
        with filtered_path.open("xb") as filtered:
            created = True
            completed = subprocess.run(
                ["grep", "-E", "--", pattern, dfs_results],
                stdout=filtered,
            )
        if completed.returncode not in (0, 1):
            raise DiagnosticError(
                f'grep failed with exit status {completed.returncode}'
            )
        with filtered_path.open("rb") as filtered:
            count = sum(1 for _ in filtered)
    except FileNotFoundError as error:
        if created:
            filtered_path.unlink(missing_ok=True)
        if error.filename == "grep":
            raise DiagnosticError('command not found: "grep"') from error
        raise DiagnosticError(
            f'cannot create result file "{filtered_path}": {error}'
        ) from error
    except OSError as error:
        if created:
            filtered_path.unlink(missing_ok=True)
        raise DiagnosticError(
            f'cannot process result file "{filtered_path}": {error}'
        ) from error
    except BaseException:
        if created:
            filtered_path.unlink(missing_ok=True)
        raise

    return str(filtered_path), count


def _display_result_hierarchy(
    selected_segment: str,
    segment_list: list[str],
    remaining_letters: str,
    dfs_results: str,
    result_count: int,
    depth: int,
    *,
    quiet: bool,
) -> None:
    if result_count == 0:
        return

    print(
        f'{"  " * depth}{selected_segment} ({result_count})',
        file=sys.stderr,
    )

    segment_letter_map = build_segment_letter_map(
        segment_list, remaining_letters, quiet=quiet
    )
    compatible_segments = list(segment_letter_map)
    for segment, child_remaining_letters in segment_letter_map.items():
        child_segment_list = remove_segment(
            compatible_segments, segment
        )
        child_results, child_count = grep_segment_results(
            dfs_results, segment
        )
        try:
            _display_result_hierarchy(
                segment,
                child_segment_list,
                child_remaining_letters,
                child_results,
                child_count,
                depth + 1,
                quiet=quiet,
            )
        finally:
            Path(child_results).unlink(missing_ok=True)


def display_result_hierarchy(
    dfs_results: str,
    segment_list: list[str],
    selected_segment: str,
    remaining_letters: str,
    *,
    quiet: bool = True,
) -> None:
    segment_list = remove_segment(segment_list, selected_segment)
    try:
        remaining_letters = subtract_letters(
            remaining_letters, segment_letters(selected_segment)
        )
    except DiagnosticError as error:
        raise DiagnosticError(
            f'selected segment is not contained in remaining letters: '
            f'"{selected_segment}"'
        ) from error

    selected_results, result_count = grep_segment_results(
        dfs_results, selected_segment
    )
    try:
        _display_result_hierarchy(
            selected_segment,
            segment_list,
            remaining_letters,
            selected_results,
            result_count,
            0,
            quiet=quiet,
        )
    finally:
        Path(selected_results).unlink(missing_ok=True)


def remaining_letters(
    root: Path, sentence: str, form: str, named_letters: str
) -> str:
    letters = sentence_letters(root, sentence)
    if form == "o":
        return named_letters
    return subtract_letters(letters, named_letters)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog=PROGRAM,
        usage=(
            "%(prog)s -s SEGMENT [--top | --first] [-r FILE]... "
            "[-i FILE]... [-n N] RESULT-FILE"
        ),
        description=(
            "filter dfs-anagrams results and select candidate pair segments"
        ),
    )
    result.add_argument(
        "result_file", metavar="RESULT-FILE", help="dfs-anagrams result file"
    )
    result.add_argument(
        "-s", "--segment", required=True, type=parse_segment,
        help="one word or two words separated by one space or comma",
    )
    order = result.add_mutually_exclusive_group()
    order.add_argument(
        "--top", action="store_const", const="top-segments",
        dest="selector", help="select pairs by frequency (default)",
    )
    order.add_argument(
        "--first", action="store_const", const="first-segments",
        dest="selector", help="select pairs in encounter order",
    )
    result.set_defaults(selector="top-segments")
    result.add_argument(
        "-r", "--reject", action="append", default=[], metavar="FILE",
        help="discard rows containing pairs in FILE; may be repeated",
    )
    result.add_argument(
        "-i", "--ignore", action="append", default=[], metavar="FILE",
        help="do not select pairs in FILE; may be repeated",
    )
    result.add_argument(
        "-n", type=parse_nonnegative_integer, metavar="N",
        help="print at most N pairs",
    )
    return result


def run_command(
    argv: list[str], *, stdout: int | IO[Any], text: bool = False
) -> subprocess.CompletedProcess[Any]:
    try:
        return subprocess.run(
            argv, check=True, stdout=stdout, text=text
        )
    except FileNotFoundError as error:
        raise DiagnosticError(f'command not found: "{argv[0]}"') from error
    except subprocess.CalledProcessError as error:
        raise DiagnosticError(
            f'{argv[0]} failed with exit status {error.returncode}'
        ) from error


def filter_dfs_results(args: argparse.Namespace) -> str:
    filter_argv = ["filter-segments", "--wf"]
    for path in args.reject:
        filter_argv.extend(["--reject", path])
    filter_argv.append(args.result_file)

    filtered = tempfile.NamedTemporaryFile(mode="w+b", delete=False)
    try:
        run_command(filter_argv, stdout=filtered)
    except BaseException:
        filtered.close()
        Path(filtered.name).unlink(missing_ok=True)
        raise
    filtered.close()
    return filtered.name


def generate_segments(
    dfs_results: str, args: argparse.Namespace
) -> list[str]:
    selector_argv = [args.selector, "--pairs"]
    if args.n is not None:
        selector_argv.extend(["-n", str(args.n)])
    for path in args.ignore:
        selector_argv.extend(["--ignore", path])
    selector_argv.append(dfs_results)
    selected = run_command(
        selector_argv, stdout=subprocess.PIPE, text=True
    )

    return selected.stdout.splitlines()


def _select_segments_impl(
    dfs_results: str, args: argparse.Namespace, selected_segment: str,
    remaining: str
) -> None:
    segment_list = generate_segments(dfs_results, args)
    display_result_hierarchy(
        dfs_results, segment_list, selected_segment, remaining
    )


def select_segments(
    args: argparse.Namespace, selected_segment: str, remaining: str
) -> None:
    dfs_results = filter_dfs_results(args)
    try:
        _select_segments_impl(
            dfs_results, args, selected_segment, remaining
        )
    finally:
        Path(dfs_results).unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        sentence, form, named_letters = parse_result_filename(
            Path(args.result_file)
        )
        root = workflow_root()
        remaining = remaining_letters(
            root, sentence, form, named_letters
        )
        select_segments(args, args.segment, remaining)
    except DiagnosticError as error:
        print(f"{PROGRAM}: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main())
