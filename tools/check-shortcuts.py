#!/usr/bin/env python3
"""Fails when two actions ask for the same keyboard shortcut.

KXmlGuiWindow checks for this itself, and reacts by opening a modal message box
while the window is still being built. That greets the user at startup, and it
hangs the end-to-end test before it can reach the assertion written to catch
exactly this. A guard that only works once the window is up is no guard, so this
reads the source instead and takes a second.

Only the explicit shortcuts are checked. The ones KStandardAction hands out are
KDE's own and cannot collide with each other.
"""

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

CALL = re.compile(r'addAction\(QStringLiteral\("(?P<name>[a-z_0-9]+)"')
KEYS = re.compile(r'QKeySequence\((?P<keys>[^)]*)\)')


def main():
    source = ROOT / "src" / "ui" / "MainWindow.cpp"
    if not source.exists():
        print(f"{source} is missing", file=sys.stderr)
        return 1

    text = source.read_text(encoding="utf-8")

    # Each call is taken only as far as the next one starts. Reading past that,
    # which a single greedy pattern does whenever a call has no shortcut at all,
    # pairs a name with somebody else's keys, and a guard that names the wrong
    # action sends the next person hunting in the wrong file.
    calls = list(CALL.finditer(text))
    wanted = collections.defaultdict(list)
    for index, call in enumerate(calls):
        end = calls[index + 1].start() if index + 1 < len(calls) else len(text)
        for keys in KEYS.finditer(text[call.end():end]):
            spelling = " ".join(keys.group("keys").split())
            # QKeySequence::Delete and friends are standard sequences, not literals.
            if "::" in spelling and "Qt::" not in spelling:
                continue
            wanted[spelling].append(call.group("name"))

    clashes = {keys: names for keys, names in wanted.items() if len(names) > 1}
    if clashes:
        print("Two actions want the same shortcut:\n", file=sys.stderr)
        for keys, names in sorted(clashes.items()):
            print(f"  {keys}  ->  {', '.join(names)}", file=sys.stderr)
        print("\nKXmlGuiWindow answers this with a modal box during construction.", file=sys.stderr)
        return 1

    print(f"All {len(wanted)} explicit shortcuts are unique.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
