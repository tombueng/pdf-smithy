# Licensing

Short version: **the application is GPL-3.0-or-later, the reusable engine is
MIT.** If you are shipping PDF Smithy, the GPL applies. If you want to build
something on its engine, MIT applies.

## Why it is split

The project would have preferred to be MIT throughout. One dependency makes
that impossible.

| Dependency | Licence | Permissive? |
|---|---|---|
| QPDF | Apache-2.0 / MIT | ✅ |
| Tesseract | Apache-2.0 | ✅ |
| Leptonica | BSD-2-Clause | ✅ |
| HarfBuzz / FreeType | MIT / FTL | ✅ |
| Qt 6 | LGPL-3.0 | ✅ (dynamically linked) |
| KDE Frameworks 6 | LGPL-2.1+ | ✅ (dynamically linked) |
| **Poppler** | **GPL-2.0 / GPL-3.0** | ❌ **forces GPL** |

Poppler is the only mature, distribution-packaged, Qt-native PDF renderer on
Linux. Linking it makes the combined work GPL — no LICENSE file can change
that, and claiming MIT while shipping a GPL binary would simply be untrue.

The alternatives were considered and rejected:

- **MuPDF** is AGPL-3.0, which is more restrictive still.
- **pdfium** is BSD-3-Clause and would solve the problem, but no distribution
  packages it. Building it requires Google's `depot_tools`, which would make
  compiling this project and running its CI unreasonable.

## What is licensed how

| Path | Licence | Contains |
|---|---|---|
| `src/core/` | **MIT** | Document model, undo commands, QPDF writer, page ranges, splitter, compressor, OCR pipeline |
| `src/render/` | GPL-3.0-or-later | The Poppler backend — the only Poppler-linked code |
| `src/print/`, `src/ui/`, `src/cli/` | GPL-3.0-or-later | Imposition, window, command line |
| `tests/` | matches what it tests | |
| `data/icons/` | CC-BY-SA-4.0 | Application icon |
| `data/*.metainfo.in` | CC0-1.0 | AppStream metadata |

Every file carries an SPDX header, so the licence of any given file is stated
in the file itself and machine-checkable.

## The door that is deliberately left open

`src/core/RenderBackend.h` is a pure interface, and `src/core/` never includes
a Poppler header. Rendering reaches the engine through that interface alone.

This is not architectural decoration. It means that on the day a permissively
licensed renderer becomes packageable, swapping `PopplerBackend` for it is a
single new class — and the whole project can move to MIT without touching a
line of engine code.

## Using the engine in your own project

`ps_core` is MIT. You can link it, vendor it, or copy pieces of it into a
proprietary product. It gives you:

- lossless page assembly across multiple source documents
- undoable page operations
- page-range parsing that matches what users type in print dialogs
- splitting by count, ranges or file size
- compression with honest before/after reporting
- OCR producing an invisible text layer, and skew correction by transform

You will need to supply a `RenderBackend` implementation, since the MIT half
deliberately has no renderer of its own.
