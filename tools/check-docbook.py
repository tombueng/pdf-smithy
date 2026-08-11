#!/usr/bin/env python3
"""Fails when the handbook would not build, or reads as if two chapters were one.

F1 opens doc/index.docbook, and the only thing standing between a mistyped tag
and a user who presses F1 is the build, which happens on a developer's machine,
long after the mistake, and only when KDocTools is installed. A misplaced
</sect1> is therefore exactly the kind of fault that reaches a release, so it is
checked here in a second instead.

Two faults get their own checks because a validator has nothing to say about
either. An empty title compiles perfectly and produces a blank entry in the
table of contents, and two chapters under one heading leave the reader unable to
tell which of them a cross-reference meant.

    tools/check-docbook.py
"""

import glob
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HANDBOOK = ROOT / "doc" / "index.docbook"

# The handbook refers to its DTD as "dtd/kdedbx45.dtd", which resolves through an
# XML catalogue rather than through the file system. KDocTools ships the one that
# knows about the KDE variant of DocBook; the system catalogue knows about plain
# DocBook, which the KDE variant is built on top of, so both are needed and
# neither is guaranteed to be where the other distribution put it.
CATALOGUE_CANDIDATES = [
    "/etc/xml/catalog",
    "/usr/share/kf6/kdoctools/customization/catalog.xml",
    "/usr/local/share/kf6/kdoctools/customization/catalog.xml",
]
CATALOGUE_PATTERNS = [
    "/usr/share/*/kdoctools/customization/catalog.xml",
    "/usr/local/share/*/kdoctools/customization/catalog.xml",
    "/usr/share/xml/docbook/**/catalog.xml",
]

# xmllint says these when it could not find the DTD at all, as opposed to when it
# found one and the document broke its rules. The difference decides whether a
# failure is the handbook's fault or the machine's.
UNRESOLVED = ("failed to load", "no DTD found", "failed to load external entity")

TITLE = re.compile(r"<title\s*/>|<title(?:\s[^>]*)?>(.*?)</title>", re.S)
CHAPTER_TITLE = re.compile(r"<chapter(?:\s[^>]*)?>\s*<title(?:\s[^>]*)?>(.*?)</title>", re.S)


def catalogues():
    found = list(CATALOGUE_CANDIDATES)
    for pattern in CATALOGUE_PATTERNS:
        found.extend(sorted(glob.glob(pattern, recursive=True)))
    # Order is kept, because xmllint consults the catalogues in the order given
    # and the first answer wins.
    return [path for i, path in enumerate(found) if os.path.exists(path) and path not in found[:i]]


def run_xmllint(arguments):
    # Run from the handbook's own directory: a relative DTD reference is resolved
    # against the document, so a project that ever ships its own dtd/ directory
    # keeps working without this script being told about it.
    result = subprocess.run(
        ["xmllint", "--noout", *arguments, HANDBOOK.name],
        cwd=HANDBOOK.parent,
        capture_output=True,
        text=True,
        env=dict(os.environ, XML_CATALOG_FILES=" ".join(catalogues()), LC_ALL="C"),
    )
    return result.returncode, (result.stderr or result.stdout).strip()


def check_syntax():
    """Returns a list of complaints, and whether the DTD took part."""
    code, output = run_xmllint(["--valid"])
    if code == 0:
        return [], True

    if any(marker in output for marker in UNRESOLVED):
        code, output = run_xmllint([])
        if code == 0:
            return [], False
        return [f"The handbook is not well-formed XML:\n{output}"], False

    fault = "does not parse" if "parser error" in output else "does not follow the DocBook DTD"
    return [f"The handbook {fault}:\n{output}"], True


def check_titles(text):
    complaints = []

    for index, match in enumerate(TITLE.finditer(text), start=1):
        heading = (match.group(1) or "").strip()
        if not heading:
            line = text.count("\n", 0, match.start()) + 1
            complaints.append(f"Line {line}: title number {index} is empty.")

    headings = [" ".join(heading.split()) for heading in CHAPTER_TITLE.findall(text)]
    for heading in sorted({h for h in headings if headings.count(h) > 1}):
        complaints.append(f"Two chapters are both called “{heading}”.")

    return complaints, len(headings)


def main():
    if not HANDBOOK.exists():
        print(f"{HANDBOOK} is missing", file=sys.stderr)
        return 1

    text = HANDBOOK.read_text(encoding="utf-8")
    complaints, chapters = check_titles(text)

    if shutil.which("xmllint") is None:
        validated = None
    else:
        syntax_complaints, validated = check_syntax()
        complaints = syntax_complaints + complaints

    if complaints:
        for complaint in complaints:
            print(complaint, file=sys.stderr)
        return 1

    if validated is None:
        print("xmllint is not installed, so only the titles were checked.")
    elif validated:
        print(f"The handbook is valid DocBook, and its {chapters} chapters have distinct titles.")
    else:
        print(
            f"The handbook is well-formed and its {chapters} chapters have distinct titles. "
            "The KDE DocBook DTD could not be resolved, so it was not validated against it."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
