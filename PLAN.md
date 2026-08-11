# PDF Smithy — Entwicklungsplan

> Ein nativer PDF-Editor für Linux, der es mit Acrobat Pro aufnimmt.
> Qt 6 · KDE Frameworks 6 · C++20

---

## 1. Warum das hier gebaut wird

Auf Linux gibt es heute drei unbefriedigende Wege, ein PDF zu bearbeiten:

1. **Fünf Werkzeuge nebeneinander** — PDF Arranger zum Sortieren, OCRmyPDF auf der
   Kommandozeile, Ghostscript zum Komprimieren, GIMP für den Stempel, LibreOffice
   Draw für Text. Jedes kann eine Sache, keines kennt den Zustand des anderen.
2. **Eine Website** — vertrauliche Verträge, Gehaltsabrechnungen und Arztbriefe
   werden auf einen fremden Server geladen. Das ist der eigentliche Schmerz.
3. **Qoppa PDF Studio** — proprietär, in Java, kostenpflichtig, ohne
   Desktop-Integration. Funktioniert, fühlt sich aber wie ein Fremdkörper an.

**PDF Smithy schließt diese Lücke:** ein Werkzeug, alles lokal, nativ integriert,
frei.

### Der Maßstab

Nicht Adobe Acrobat ist der unmittelbare Wettbewerber, sondern **Qoppa PDF Studio** —
ein bezahlter Acrobat-Ersatz, der bereits auf Linux läuft. Unsere Vorteile:

| | PDF Studio | PDF Smithy |
|---|---|---|
| Preis | ab 99 $ | frei |
| Technik | Java/Swing | nativ C++/Qt6 |
| Desktop-Integration | keine | Plasma, GNOME, XFCE |
| Quelloffen | nein | ja |
| Startzeit | träge | sofort |

---

## 2. Leitprinzipien

Diese fünf Sätze entscheiden jede Detailfrage im Zweifel:

1. **Kein Byte verlässt den Rechner.** Keine Telemetrie, kein Cloud-Dienst, keine
   Netzwerkverbindung ohne ausdrückliche Nutzerhandlung. Das ist kein Feature,
   das ist das Fundament.
2. **Verlustfrei, wo immer möglich.** Seiten umsortieren darf ein PDF nicht neu
   rendern. Wir arbeiten auf der Objektstruktur, nicht auf Pixeln.
3. **Nichts ist endgültig.** Jede Aktion landet auf dem Undo-Stack. Der Nutzer
   soll experimentieren dürfen, ohne Angst vor seinem eigenen Dokument.
4. **Der einfache Fall braucht einen Klick.** Zwei PDFs zusammenkleben soll so
   leicht sein wie zwei Dateien in ein Fenster ziehen. Der schwere Fall soll
   möglich sein — der leichte muss trivial sein.
5. **Alles, was die GUI kann, kann auch die Kommandozeile.** Gemeinsame Engine,
   zwei Oberflächen. Automatisierbarkeit ist kein Nachgedanke.

---

## 3. Architektur

### 3.1 Schichtenmodell

```
┌──────────────────────────────────────────────────────────┐
│  pdf-smithy (GUI)          │  pdf-smithy-cli             │
│  Qt Widgets + KXmlGui      │  Kommandozeile              │
├──────────────────────────────────────────────────────────┤
│                      pw_core                             │
│  Document · Source · PageRef · UndoStack · Writer        │
│  Operationen: Seiten · OCR · Kompression · Stempel · …   │
├───────────────────────┬──────────────────────────────────┤
│  RenderBackend        │  Struktur (QPDF, Apache-2.0)     │
│  (Interface, MIT)     │  OCR (Tesseract, Apache-2.0)     │
│  └─ PopplerBackend    │  Bild (Leptonica, MIT)           │
│     (GPL, austausch-  │  Text (HarfBuzz+FreeType, MIT)   │
│      bar)             │                                  │
└───────────────────────┴──────────────────────────────────┘
```

**Warum diese Trennung?** Poppler ist die einzige GPL-Abhängigkeit im Stapel.
Hinter einem Interface versteckt bleibt sie austauschbar — sobald pdfium (BSD)
paketiert verfügbar ist, kann das Projekt permissiv werden, ohne dass eine
einzige Zeile Kernlogik anzufassen wäre.

### 3.2 Das Dokumentmodell

Der Kern ist bewusst schlicht und trägt bis in Stufe 6:

```cpp
struct PageRef {
    int sourceId;     // welches geladene Quelldokument
    int sourcePage;   // welche Seite darin
    int rotation;     // Drehung, die der Nutzer hinzugefügt hat
    // später: Overlays, Anmerkungen, Redaktionen
};

class Document {
    QVector<Source*>  m_sources;   // beliebig viele geöffnete PDFs
    QVector<PageRef>  m_pages;     // die Seitenfolge des Ergebnisses
    QUndoStack       *m_undo;
};
```

Daraus folgt fast alles von selbst:

- **Zusammenkleben** ist das Anhängen von `PageRef`s einer zweiten Quelle.
- **Splitten** ist das Aufteilen der Liste auf mehrere Ausgaben.
- **Löschen, Drehen, Umsortieren** sind Listenoperationen — sekundenschnell,
  auch bei 2000 Seiten, weil kein PDF-Inhalt angefasst wird.
- **Rückgängig** ist trivial, weil Operationen den Quellinhalt nie zerstören.
- Erst beim **Speichern** baut QPDF das Ergebnis aus den Quellen zusammen.

### 3.3 Nebenläufigkeit

Poppler ist nicht threadsicher. Jede `Source` bekommt daher einen eigenen Mutex;
Renderaufträge verschiedener Dokumente laufen parallel, Aufträge desselben
Dokuments serialisiert. Vorschaubilder werden in einem `QThreadPool` erzeugt und
in einem nach `(Quelle, Seite, Breite)` geschlüsselten Cache gehalten — **die
Drehung wird erst beim Zeichnen angewandt**, damit Drehen den Cache nicht
entwertet.

Lang laufende Arbeiten (OCR, Kompression) laufen als abbrechbare Aufträge mit
Fortschrittsanzeige, niemals im GUI-Thread.

---

## 4. Funktionsumfang nach Stufen

### Stufe 1 — Fundament ✱ Kernversprechen

| Funktion | Umsetzung |
|---|---|
| Öffnen, mehrere Quellen | QPDF + Poppler pro Datei |
| Seitenraster mit Vorschau | `QListView` im Icon-Modus, stufenlos zoombar |
| Umsortieren per Drag & Drop | inkl. Mehrfachauswahl |
| Löschen, Drehen, Duplizieren | Undo/Redo für alles |
| Einfügen aus anderer Datei | Dateien ins Fenster ziehen |
| Speichern / Speichern unter | verlustfrei über QPDF |
| Plasma-Integration | KXmlGui, KConfig, KIO, KCrash |

### Stufe 2 — Kleben, Splitten, Drucken ✱ der Alltag

| Funktion | Umsetzung |
|---|---|
| **Zusammenkleben** | Dateien ins Fenster ziehen — fertig |
| **Splitten** | nach Seitenbereich, nach Anzahl, nach Lesezeichen, nach Dateigröße |
| Seiten extrahieren | Auswahl → neues Dokument |
| **Druck, umfassend** | Bereich, Duplex, N-up, Broschüre, Skalierung, Graustufen, Papierschacht, Poster |
| Druckvorschau | echte Vorschau des Ergebnisses, nicht des Quelldokuments |
| Export als Bild | PNG/JPEG/TIFF, wählbare Auflösung |

### Stufe 3 — Scans und Kompression

| Funktion | Umsetzung |
|---|---|
| **OCR mit unsichtbarem Textlayer** | Tesseract, Render-Modus 3 → Text markier- und kopierbar |
| Sprachen | Deutsch, Englisch, weitere nachrüstbar |
| **Geraderücken** | Leptonica `pixDeskew` |
| Flecken entfernen, Kontrast | Leptonica |
| **Kompression, mehrstufig** | Bilder neu abtasten, JPEG-Qualität, Objektströme, Schriften zusammenführen, Duplikate entfernen |
| Kompressionsvorschau | „von 12,4 MB auf 1,8 MB" **vor** dem Speichern, mit Qualitätsvergleich |
| Bilder → PDF | Scans direkt einlesen |

### Stufe 4 — Werkzeuge

| Funktion | Umsetzung |
|---|---|
| **Unterschrift-Stempel** | Bild einmalig erfassen (Datei/Kamera/Freihand), speichern, überall platzieren |
| Wasserzeichen | Text oder Bild, Deckkraft, Drehung, Stapelanwendung |
| Anmerkungen | Markieren, Notiz, Freihand, Formen, Textfeld |
| **Echte Redaktion** | Inhalt wird *entfernt*, nicht überdeckt |
| Metadaten | bearbeiten und bereinigen |
| Verschlüsselung | Passwort setzen/entfernen, Berechtigungen |
| Kopf-/Fußzeilen, Bates | Seitenzahlen, Nummerierung |
| Zuschneiden, N-up | Ränder beschneiden, Mehrfachnutzen |
| Lesezeichen, Links | Gliederung bearbeiten |
| Anhänge | eingebettete Dateien |

### Stufe 5 — Formulare

| Funktion | Umsetzung |
|---|---|
| Ausfüllen | AcroForm-Felder, Speichern der Werte |
| **Felder anlegen** | Text, Ankreuzfeld, Auswahl, Liste, Knopf, Unterschrift |
| Appearance-Streams | selbst erzeugen, damit jeder Betrachter es korrekt zeigt |
| Reihenfolge, Validierung | Tab-Reihenfolge, Pflichtfelder, Formate |
| Import/Export | FDF, XFDF, CSV |

### Stufe 6 — Textbearbeitung ✱ der Brocken

Das ist die Fähigkeit, die Acrobat zu Acrobat macht, und der Grund, warum es
kein gutes freies Gegenstück gibt. Ehrliche Einschätzung: **Monate, nicht Tage.**

| Schritt | Inhalt |
|---|---|
| Content-Stream-Parser | Operatoren lesen, Textblöcke identifizieren |
| Schriftanalyse | eingebettete Subsets, CID-Fonts, Encoding-Tabellen |
| Bearbeiten im Absatz | Zeichen ersetzen bei vorhandenen Glyphen |
| Schrift nachrüsten | HarfBuzz-Shaping, FreeType-Subsetting, Neueinbettung |
| Neuumbruch | Zeilenumbruch im Textblock |

Realistisch beginnt das als **Textkorrektur** (Tippfehler, Zahlen, Namen) und
wächst zur Absatzbearbeitung. Ein ehrliches „ersetzt Zeichen zuverlässig" ist
mehr wert als ein unzuverlässiges „bearbeitet alles".

### Stufe 7 — Profi

| Funktion | Umsetzung |
|---|---|
| **Barrierefreiheit / PDF-UA** | Tags, Lesereihenfolge, Alt-Texte |
| PDF/A | Konvertierung und Prüfung |
| Dokumentvergleich | Text-Diff plus visuelles Overlay |
| **Stapelverarbeitung** | Aktionen aufzeichnen, auf Ordner anwenden |
| Digitale Signatur | PAdES prüfen und erzeugen (pyHanko-Äquivalent) |

> **Warum Barrierefreiheit strategisch zählt:** Seit dem
> Barrierefreiheitsstärkungsgesetz (Juni 2025) müssen Unternehmen barrierefreie
> Dokumente liefern. Unter Linux gibt es dafür **kein** Werkzeug. Das ist keine
> Nachahmung von Acrobat, sondern eine offene Flanke.

---

## 5. Kommandozeile

Gleiche Engine, eigene Oberfläche — für Skripte, Server und Stapel:

```bash
pdf-smithy-cli merge a.pdf b.pdf -o zusammen.pdf
pdf-smithy-cli split rechnungen.pdf --alle 1 -o teil-%03d.pdf
pdf-smithy-cli pages vertrag.pdf --behalten 1-3,7 --drehen 4:90 -o kurz.pdf
pdf-smithy-cli ocr scan.pdf --sprache deu+eng --geraderuecken -o durchsuchbar.pdf
pdf-smithy-cli compress gross.pdf --stufe ebook -o klein.pdf
pdf-smithy-cli stamp vertrag.pdf --bild unterschrift.png --seite letzte --position unten-rechts
pdf-smithy-cli info dokument.pdf --json
```

Deutsche *und* englische Optionsnamen, maschinenlesbare Ausgabe mit `--json`,
sinnvolle Rückgabewerte, Shell-Vervollständigung für bash/zsh/fish.

---

## 6. Bedienung und Oberfläche

### 6.1 Das Grundgerüst

```
┌───────────────────────────────────────────────────────────┐
│ Datei Bearbeiten Ansicht Seiten Werkzeuge Hilfe          │
├───────────────────────────────────────────────────────────┤
│ [Öffnen][Speichern] │ [↺][↻][🗑][⧉] │ [OCR][Komprimieren] │
├──────────┬────────────────────────────────────────────────┤
│          │                                                │
│ Seiten   │        Seitenraster mit Vorschaubildern        │
│ Lesez.   │        (Mehrfachauswahl, Drag & Drop)          │
│ Anhänge  │                                                │
│ Ebenen   │                                                │
│          │                                                │
├──────────┴────────────────────────────────────────────────┤
│ 24 Seiten · 3 ausgewählt · 4,2 MB          [Zoom ──●───]  │
└───────────────────────────────────────────────────────────┘
```

### 6.2 Was die Bedienung ausmacht

- **Drag & Drop überall** — Dateien ins Fenster ziehen fügt an, zwischen zwei
  Seiten ziehen fügt dort ein, aus dem Fenster ziehen exportiert.
- **Der Zoomregler** verwandelt das Raster stufenlos von Übersicht (60 Seiten
  auf einen Blick) zur Detailansicht — eine Ansicht statt vier.
- **Sofortrückmeldung** — Kompressionsergebnis, OCR-Fortschritt und Seitenzahl
  stehen immer sichtbar, nie in einem Modaldialog versteckt.
- **Tastatur vollwertig** — jede Funktion erreichbar, KDE-Standardkürzel,
  vollständig konfigurierbar über KShortcutsDialog.
- **Ein einziges Fenster.** Kein Dialogdschungel. Werkzeuge erscheinen als
  Seitenleiste, nicht als modales Fenster, das den Blick aufs Dokument nimmt.
- **Zerstörungsfrei bis zum Speichern** — die Titelleiste sagt jederzeit, ob es
  ungesicherte Änderungen gibt.

### 6.3 Desktop-Integration

| Umgebung | Integration |
|---|---|
| **Plasma** | KXmlGui-Menüs, KConfig, KIO (`sftp://`, `smb://` direkt öffnen), KWallet für Passwörter, KCrash, Dolphin-Servicemenüs, Purpose zum Teilen |
| **GNOME/XFCE** | sauberes `.desktop`, MIME-Verknüpfung, Symbol nach freedesktop-Standard, folgt dem Systemfarbschema |
| **Alle** | Wayland-nativ, HiDPI, dunkles Thema, Bildschirmleser-tauglich |

---

## 7. Mehrsprachigkeit

- `KLocalizedString` (`i18n()`, `i18np()`) von der ersten Zeile an — nicht
  nachträglich.
- **Deutsch und Englisch** vollständig zum Start, Gettext-`.po`-Dateien.
- Englisch ist die Quellsprache im Code, Deutsch die erste Übersetzung.
- Vorbereitet für Weblate/KDE-i18n, damit Beiträge ohne Codezugriff möglich sind.
- Zahlen, Daten und Papierformate folgen der Locale (A4 vs. Letter).

---

## 8. Qualitätssicherung

### 8.1 Testebenen

| Ebene | Werkzeug | Prüft |
|---|---|---|
| **Unit** | QTest | Dokumentmodell, Undo-Kommandos, Seitenlogik, CLI-Parser |
| **Integration** | QTest | Speichern → erneut öffnen → Struktur identisch; OCR erzeugt Text; Kompression verkleinert |
| **E2E (GUI)** | QTest + `QTest::mouseClick`/`keyClick` | Öffnen, Umsortieren, Drehen, Speichern über die echte Oberfläche |
| **Fuzzing** | kaputte PDFs | darf niemals abstürzen |
| **Sanitizer** | ASan, UBSan | in CI bei jedem Push |

### 8.2 Testkorpus

Eigene, erzeugte PDFs (keine fremden Rechte): normal, mehrseitig, gedreht,
verschlüsselt, mit Formular, mit Anhang, absichtlich beschädigt, sehr groß
(2000 Seiten), CJK-Schriften, Scan ohne Textlayer.

### 8.3 Grundsätze

- Jeder Fehler bekommt zuerst einen Test, dann die Korrektur.
- Kein `git push` ohne grüne CI.
- `clang-format` und `clang-tidy` blockierend.

---

## 9. CI, Paketierung, Auslieferung

### 9.1 GitHub Actions

| Job | Inhalt |
|---|---|
| `build` | Ubuntu 24.04 + 26.04, GCC und Clang |
| `test` | gesamte Testsuite, Xvfb für GUI-Tests |
| `sanitize` | ASan/UBSan-Lauf |
| `format` | `clang-format --dry-run -Werror` |
| `tidy` | `clang-tidy` auf geänderte Dateien |
| `package` | `.deb`, Flatpak, AppImage als Artefakte |
| `release` | bei Tag: Pakete anhängen, Changelog erzeugen |

### 9.2 Auslieferungswege

- **`.deb`** — Debian/Ubuntu/Kubuntu, per debhelper
- **Flatpak** — KDE-Runtime, später Flathub (`io.github.tombueng.PdfSmithy`)
- **AppImage** — läuft überall ohne Installation
- **AUR** — PKGBUILD für Arch/Manjaro
- **COPR** — Fedora/openSUSE

### 9.3 Dokumentation

| Dokument | Zweck |
|---|---|
| `README.md` | Was es ist, Bildschirmfotos, Installation in drei Zeilen |
| `docs/handbuch/` | Nutzerhandbuch, deutsch und englisch, mit Bildern |
| `docs/entwicklung.md` | Bauen, Architektur, Beitragen |
| KDE-Hilfe | F1 öffnet das Handbuch, DocBook |
| `man`-Seiten | für beide Binaries |
| `CONTRIBUTING.md` | Codestil, Ablauf, Verhaltenskodex |

---

## 10. Lizenz

**Der Wunsch war MIT — das geht nur teilweise, und zwar aus einem harten Grund.**

Poppler steht unter GPL-2/3. Wird es verlinkt, ist das kombinierte Werk GPL,
unabhängig davon, was in der LICENSE-Datei steht. pdfium (BSD) wäre die
Alternative, ist aber in keiner Distribution paketiert und bräuchte Googles
`depot_tools` — das würde Bauen und CI unzumutbar machen.

Der gewählte Kompromiss:

| Teil | Lizenz | Begründung |
|---|---|---|
| `pw_core` (Engine, ohne Rendering) | **MIT** | frei weiterverwendbar, hängt nur an QPDF/Tesseract/Leptonica — alle permissiv |
| Render-Backend + GUI | **GPL-3.0-or-later** | Poppler erzwingt es |
| Symbole, Grafik | CC-BY-SA-4.0 | KDE-Konvention |

Sobald pdfium paketiert verfügbar ist, wird ein Backend ausgetauscht und das
gesamte Projekt kann permissiv werden. Die Architektur hält diese Tür offen.
Details in `LICENSING.md`, Dateiköpfe REUSE-konform mit SPDX.

---

## 11. Stand: was gebaut ist

Geprüft gegen den Code, nicht aus dem Gedächtnis. Legende:
**✓** fertig und getestet · **◐** teilweise · **✗** nicht vorhanden

### Stufe 1 — Fundament ✓

Vollständig. Öffnen mehrerer Quellen, Seitenraster mit Vorschau, Umsortieren
per Drag & Drop, Löschen/Drehen/Duplizieren mit Undo, Einfügen mit Positionswahl,
verlustfreies Speichern. Kontextmenü über KXmlGui.

### Stufe 2 — Kleben, Splitten, Drucken ◐

| | |
|---|---|
| ✓ | Zusammenkleben, Seiten extrahieren |
| ✓ | Splitten: nach Anzahl, nach Bereichen, nach Dateigröße, **nach Lesezeichen** — mit Vorschau der Dateinamen |
| ✓ | Druck: Bereich, N-up, Broschüre, manueller Duplex, Skalierung, Graustufen |
| ✓ | Druckvorschau — zeichnet durch dieselbe Routine wie der Drucker |
| ✓ | Export als Bild (PNG/JPEG/TIFF) und Bilder → PDF |

### Stufe 3 — Scans und Kompression ◐

| | |
|---|---|
| ✓ | OCR mit unsichtbarem Textlayer, Sprachen, Geraderücken |
| ✓ | Kompression mehrstufig, mit ehrlicher Begründung wenn nichts zu holen ist |
| ◐ | Kompressionsbericht **nach** der Anwendung statt Vorschau davor |
| ✓ | Flecken entfernen und Ausleuchtung — nur auf dem Erkennungsbild, die Seite bleibt unangetastet |
| ✓ | Bilder → PDF, mit korrekter Größenrechnung aus der Bildauflösung |

### Stufe 4 — Werkzeuge ◐ (6 von 10)

| | |
|---|---|
| ✓ | Unterschrift-Stempel mit Bibliothek, Hintergrundentfernung, interaktiver Platzierung |
| ✓ | Wasserzeichen (Text) |
| ✓ | Metadaten bearbeiten und bereinigen (auch JavaScript und Anhänge) |
| ✓ | Verschlüsselung: AES-256, Passwort setzen und entfernen, Berechtigungen |
| ✓ | Kopf-/Fußzeilen, Seitenzahlen und Bates-Nummerierung |
| ✓ | Zuschneiden über /CropBox, mit automatischer Randerkennung |
| ✓ | **Echte Schwärzung** — zeichenweiser Content-Stream-Filter, Bildpixel werden bearbeitet |
| ✓ | **N-up als Dokumentoperation** — Seiten als Form-XObjects auf Blätter gesetzt, Text bleibt Text |
| ✓ | **Anmerkungen** — Hervorheben, Unterstreichen, Durchstreichen, Rechteck, Ellipse, Linie, Freihand, Textfeld, Notiz |
| ✓ | **Lesezeichen und Links** — Inhaltsseitenleiste, Bearbeiten, Links folgen ihrer Seite |

### Stufen 5 bis 7 ✗

Formulare, Textbearbeitung, PDF/A, PDF/UA, Vergleich, Stapelverarbeitung:
nichts davon gebaut.

### Querschnitt

| | |
|---|---|
| ✓ | Kommandozeile, Deutsch/Englisch (277 Meldungen), 11 Testsuiten, CI, `.deb` |
| ◐ | Flatpak- und AppImage-Rezepte geschrieben, aber nie gebaut |
| ✗ | Nutzerhandbuch, Entwicklerdoku, `man`-Seiten, F1-Hilfe |
| ◐ | KIO für Netzwerkfreigaben ✓ — KWallet, Dolphin-Servicemenüs und Purpose fehlen |

---

## 12. Zwei gebrochene Zusagen — eingelöst

Beide sind behoben. Sie stehen hier weiter, weil der Weg dahin festgehalten
gehört.

**1. ~~Die `.desktop`-Datei verspricht Netzwerkprotokolle, die es nicht gibt.~~**
*Behoben durch Einbau statt Streichen: Öffnen und Speichern laufen über URLs,
entfernte Dateien werden geholt, lokal bearbeitet und zurückgeschrieben.*
Sie trägt `X-KDE-Protocols=file,smb,sftp,fish,webdav,webdavs`, aber geöffnet
wird mit `QFileDialog`. Ein Klick auf ein PDF in einer SMB-Freigabe schlägt
fehl. Entweder KIO wirklich einbauen oder die Zeile entfernen — die Zeile
stehen zu lassen ist die einzige Variante, die nicht geht.

**2. ~~Leitprinzip 5 ist verletzt.~~** *Behoben: `sign` und `watermark` sind in
der Kommandozeile, Duplizieren geht über den Bereichs-Syntax
(`--keep 1,1,2`). Dazu neu: `meta`, `protect`, `from-images`,
`export-images`.*

Ursprünglicher Befund: „Alles, was die GUI kann, kann auch die
Kommandozeile." Tatsächlich:

```
GUI:  drehen  löschen  duplizieren  auslagern  einfügen  ocr  komprimieren  signieren  wasserzeichen
CLI:  pages   pages    —            pages      merge     ocr  compress      —          —
```

Signieren, Wasserzeichen und Duplizieren fehlen in der Kommandozeile.

---

## 13. Reihenfolge von hier an

### ~~Block 0 — Zusagen einlösen~~ ✓ erledigt

### Block A — schnelle Gewinne  ·  ✓ fertig

Erledigt: Metadaten, Verschlüsselung, Bilder → PDF, Seiten → Bilder,
Druckvorschau, Seitenzahlen mit Bates-Nummerierung, Zuschneiden mit
Randerkennung, Splitten nach Lesezeichen — jeweils in Oberfläche *und*
Kommandozeile.

Nachgezogen und damit abgeschlossen:

| Funktion | Umsetzung |
|---|---|
| Bild-Wasserzeichen in der Oberfläche | `WatermarkDialog` mit Text/Bild-Umschaltung und Vorschau |
| Kompressionsvorschau vor dem Anwenden | `Compressor::estimate()` — misst ≤ 4 gestreute Seiten und rechnet hoch, ausdrücklich als Schätzung beschriftet |
| N-up als Dokumentoperation | `src/core/NUp.{h,cpp}` — jede Seite wird zum Form-XObject und auf das Blatt gesetzt, nichts wird gerastert. An `ct.26.16.pdf` (178 Seiten, 17 MB) nachgemessen: 45 Blatt in 0,4 s, 8,6 kB größer als das Original, JPEGs byteidentisch. Raster wird aus „Seiten pro Blatt“ nahezu quadratisch abgeleitet, die lange Blattseite folgt der langen Rasterseite. |

### Block B — Lesezeichen und Navigation  ·  ✓ fertig

Der schwerwiegendste Teil war kein neues Feature, sondern ein Verlust: **die
Gliederung ging bei jedem Speichern verloren**, weil der Writer bei
`emptyPDF()` beginnt — dieselbe Klasse Fehler wie damals bei den Metadaten.

`src/core/Outline.{h,cpp}` liest und schreibt den Gliederungsbaum. Im `Document`
sind die Einträge **an die Seite verankert** (`sourceId` + `sourcePage`), nicht
an ihre Nummer: eine umsortierte Seite nimmt ihr Lesezeichen mit, eine
gelöschte nimmt es weg. Beim Zusammenfügen werden beide Inhaltsverzeichnisse
hintereinandergehängt.

Interne Links folgen dem Umsortieren von selbst, weil QPDF das Seitenobjekt
mitkopiert. Der harte Fall war die *gelöschte* Zielseite: QPDFs
`copyForeignObject` hinterlässt dort ein `null`, und ein Link darauf ist ein
Rechteck, das beim Klicken nichts tut. `DocumentWriter::pruneDeadLinks` erkennt
beide Formen — reales Seitenobjekt außerhalb des Seitenbaums *und* das
zurückgelassene `null` — und entfernt den Link.

Oberfläche: `OutlineDock` mit Umbenennen an Ort und Stelle, Verschachteln,
Verschieben, Löschen und Ziehen — alles auf demselben Undo-Stapel wie die
Seitenoperationen. Kommandozeile: `outline --list/--json/--add/--from-json/--clear`.

### Block C — Echte Redaktion  ·  ✓ fertig

Nicht schwarz übermalen, sondern entfernen. `src/core/Redaction.{h,cpp}`:

1. Content-Stream-Filter über `QPDFPageObjectHelper::filterContents`
2. Zustandsmaschine für Grafik- *und* Textzustand (`cm`, `q`, `Q`, `Tm`, `Td`,
   `TD`, `T*`, `TL`, `Tf`, `Tz`, `Tc`, `Tw`, `Ts`). Verworfen wird **glyphen-,
   nicht operatorweise**: eine ganze Zeile steht meist in einem einzigen `Tj`,
   und den Operator zu streichen nähme den Rest der Zeile mit. Jede entfernte
   Glyphe wird durch die gleichwertige `TJ`-Abstandszahl ersetzt, damit alles
   Übrige exakt stehen bleibt und die Textmatrix am Ende dort steht, wo sie
   ohne Eingriff stünde. Ein `Tm` an dieser Stelle wäre falsch — es setzt auch
   die Zeilenmatrix zurück und zerbräche das folgende `Td`.
3. Für Dokumente ohne `/Widths` liegen die echten Metriken der 14 Standard-
   schriften bei (`Core14Widths.h`, erzeugt aus den URW-base35-AFMs). Ohne sie
   säße jede Glyphe nach der ersten an der falschen Stelle.
4. Bilder werden **pixelweise bearbeitet** und unter neuem Namen neu
   eingebettet, damit eine zweite Platzierung desselben Bildes ihre Pixel
   behält; das Original fällt über `removeUnreferencedResources()` weg. Was
   sich nicht dekodieren lässt (Fax, JPEG 2000), wird über
   `Options::pageRasteriser` zur Bildseite verflacht — oder entfernt und
   gemeldet, statt still das Falsche zu tun.
5. Anmerkungen, die in einen Bereich ragen, werden gelöscht.
6. **Verifikation als Test** (`tst_redaction`, 12 Fälle): der extrahierte Text
   darf das Geschwärzte nicht mehr enthalten, die Nachbarwörter schon; bei
   Bildern werden die *gespeicherten Pixel* geprüft, nicht das gerenderte Bild.
   An `testdata/brief-1902.pdf` nachgemessen: OCR-Textebene weg, Bildpixel
   schwarz, Rest der Seite im Mittel 0,24/255 abweichend.

Erreichbar als `page_redact` im Menü und im Kontextmenü, als
`pdf-smithy-cli redact --area SEITE:X,Y,B,H` und über `PageProcessor::redact`
als ein einziger Undo-Schritt. Der Dialog zeigt vor dem Zugriff die Wörter, die
verschwinden, und benennt seine eigenen Grenzen (`Redaction::limitations()`).

### Block D — Anmerkungen  ·  ✓ fertig, samt XFDF

`src/core/Annotation.{h,cpp}`: neun Typen, jeder mit **eigenem Erscheinungsbild-Stream**.
Die Alternative — den Lesern zu überlassen, wie sie eine Anmerkung aus ihren
Eigenschaften zeichnen — erlaubt die Spezifikation zwar, sieht aber in jedem
Programm anders aus; bei einer Hervorhebung über einem Beweisstück ist das keine
Geschmacksfrage.

Das Erscheinungsbild wird in **Anzeigekoordinaten** gezeichnet und über die
`/Matrix` des Formulars in den Seitenraum getragen. Dadurch bleibt „die
Unterstreichung liegt unter dem Wort“ auch auf einer um 90° gedrehten Seite
richtig, statt stillschweigend zu „am linken Rand“ zu werden — und gedrehte
Seiten sind bei Scans der Normalfall.

Hervorhebungen werden mit `/BM /Multiply` gezeichnet, damit schwarzer Text unter
Gelb schwarz bleibt statt grau zu werden. `remove()` fasst `/Widget` nie an:
ein Formular zu verlieren, weil jemand „Kommentare entfernen“ gewählt hat, wäre
nicht zu rechtfertigen. `flatten()` zeichnet sie in die Seite und nimmt sie
danach weg.

Oberfläche: `AnnotateDialog` mit zehn Werkzeugen, Seitennavigation,
Kommentarliste über das ganze Dokument und Auswählen/Verschieben/Löschen.
Vorhandene Anmerkungen werden beim Öffnen eingelesen und auf die Ausrichtung
der Zeile abgebildet. Kommandozeile: `annotate --list/--highlight/--note/--clear/--flatten`.

`PageCanvas` ist die gemeinsame Basis von Schwärzungs- und Anmerkungsansicht;
die drei Umrechnungen zwischen Mauspixeln, größenfestem Bruchteil und
PDF-Punkten stehen nur noch an einer Stelle.

XFDF-Im- und Export: 526 Bytes statt 1,27 MB Dokument. Kommentare für Seiten,
die es nicht gibt, werden ausgelassen **und gemeldet** statt auf die letzte
Seite geraten.

### Block E — Formulare  ·  ✓ fertig

`src/core/Forms.{h,cpp}`: sechs Feldarten lesen, ausfüllen, festschreiben.
Zwei Fallen, beide durch Tests abgedeckt:

- **Ein Wert ohne Erscheinungsbild ist unsichtbar** in etwa der Hälfte der
  benutzten Betrachter, die druckenden eingeschlossen. Daher wird nach jedem
  Ausfüllen `generateAppearancesIfNeeded()` gerufen und im Test durch
  Textextraktion aus der Seite geprüft, nicht durch Nachlesen des Werts.
- **Der Ankreuzzustand heißt nicht immer `/Yes`.** Das Testdokument benutzt
  `/Ja`; wer `/Yes` schreibt, setzt einen Wert, den das Feld nicht zeigen kann,
  und es sieht nach einem Fehler des Betrachters aus. Der Zustand wird aus dem
  `/AP`-Wörterbuch des Widgets gelesen.

Gesperrte Felder werden **gezeigt und deaktiviert**, nicht versteckt, und beim
Ausfüllen mit Begründung übersprungen. Ein falsch geschriebener Feldname wird
gemeldet — ein Tippfehler, der nichts füllt und nichts sagt, sieht wie Erfolg
aus. Oberfläche: `FormDialog`. Kommandozeile: `form --list/--set/--flatten`.


Ausfüllen zuerst, Felder anlegen danach. Appearance-Streams selbst erzeugen,
sonst zeigt es kein fremder Betrachter richtig an.

### Block F — Textbearbeitung  ·  ✓ fertig, mit benannten Grenzen

`src/core/TextEdit.{h,cpp}`. Was Leute mit „Text im PDF ändern" fast immer
meinen, ist: im Brief steht ein Tippfehler, das Datum ist falsch, der Name
stimmt nicht. Genau das leistet dies — im Anweisungsstrom der Seite, in deren
eigener Schrift, alles andere bleibt Byte für Byte unberührt.

**Kein Umbruch, und das ist keine Faulheit:** ein PDF hat kein Absatzmodell,
eine Seite ist eine Liste von Glyphen an Koordinaten. Nichts in der Datei sagt,
welche Zeile auf welche folgt, also kann ein längerer Satz die nächste Zeile
nicht nach unten schieben. Statt zu raten wird eine zu breite Ersetzung
**gestaucht** (bis 60 %, darüber gemeldet), eine kürzere macht die Zeile
einfach kürzer. Sie zurückzustrecken hätte 400 % breite Buchstaben ergeben — der
Test, der das gefunden hat, war der lehrreichste des Blocks.

**Wo es sich weigert.** Eine eingebettete Schrift trägt meist nur die Zeichen,
die das Dokument benutzt hat. Auf `ct.26.16.pdf` nachgemessen: „Standpunkt" →
„Gegenstandpunkt" wird abgelehnt, weil die Überschriftenschrift kein G, e, g, s
enthält — mit genau diesen Zeichen benannt. „Form" → „Größe" im Textkörper
gelingt, weil diese Schrift ö und ß hat. Ein falsches Zeichen still zu zeichnen
wäre die schlechtere Antwort.

**Was die Schrift bedeutet**, kommt aus drei Quellen, und alle drei waren nötig,
damit echte Dokumente überhaupt lesbar sind: `/ToUnicode`, benannte Encodings,
und — die Form, die Zeitschriften und Office-Pakete tatsächlich benutzen — ein
`/Encoding`-Wörterbuch mit `/Differences` aus **Glyphennamen**. Für letzteres
liegt `GlyphNames.h` bei (aus den Encoding-Vektoren erzeugt, die Ghostscript
mitbringt); ein Name wie `/one.fitted` verliert seinen Variantenzusatz und ist
eine Eins.

Der `/ToUnicode`-Parser trennte anfangs an `<` mit `SkipEmptyParts` — das
überspringt keine Zeilenumbrüche, jedes Paar verschob sich um eins, und die
ganze Seite kam als Fragezeichen zurück. Jetzt über einen Ausdruck, beide
Bereichsformen eingeschlossen.

Oberfläche: `TextEditDialog` mit umrandeten Zeilen auf der Seite — gepunktet,
was sich nicht beschreiben lässt, sodass die Absage vor dem Tippen kommt.
Kommandozeile: `text --page N` und `text --replace SEITE:ZEILE=Text`.

### Block G — Profiliga  ·  ✓ fertig

| Funktion | Umsetzung |
|---|---|
| **PDF/A** | `src/core/Archival.{h,cpp}`, Umwandlung über Ghostscript (AGPL an der Prozessgrenze), Stufen 1b/2b/3b, sRGB-Profil vom System. Drei echte Ghostscript-Fallen dabei gefunden: der Sandkasten braucht `--permit-file-read` für das Profil, die `PDFA_def.ps` muss **absolut** übergeben werden (sonst nimmt gs still seine eigene und schreibt gar kein `/OutputIntents`, mit Rückgabewert 0), und Titel müssen als UTF-16BE-Hexstrings hinein. |
| **Prüfung** | `inspect()` — bewusst *nicht* „validieren" genannt: ein echter PDF/A-Prüfer ist eine große Software (veraPDF). Findet die häufigen, behebbaren Mängel — Verschlüsselung, Skripte, Anhänge, nicht eingebettete Schriften, fehlender Output Intent — und sagt in `inspectionLimitations()`, dass es das tut. |
| **Vergleich** | `src/core/Compare.{h,cpp}` mit echter LCS über Wortlisten, nicht Mengendifferenz: `a b c` gegen `a c b` *ist* eine Änderung. Text gleich, Pixel verschieden heißt `VisualOnly` — ein Bild wurde getauscht oder eine Schrift ersetzt. Beide Seiten werden auf gleiche Pixelbreite gerendert, deshalb wird die Seitengröße **zusätzlich** verglichen: derselbe Inhalt von A4 auf A3 gäbe sonst identische Pixel. |
| **Stapel** | `batch --op compress/sanitize/flatten-comments/flatten-form/ocr --out-dir`. Bewusst sequenziell: Ghostscript und Tesseract belegen je einen Kern, vier Dateien gleichzeitig machen die Maschine unbenutzbar und werden nicht früher fertig. |
| **PDF/UA** | offen — braucht Struktur-Tags, was ein eigener Block wäre |


---

## 14. Querschnittsthemen

| Thema | Stand |
|---|---|
| Man-Pages | ✓ `docs/pdf-smithy.1.in` und `docs/pdf-smithy-cli.1.in`. Die Befehlstabelle wird aus der Hilfe des Programms erzeugt, und `manpage-current` als Test verhindert das Abdriften — ein Handbuch, das Befehle nennt, die es nicht gibt, ist schlimmer als keines. |
| Tastenkürzel | ✓ `unique-shortcuts` prüft am Quelltext. Nötig, weil KXmlGuiWindow einen Konflikt mit einem *modalen Dialog während des Aufbaus* beantwortet: das begrüßt den Nutzer beim Start und hängt den E2E-Test auf, bevor er sagen kann, warum. Genau das ist zweimal passiert. |
| Übersetzbarkeit | ✓ `translatable-strings` verbietet `tr()` und `QCoreApplication::translate`, die `Messages.sh` nicht extrahiert. Fand beim Einführen sofort einen Altfall. |
| Locale-Sicherheit | ✓ Jeder Test läuft **zweimal**, unter `LC_ALL=C` und `de_DE.UTF-8`. Aufgenommen, nachdem sich zeigte, dass QPDFs `getNumericValue()` durch `strtod` geht und auf deutschen Systemen jede gebrochene Zahl als Null liest. |
| Handbuch, F1-Hilfe (DocBook) | offen |
| Flatpak/AppImage in der CI tatsächlich bauen | ✓ `release.yml` baut beide und hängt sie an Tags. Flatpak-Laufzeit von 6.8 (End of Life) auf 6.10 gehoben; Poppler und Ghostscript müssen im Bündel selbst gebaut werden, weil die KDE-Laufzeit sie nicht mitbringt und ein Sandkasten das `gs` des Wirts nicht leihen kann. Manifest mit `--download-only` geprüft, alle Prüfsummen selbst berechnet. AppImage läuft hier von Anfang bis Ende durch. |
| Handbuch, F1-Hilfe (DocBook) | ✓ 14 Kapitel, `handbook-valid` als Test, F1 löst `help:/pdf-smithy` auf |
| **Stille Locale-Rückfälle** | ✓ Der wichtigste Fund der CI-Arbeit: glibc fällt bei einer *nicht erzeugten* Locale **stillschweigend auf C zurück**. Die deutschen Testläufe wären damit zu grünen Kopien der C-Läufe geworden — schlimmer als ein Fehlschlag, weil es wie Abdeckung aussieht. Jetzt prüft `tst_pdfgeometry`, dass das Dezimaltrennzeichen unter `de_DE` wirklich ein Komma ist, und die CI erzeugt die Locale und prüft es nach. |
| KWallet, Dolphin-Servicemenüs, Purpose | offen |
| PDF/UA | offen — braucht Struktur-Tags, ein eigener Block |
