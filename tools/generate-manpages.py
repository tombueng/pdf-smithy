#!/usr/bin/env python3
"""Rewrites the COMMANDS section of docs/pdf-smithy-cli.1.in from the program.

Run after adding a command; tools/check-manpage.py runs as a test and will say
when this is overdue.

    tools/generate-manpages.py build/bin/pdf-smithy-cli
"""

import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) < 2:
        print("usage: generate-manpages.py path/to/pdf-smithy-cli", file=sys.stderr)
        return 1

    environment = dict(os.environ, LC_ALL="C")
    result = subprocess.run([sys.argv[1]], capture_output=True, text=True, env=environment)
    text = result.stderr or result.stdout

    rows, inside = [], False
    for line in text.splitlines():
        if line.strip() == "Commands:":
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            # name, the argument sketch, then the description after two spaces.
            match = re.match(r"\s{2}(\S+)\s+(\S.*?)\s{2,}(\S.*)$", line)
            if match:
                rows.append((match.group(1), match.group(2).strip(), match.group(3).strip()))

    if not rows:
        print("No command list found in the program's help output.", file=sys.stderr)
        return 1

    body = "\n".join(f'.TP\n.BI "{name}" " {args}"\n{description}' for name, args, description in rows)

    path = ROOT / "docs" / "pdf-smithy-cli.1.in"
    page = path.read_text(encoding="utf-8")
    before, rest = page.split(".SH COMMANDS\n", 1)
    _, after = rest.split(".SH OPTIONS", 1)
    path.write_text(before + ".SH COMMANDS\n" + body + "\n.SH OPTIONS" + after, encoding="utf-8")
    print(f"wrote {len(rows)} commands into {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
