#!/usr/bin/env python3
"""Fails when user-facing text is written in a form the translators never see.

Messages.sh extracts only the KDE keywords: i18n, i18nc, i18np, i18ncp. A
string wrapped in QCoreApplication::translate or QObject::tr compiles, runs and
looks perfectly translatable, but never reaches the .pot file, so it stays
English for every user in every language. That happened once here, to the error
messages of the redaction engine of all things, and nothing caught it: the
build was clean, the tests passed and the catalogue reported itself complete.

Hence this. Widget subclasses that genuinely want tr() are not the pattern in
this codebase; if that ever changes, the exception belongs here in writing
rather than in someone's memory.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FORBIDDEN = re.compile(r"\b(QCoreApplication::translate|QObject::tr|(?<![\w:])tr)\s*\(\s*[\"u]")


def main():
    offences = []
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in {".cpp", ".h"}:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if FORBIDDEN.search(line):
                offences.append(f"{path.relative_to(ROOT)}:{number}: {stripped}")

    if offences:
        print("User-facing text that Messages.sh will not extract:\n", file=sys.stderr)
        for offence in offences:
            print("  " + offence, file=sys.stderr)
        print("\nUse i18n(), i18nc(), i18np() or i18ncp() instead.", file=sys.stderr)
        return 1

    print("All user-facing text uses the KDE i18n keywords.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
