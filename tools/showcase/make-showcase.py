#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Builds the showcase document the handbook is illustrated from.

    tools/showcase/make-showcase.py [--cli PATH] [--out PATH] [--keep-work]

The result is tools/showcase/showcase.pdf: fifteen pages about an island that
does not exist, carrying one of everything the handbook needs a picture of.
Typography, a photograph, three vector plates, a page turned a quarter, a real
two page form, four kinds of comment and a nested table of contents.

## Why a script and not a file somebody made once

A showcase that cannot be rebuilt stops being true. A heading moves, a colour
changes, somebody adds a page, and the screenshots in the handbook quietly
start showing a document nobody can produce again. Everything here is made from
the prose in text/, the field description in form-fields.json, the comments in
comments.xfdf and the contents tree in outline.json, so any of those can be
edited and the document comes out the same way every time.

## Why the program's own command line

Almost every page is built by pdf-smithy-cli: typeset sets the text, "objects
insert" draws the charts and diagrams and places the photograph, "field
from-json" builds the form, "annotate --import-xfdf" adds the comments,
"outline --from-json" writes the contents, and "pages --rotate" turns the one
page that is meant to arrive turned. That is deliberate. Building the showcase
exercises the tools it is showing off, so a fault in either shows up here
first. The single exception is the photograph, which is arithmetic in
artwork.py, because a picture in a public repository must not be anybody's.

## Page numbers

Nothing in the data files carries an absolute page number, because a page added
to the foreword would silently move every comment onto the wrong sheet. The
data files number pages within their own section and this script maps them onto
the finished document from the page counts the typesetter reports.

Needs nothing installed beyond Python 3 and a built pdf-smithy-cli.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEXT = HERE / "text"
REPO = HERE.parent.parent

# A4 in points, and the margins every section is set to. Kept here rather than
# repeated down the file because the plates position their artwork against them.
PAGE_W, PAGE_H = 595.276, 841.89
MARGIN_L, MARGIN_R = 70.87, 70.87  # 25 mm
MARGIN_T, MARGIN_B = 62.36, 68.03  # 22 mm and 24 mm
TEXT_W = PAGE_W - MARGIN_L - MARGIN_R

# The house colours. One deep blue for headings and rules, one warm red for the
# things that want attention, and a grey that is not black for captions.
INK = "#1d3557"
ACCENT = "#c1442e"
QUIET = "#5b6570"
RULE = "#b9c4cf"

BODY_STYLE = [
    "--typeface", "Times",
    "--font-size", "10.5",
    "--leading", "14",
    "--align", "justify",
    "--hyphenate",
    "--language", "en",
    "--style", f"h1:font=Helvetica,bold,size=19,space-before=0,space-after=10,colour={INK}",
    "--style", f"h2:font=Helvetica,bold,size=12.5,space-before=15,space-after=5,colour={INK}",
    "--style", f"h3:font=Helvetica,bold,size=11,space-before=12,space-after=4,colour={QUIET}",
]

MARGINS = [
    "--margin-mm", "25",
    "--top-mm", "22",
    "--bottom-mm", "24",
]


_WIDTHS: dict[tuple[str, float, str], float] = {}


class Failure(RuntimeError):
    """Anything that should stop the build with a sentence a person can act on."""


class Cli:
    """The program under test, run with a locale that does not change its words."""

    def __init__(self, binary: Path, work: Path):
        self.binary = binary
        self.work = work
        self.calls = 0

    def __call__(self, *args: object) -> str:
        self.calls += 1
        command = [str(self.binary)] + [str(a) for a in args]
        environment = dict(os.environ, LC_ALL="C.UTF-8", LANGUAGE="en", LANG="C.UTF-8")
        finished = subprocess.run(command, capture_output=True, text=True, env=environment)
        if finished.returncode != 0:
            raise Failure(
                "pdf-smithy-cli exited {code} on:\n  {line}\n{err}{out}".format(
                    code=finished.returncode,
                    line=" ".join(command[1:]),
                    err=finished.stderr.strip(),
                    out=finished.stdout.strip(),
                )
            )
        return finished.stdout

    def pages(self, pdf: Path) -> int:
        return int(json.loads(self("info", pdf, "--json"))["pageCount"])


# ── Drawing ───────────────────────────────────────────────────────────────
#
# Every plate is a list of these, played onto a page by "objects insert". They
# exist so that the drawing below reads as a drawing rather than as forty
# command lines, and so that shapes sharing a colour go over in one call.


class Sheet:
    """One page being drawn on, and the calls that will draw it."""

    def __init__(self, cli: Cli, pdf: Path, page: int, work: Path):
        self.cli = cli
        self.pdf = pdf
        self.page = page
        self.work = work
        self.step = 0
        # The name a step writes to is built from this rather than from the
        # file it read, because a plate takes about forty steps and a name that
        # grows by one suffix each time runs past what the filesystem will hold.
        self.stem = pdf.stem

    def _insert(self, *args: object) -> None:
        self.step += 1
        target = self.work / f"{self.stem}-draw{self.step}.pdf"
        self.cli("objects", "insert", self.pdf, "--page", self.page, *args, "-o", target)
        self.pdf = target

    def box(self, rects, fill=None, stroke=None, width=None, opacity=None) -> None:
        self._shape("rectangle", rects, fill, stroke, width, opacity)

    def disc(self, rects, fill=None, stroke=None, width=None, opacity=None) -> None:
        self._shape("ellipse", rects, fill, stroke, width, opacity)

    def line(self, rects, stroke=INK, width=0.8, opacity=None) -> None:
        self._shape("line", rects, None, stroke, width, opacity)

    def rule(self, x, y, length, stroke=RULE, width=0.7) -> None:
        self.line([(x, y, length, 0)], stroke=stroke, width=width)

    def _shape(self, kind, rects, fill, stroke, width, opacity) -> None:
        args: list[object] = ["--shape", kind]
        for rect in rects:
            args += ["--rect", ",".join(f"{v:g}" for v in rect)]
        if fill:
            args += ["--fill", fill]
        if stroke:
            args += ["--stroke", stroke]
        if width is not None:
            args += ["--line-width", f"{width:g}"]
        if opacity is not None:
            args += ["--opacity", str(int(opacity))]
        self._insert(*args)

    def text(self, x, y, string, size=9, family="Helvetica", colour="#000000", width=400) -> None:
        self._insert(
            "--shape", "text",
            "--text", string,
            "--rect", f"{x:g},{y:g},{width:g},{size * 1.4:g}",
            "--font-family", family,
            "--font-size", f"{size:g}",
            "--fill", colour,
        )

    def measure(self, string, size, family) -> float:
        """
        How wide that text will be set, asked of the program rather than guessed.

        Ranging a column of numbers to the right needs the width to a point, and
        an average character width is wrong by a word by the end of a line. The
        answers are cached because a chart asks the same question for every
        gridline label and each ask is a process.
        """
        key = (string, size, family)
        if key not in _WIDTHS:
            answer = json.loads(
                self.cli("typeset", "measure", "--text", string, "--typeface", family,
                         "--font-size", f"{size:g}", "--json")
            )
            _WIDTHS[key] = float(answer["widthPoints"])
        return _WIDTHS[key]

    def right(self, x, y, string, size=9, family="Helvetica", colour="#000000") -> None:
        """Text ending at x."""
        self.text(x - self.measure(string, size, family), y, string, size=size, family=family, colour=colour)

    def centre(self, x, y, string, size=9, family="Helvetica", colour="#000000") -> None:
        self.text(x - self.measure(string, size, family) / 2.0, y, string, size=size, family=family, colour=colour)

    def picture(self, path: Path, rect, quality: int = 86) -> None:
        self._insert(
            "--shape", "image",
            "--image", path,
            "--rect", ",".join(f"{v:g}" for v in rect),
            "--quality", str(quality),
        )


# ── The sections ──────────────────────────────────────────────────────────


def typeset(cli: Cli, source: Path, target: Path, *extra: object) -> Path:
    cli("typeset", source, "-o", target, *MARGINS, *BODY_STYLE, *extra)
    return target


def caption(sheet: Sheet, x: float, y: float, lines: list[str], width: float = TEXT_W) -> None:
    """The italic note under a plate, with the hairline that separates it."""
    sheet.rule(x, y + 12, width)
    for i, line in enumerate(lines):
        sheet.text(x, y - i * 11, line, size=8.5, family="Times", colour=QUIET)


def build_cover(cli: Cli, work: Path) -> Path:
    pdf = typeset(
        cli, TEXT / "01-cover.md", work / "01-cover.pdf",
        "--title", "Kestrel Island Field Station Review",
        "--author-name", "The Kestrel Island Trust",
        "--top-mm", "78",
        "--align", "left",
        "--style", f"h1:font=Helvetica,bold,size=40,space-after=18,colour={INK}",
        "--style", f"h2:font=Helvetica,size=17,space-before=0,space-after=26,colour={ACCENT}",
        "--style", "body:size=11,leading=16,align=left",
    )
    sheet = Sheet(cli, pdf, 1, work)

    # An emblem rather than a logo: a sun over water, drawn out of the same
    # three shapes the plates use, so the cover is made of the document's own
    # vocabulary instead of a picture pasted onto it.
    cx, cy, r = PAGE_W / 2.0, 300.0, 96.0
    sheet.disc([(cx - r, cy - r, 2 * r, 2 * r)], fill="#eaf0f6")
    sheet.disc([(cx - r, cy - r, 2 * r, 2 * r)], stroke=INK, width=1.2)
    sheet.disc([(cx - 30, cy + 6, 60, 60)], fill="#f0a04b", opacity=90)
    for i in range(7):
        y = cy + 2 - i * 9
        half = (r * r - (y - cy) ** 2) ** 0.5 if abs(y - cy) < r else 0
        if half > 6:
            sheet.line([(cx - half + 4, y, 2 * half - 8, 0)], stroke=INK, width=1.0 if i % 2 else 1.8)

    sheet.rule(MARGIN_L, 168, TEXT_W, stroke=RULE, width=0.9)
    sheet.text(MARGIN_L, 148, "Thirty-first season", size=9.5, family="Helvetica", colour=QUIET)
    sheet.right(PAGE_W - MARGIN_R, 148, "Not a real place", size=9.5, family="Helvetica", colour=QUIET)
    return sheet.pdf


def build_plate_photograph(cli: Cli, work: Path) -> Path:
    from artwork import write_sunrise

    picture = work / "north-shore.png"
    write_sunrise(picture, 1280, 800)

    pdf = typeset(cli, TEXT / "05-plate-photograph.md", work / "05-plate.pdf", "--footer", "{page}")
    sheet = Sheet(cli, pdf, 1, work)

    height = TEXT_W * 800.0 / 1280.0
    top = 600.0
    sheet.picture(picture, (MARGIN_L, top - height, TEXT_W, height))
    sheet.box([(MARGIN_L, top - height, TEXT_W, height)], stroke=INK, width=0.8)
    caption(
        sheet, MARGIN_L, top - height - 26,
        [
            "The north shore at the hour the counts begin, from the top of the path.",
            "The near headland is the one that shelters the landing; the far one is across the sound.",
        ],
    )
    return sheet.pdf


def build_plate_chart(cli: Cli, work: Path) -> Path:
    pdf = typeset(cli, TEXT / "06-plate-chart.md", work / "06-plate.pdf", "--footer", "{page}")
    sheet = Sheet(cli, pdf, 1, work)

    months = ["One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten"]
    ringed = [0, 38, 96, 74, 61, 55, 88, 132, 168, 121]

    left, bottom = MARGIN_L + 34, 250.0
    plot_w, plot_h = TEXT_W - 34, 300.0
    top_value = 180.0
    slot = plot_w / len(ringed)
    bar_w = slot * 0.62

    # The grid first, so the bars stand on it rather than behind it.
    for value in range(0, int(top_value) + 1, 30):
        y = bottom + plot_h * value / top_value
        sheet.rule(left, y, plot_w, stroke="#e2e8ee" if value else RULE, width=0.7)
        sheet.right(left - 8, y - 3, str(value), size=8, colour=QUIET)

    bars = []
    for i, value in enumerate(ringed):
        if value == 0:
            continue
        x = left + i * slot + (slot - bar_w) / 2.0
        bars.append((x, bottom, bar_w, plot_h * value / top_value))
    sheet.box(bars, fill=INK, opacity=88)

    # The best month, said twice: once in colour and once in words, because a
    # chart that needs its caption read to be understood is half a chart.
    best = ringed.index(max(ringed))
    x = left + best * slot + (slot - bar_w) / 2.0
    sheet.box([(x, bottom, bar_w, plot_h * max(ringed) / top_value)], fill=ACCENT)

    for i, value in enumerate(ringed):
        centre = left + i * slot + slot / 2.0
        sheet.centre(centre, bottom - 14, months[i], size=8, colour=QUIET)
        if value:
            sheet.centre(centre, bottom + plot_h * value / top_value + 5, str(value), size=8, colour=INK)

    sheet.line([(left, bottom, plot_w, 0)], stroke=INK, width=1.1)
    sheet.text(MARGIN_L, bottom + plot_h + 22, "Birds ringed each month of the season",
               size=10, family="Helvetica", colour=INK)
    sheet.right(PAGE_W - MARGIN_R, bottom + plot_h + 22, "833 in all", size=9, colour=ACCENT)

    caption(
        sheet, MARGIN_L, bottom - 42,
        [
            "The first month is not a low bar. It is no bar, because there was nobody on the island to count.",
            "The ninth month, in red, is the best the station has recorded.",
        ],
    )
    return sheet.pdf


def build_plate_diagram(cli: Cli, work: Path) -> Path:
    pdf = typeset(cli, TEXT / "07-plate-diagram.md", work / "07-plate.pdf", "--footer", "{page}")
    sheet = Sheet(cli, pdf, 1, work)

    sea_top, sea_bottom = 470.0, 250.0
    well_x, well_w = 210.0, 76.0
    well_bottom, well_top = 262.0, 560.0

    # Water, then the sea bed, then the well standing in both.
    sheet.box([(MARGIN_L, sea_bottom, TEXT_W, sea_top - sea_bottom)], fill="#a8c8e0", opacity=45)
    sheet.box([(MARGIN_L, sea_bottom - 18, TEXT_W, 18)], fill="#c8bfae")
    sheet.rule(MARGIN_L, sea_top, TEXT_W, stroke="#4a86b8", width=1.1)

    sheet.box([(well_x, well_bottom, well_w, well_top - well_bottom)], fill="#ffffff", opacity=92)
    sheet.box([(well_x, well_bottom, well_w, well_top - well_bottom)], stroke=INK, width=1.2)
    # The water standing inside the well, a little below the sea outside it.
    sheet.box([(well_x + 3, well_bottom + 3, well_w - 6, sea_top - 12 - well_bottom)], fill="#a8c8e0", opacity=70)

    # The inlet, drawn as a gap in the wall rather than a hole in the drawing.
    sheet.box([(well_x - 9, well_bottom + 6, 9, 9)], fill="#ffffff")
    sheet.line([(well_x - 9, well_bottom + 6, 9, 0), (well_x - 9, well_bottom + 15, 9, 0)], stroke=INK, width=1.0)

    # Float, shaft, counter, card.
    sheet.disc([(well_x + well_w / 2 - 17, sea_top - 24, 34, 22)], fill="#f0a04b", stroke=INK, width=1.0)
    sheet.line([(well_x + well_w / 2, sea_top - 8, 0, well_top + 34 - sea_top + 8)], stroke=INK, width=1.4)
    sheet.box([(well_x - 6, well_top + 34, well_w + 12, 40)], fill="#eef2f6", stroke=INK, width=1.2)
    sheet.centre(well_x + well_w / 2, well_top + 49, "counter", size=9, colour=INK)
    sheet.box([(well_x + well_w + 34, well_top + 40, 54, 28)], fill="#ffffff", stroke=QUIET, width=0.9)
    sheet.centre(well_x + well_w + 61, well_top + 50, "card", size=9, colour=QUIET)
    sheet.line([(well_x + well_w + 6, well_top + 54, 28, 0)], stroke=QUIET, width=0.9)
    sheet.disc([(well_x + well_w + 30, well_top + 51.5, 5, 5)], fill=QUIET)

    labels = [
        (MARGIN_L, sea_top + 8, "sea outside, chop and all", "#2f6690"),
        (well_x + well_w + 16, sea_top - 20, "float", INK),
        (well_x + well_w + 16, (sea_top + well_top) / 2 - 6, "shaft", INK),
        (MARGIN_L, well_bottom + 4, "inlet, too small to pass a wave", INK),
        (MARGIN_L, sea_bottom - 14, "sea bed", QUIET),
    ]
    for x, y, string, colour in labels:
        sheet.text(x, y, string, size=8.5, colour=colour)

    # Leaders from the two labels that sit away from the thing they name.
    sheet.line([(well_x + well_w, sea_top - 14, 14, 0)], stroke=INK, width=0.7)
    sheet.disc([(well_x + well_w + 11, sea_top - 15.5, 3, 3)], fill=INK)
    sheet.line([(MARGIN_L + 118, well_bottom + 8, well_x - 128, 0)], stroke=INK, width=0.7)
    sheet.disc([(well_x - 12, well_bottom + 6.5, 3, 3)], fill=INK)

    caption(
        sheet, MARGIN_L, 214,
        [
            "The stilling well in the sound. Nothing in the chain needs power except the counter,",
            "and the counter runs a year on two cells. The card is fetched on foot once a fortnight.",
        ],
    )
    return sheet.pdf


def build_plate_colour(cli: Cli, work: Path) -> Path:
    pdf = typeset(cli, TEXT / "08-plate-colour.md", work / "08-plate.pdf", "--footer", "{page}")
    sheet = Sheet(cli, pdf, 1, work)

    washes = [
        ("Slip grey", "#5d6b73"),
        ("Boat house blue", "#2f6690"),
        ("Rope", "#c98b3a"),
        ("Rust", "#a8412c"),
    ]

    cx, cy, r = PAGE_W / 2.0, 430.0, 82.0
    places = [(cx - 58, cy + 34), (cx + 58, cy + 34), (cx - 58, cy - 34), (cx + 58, cy - 34)]
    for (name, colour), (x, y) in zip(washes, places):
        sheet.disc([(x - r, y - r, 2 * r, 2 * r)], fill=colour, opacity=55)
    for (name, colour), (x, y) in zip(washes, places):
        sheet.disc([(x - r, y - r, 2 * r, 2 * r)], stroke=colour, width=1.4)

    # The same four colours laid flat, so the reader can see what each one is
    # before the others are put over it.
    swatch_w, gap = 96.0, 16.0
    left = (PAGE_W - (4 * swatch_w + 3 * gap)) / 2.0
    for i, (name, colour) in enumerate(washes):
        x = left + i * (swatch_w + gap)
        sheet.box([(x, 258, swatch_w, 44)], fill=colour)
        sheet.box([(x, 258, swatch_w, 44)], stroke="#ffffff", width=1.0)
        sheet.centre(x + swatch_w / 2, 244, name, size=8, colour=QUIET)
        sheet.centre(x + swatch_w / 2, 232, colour, size=7.5, family="Courier", colour=QUIET)

    caption(
        sheet, MARGIN_L, 202,
        [
            "Four washes at fifty-five parts in a hundred, and the same four laid flat underneath.",
            "The rings are stroked at full strength, which is why they survive being overlapped.",
        ],
    )
    return sheet.pdf


def build_form_pages(cli: Cli, work: Path) -> tuple[Path, Path]:
    """
    The two form pages, drawn but not yet fielded.

    The captions are placed rather than flowed, because a label has to line up
    with the box it names to within a point or two and no typesetter can be
    asked to guarantee that against a field whose position is written down
    somewhere else.
    """
    permit = typeset(cli, TEXT / "11-permit.md", work / "11-permit.pdf", "--footer", "{page}")
    sheet = Sheet(cli, permit, 1, work)
    layout = form_layout()

    for group in layout["permit"]["headings"]:
        sheet.text(MARGIN_L, group["y"], group["text"], size=10, family="Helvetica", colour=INK)
        sheet.rule(MARGIN_L, group["y"] - 6, TEXT_W, stroke=RULE, width=0.7)
    for label in layout["permit"]["labels"]:
        sheet.text(label["x"], label["y"], label["text"], size=9, family="Helvetica", colour=QUIET)
    sheet.text(MARGIN_L, layout["permit"]["note_y"], layout["permit"]["note"], size=8,
               family="Times", colour=ACCENT)
    permit = sheet.pdf

    declaration = typeset(cli, TEXT / "12-declaration.md", work / "12-declaration.pdf", "--footer", "{page}")
    sheet = Sheet(cli, declaration, 1, work)
    for group in layout["declaration"]["headings"]:
        sheet.text(MARGIN_L, group["y"], group["text"], size=10, family="Helvetica", colour=INK)
        sheet.rule(MARGIN_L, group["y"] - 6, TEXT_W, stroke=RULE, width=0.7)
    for label in layout["declaration"]["labels"]:
        sheet.text(label["x"], label["y"], label["text"], size=9, family="Helvetica", colour=QUIET)
    sheet.text(MARGIN_L, layout["declaration"]["note_y"], layout["declaration"]["note"], size=8,
               family="Times", colour=ACCENT)
    return permit, sheet.pdf


def form_layout() -> dict:
    """
    Where the captions go, read from the same file the fields are read from.

    Keeping both in one file is what stops a label and its box drifting apart:
    move a field in form-fields.json and its caption moves with it.
    """
    fields = json.loads((HERE / "form-fields.json").read_text(encoding="utf-8"))
    furniture = json.loads((HERE / "form-furniture.json").read_text(encoding="utf-8"))

    layout = {}
    for section, content in furniture.items():
        layout[section] = {
            "headings": content["headings"],
            "labels": list(content["labels"]),
            "note": content["note"],
            "note_y": content["noteY"],
        }

    section_of = {1: "permit", 2: "declaration"}
    for field in fields:
        labels = layout[section_of[field.get("page", 1)]]["labels"]

        caption_text = field.get("caption")
        if caption_text:
            rect = field.get("rect") or field["rects"][0]
            if field.get("captionAt", "above") == "above":
                labels.append({"x": rect["x"], "y": rect["y"] + rect["height"] + 5, "text": caption_text})
            else:
                labels.append(
                    {"x": rect["x"] + rect["width"] + 8, "y": rect["y"] + rect["height"] / 2 - 3.2,
                     "text": caption_text}
                )

        # A radio group has one rectangle per choice, and every one of them
        # needs its choice written beside it. The tooltip is not a label: it is
        # not on the paper, and a printed copy of the form would ask the reader
        # to pick between three unmarked circles.
        for i, place in enumerate(field.get("rects", [])):
            if i < len(field.get("options", [])):
                labels.append(
                    {"x": place["x"] + place["width"] + 8, "y": place["y"] + place["height"] / 2 - 3.2,
                     "text": field["options"][i]}
                )
    return layout


# ── Putting it together ───────────────────────────────────────────────────


def assemble(cli: Cli, work: Path, out: Path) -> dict[str, int]:
    """Builds every section, merges them, and returns where each section starts."""
    sections: list[tuple[str, Path]] = []

    sections.append(("cover", build_cover(cli, work)))
    sections.append(("contents", typeset(cli, TEXT / "02-contents.md", work / "02-contents.pdf",
                                         "--footer", "{page}")))
    sections.append(("foreword", typeset(cli, TEXT / "03-foreword.md", work / "03-foreword.pdf",
                                         "--header", "{title}", "--footer", "{page}",
                                         "--title", "Kestrel Island Field Station Review")))
    sections.append(("spread", typeset(cli, TEXT / "04-spread.md", work / "04-spread.pdf",
                                       "--columns", "2", "--gutter", "18", "--font-size", "9.6",
                                       "--leading", "12.8",
                                       "--header", "{title}", "--footer", "{page}",
                                       "--title", "The season in brief")))
    sections.append(("plate-photograph", build_plate_photograph(cli, work)))
    sections.append(("plate-chart", build_plate_chart(cli, work)))
    sections.append(("plate-diagram", build_plate_diagram(cli, work)))
    sections.append(("plate-colour", build_plate_colour(cli, work)))

    # The wide sheet is set on paper turned the long way round, which is not the
    # same thing as a page that has been turned afterwards. The next section is
    # one of those, and the two sitting next to one another is the point.
    sections.append(("timeline", typeset(cli, TEXT / "09-timeline.md", work / "09-timeline.pdf",
                                         "--size", "297x210mm", "--margin-mm", "18",
                                         "--font-size", "9.5", "--leading", "13",
                                         "--footer", "{page}")))
    sections.append(("turned", typeset(cli, TEXT / "10-turned.md", work / "10-turned.pdf",
                                       "--footer", "{page}")))

    permit, declaration = build_form_pages(cli, work)
    sections.append(("permit", permit))
    sections.append(("declaration", declaration))

    sections.append(("remarks", typeset(cli, TEXT / "13-remarks.md", work / "13-remarks.pdf",
                                        "--footer", "{page}")))
    sections.append(("colophon", typeset(cli, TEXT / "14-colophon.md", work / "14-colophon.pdf",
                                         "--footer", "{page}")))

    starts: dict[str, int] = {}
    page = 1
    for name, pdf in sections:
        starts[name] = page
        page += cli.pages(pdf)
    total = page - 1

    merged = work / "merged.pdf"
    cli("merge", *[pdf for _, pdf in sections], "-o", merged)
    if cli.pages(merged) != total:
        raise Failure(f"merge produced {cli.pages(merged)} pages where {total} were expected")

    # The page that arrived turned. /Rotate changes, the bytes underneath do not.
    turned = work / "turned.pdf"
    cli("pages", merged, "-o", turned, "--rotate", f"{starts['turned']}:90")

    fielded = add_form(cli, work, turned, starts)
    commented = add_comments(cli, work, fielded, starts)
    final = add_outline(cli, work, commented, starts)

    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(final, out)
    return dict(starts, __total__=total)


def add_form(cli: Cli, work: Path, pdf: Path, starts: dict[str, int]) -> Path:
    """Builds the permit from its description, then the behaviour on top of it."""
    described = json.loads((HERE / "form-fields.json").read_text(encoding="utf-8"))
    absolute = []
    for field in described:
        entry = {k: v for k, v in field.items() if k not in ("caption", "captionAt")}
        entry["page"] = starts["permit"] + field.get("page", 1) - 1
        absolute.append(entry)

    description = work / "form-absolute.json"
    description.write_text(json.dumps(absolute, indent=2), encoding="utf-8")

    built = work / "form.pdf"
    cli("field", "from-json", pdf, "--from-json", description, "-o", built)

    # A number that has to be a number, a date that shows as one, and the two
    # buttons. All three are actions rather than appearance, and all three are
    # things a form built by hand almost never has.
    validated = work / "form-validated.pdf"
    cli("field", "validate", built, "--name", "Stay.Nights", "--between", "1,21", "-o", validated)

    formatted = work / "form-formatted.pdf"
    cli("field", "format", validated, "--name", "Stay.Arrival", "--as", "date", "-o", formatted)

    reset = work / "form-reset.pdf"
    cli("field", "button", formatted, "--name", "Form.Clear", "--action", "reset", "-o", reset)

    sent = work / "form-send.pdf"
    cli("field", "button", reset, "--name", "Form.Send", "--action", "url",
        "--target", "https://example.invalid/kestrel/permit", "-o", sent)

    # The radio group is the one field a description cannot arrive switched on:
    # a group holds one value across several widgets, and which of them is on is
    # a property of the group rather than of any button in it. So it is filled
    # in afterwards, the same way a person would fill it in.
    filled = work / "form-filled.pdf"
    cli("form", sent, "--set", "Stay.Berth=bunkhouse", "-o", filled)
    return filled


def add_comments(cli: Cli, work: Path, pdf: Path, starts: dict[str, int]) -> Path:
    """
    The four marks, moved onto the page the remarks section turned out to be.

    XFDF counts pages from zero, and the file in the repository counts them from
    the start of its own section, so both corrections happen here.
    """
    tree = ElementTree.parse(HERE / "comments.xfdf")
    namespace = "http://ns.adobe.com/xfdf/"
    ElementTree.register_namespace("", namespace)
    for element in tree.getroot().iter():
        if "page" in element.attrib:
            within = int(element.attrib["page"])
            element.set("page", str(starts["remarks"] - 1 + within))

    moved = work / "comments-absolute.xfdf"
    tree.write(moved, encoding="utf-8", xml_declaration=True)

    marked = work / "marked.pdf"
    cli("annotate", pdf, "--import-xfdf", moved, "-o", marked)
    return marked


def add_outline(cli: Cli, work: Path, pdf: Path, starts: dict[str, int]) -> Path:
    """The contents, written last, from the sections that turned out to exist."""

    def resolve(entries: list[dict]) -> list[dict]:
        out = []
        for entry in entries:
            section = entry["section"]
            if section not in starts:
                raise Failure(f"outline.json names the section \"{section}\", which nothing builds")
            item = {"title": entry["title"], "page": starts[section] + entry.get("offset", 0)}
            if entry.get("children"):
                item["children"] = resolve(entry["children"])
            out.append(item)
        return out

    described = json.loads((HERE / "outline.json").read_text(encoding="utf-8"))
    resolved = work / "outline-absolute.json"
    resolved.write_text(json.dumps(resolve(described), indent=2), encoding="utf-8")

    final = work / "showcase.pdf"
    cli("outline", pdf, "--from-json", resolved, "-o", final)
    return final


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the showcase document the handbook is illustrated from.")
    parser.add_argument("--cli", type=Path, default=REPO / "build" / "bin" / "pdf-smithy-cli",
                        help="the pdf-smithy-cli to build with")
    parser.add_argument("--out", type=Path, default=HERE / "showcase.pdf", help="where to write the document")
    parser.add_argument("--keep-work", action="store_true", help="leave the intermediate files behind and say where")
    arguments = parser.parse_args()

    if not arguments.cli.exists():
        print(f"No pdf-smithy-cli at {arguments.cli}. Build the project first, or pass --cli.", file=sys.stderr)
        return 2

    sys.path.insert(0, str(HERE))
    work = Path(tempfile.mkdtemp(prefix="showcase-"))
    try:
        cli = Cli(arguments.cli, work)
        starts = assemble(cli, work, arguments.out)
    except Failure as failure:
        print(f"The showcase could not be built:\n{failure}", file=sys.stderr)
        return 1
    finally:
        if arguments.keep_work:
            print(f"Intermediate files left in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    total = starts.pop("__total__")
    print(f"Wrote {arguments.out}: {total} pages, {arguments.out.stat().st_size // 1024} kB, {cli.calls} calls.")
    for name, page in starts.items():
        print(f"  page {page:>3}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
