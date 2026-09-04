#!/usr/bin/env python3

import argparse
import signal

from nutrimatic.counts import strip_count_prefix


DEFAULT_DICTIONARY = "/usr/share/dict/words"


def main() -> None:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-v", action="store_true", help="select non-matching lines"
    )
    parser.add_argument(
        "-d", "--dict", metavar="FILE", default=DEFAULT_DICTIONARY,
        help="dictionary file (default: %(default)s)",
    )
    parser.add_argument("text_file", metavar="TEXT_FILE")
    args = parser.parse_args()

    with open(args.dict, encoding="utf-8") as dictionary_file:
        dictionary = {line.rstrip("\r\n") for line in dictionary_file}

    with open(args.text_file, encoding="utf-8") as input_file:
        for line in input_file:
            text = strip_count_prefix(line.rstrip("\r\n"))
            words = text.split(" ")
            matches = all(word in dictionary for word in words)
            if matches != args.v:
                print(line, end="")


if __name__ == "__main__":
    main()
