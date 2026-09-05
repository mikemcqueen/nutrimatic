#!/usr/bin/env python3

import argparse
import re
import signal

from nutrimatic.counts import strip_count_prefix


DEFAULT_DICTIONARY = "/usr/share/dict/words"

NON_WORD_RE = re.compile(r"[^A-Za-z0-9]")


def clean_word(text: str) -> str:
    return NON_WORD_RE.sub("", text).lower()


def strip_line_prefix(line: str) -> str:
    return strip_count_prefix(line.rstrip("\r\n"))


def load_dictionary(path: str) -> set[str]:
    dictionary = set()
    with open(path, encoding="utf-8", errors="replace") as dictionary_file:
        for line in dictionary_file:
            line = strip_line_prefix(line)
            if "-" in line:
                continue
            word = clean_word(line)
            if word:
                dictionary.add(word)
    return dictionary


def main() -> None:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser(
        description=(
            "print lines of TEXT_FILE for which every space-separated word "
            "is in the dictionary, ignoring any leading count prefix"
        ),
    )
    parser.add_argument(
        "-v", action="store_true", help="select non-matching lines"
    )
    parser.add_argument(
        "-d", "--dict", metavar="FILE", action="append",
        help=(
            "dictionary file; may be repeated to union files "
            f"(default: {DEFAULT_DICTIONARY})"
        ),
    )
    parser.add_argument("text_file", metavar="TEXT_FILE")
    args = parser.parse_args()

    dictionary = set()
    for path in args.dict or [DEFAULT_DICTIONARY]:
        dictionary.update(load_dictionary(path))

    with open(args.text_file, encoding="utf-8") as input_file:
        for line in input_file:
            text = strip_line_prefix(line)
            words = text.split(" ")
            matches = all(word in dictionary for word in words)
            if matches != args.v:
                print(line, end="")


if __name__ == "__main__":
    main()
