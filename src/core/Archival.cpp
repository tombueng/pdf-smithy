/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Archival.h"
#include "PdfFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <cstdio>
#include <set>

namespace ps {

namespace {

/** Embedding fonts into a long scanned book is not quick; give it room. */
constexpr int GhostscriptTimeoutMs = 15 * 60 * 1000;

/** How deep to follow form XObjects when looking for fonts. */
constexpr int MaxResourceDepth = 8;

/**
 * The faults that stop a file being archival, as things rather than sentences.
 *
 * Kept separate from their wording because each one has to be said twice: once
 * as a problem the file still has, and once as a change the conversion made. A
 * single phrasing would have to serve both, and "the document is encrypted" is
 * not a useful thing to tell someone about a document that has just been
 * decrypted.
 */
enum class Fault {
    Encrypted,
    Scripts,
    EmbeddedFiles,
    UnembeddedFonts,
    NoOutputIntent,
};

struct Scan {
    /** In a fixed order, so two scans can be compared and the output is stable. */
    QVector<Fault> faults;

    QStringList unembeddedFonts;

    QString claimedPart;
    QString claimedConformance;

    bool contains(Fault fault) const { return faults.contains(fault); }

    /** The designation the file claims, such as `PDF/A-2B`, or empty. */
    QString claimedLevel() const
    {
        if (claimedPart.isEmpty()) {
            return {};
        }
        // A standard's name is the same in every language, so it is deliberately
        // not translated.
        return QLatin1String("PDF/A-") + claimedPart + claimedConformance.toUpper();
    }
};

std::string nameOf(QPDFObjectHandle dict, const char *key)
{
    if (!dict.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = dict.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

bool hasFontFile(QPDFObjectHandle descriptor)
{
    if (!descriptor.isDictionary()) {
        return false;
    }
    // Type 1, TrueType and the compact/OpenType forms respectively. Any one of
    // them means the glyphs travel with the document.
    return descriptor.hasKey("/FontFile") || descriptor.hasKey("/FontFile2") || descriptor.hasKey("/FontFile3");
}

bool fontIsEmbedded(QPDFObjectHandle font)
{
    const std::string subtype = nameOf(font, "/Subtype");

    if (subtype == "/Type3") {
        // A Type 3 font's glyphs are content streams inside the document
        // already, so there is nothing to embed and nothing to lose.
        return true;
    }

    if (subtype == "/Type0") {
        // The composite font is a wrapper; the descriptor that matters belongs
        // to the CIDFont underneath it.
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() > 0) {
            return hasFontFile(descendants.getArrayItem(0).getKey("/FontDescriptor"));
        }
        return false;
    }

    // A base-14 font such as Helvetica is legal in an ordinary PDF and usually
    // has no descriptor at all. That is exactly the case PDF/A rules out.
    return hasFontFile(font.getKey("/FontDescriptor"));
}

QString fontLabel(QPDFObjectHandle font)
{
    QString name = QString::fromStdString(nameOf(font, "/BaseFont"));
    if (name.startsWith(QLatin1Char('/'))) {
        name.remove(0, 1);
    }
    // Subsetting prefixes a random six-letter tag, which means nothing to
    // anybody reading the report.
    static const QRegularExpression subsetTag(QStringLiteral("^[A-Z]{6}\\+"));
    name.remove(subsetTag);
    return name.isEmpty() ? i18nc("@item a font the file does not name", "an unnamed font") : name;
}

/** Walks a resource dictionary and every form XObject reachable from it. */
void collectUnembeddedFonts(QPDFObjectHandle resources, QStringList &found, std::set<QPDFObjGen> &visited,
                            int depth = 0)
{
    if (depth > MaxResourceDepth || !resources.isDictionary()) {
        return;
    }

    QPDFObjectHandle fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        for (const auto &[key, font] : fonts.getDictAsMap()) {
            Q_UNUSED(key)
            if (!font.isDictionary()) {
                continue;
            }
            // Only indirect objects can be shared, and a direct object's
            // generation pair is always 0/0, and recording those would make the
            // first one seen swallow all the rest.
            if (font.isIndirect()) {
                if (visited.count(font.getObjGen()) > 0) {
                    continue;
                }
                visited.insert(font.getObjGen());
            }
            if (!fontIsEmbedded(font)) {
                const QString label = fontLabel(font);
                if (!found.contains(label)) {
                    found.append(label);
                }
            }
        }
    }

    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    if (!xobjects.isDictionary()) {
        return;
    }
    for (const auto &[key, object] : xobjects.getDictAsMap()) {
        Q_UNUSED(key)
        if (!object.isStream()) {
            continue;
        }
        if (object.isIndirect()) {
            if (visited.count(object.getObjGen()) > 0) {
                continue;
            }
            visited.insert(object.getObjGen());
        }
        collectUnembeddedFonts(object.getDict().getKey("/Resources"), found, visited, depth + 1);
    }
}

bool documentHasScripts(QPDFObjectHandle root)
{
    QPDFObjectHandle names = root.getKey("/Names");
    if (names.isDictionary() && names.hasKey("/JavaScript")) {
        return true;
    }
    // An /OpenAction that is merely a destination is harmless and common; only
    // one that runs something is a fault.
    QPDFObjectHandle openAction = root.getKey("/OpenAction");
    if (openAction.isDictionary() && (openAction.hasKey("/JS") || nameOf(openAction, "/S") == "/JavaScript")) {
        return true;
    }
    // Additional actions fire on events the user never asked about, which is
    // the whole reason the standard forbids them.
    return root.hasKey("/AA");
}

int embeddedFileCount(QPDFObjectHandle root)
{
    int count = 0;

    QPDFObjectHandle names = root.getKey("/Names");
    if (names.isDictionary()) {
        QPDFObjectHandle files = names.getKey("/EmbeddedFiles");
        if (files.isDictionary()) {
            QPDFObjectHandle list = files.getKey("/Names");
            if (list.isArray()) {
                // The array alternates name, reference, so it holds half as many
                // files as it has entries.
                count += list.getArrayNItems() / 2;
            }
        }
    }
    return count;
}

bool pageHasAttachment(QPDFPageObjectHelper &page)
{
    QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
    if (!annotations.isArray()) {
        return false;
    }
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        if (nameOf(annotations.getArrayItem(i), "/Subtype") == "/FileAttachment") {
            return true;
        }
    }
    return false;
}

bool hasPdfAOutputIntent(QPDFObjectHandle root)
{
    QPDFObjectHandle intents = root.getKey("/OutputIntents");
    if (!intents.isArray()) {
        return false;
    }
    for (int i = 0; i < intents.getArrayNItems(); ++i) {
        QPDFObjectHandle intent = intents.getArrayItem(i);
        if (nameOf(intent, "/S") == "/GTS_PDFA1" && intent.isDictionary() && intent.hasKey("/DestOutputProfile")) {
            return true;
        }
    }
    return false;
}

/** One `pdfaid` value out of an XMP packet, written either way round. */
QString xmpValue(const QString &packet, const QString &field, const QString &allowed)
{
    // XMP puts simple properties either in an attribute or in an element, and
    // both spellings are common enough that reading only one of them means
    // failing to recognise perfectly good files.
    const QRegularExpression attribute(QStringLiteral("pdfaid:%1\\s*=\\s*[\"']\\s*(%2)\\s*[\"']").arg(field, allowed));
    QRegularExpressionMatch match = attribute.match(packet);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const QRegularExpression element(QStringLiteral("<pdfaid:%1[^>]*>\\s*(%2)").arg(field, allowed));
    match = element.match(packet);
    return match.hasMatch() ? match.captured(1) : QString();
}

void readXmpIdentification(QPDFObjectHandle root, Scan *scan)
{
    QPDFObjectHandle metadata = root.getKey("/Metadata");
    if (!metadata.isStream()) {
        return;
    }

    std::shared_ptr<Buffer> buffer;
    try {
        buffer = metadata.getStreamData();
    } catch (const std::exception &) {
        // An XMP packet this cannot be decoded is not worth failing over; the
        // rest of the inspection still has something to say.
        return;
    }

    const QString packet
        = QString::fromUtf8(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
    scan->claimedPart = xmpValue(packet, QStringLiteral("part"), QStringLiteral("[0-9]+"));
    scan->claimedConformance = xmpValue(packet, QStringLiteral("conformance"), QStringLiteral("[A-Za-z]"));
}

bool scanDocument(const QString &path, Scan *scan, QString *error)
{
    *scan = Scan {};

    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle root = pdf.getRoot();

        readXmpIdentification(root, scan);

        const bool encrypted = pdf.isEncrypted();
        const bool scripts = documentHasScripts(root);

        int attachments = embeddedFileCount(root);
        QStringList unembedded;
        std::set<QPDFObjGen> visited;
        for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            if (pageHasAttachment(page)) {
                ++attachments;
            }
            collectUnembeddedFonts(page.getAttribute("/Resources", false), unembedded, visited);
        }
        scan->unembeddedFonts = unembedded;

        // PDF/A-3 exists precisely so that a document may carry its source
        // spreadsheet along, so an attachment is only a fault below that level.
        const bool attachmentsAllowed = scan->claimedPart == QLatin1String("3");

        // Appended in a fixed order rather than as they are discovered, because
        // convert() compares two of these lists.
        if (encrypted) {
            scan->faults.append(Fault::Encrypted);
        }
        if (scripts) {
            scan->faults.append(Fault::Scripts);
        }
        if (attachments > 0 && !attachmentsAllowed) {
            scan->faults.append(Fault::EmbeddedFiles);
        }
        if (!unembedded.isEmpty()) {
            scan->faults.append(Fault::UnembeddedFonts);
        }
        if (!hasPdfAOutputIntent(root)) {
            scan->faults.append(Fault::NoOutputIntent);
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

QString faultAsProblem(Fault fault, const Scan &scan)
{
    switch (fault) {
    case Fault::Encrypted:
        return i18n("The document is encrypted. An archive has to be able to open it in fifty years' time, "
                    "without a password, so PDF/A does not allow that.");
    case Fault::Scripts:
        return i18n("The document contains scripts, or an action that runs when it is opened. "
                    "PDF/A allows neither.");
    case Fault::EmbeddedFiles:
        return i18n("The document carries other files inside it. Only PDF/A-3 allows that.");
    case Fault::UnembeddedFonts:
        return i18np("%1 font is not embedded (%2), so its text will be set in whatever the reader happens to "
                     "have instead.",
                     "%1 fonts are not embedded (%2), so their text will be set in whatever the reader happens "
                     "to have instead.",
                     static_cast<int>(scan.unembeddedFonts.size()),
                     scan.unembeddedFonts.join(i18nc("@item separator in a list of font names", ", ")));
    case Fault::NoOutputIntent:
        return i18n("The document has no output intent, so nothing in it says what its colours are meant to "
                    "look like.");
    }
    return {};
}

QString faultAsChange(Fault fault)
{
    switch (fault) {
    case Fault::Encrypted:
        return i18n("The password protection was removed, because PDF/A does not allow it.");
    case Fault::Scripts:
        return i18n("Scripts and actions that run on their own were removed.");
    case Fault::EmbeddedFiles:
        return i18n("The files carried inside the document were dropped. Convert to PDF/A-3 if they matter.");
    case Fault::UnembeddedFonts:
        return i18n("The fonts were embedded, so the text keeps its shape on a machine that does not have them.");
    case Fault::NoOutputIntent:
        return i18n("An sRGB output intent was added, so the colours are stated rather than assumed.");
    }
    return {};
}

int partOf(Archival::Level level)
{
    switch (level) {
    case Archival::Level::PdfA1b:
        return 1;
    case Archival::Level::PdfA2b:
        return 2;
    case Archival::Level::PdfA3b:
        return 3;
    }
    return 2;
}

/** Directories that hold ICC profiles on a Linux desktop. */
QStringList iccSearchDirectories()
{
    QStringList directories {
        QStringLiteral("/usr/share/color/icc"),
        QStringLiteral("/usr/share/color/icc/colord"),
        QStringLiteral("/usr/local/share/color/icc"),
        QStringLiteral("/usr/share/colord/icc"),
    };

    // Ghostscript ships an sRGB profile of its own, but under its version
    // number, so the directory has to be found rather than named.
    const QDir ghostscript(QStringLiteral("/usr/share/ghostscript"));
    const QStringList versions = ghostscript.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &version : versions) {
        directories.append(ghostscript.filePath(version) + QLatin1String("/iccprofiles"));
    }
    return directories;
}

/**
 * An sRGB profile from the system, or empty.
 *
 * sRGB specifically: it is what an ordinary document's colours already mean, so
 * naming it changes nothing about how the file looks while giving the standard
 * the definition it insists on.
 */
QString findSrgbProfile()
{
    // Lower case throughout, because these are compared case-insensitively:
    // distributions disagree about whether it is sRGB.icc or srgb.icc.
    static const QStringList wanted {
        QStringLiteral("srgb.icc"),
        QStringLiteral("srgb.icm"),
        QStringLiteral("srgb_v4_icc_preference.icc"),
        QStringLiteral("srgb-v2-micro.icc"),
        QStringLiteral("default_rgb.icc"),
    };

    for (const QString &path : iccSearchDirectories()) {
        const QDir directory(path);
        if (!directory.exists()) {
            continue;
        }
        const QFileInfoList files = directory.entryInfoList(QDir::Files, QDir::Name);
        for (const QString &name : wanted) {
            for (const QFileInfo &file : files) {
                if (file.fileName().toLower() == name) {
                    return file.absoluteFilePath();
                }
            }
        }
    }
    return {};
}

/** A PostScript byte string, `(…)`, safe for any path. */
QString postScriptLiteral(const QString &text)
{
    QString escaped;
    const QByteArray utf8 = text.toUtf8();
    for (const char byte : utf8) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value == '(' || value == ')' || value == '\\') {
            escaped += QLatin1Char('\\');
            escaped += QLatin1Char(byte);
        } else if (value < 32 || value > 126) {
            // Octal, so nothing in the path can be mistaken for syntax and
            // nothing depends on how the interpreter reads high bytes.
            escaped += QLatin1Char('\\');
            escaped += QStringLiteral("%1").arg(static_cast<uint>(value), 3, 8, QLatin1Char('0'));
        } else {
            escaped += QLatin1Char(byte);
        }
    }
    return QLatin1Char('(') + escaped + QLatin1Char(')');
}

/**
 * A document title or author for a `pdfmark`, as UTF-16.
 *
 * Written as a hexadecimal string with a byte-order mark rather than plain
 * text, which is the only form that survives a name with an umlaut in it.
 */
QString pdfmarkText(const QString &text)
{
    QString hex = QStringLiteral("<feff");
    for (const QChar character : text) {
        hex += QStringLiteral("%1").arg(static_cast<uint>(character.unicode()), 4, 16, QLatin1Char('0'));
    }
    return hex + QLatin1Char('>');
}

/**
 * Writes the prologue Ghostscript needs to make a PDF/A file.
 *
 * Everything the standard wants that is not a command-line switch lives here:
 * the document information, the ICC profile as a stream, and the output intent
 * in the catalogue that points at it.
 */
bool writeDefinitionFile(const QString &path, const QString &iccProfile, const Archival::Options &options,
                         QString *error)
{
    QString program = QStringLiteral("%!\n");

    if (!options.title.isEmpty() || !options.author.isEmpty()) {
        program += QLatin1String("[");
        if (!options.title.isEmpty()) {
            program += QLatin1String(" /Title ") + pdfmarkText(options.title);
        }
        if (!options.author.isEmpty()) {
            program += QLatin1String(" /Author ") + pdfmarkText(options.author);
        }
        program += QLatin1String(" /DOCINFO pdfmark\n");
    }

    if (!iccProfile.isEmpty()) {
        program += QLatin1String("/ICCProfile ") + postScriptLiteral(iccProfile) + QLatin1String(" def\n");
        program += QLatin1String("[/_objdef {icc_PDFA} /type /stream /OBJ pdfmark\n");
        // The number of components is hard-coded because the profile is always
        // an RGB one; the sample prologue Ghostscript ships guesses at it and
        // guesses wrong under a device-independent colour strategy.
        program += QLatin1String("[{icc_PDFA} << /N 3 >> /PUT pdfmark\n");
        program += QLatin1String("[{icc_PDFA} ICCProfile (r) file /PUT pdfmark\n");
        program += QLatin1String("[/_objdef {OutputIntent_PDFA} /type /dict /OBJ pdfmark\n");
        program += QLatin1String("[{OutputIntent_PDFA} <<\n");
        program += QLatin1String("  /Type /OutputIntent\n");
        program += QLatin1String("  /S /GTS_PDFA1\n");
        program += QLatin1String("  /DestOutputProfile {icc_PDFA}\n");
        program += QLatin1String("  /OutputConditionIdentifier (sRGB)\n");
        program += QLatin1String(">> /PUT pdfmark\n");
        program += QLatin1String("[{Catalog} <</OutputIntents [ {OutputIntent_PDFA} ]>> /PUT pdfmark\n");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = i18n("Could not write “%1”.", path);
        }
        return false;
    }
    // Latin-1 is enough and is what the interpreter expects: everything above
    // 126 has already been escaped into ASCII by the helpers above.
    file.write(program.toLatin1());
    file.close();
    return true;
}

} // namespace

bool Archival::isAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("gs")).isEmpty();
}

QString Archival::describe(Level level)
{
    switch (level) {
    case Level::PdfA1b:
        return i18nc("@item PDF/A level", "PDF/A-1b (oldest, strictest, accepted everywhere)");
    case Level::PdfA2b:
        return i18nc("@item PDF/A level", "PDF/A-2b (what most archives ask for)");
    case Level::PdfA3b:
        return i18nc("@item PDF/A level", "PDF/A-3b (as PDF/A-2b, but attachments may stay)");
    }
    return {};
}

QStringList Archival::inspectionLimitations()
{
    return {
        i18n("This is not a full validation. It looks for the faults that account for most rejections "
             "(encryption, scripts, attachments, fonts that are not embedded, a missing output intent) "
             "and nothing else."),
        i18n("A file can pass every check here and still be refused by a strict validator such as veraPDF, "
             "which is the tool to reach for when an archive has to sign the file off."),
        i18n("Only what the file says about itself is read. A document claiming PDF/A-2b is taken at its word "
             "on that point."),
    };
}

Archival::Findings Archival::inspect(const QString &pdf, QString *error)
{
    Findings findings;

    Scan scan;
    if (!scanDocument(pdf, &scan, error)) {
        return findings;
    }

    findings.claimedLevel = scan.claimedLevel();
    for (const Fault fault : scan.faults) {
        findings.problems.append(faultAsProblem(fault, scan));
    }
    // Two conditions, and both are needed: a file with no faults that never
    // claims to be archival is simply a tidy PDF, and a file that claims the
    // standard while breaking it is worse than one that claims nothing.
    findings.looksArchival = !findings.claimedLevel.isEmpty() && findings.problems.isEmpty();
    return findings;
}

bool Archival::convert(const QString &inputPdf, const QString &outputPdf, const Options &options, Report *report,
                       QString *error)
{
    const QFileInfo inputInfo(inputPdf);
    if (!inputInfo.exists()) {
        if (error) {
            *error = i18n("“%1” does not exist.", inputPdf);
        }
        return false;
    }

    const QString ghostscript = QStandardPaths::findExecutable(QStringLiteral("gs"));
    if (ghostscript.isEmpty()) {
        if (error) {
            *error = i18n("Ghostscript is not installed, so documents cannot be converted for archiving. "
                          "Install the “ghostscript” package.");
        }
        return false;
    }

    Report local;

    // What is wrong with the file now, so that afterwards the report can say
    // what the conversion actually dealt with rather than what it usually does.
    Scan before;
    const bool scannedBefore = scanDocument(inputPdf, &before, nullptr);

    QString iccProfile = options.iccProfilePath;
    if (!iccProfile.isEmpty() && !QFileInfo::exists(iccProfile)) {
        if (error) {
            *error = i18n("The colour profile “%1” does not exist.", iccProfile);
        }
        return false;
    }
    if (iccProfile.isEmpty()) {
        iccProfile = findSrgbProfile();
    }
    if (iccProfile.isEmpty()) {
        local.warnings.append(i18n("No sRGB colour profile was found on this system, so Ghostscript's built-in "
                                   "default was used. The file will have no output intent, which a strict check "
                                   "may object to. Installing the “icc-profiles-free” package fixes it."));
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        if (error) {
            *error = i18n("Could not create a temporary directory.");
        }
        return false;
    }
    const QString definition = workspace.filePath(QStringLiteral("pdf-smithy-pdfa.ps"));
    if (!writeDefinitionFile(definition, iccProfile, options, error)) {
        return false;
    }

    // Work into a temporary beside the destination so a failure never leaves a
    // half-written file where the user expects their document.
    QTemporaryFile temp(QFileInfo(outputPdf).absolutePath() + QLatin1String("/.pdf-smithy-pdfa-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(outputPdf).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    const auto cleanUp = [&tempPath] { QFile::remove(tempPath); };

    QStringList arguments;
    if (!iccProfile.isEmpty()) {
        // Ghostscript's sandbox is on by default and the prologue opens the
        // profile itself, so that one file has to be named as readable. It must
        // come first, before the switches it governs.
        arguments << QStringLiteral("--permit-file-read=%1").arg(iccProfile);
    }
    arguments << QStringLiteral("-dPDFA=%1").arg(partOf(options.level)) << QStringLiteral("-dBATCH")
              << QStringLiteral("-dNOPAUSE")
              << QStringLiteral("-dNOOUTERSAVE")
              // 1 means "leave out anything that cannot be made conformant and
              // carry on", which is what somebody asking for an archival copy
              // wants; the alternative is no file at all.
              << QStringLiteral("-dPDFACompatibilityPolicy=1")
              << QStringLiteral("-sColorConversionStrategy=UseDeviceIndependentColor")
              << QStringLiteral("-sDEVICE=pdfwrite") << QStringLiteral("-sOutputFile=%1").arg(tempPath) << definition
              << inputPdf;

    QProcess process;
    process.start(ghostscript, arguments);
    if (!process.waitForStarted(10000)) {
        cleanUp();
        if (error) {
            *error = i18n("Ghostscript could not be started.");
        }
        return false;
    }
    if (!process.waitForFinished(GhostscriptTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        cleanUp();
        if (error) {
            *error = i18n("Converting took too long and was stopped.");
        }
        return false;
    }

    const QString diagnostics = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        cleanUp();
        if (error) {
            *error = diagnostics.isEmpty() ? i18n("Ghostscript reported an error.") : diagnostics;
        }
        return false;
    }
    if (QFileInfo(tempPath).size() <= 0) {
        cleanUp();
        if (error) {
            *error = i18n("Converting produced an empty file.");
        }
        return false;
    }

    // Ghostscript says this when it cannot keep a colour device-independent and
    // falls back. The file is still produced and still usable, so it is a
    // warning rather than a failure, but a validator may well disagree.
    if (diagnostics.contains(QStringLiteral("cannot guarantee creating a conformant"))) {
        local.warnings.append(i18n("Some colours could not be converted to a device-independent space, so a "
                                   "strict check may object to them."));
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(outputPdf).constData()) != 0) {
        cleanUp();
        if (error) {
            *error = i18n("Could not replace “%1”.", outputPdf);
        }
        return false;
    }

    Scan after;
    if (scanDocument(outputPdf, &after, nullptr)) {
        if (!after.claimedLevel().isEmpty() && after.claimedLevel() != before.claimedLevel()) {
            local.changes.append(i18n("The file now identifies itself as %1.", after.claimedLevel()));
        }
        if (scannedBefore) {
            for (const Fault fault : before.faults) {
                if (!after.contains(fault)) {
                    local.changes.append(faultAsChange(fault));
                }
            }
        }
        for (const Fault fault : after.faults) {
            // Saying the output intent is missing directly after explaining why
            // no profile was found would only repeat the same fact.
            if (fault == Fault::NoOutputIntent && iccProfile.isEmpty()) {
                continue;
            }
            local.warnings.append(faultAsProblem(fault, after));
        }
    } else {
        local.warnings.append(i18n("The converted file could not be read back, so what it now contains is "
                                   "unverified."));
    }

    if (!scannedBefore) {
        local.warnings.append(i18n("The original could not be examined, so the list of changes is incomplete."));
    }

    local.converted = true;
    if (report) {
        *report = local;
    }
    return true;
}

} // namespace ps
