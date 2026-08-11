#!/usr/bin/env python3
"""Folds the translation slices in po/slices/ into one table for fill-po.py.

Translating nine hundred strings is work that divides well, but a .po file does
not: it is one file, and several writers would mean one writer. So the messages
are split into slices, translated separately, and joined here.

Two shapes arrive:

  * done-N.json: singulars, `{key: "German"}`, where a context-qualified key
    is context + U+0004 + msgid, the separator gettext itself uses.
  * done-plural.json: `{key: ["singular", "plural"]}`, context separated by a
    vertical bar, because that is what fill-po.py already expected.

Both are normalised here to fill-po.py's format, which uses the bar throughout.

    tools/merge-translations.py            # writes po/slices/merged.json
    tools/fill-po.py po/de/pdf-smithy.po po/slices/merged.json
"""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SLICES = ROOT / "po" / "slices"


def main():
    singles, plurals = {}, {}
    collisions = []

    for path in sorted(SLICES.glob("done-*.json")):
        table = json.loads(path.read_text(encoding="utf-8"))
        for key, value in table.items():
            # gettext separates context from id with U+0004; fill-po.py uses a
            # bar. Same idea, and the bar cannot appear in a context here.
            normalised = key.replace("", "|")
            target = plurals if isinstance(value, list) else singles
            if normalised in target and target[normalised] != value:
                collisions.append((path.name, normalised))
            target[normalised] = value

    if collisions:
        # Two slices disagreeing about one string is not fatal (the last wins),
        # but it means the same message was in two slices, which is a mistake in
        # the split rather than in the translation, and worth saying out loud.
        print(f"note: {len(collisions)} strings appear in more than one slice", file=sys.stderr)
        for name, key in collisions[:5]:
            print(f"  {name}: {key[:70]}", file=sys.stderr)

    out = SLICES / "merged.json"
    out.write_text(json.dumps({"single": singles, "plural": plurals}, ensure_ascii=False, indent=1),
                   encoding="utf-8")
    print(f"wrote {out}: {len(singles)} singular, {len(plurals)} plural")


if __name__ == "__main__":
    main()
