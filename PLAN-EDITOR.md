# Stufe 8 — Das Profiwerkzeug

Ziel: nicht „auch ein PDF-Editor", sondern **das Werkzeug, das ein Fachmann
wählt** — mit einer Seitenansicht, in der man Inhalte direkt anfasst, und mit
Prüf- und Druckvorstufenfunktionen, die es unter Linux bisher gar nicht gibt.

Recherchiert an Acrobat Pro, Enfocus PitStop Pro, PDF Studio Pro, Foxit,
PDF-XChange und den Vorstufenanforderungen von PDF/X. Wo diese Werkzeuge etwas
können, steht es hier. Wo sie es nicht können und es sinnvoll ist, steht es auch.

---

## 1. Das tragende Prinzip

> **Bearbeiten heißt den Anweisungsstrom umschreiben, nicht das Dokument neu
> erzeugen.**

Ein bewegtes Bild wird zu `q <cm> … Q` um die bestehenden Operatoren herum. Ein
gelöschtes Objekt sind weggelassene Operatoren. Neues wird angehängt. Alles
Unberührte behält seine Bytes.

Deshalb *nicht* rendern-und-neu-zeichnen: ein Editor, der beim ersten Verschieben
eines Kastens den ganzen Scan neu kodiert, ist ein Konverter.

**Zweites Prinzip: ehrlich über Grenzen.** Jede Klasse hat `limitations()`. Wo
etwas nicht geht, wird es benannt statt still falsch gemacht. Das ist der Grund,
dem Werkzeug zu trauen.

---

## 2. Was heute steht

Seitenverwaltung · Zusammenfügen und Teilen · verlustfreies Speichern · OCR ·
Geraderücken · Komprimierung · Signaturstempel · Wasserzeichen · Seitenzahlen und
Bates · Zuschneiden · N-up · Metadaten · Verschlüsselung · **echte Schwärzung** ·
**Anmerkungen mit XFDF** · **Lesezeichen und Links** · **Formulare ausfüllen** ·
**Textkorrektur** · **PDF/A** · **Dokumentvergleich** · Stapelverarbeitung ·
Drucken mit Ausschießen · Kommandozeile für alles · 51 Tests in zwei Locales.

**Die Lücke:** es ist ein Miniaturbildgitter mit Werkzeugdialogen. Ein Profitool
ist eine Dokumentansicht, auf der man arbeitet.

---

## 3. Der Katalog

Legende: **✓** vorhanden · **○** geplant · **◐** teilweise · **⚑** schwer, eigener Zug

### 3.1 Objektmodell und Direktbearbeitung  ·  Phase 1–3

| | Funktion |
|---|---|
| ○ | Content-Stream in anfassbare Objekte zerlegen: Text, Bild, Pfad, Zeichnung, Schattierung, Inline-Bild |
| ○ | Grenzen je Objekt in Anzeigepunkten, Clip-Zustand berücksichtigt |
| ○ | Z-Reihenfolge lesen und ändern |
| ○ | Treffertest über genäherten Pfadumriss |
| ○ | Textruns mit gleicher Grundlinie, Schrift und Größe zu Absätzen gruppieren |
| ○ | Auswahl: Klick, Gummiband, Tab, Alt+Klick greift darunter, Strg+A |
| ○ | Verschieben mit Ziehen und mit Pfeiltasten, feiner mit Umschalt |
| ○ | Skalieren über acht Griffe, Seitenverhältnis mit Umschalt |
| ○ | Drehen über Griff, 15°-Raster mit Strg |
| ○ | Neigen (Scheren) |
| ○ | Spiegeln waagerecht und senkrecht |
| ○ | Löschen, Ausschneiden, Kopieren, Einfügen, an gleicher Stelle einfügen |
| ○ | Kopieren zwischen Seiten und zwischen Dokumenten, Ressourcen mit |
| ○ | Duplizieren mit Versatz |
| ○ | Gruppieren und Gruppierung aufheben (als Form-XObject) |
| ○ | Sperren gegen Verschieben |
| ○ | Ausrichten: links, Mitte, rechts, oben, Mitte, unten — an Auswahl, Seite oder Rand |
| ○ | Verteilen mit gleichen Abständen, waagerecht und senkrecht |
| ○ | Gleiche Breite, Höhe, Größe machen |
| ○ | Hilfslinien, Raster, magnetisches Einrasten an Objekten und Rändern |
| ○ | Lineale in Punkt, Millimeter, Zoll, Pica |
| ○ | Position und Größe numerisch eingeben |
| ○ | Eigenschaftenleiste: Füllung, Kontur, Strichbreite, Strichart, Deckkraft, Mischmodus |
| ○ | Mehrfachauswahl mit gemeinsamer Bearbeitung |
| ⚑ | Zeichnung (Form-XObject) auflösen, um hineinzukommen |

### 3.2 Text  ·  Phase 3–4

| | Funktion |
|---|---|
| ✓ | Zeile ersetzen in der Schrift der Seite |
| ✓ | Ablehnung mit Nennung der fehlenden Zeichen |
| ✓ | `/ToUnicode`, benannte Encodings, `/Differences` mit Glyphennamen |
| ○ | Schreibmarke auf der Seite, Tippen, Auswählen mit Maus und Tastatur |
| ○ | Absatz umbrechen innerhalb seines Rahmens |
| ○ | Neuer Textrahmen als echter Seiteninhalt |
| ○ | Schrift, Größe, Farbe, Laufweite, Zeilenabstand, Wortabstand wählen |
| ○ | Fett, kursiv über echte Schriftschnitte (nicht künstlich geneigt) |
| ○ | Ausrichtung links, rechts, zentriert, Blocksatz |
| ○ | Hoch- und tiefgestellt, Kapitälchen, Unterstreichen, Durchstreichen |
| ○ | Aufzählungen und Nummerierung |
| ○ | Tabulatoren und Einzüge |
| ○ | Textfluss über verbundene Rahmen |
| ○ | Text auf einem Pfad |
| ○ | Suchen und Ersetzen im ganzen Dokument, mit regulären Ausdrücken |
| ○ | Rechtschreibprüfung über Sonnet, während des Tippens |
| ○ | Silbentrennung über Hyphen |
| ○ | Sonderzeichen einfügen, Zeichentabelle der Schrift |
| ○ | Text aus Datei einfügen (Klartext, Markdown) und setzen |
| ⚑ | **Klartext oder Markdown zu PDF setzen** — eigener Satzmotor mit Umbruch, Seitenumbruch, Kopf- und Fußzeilen |
| ○ | Text als Kurven ausgeben (Schrift in Pfade wandeln) |
| ○ | OCR-Textebene bearbeiten und korrigieren |
| ○ | Schreibrichtung rechts-nach-links |

### 3.3 Schriften  ·  Phase 5

| | Funktion |
|---|---|
| ○ | Inventar: welche Schriften, wo benutzt, eingebettet oder nicht, Teilmenge oder vollständig |
| ○ | Glyphenabdeckung je Schrift anzeigen |
| ⚑ | **Einbetten** mit Teilmengenbildung: TrueType (`glyf`, `loca`, `cmap`, `hmtx`, `head`, `maxp`, `hhea`, `post`) und CFF |
| ○ | Systemschrift über Fontconfig finden und einbetten |
| ○ | Schrift im ganzen Dokument ersetzen, mit Warnung über Breitenänderung |
| ○ | Einbettung entfernen, um die Datei zu verkleinern |
| ○ | Teilmengen derselben Schrift zusammenlegen |
| ○ | Fehlende `/ToUnicode` erzeugen, damit Text kopierbar wird |
| ○ | Type3-Schriften erkennen und benennen |
| ○ | Schriftlizenz-Flags (`fsType`) lesen und vor dem Einbetten warnen |
| ○ | Doppelte Schriftobjekte zusammenführen |

### 3.4 Bilder  ·  Phase 6

| | Funktion |
|---|---|
| ○ | Inventar: Seite, Platzierung, Pixelmaß, **effektive dpi**, Kodierung, Farbraum, Bittiefe, Maske |
| ○ | Verschieben, skalieren, drehen, spiegeln, neigen |
| ○ | Beschneiden — echt, die Pixel werden entfernt, nicht nur ein Clip gesetzt |
| ○ | Ersetzen an gleicher Stelle |
| ○ | Einfügen aus Datei, Zwischenablage, Kamera, Bildschirmausschnitt |
| ○ | Einzelbild komprimieren: dpi und Güte je Bild, Rest unberührt |
| ○ | Farbraum wandeln: Grau, RGB, CMYK, Bitmap |
| ○ | Helligkeit, Kontrast, Gamma, Schärfen, Entrauschen |
| ○ | Entrastern für Scans, Weißabgleich, Rand entfernen |
| ○ | Alphakanal und Maske bearbeiten, freistellen |
| ○ | Herausziehen als PNG, JPEG, TIFF |
| ○ | Löschen samt Ressource |
| ○ | Alle Bilder einer Seite oder des Dokuments in einem Zug behandeln |
| ○ | Duplikate finden und zusammenführen |
| ○ | JPEG neu kodieren vermeiden, wo unnötig (verlustfrei durchreichen) |

### 3.5 Farbe und Druckvorstufe  ·  Phase 7

| | Funktion |
|---|---|
| ○ | Farbräume im Dokument auflisten: DeviceRGB, DeviceGray, DeviceCMYK, ICCBased, Indexed, Separation, DeviceN, Lab |
| ○ | **In Graustufen wandeln** — Dokument, Seite oder Objekt |
| ○ | In Schwarzweiß wandeln, mit Schwelle oder Rasterung |
| ○ | RGB nach CMYK und zurück, über ICC-Profile |
| ○ | Sonderfarben auflisten, in Prozess wandeln, umbenennen, zusammenlegen |
| ○ | Sonderfarben-Aliase auflösen (zwei Namen, eine Farbe) |
| ○ | Farbe global ersetzen (alle Vorkommen von X werden Y) |
| ○ | Reines Schwarz gegen Schwarzaufbau, Schwarz überdrucken setzen |
| ○ | Überdrucken je Objekt anzeigen und setzen |
| ○ | Gesamtfarbauftrag prüfen, Grenze setzen, Überschreitungen zeigen |
| ○ | Separationsvorschau, einzelne Auszüge ein- und ausblenden |
| ○ | Tonwertumfang je Auszug prüfen |
| ○ | ICC-Profil einbetten, austauschen, entfernen |
| ○ | Output Intent setzen und prüfen |
| ○ | Rendering Intent je Objekt |
| ○ | Transparenz reduzieren (flatten) mit wählbarer Auflösung |
| ○ | Haarlinien finden und auf Mindestbreite setzen |
| ○ | Weiße Objekte auf Überdrucken prüfen (verschwinden im Druck) |
| ○ | Papierweiß-Simulation in der Vorschau |
| ○ | Farbblindheits-Simulation |

### 3.6 Preflight  ·  Phase 8

| | Funktion |
|---|---|
| ○ | Profile: PDF/A-1b, -2b, -3b, PDF/X-1a, -3, -4, druckfertig, Web, barrierefrei, eigenes |
| ○ | Profil als JSON speichern, laden, teilen |
| ○ | Befund mit Schwere, Seite, Ort, Objekt und Sprungmarke |
| ○ | **Behebungen, die sich anwenden lassen**, jede mit Vorschau, was sie ändert |
| ○ | Prüfen: Schriften nicht eingebettet, Teilmenge ohne `/ToUnicode`, Type3, geschützt |
| ○ | Prüfen: Bildauflösung unter und über Grenzen, je Bildart |
| ○ | Prüfen: Farbräume gegen Profil, Sonderfarbenzahl, ICC fehlt |
| ○ | Prüfen: Transparenz, Mischmodi, Weichzeichner |
| ○ | Prüfen: Boxen fehlen, MediaBox ≠ CropBox, Beschnittzugabe zu klein |
| ○ | Prüfen: Seitengrößen und Drehungen gemischt |
| ○ | Prüfen: JavaScript, Anhänge, Multimedia, offene Aktionen |
| ○ | Prüfen: Verschlüsselung, Berechtigungen |
| ○ | Prüfen: Metadaten fehlen, XMP widerspricht `/Info` |
| ○ | Prüfen: Haarlinien, Strichbreite null, Text unter Mindestgröße |
| ○ | Prüfen: Text zu nah am Rand, Objekte außerhalb der TrimBox |
| ○ | Prüfen: leere Seiten, doppelte Seiten |
| ○ | Bericht als PDF mit Hervorhebungen und als JSON |
| ○ | Stapel-Preflight über einen Ordner mit Sammelbericht |
| ○ | Ehrlichkeit: welche Regeln geprüft wurden, welche nicht, und dass veraPDF der verbindliche Prüfer ist |

### 3.7 Formulare erstellen  ·  Phase 9

| | Funktion |
|---|---|
| ✓ | Ausfüllen, Ankreuzzustand richtig, gesperrte Felder, Erscheinungsbilder erzeugen |
| ✓ | Festschreiben |
| ○ | Feld anlegen: Text, Ankreuzfeld, Optionsgruppe, Auswahlliste, Kombinationsfeld, Schaltfläche, Signaturfeld, Barcode |
| ○ | Platzieren, verschieben, in Größe ändern wie jedes Objekt |
| ○ | Name, Beschriftung, Werkzeugtipp |
| ○ | Erforderlich, gesperrt, mehrzeilig, Zeichenzahl, Passwortfeld, Kammfeld, Rechtschreibprüfung |
| ○ | Aussehen: Rahmenfarbe und -stil, Hintergrund, Schrift, Größe, Ausrichtung |
| ○ | Listeneinträge mit Export-Werten, Anfangswert, Sortierung |
| ○ | Tabulatorfolge je Seite, sichtbar bearbeitbar |
| ○ | Format: Zahl, Währung, Prozent, Datum, Zeit, Postleitzahl, Telefon, eigenes |
| ○ | Prüfung: Bereich, Länge, eigenes Skript — **mit Warnung**, dass Skripte in PDF/A verboten sind |
| ○ | Berechnung: Summe, Produkt, Mittel, Minimum, Maximum, eigenes; Reihenfolge in `/CO` |
| ○ | Aktionen: Formular zurücksetzen, absenden, Seite öffnen, URL, Datei |
| ○ | Felder erkennen — Linien und Kästchen auf einer gescannten Vorlage vorschlagen |
| ○ | Felder aus einem anderen Dokument übernehmen |
| ○ | Felder umbenennen im Stapel, Präfixe |
| ○ | Daten importieren und exportieren: FDF, XFDF, XML, CSV |
| ○ | Mehrere ausgefüllte Formulare zu einer Tabelle sammeln |
| ○ | XFA erkennen und benennen (nicht unterstützt, aber nicht verschweigen) |

### 3.8 Seiten und Layout  ·  Phase 10

| | Funktion |
|---|---|
| ✓ | Sortieren, drehen, löschen, duplizieren, herausziehen, zuschneiden, N-up |
| ○ | Alle fünf Boxen bearbeiten: Media, Crop, Bleed, Trim, Art |
| ○ | Seitengröße ändern mit Skalieren oder mit Zentrieren |
| ○ | Seiten auf gemeinsame Größe bringen |
| ○ | Leere Seite einfügen, Seite aus Vorlage |
| ○ | Seite teilen (eine Doppelseite in zwei) |
| ○ | Seiten zusammenlegen (zwei auf eine, überlagernd) |
| ○ | Ausschießen: Broschüre, Sammelform, Nutzen mit Beschnittmarken |
| ○ | Beschnittmarken, Passermarken, Farbkontrollstreifen, Seiteninformation |
| ○ | Poster: eine Seite auf mehrere Blätter mit Überlappung |
| ○ | Skalieren und Zentrieren mit Rand |
| ○ | Kopf- und Fußzeilen mit Feldern, mehrzeilig, je Bereich |
| ✓ | Seitenzahlen und Bates |
| ○ | Seitenbeschriftungen `/PageLabels`: römisch, alphabetisch, Präfix, Abschnitte |
| ○ | Hintergrund und Wasserzeichen als bearbeitbares Element, nicht eingebrannt |
| ○ | Seiten verschachteln (Reader-Spreads zu Printer-Spreads) |
| ○ | Leere Seiten finden und entfernen |
| ○ | Seiten nach Inhalt sortieren (Barcode, Text, Seitenzahl) |

### 3.9 Struktur, Navigation, Barrierefreiheit  ·  Phase 11

| | Funktion |
|---|---|
| ✓ | Inhaltsverzeichnis lesen, schreiben, bearbeiten, folgt der Seite |
| ✓ | Interne Links folgen der Seite, tote werden entfernt |
| ○ | Link zeichnen, Ziel wählen: Seite, URL, Datei, benannte Stelle |
| ○ | Links im Stapel erzeugen (alle URLs im Text verlinken) |
| ○ | Benannte Stellen verwalten |
| ○ | Lesezeichen aus Überschriften vorschlagen (Textgröße als Hinweis) |
| ○ | Anhänge: auflisten, hinzufügen, herausziehen, entfernen |
| ○ | Ebenen (OCG): ein- und ausblenden, umbenennen, zusammenlegen, entfernen, Startzustand |
| ○ | Artikel-Ketten |
| ⚑ | **Struktur-Tags**: Baum anzeigen, bearbeiten, Leseordnung ändern |
| ○ | Alternativtexte für Bilder, Tabellenköpfe, Sprache je Abschnitt |
| ○ | PDF/UA prüfen und berichten |
| ○ | Tags automatisch vorschlagen aus Textgrößen und Anordnung |
| ○ | Vorlesereihenfolge sichtbar machen und ziehen |

### 3.10 Anmerkungen  ·  ✓ mit Ergänzungen

| | Funktion |
|---|---|
| ✓ | Neun Typen mit eigenen Erscheinungsbildern, XFDF |
| ○ | Stempel mit Bibliothek: „Genehmigt", „Entwurf", eigene, mit Datum und Name |
| ○ | Textkommentar in der Seitenleiste, Antworten, Status, erledigt |
| ○ | Anmerkungen zusammenführen aus mehreren Rückläufen |
| ○ | Nach Autor, Typ, Datum, Status filtern |
| ○ | Zusammenfassung als Dokument drucken |
| ○ | Maßwerkzeuge: Strecke, Umfang, Fläche, mit Maßstab und Einheit |
| ○ | Wolkenform, Pfeilspitzenarten, Linienenden |
| ○ | Ton- und Dateianlagen als Anmerkung |

### 3.11 Sicherheit und Signatur  ·  Phase 12

| | Funktion |
|---|---|
| ✓ | AES-256, Passwort setzen und entfernen, Berechtigungen |
| ✓ | Echte Schwärzung |
| ○ | Schwärzungsmuster: Suchen und alle Vorkommen schwärzen, reguläre Ausdrücke |
| ○ | Vorlagen: Sozialversicherungsnummer, IBAN, Telefon, E-Mail |
| ○ | Bereinigen: verborgene Ebenen, gelöschter Inhalt, Metadaten, Lesezeichen, Anhänge, JavaScript |
| ⚑ | **Kryptografische Signatur**: PKCS#12, PKCS#11, Signaturfeld, Zeitstempel |
| ○ | Signatur prüfen, Zertifikatskette, Widerrufsprüfung |
| ○ | Mehrere Signaturen, inkrementelle Aktualisierung |
| ○ | Zertifikatsbasierte Verschlüsselung für Empfänger |
| ○ | Berechtigungen prüfen und anzeigen, was ein Leseprogramm erlaubt |

### 3.12 Umwandeln  ·  Phase 13

| | Funktion |
|---|---|
| ✓ | Bilder → PDF, PDF → Bilder, OCR |
| ✓ | PDF/A |
| ○ | PDF/X-1a, -3, -4 |
| ○ | PDF → Text mit Layout, PDF → Markdown |
| ○ | PDF → HTML mit eingebetteten Bildern |
| ○ | PDF → CSV aus Tabellen (Tabellenerkennung) |
| ○ | PDF → SVG je Seite |
| ○ | Klartext, Markdown, HTML → PDF über den eigenen Satzmotor |
| ○ | PostScript und EPS → PDF über Ghostscript |
| ○ | ODT und DOCX → PDF über LibreOffice, wenn vorhanden |
| ○ | Linearisieren für schnelles Web-Anzeigen |
| ○ | PDF-Version herauf- und herabsetzen |

### 3.13 Ansicht und Bedienung  ·  Phase 2

| | Funktion |
|---|---|
| ○ | Durchgehende Seitenansicht, scrollbar, mit Miniaturleiste |
| ○ | Zoom: Stufen, Breite, Seite, Auswahl, Strg+Rad, Pinch, Lupe |
| ○ | Einzelseite, durchgehend, Doppelseite, Doppelseite mit Deckblatt |
| ○ | Geteilte Ansicht, zwei Stellen desselben Dokuments |
| ○ | Vollbild und Präsentationsmodus |
| ○ | Seitenleisten: Seiten, Inhalt, Kommentare, Anhänge, Ebenen, Schriften, Preflight |
| ○ | Suche über das Dokument mit Trefferliste |
| ○ | Lesezeichenleiste, zuletzt besuchte Stellen, vor und zurück |
| ○ | Dunkles Thema und Papierfarbe umstellen |
| ○ | Sitzung wiederherstellen, mehrere Reiter |
| ○ | Tastenkürzel vollständig und anpassbar |
| ○ | Werkzeugleisten anpassbar |
| ○ | Barrierefreie Bedienung: Tastatur überall, Bildschirmleser-Beschriftungen |

### 3.14 Automatisierung  ·  Phase 14

| | Funktion |
|---|---|
| ✓ | Kommandozeile für jede Dokumentoperation, Stapelverarbeitung |
| ○ | Benannte Aktionsfolgen, in der Oberfläche zusammengestellt, auf der Kommandozeile ausführbar |
| ○ | Überwachter Ordner |
| ○ | D-Bus-Schnittstelle |
| ○ | Dolphin-Servicemenüs: zusammenfügen, komprimieren, OCR, in Bilder |
| ○ | Purpose-Teilen |
| ○ | KWallet für Passwörter |
| ○ | Vorschau-Erweiterung für Dolphin und Gwenview |

---

## 3a. Stand der Umsetzung

| Zug | Stand |
|---|---|
| **Phase 1 · Objektmodell** | ✓ `PageObjects` und `PageComposer`, 14 Tests in beiden Locales. Der tragende Fall: dasselbe Rechteck normal und unter vierfacher Skalierung, beide um 40 Punkte verschoben — beide bewegen sich um 40 Punkte, weil die Verschiebung mit der aktuellen Matrix konjugiert wird (`CTM × T × CTM⁻¹`). Ohne das verschiebt sich ein Objekt um seinen eigenen Maßstab falsch. Weiter nachgewiesen: Unberührtes bleibt byteidentisch; Gelöschtes verschwindet aus dem *gerenderten Bild*; Grenzen stimmen mit der Farbe im gerenderten Bild überein; gedrehte Seiten; Treffertest nimmt bei Vektorgrafik die Form statt des Rechtecks; auf Seite 3 des echten Magazins über 20 Objekte richtig erkannt |
| **Phase 2 · Dokumentansicht** | offen — der nächste Schritt |
| **Schrifteinbettung** | Code steht (`FontEmbedder` 1466, `FontInventory` 1009 Zeilen), **Tests noch nicht nachgewiesen** — der Agent brach am Sitzungslimit ab, mitten in der Korrektur einer eigenen Testannahme über die sfnt-Prüfsumme des `head`-Blocks |
| **Bilder** | ✓ 24 Testfälle grün in beiden Locales (`ImageEdit`, 2020 Zeilen). Der Agent brach beim Hinzufügen dreier weiterer Fälle ab, das Vorhandene trägt |
| **Farbe und Druckvorstufe** | Code steht (`ColourTools` 3603 Zeilen, 19 Testfälle), **ungeprüft** — Agent brach beim Neuschreiben von `mapSpotColours` ab |
| **Preflight** | ✓ 37 Testfälle grün in beiden Locales (`Preflight`, 3047 Zeilen) |
| **Formularerstellung** | Code steht (`FormBuilder` 2347 Zeilen, 20 Testfälle), **ungeprüft** — Agent brach früh in der Umsetzung ab |
| **Seiten und Layout** | Code steht (`PageLayout` 1658 Zeilen, 27 Testfälle), **ungeprüft** — Agent brach bei einem echten Fund ab: Druckermarken benutzen WinAnsi, er wandelte nach Latin-1 |
| **Export und PDF/X** | ◐ `Convert` (2594 Zeilen) übersetzt und der Testlauf ist grün — **aber die Testdatei hat noch keine Fälle**, das Grün sagt also nichts. Hier fehlt die Prüfung ganz |
| **Satzmotor** | Code steht (`Typeset` 2050 Zeilen, 20 Testfälle), **ungeprüft** — Agent brach beim Beheben zweier Testfehler ab |

---

### Wie dieser Stand zustande kam, und was das für den nächsten Zug heißt

Acht Züge liefen gleichzeitig als eigenständige Agenten. Alle acht wurden vom
**Sitzungslimit** unterbrochen, jeder mitten in der Arbeit — mehrere davon
mitten in der Behebung eines Fehlers, den sie selbst gefunden hatten. Der Code
liegt vollständig auf der Platte und **übersetzt**, aber:

> **Kein einziger dieser acht Züge hat seine Tests selbst bestanden gesehen.**

Nachgemessen nach dem Abbruch: **60 von 70 Tests bestanden.** Drei Module sind
trotz des Abbruchs grün — `ImageEdit`, `Preflight` und `Convert`. Fünf scheitern
in beiden Locales: `Typeset`, `PageLayout`, `FormBuilder`, `FontEmbedder` /
`FontInventory`, `ColourTools`.

Das ist ausdrücklich kein „fertig". Wer hier weitermacht, fängt nicht bei null
an, aber auch nicht bei etwas Verlässlichem. Die Reihenfolge, die sich daraus
ergibt:

1. **Erst die Suite grün bekommen.** Jeder der acht Züge hat Tests geschrieben;
   sie müssen laufen, bevor irgendetwas davon angebunden wird.
2. **Dann die Funde nachziehen**, die die Agenten beim Abbruch in der Hand
   hatten — etwa die WinAnsi-statt-Latin-1-Wandlung bei den Druckermarken und
   die sfnt-Prüfsumme des `head`-Blocks, deren Anpassungsfeld beim Berechnen auf
   null stehen muss.
3. **Erst danach** Oberfläche und Kommandozeile.

Ein Modul, das übersetzt, aber dessen Tests nie gelaufen sind, ist eine
Behauptung, keine Funktion.

---

## 4. Züge und Abhängigkeiten

```
   Phase 1  Objektmodell  ── blockiert alles Sichtbare
        │
        ├── Phase 2  Dokumentansicht ── Phase 3  Direktbearbeitung
        │                                     │
        │                              Phase 4  Inhalte einfügen
        │
   unabhängig, sofort startbar:
        ├── Schrifteinbetter (⚑ Kern von Phase 4 und 5)
        ├── Phase 6  Bilder        (XObject-Ebene)
        ├── Phase 7  Farbe         (Ressourcen-Ebene)
        ├── Phase 8  Preflight     (nur lesend, dann Behebungen)
        ├── Phase 9  Formulare     (Annots-Ebene)
        ├── Phase 10 Seiten/Layout (Seitenbaum)
        ├── Phase 11 Struktur      (Katalog-Ebene)
        ├── Phase 12 Signatur      (eigenständig)
        ├── Phase 13 Umwandeln     (eigenständig)
        └── Phase 14 Automatisierung
```

**Sofort parallel**: Schrifteinbetter, Bilder, Farbe, Preflight, Formulare,
Seiten/Layout, Umwandeln, Satzmotor. Das sind acht unabhängige Züge.

---

## 5. Was unverhandelbar bleibt

1. **Verlustfrei.** Unberührter Inhalt behält seine Bytes; ein Test vergleicht
   Content-Streams unbearbeiteter Seiten byteweise.
2. **Ehrlich über Grenzen.** Jede Klasse hat `limitations()`. Nie still das
   Falsche tun.
3. **Locale-sicher.** Zahlen nur über `PdfGeometry::number()` und
   `numericValue()`. Jeder Test läuft unter `C` **und** `de_DE.UTF-8`.
4. **Übersetzbar.** Nur `i18n`-Schlüsselwörter.
5. **Kommandozeilen-Parität.** Jede Dokumentoperation ist skriptbar.
6. **An echten Dokumenten nachgewiesen**, nicht nur an Fixtures.
7. **MIT im Kern.** Poppler und Ghostscript bleiben hinter Schnittstellen und
   Prozessgrenzen.

---

## 6. Befunde aus der Anbindung an die Kommandozeile

Beim Freilegen der neun Module über die Kommandozeile ist jeder Kopf gegen das
gelesen worden, was ein Anwender erwartet. Was dabei als *fehlend* auffiel,
steht hier — nicht als Wunschliste, sondern als belegte Lücke.

### Schriften — die schwerwiegendste Gruppe

| Lücke | Warum sie zählt |
|---|---|
| `FontEmbedder::embed` bindet vorhandenen Text nicht an die neu eingebettete Schrift | „Diesem Dokument fehlt Helvetica" ist damit nicht behebbar; die Seite ruft weiter `/F1` auf |
| Kein Ersetzen einer Schrift durch eine andere | Ausdrücklich verlangt; existiert im Kern gar nicht |
| Kein Verkleinern einer bereits eingebetteten Schrift | `mergeSubsets` fügt zusammen, reduziert aber nichts |
| `FontEmbedder::embed` nimmt ein lebendes `QPDF &` statt Pfaden | Zwingt QPDF-Code in jeden Aufrufer; als einziges Kernmodul aus der Reihe |
| `FontUse` kennt kein Symbol-Kennzeichen | `fonts list` kann symbolische Schriften nicht ausweisen |
| `FontUse::licence` ist ein übersetzter Satz, kein Code | Eine Prüfung darauf bräche in jeder anderen Sprache |

### Bilder

| Lücke | Warum sie zählt |
|---|---|
| Kein Drehen im Kern | Über Extrahieren → Drehen → Ersetzen nachgebildet, dadurch nur Vierteldrehungen und eine zusätzliche JPEG-Generation |
| Reine Qualitätsneucodierung nicht möglich | `recompress()` überspringt Bilder, deren Pixelzahl unverändert bleibt — `--quality 40` allein täte nichts |
| JPEG 2000 wird weder gelesen noch geschrieben | Wird namentlich abgelehnt statt still auf JPEG auszuweichen |

### Farbe

| Lücke | Warum sie zählt |
|---|---|
| `inkCoverage` hat keine Auflösungsangabe | Fest 72 dpi; für eine Deckungsprüfung grob |
| `recompressPhotographs` hat keinen eigenen Schalter | Über `--quality 0` erreichbar, aber verdeckt |

Alle drei Gruppen halten sich an Regel 2 aus Abschnitt 5: sie sagen, was sie
nicht können, statt es vorzutäuschen. Das macht die Lücken erträglich — aber
nicht kleiner.

### Erledigt

- **Bilder drehen** — `ImageEdit::rotate()`. Über die Platzierungsmatrix statt
  über die Pixel: der gespeicherte Strom kommt byteweise unverändert heraus,
  jeder Winkel geht, und selbst ein Bild, das dieses Werkzeug gar nicht
  decodieren kann, lässt sich drehen. Nachgewiesen dadurch, dass vier
  Vierteldrehungen wieder bei der Ausgangsplatzierung landen — der Test, der
  eine falsch herum gerechnete Konjugation auffliegen lässt.
- **Neucodierung bei gleicher Größe** — `Recompress::forceReencode`.
- **WinAnsi statt Latin-1**, in beide Richtungen: beim Schreiben von
  Wasserzeichen und Signaturen (`Overlay.cpp` benutzte `toLatin1()`) und beim
  *Lesen* von Text (`TextEdit.cpp` las WinAnsi als Latin-1, MacRoman komplett
  falsch). Die Leserichtung war die schwerwiegendere: sie betraf Textauszug,
  Suche und Schwärzung. Tabellen jetzt erzeugt, nicht abgetippt
  (`tools/generate-encodings.py`).
