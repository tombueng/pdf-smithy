# The road to better-than-Acrobat

This is the backlog for the continuous-improvement run: brainstorm, score,
build the best, repeat. It is a working document — each cycle refines it.

## The goal, in one line

Do more than Acrobat in nearly every respect, and do it faster, simpler and
better — **without the clutter, and without the cloud** — while still meeting
professional demands.

## The contract this run holds to

- **Local only.** No cloud features by design. At most, open a document from a
  cloud drive and save one back; never an orchestrated round-trip through a
  service. Signing is an image (drawn, or stylus/finger on touch) or a
  certificate from a file on disk. Nothing more.
- **One branch, never `main`, never a push.** Every cycle commits to
  `continuous-improvement`. Review and merge stay a human's call.
- **Green or it doesn't land.** A cycle builds and passes the full test suite
  before it commits; otherwise it reverts and records why.
- **Small, reversible steps.** One coherent increment per cycle.
- **No attribution noise** in commits, code or docs.

To steer it: say so. To stop it: say "stop".

## What is already strong (do not rebuild)

A survey of the tree, not guesses:

- **Breadth rivals Acrobat already** — forms (build / fill / batch-fill /
  flatten), redaction, OCR, compare, preflight, PDF/A, compression, encryption,
  annotations, comments (xfdf), export to HTML/Markdown/images, N-up, split,
  extract, insert, colour tools, fonts, outline, metadata.
- **Preflight already fixes, not just reports** — `Preflight::applyFixes`,
  `knownFixes`, `describeFix`, and per-finding `fixId` / `fixDescription`. The
  "propose and apply fixes" idea is mostly built; the gap is surfacing it well.
- **The viewer is a real engine** — `PageView` is a continuous-scroll,
  pre-rendered, worker-threaded, byte-budgeted pipeline with kinetic scrolling,
  built-in text selection, and explicit View/Edit modes. `PopplerBackend` pools
  several document handles so pages render in parallel.
- **Signing by image exists** — `SignEditor`, `SignatureCanvas`,
  `SignatureStore` (drawn / stylus).
- **Clean architecture** — MIT core isolated from the GPL Poppler render layer;
  qpdf-based writing throughout; a CLI verb + core module + tests for each tool.

The honest conclusion: the gap to "the one everyone loves" is **not** missing
features. It is depth in a few pro areas, a handful of signature conveniences,
and cohesion/polish — reached without the user at the keyboard only where the
work is testable, not where it needs their eye.

## Scoring

Each item 1–5. **Impact** (user value), **Edge** (differentiation vs
Acrobat/RUPS), **Fit** (local-first, anti-clutter), **Safe** (reversible and
*verifiable without a human's aesthetic judgment* — this run is unattended, so
this is weighted heavily). **Cost** is effort (low is good). Rank favours high
Impact·Edge·Safe at low Cost.

| # | Item | Status | Impact | Edge | Fit | Safe | Cost | Rank |
|---|------|--------|:--:|:--:|:--:|:--:|:--:|:--:|
| 1 | **PDF object-graph inspector** (xref/trailer/root → dict/array/stream tree, searchable; core + CLI first, UI later) | gap | 5 | 5 | 5 | 5 | 3 | **A** |
| 2 | **Form autofill vault** (local store of name/address/email/IBAN…, smart field→value mapping, opt-in learning; core + CLI first) | gap | 5 | 5 | 5 | 4 | 3 | **A** |
| 3 | **Form completeness audit** (every field type — text/checkbox/radio/choice/combo/list/pushbutton/sig — round-trips build→fill→flatten; fill gaps found) | partial | 4 | 3 | 5 | 5 | 2 | **A** |
| 4 | **Attribute editing on the object tree** (change what is sensible: metadata, page boxes, annot props) — builds on #1 | gap | 4 | 5 | 4 | 4 | 4 | B |
| 5 | **Certificate-file signing** (PKCS#12 from disk, standards ByteRange; verify it is truly absent first) | likely gap | 4 | 3 | 5 | 3 | 4 | B |
| 6 | **Preflight fixes, surfaced well** (one-click apply from the report; the engine exists) | partial | 4 | 4 | 5 | 3 | 2 | B |
| 7 | **Search-as-you-type** with a result list + in-page highlight | verify | 4 | 3 | 4 | 3 | 3 | B |
| 8 | **Command palette (Ctrl+K) + action registry** — discoverability *and* structure in one | gap | 5 | 4 | 4 | 2 | 4 | C |
| 9 | **Design tokens + Welcome / empty state** (type scale, spacing, theme to the page; first impression) | gap | 5 | 4 | 4 | 1 | 3 | C* |
| 10 | **Dark / sepia reading mode** that recolours the page | gap | 3 | 3 | 4 | 2 | 3 | C |
| 11 | **Hi-DPI render sharpness** (devicePixelRatio), tiles at deep zoom, progressive paint | verify | 3 | 2 | 4 | 3 | 4 | C |
| 12 | **Live text reflow** (hold paragraphs together, re-layout on edit) | hard | 5 | 5 | 4 | 2 | 5 | D |

`C*` / low **Safe**: items 8–11 and especially 9 are aesthetic — they need the
user's eye and screenshots to get right. The unattended loop will *prepare*
them (scaffold, tokens, measurements) but hold the visible design choices for a
pairing session, per the frontend-design guidance that a picture is worth more
than a guess.

On #12 (your own question): correct — a PDF stores placed glyphs, not
paragraphs or their relationships, so true word-processor reflow of an existing
file means **reconstructing** structure (group runs into lines and blocks, infer
reading order and columns) before it can be re-flowed. That is the hardest thing
on this list and is staged last, after the inspector (#1) gives us the ground
truth of what is actually on the page to reconstruct from.

## Execution queue (what the loop builds, in order)

1. **#1 Object-graph inspector** — core `PdfInspect` + `inspect` CLI, with
   tests. Foundation for #4 and #12.
2. **#2 Autofill vault** — core + CLI, with tests.
3. **#3 Form completeness audit** — tests first, then close any gap.
4. **#5 Certificate signing** (after confirming it is absent).
5. **#6 Preflight-fix surfacing**.

Design items (#8–#11) are tracked for a pairing session, not built blind.

## Cycle log

- _cycle 0_ — Surveyed the tree, grounded the backlog, set the contract. Branch
  `continuous-improvement` opened.
