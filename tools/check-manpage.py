#!/usr/bin/env python3
"""Fails when the manual lists commands the program does not have, or vice versa.

A manual that has drifted is worse than none: it sends people to a command that
was renamed three releases ago and makes them doubt the parts that are still
right. The command table in docs/pdf-smithy-cli.1.in is generated from the
program's own help output, so the two can only disagree if somebody added a
command and forgot the page, which is exactly the moment to be told.

    tools/check-manpage.py path/to/pdf-smithy-cli
"""

import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def commands_from_help(binary):
    environment = dict(os.environ, LC_ALL="C")
    # With no arguments the program prints its help to stderr and exits nonzero.
    result = subprocess.run([binary], capture_output=True, text=True, env=environment)
    text = result.stderr or result.stdout

    found, inside = set(), False
    for line in text.splitlines():
        if line.strip() == "Commands:":
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            match = re.match(r"\s{2}(\S+)\s", line)
            if match:
                found.add(match.group(1))
    return found


def commands_from_page():
    page = (ROOT / "docs" / "pdf-smithy-cli.1.in").read_text(encoding="utf-8")
    section = page.split(".SH COMMANDS", 1)
    if len(section) != 2:
        return set()
    return set(re.findall(r'^\.BI "(\S+)"', section[1].split(".SH OPTIONS", 1)[0], re.M))


def main():
    if len(sys.argv) < 2:
        print("usage: check-manpage.py path/to/pdf-smithy-cli", file=sys.stderr)
        return 1

    program = commands_from_help(sys.argv[1])
    manual = commands_from_page()

    if not program:
        print("Could not read the command list out of the program.", file=sys.stderr)
        return 1
    if not manual:
        print("Could not read the command list out of the manual page.", file=sys.stderr)
        return 1

    missing = sorted(program - manual)
    extra = sorted(manual - program)
    if missing or extra:
        if missing:
            print("Commands the program has and the manual does not:", ", ".join(missing), file=sys.stderr)
        if extra:
            print("Commands the manual has and the program does not:", ", ".join(extra), file=sys.stderr)
        print("\nRegenerate the table with tools/generate-manpages.py.", file=sys.stderr)
        return 1

    print(f"The manual and the program agree on all {len(program)} commands.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
