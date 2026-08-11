# Contributing

Patches, bug reports and translations are all welcome.

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The dependency list is in the [README](README.md#from-source).

## The rules that matter

**Every bug gets a test before it gets a fix.** Not after — before. If the test
does not fail first, it is not testing the bug.

**Never re-render a page.** Reordering, merging, rotating and splitting all work
on the PDF object structure. A scanned contract that goes in at 600 dpi comes
out with the same image bytes. If a change would re-encode page content, it
needs a very good reason and a note in the commit message.

**Every operation is undoable.** Anything that changes the document goes through
a `QUndoCommand` in `src/core/commands/`. UI code does not mutate `Document`
directly.

**Errors inform, they do not interrupt.** Failures appear in the message bar
above the document. Modal dialogs are reserved for genuine questions, like
"save before closing?". A modal box the user has to dismiss before they can
even look at their document is a bug.

**Anything the window can do, the command line can do.** New document
operations belong in `ps_core`, not in `src/ui/`, so both front ends reach them.

**Numbers written into PDFs must use a full stop.** `snprintf("%f")` and
`std::to_string(double)` follow the C locale, which Qt initialises from the
environment — on a German system they emit `12,3400` and produce content
streams no viewer can read. Use `QByteArray::number(value, 'f', n)`. There is a
regression test that runs under a comma-decimal locale on purpose.

## Code style

`clang-format` with the config in `.clang-format` (KDE style). CI rejects
unformatted code:

```bash
clang-format -i $(find src tests -name '*.cpp' -o -name '*.h')
```

Naming follows KDE conventions: `m_` for members, `CamelCase` for types,
`camelCase` for functions. Comments explain *why*, not *what* — the code
already says what it does.

Every file needs an SPDX header. Files under `src/core/` are MIT; everything
else is GPL-3.0-or-later. See [LICENSING.md](LICENSING.md) for why, and do not
add a Poppler include to `src/core/` — that split is load-bearing.

## Tests

| File | Covers |
|---|---|
| `tst_document` | page list, signals, merging |
| `tst_pagecommands` | undo and redo for every operation |
| `tst_documentwriter` | what actually lands on disk |
| `tst_pagerange` | the range syntax users type |
| `tst_splitter` | splitting by count, ranges and size |
| `tst_compressor` | compression and its reporting |
| `tst_scanprocessor` | OCR and straightening, end to end |
| `tst_printcontroller` | imposition, including booklets |
| `tst_mainwindow` | the real window driven through its real actions |

Fixtures are generated, never committed — see `tests/TestPdf.h`. If you need a
2000-page document or a deliberately broken one, ask for it there.

## Translating

```bash
./Messages.sh                                    # refresh po/pdf-smithy.pot
msginit --locale=fr --input=po/pdf-smithy.pot --output=po/fr/pdf-smithy.po
msgfmt --check --statistics -o /dev/null po/fr/pdf-smithy.po
```

Then add the translated catalogue in `po/<code>/pdf-smithy.po`. CI checks that
the template is current, so re-run `Messages.sh` whenever you add a string.

## Commits

One change per commit, present tense, explaining why rather than what:

```
Straighten scans by transform instead of resampling

Rotating the page content with a cm matrix keeps the original image
bytes intact. Deskewing by re-rendering cost roughly 15% sharpness on
a 300 dpi scan.
```
