#!/usr/bin/env python3

import argparse
import signal
import sys

from counts import strip_count_prefix


def main() -> None:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    parser = argparse.ArgumentParser(
        description=(
            'strip counts from top-segments "count segment" output'
        ),
    )
    parser.add_argument(
        "input_file",
        metavar="INPUT_FILE",
        type=argparse.FileType("r", encoding="utf-8", errors="replace"),
    )
    args = parser.parse_args()

    with args.input_file:
        for line in args.input_file:
            sys.stdout.write(strip_count_prefix(line))


if __name__ == "__main__":
    main()
