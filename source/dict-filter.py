#!/usr/bin/env python3

import signal
import sys


DICTIONARY = "/usr/share/dict/words.big"


def main() -> None:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} TEXT_FILE")

    with open(DICTIONARY, encoding="utf-8") as dictionary_file:
        dictionary = {line.rstrip("\r\n") for line in dictionary_file}

    with open(sys.argv[1], encoding="utf-8") as input_file:
        for line in input_file:
            fields = line.rstrip("\r\n").split(" ")
            if len(fields) > 1 and all(word in dictionary for word in fields[1:]):
                print(line, end="")


if __name__ == "__main__":
    main()
