#!/usr/bin/env python3

import re


COUNT_PREFIX_RE = re.compile(r"^ *[0-9]+ ")


def strip_count_prefix(text: str) -> str:
    return COUNT_PREFIX_RE.sub("", text)
