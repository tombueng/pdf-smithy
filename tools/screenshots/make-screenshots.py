#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Takes every picture the handbook uses, from the recipes in shots.json.

The handbook's pictures are of a program that changes. A button moves, a panel
is renamed, a mode learns a gesture, and every picture is quietly a picture of
last month's window with nothing on screen saying so. The way out of that is to
keep the recipe rather than the picture, so that redoing all of them is one
command and redoing one is one command with a name after it.

    tools/screenshots/make-screenshots.py
    tools/screenshots/make-screenshots.py --only properties-a-form-field
    tools/screenshots/make-screenshots.py --list

It needs the shot helper, which is built apart from the application because
nobody installs it:

    cmake -S tools/screenshots -B /tmp/shotbuild -G Ninja -DCMAKE_BUILD_TYPE=Release
    ninja -C /tmp/shotbuild

and the showcase document, which is built by its own script and is the thing
every picture is of:

    tools/showcase/make-showcase.py

The language is pinned to English and the locale to C.UTF-8 on purpose. A
picture taken on a German desktop is a picture of the German window, and the
English handbook would then show a menu bar nobody reading it can follow.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MANIFEST = HERE / "shots.json"

# Where the helper is looked for, in order. The first is what the instructions
# above tell people to use; the second is for somebody who keeps their build
# trees together.
CANDIDATES = [
    Path("/tmp/shotbuild/bin/pdf-smithy-shot"),
    ROOT / "build-shots" / "bin" / "pdf-smithy-shot",
]


def find_helper(named: str | None) -> Path:
    if named:
        one = Path(named)
        if not one.is_file():
            sys.exit(f"there is no shot helper at {one}")
        return one
    for candidate in CANDIDATES:
        if candidate.is_file():
            return candidate
    sys.exit(
        "the shot helper was not found. Build it with\n"
        "    cmake -S tools/screenshots -B /tmp/shotbuild -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
        "    ninja -C /tmp/shotbuild\n"
        "or say where it is with --helper."
    )


def load() -> dict:
    with MANIFEST.open(encoding="utf-8") as handle:
        return json.load(handle)


def run_one(helper: Path, manifest: dict, shot: dict, out_dir: Path, dry: bool) -> bool:
    """Takes one picture. Returns True when it was written."""
    target = out_dir / f"{shot['name']}.png"

    document = shot.get("document", manifest["document"])
    command = [
        str(helper),
        "--open",
        str(ROOT / document),
        "--size",
        shot.get("size", manifest["size"]),
        "--settle",
        str(shot.get("settle", manifest["settle"])),
        "--final-settle",
        str(shot.get("final-settle", manifest["final-settle"])),
        "--grab",
        shot.get("grab", manifest["grab"]),
        "--answer-dialogs",
        shot.get("dialogs", manifest["dialogs"]),
        "--out",
        str(target),
    ]
    for step in shot["steps"]:
        command += ["--step", step]

    if dry:
        print(" ".join(command))
        return True

    # Offscreen, so that it runs the same over SSH, in a terminal and in CI, and
    # so that a developer's own desktop is not part of the picture.
    environment = dict(os.environ)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["LANGUAGE"] = "en"
    environment["LC_ALL"] = "C.UTF-8"

    finished = subprocess.run(
        command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=300
    )
    if finished.returncode != 0:
        print(f"  {shot['name']}: FAILED")
        for line in (finished.stdout + finished.stderr).splitlines():
            if line.strip() and "propagateSizeHints" not in line:
                print(f"    {line}")
        return False

    print(f"  {shot['name']}: {finished.stdout.strip().split()[-1]}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", action="append", metavar="NAME",
                        help="take just this picture; repeatable")
    parser.add_argument("--list", action="store_true",
                        help="say what each picture is of, and take none")
    parser.add_argument("--helper", metavar="PATH",
                        help="where the shot helper is, when it is not where it usually is")
    parser.add_argument("--out-dir", metavar="DIR",
                        help="write the pictures here instead of doc/screenshots")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the command each picture would need, and run none")
    arguments = parser.parse_args()

    manifest = load()
    shots = manifest["shots"]

    if arguments.only:
        known = {shot["name"] for shot in shots}
        unknown = [name for name in arguments.only if name not in known]
        if unknown:
            sys.exit(f"no such picture: {', '.join(unknown)}. --list says what there is.")
        shots = [shot for shot in shots if shot["name"] in arguments.only]

    if arguments.list:
        for shot in shots:
            print(f"{shot['name']}")
            print(f"    shows    {shot['shows']}")
            print(f"    used in  {shot['used-in']}")
        return 0

    helper = find_helper(arguments.helper)
    out_dir = Path(arguments.out_dir) if arguments.out_dir else ROOT / manifest["out-dir"]
    out_dir.mkdir(parents=True, exist_ok=True)

    document = ROOT / manifest["document"]
    if not document.is_file() and not arguments.dry_run:
        sys.exit(f"{document} is missing. Build it with tools/showcase/make-showcase.py.")

    print(f"{len(shots)} pictures into {out_dir}")
    failed = []
    for shot in shots:
        if not run_one(helper, manifest, shot, out_dir, arguments.dry_run):
            failed.append(shot["name"])

    if failed:
        print(f"\n{len(failed)} could not be taken: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
