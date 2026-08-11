/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Preflight.h"

#include "Archival.h"
#include "Compressor.h"
#include "Core14Widths.h"
#include "PdfFile.h"
#include "PdfGeometry.h"

#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QRectF>
#include <QRegularExpression>
#include <QSizeF>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTransform>

#include <KLocalizedString>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace ps {

namespace {

using PdfGeometry::boxValue;
using PdfGeometry::number;
using PdfGeometry::numericValue;

/**
 * Rule identifiers, spelled once.
 *
 * Untranslated and never shown as-is: a rule id travels in saved profiles and
 * in scripts, so it has to mean the same thing on every machine and in every
 * release. The sentence a person reads comes from describeRule().
 */
namespace Rule {
constexpr QLatin1String FontNotEmbedded("font.not-embedded");
constexpr QLatin1String FontSubsetWithoutToUnicode("font.subset-without-tounicode");
constexpr QLatin1String FontType3("font.type3");
constexpr QLatin1String FontLicenceRestricted("font.licence-restricted");
constexpr QLatin1String ImageLowResolution("image.low-resolution");
constexpr QLatin1String ImageExcessiveResolution("image.excessive-resolution");
constexpr QLatin1String ImageJpeg2000("image.jpeg2000");
constexpr QLatin1String ImageNoColourSpace("image.no-colourspace");
constexpr QLatin1String ColourRgbInPrint("colour.rgb-in-print");
constexpr QLatin1String ColourCmykInWeb("colour.cmyk-in-web");
constexpr QLatin1String ColourSpot("colour.spot-colours");
constexpr QLatin1String ColourNoIcc("colour.no-icc");
constexpr QLatin1String ColourNoOutputIntent("colour.no-output-intent");
constexpr QLatin1String TransparencyPresent("transparency.present");
constexpr QLatin1String TransparencyBlendModes("transparency.blend-modes");
constexpr QLatin1String PageMissingTrimBox("page.missing-trimbox");
constexpr QLatin1String PageMediaDiffersFromCrop("page.mediabox-differs-from-cropbox");
constexpr QLatin1String PageMixedSizes("page.mixed-sizes");
constexpr QLatin1String PageMixedRotation("page.mixed-rotation");
constexpr QLatin1String PageEmpty("page.empty");
constexpr QLatin1String InteractiveJavaScript("interactive.javascript");
constexpr QLatin1String InteractiveEmbeddedFiles("interactive.embedded-files");
constexpr QLatin1String InteractiveOpenAction("interactive.open-action");
constexpr QLatin1String InteractiveAnnotationsOutside("interactive.annotations-outside-page");
constexpr QLatin1String SecurityEncrypted("security.encrypted");
constexpr QLatin1String SecurityPermissions("security.permissions-restricted");
constexpr QLatin1String MetadataNoTitle("metadata.no-title");
constexpr QLatin1String MetadataXmpDisagrees("metadata.xmp-disagrees-with-info");
constexpr QLatin1String StructureNoTags("structure.no-tags");
constexpr QLatin1String StructureNoLanguage("structure.no-language");
constexpr QLatin1String StructureImagesWithoutAlt("structure.images-without-alt");
constexpr QLatin1String StrokeHairline("stroke.hairline");
constexpr QLatin1String TextTiny("text.tiny");
constexpr QLatin1String ContentOutsideTrimBox("content.outside-trimbox");
} // namespace Rule

namespace Fix {
constexpr QLatin1String EmbedFonts("fix.embed-fonts");
constexpr QLatin1String RemoveJavaScript("fix.remove-javascript");
constexpr QLatin1String RemoveEmbeddedFiles("fix.remove-embedded-files");
constexpr QLatin1String RemoveOpenAction("fix.remove-open-action");
constexpr QLatin1String SetTrimBox("fix.set-trimbox");
constexpr QLatin1String SetTitle("fix.set-title");
constexpr QLatin1String Linearise("fix.linearise");
constexpr QLatin1String Decrypt("fix.decrypt");
constexpr QLatin1String DownsampleImages("fix.downsample-images");
} // namespace Fix

/** The one rule in the shipped profiles that run() cannot judge. See limitations(). */
QStringList unimplementedRules()
{
    return { Rule::ContentOutsideTrimBox };
}

/**
 * How many places one rule may name before the report starts summarising.
 *
 * A two-hundred-page magazine with no trim box would otherwise produce two
 * hundred identical findings, and a list nobody reads to the end is a list that
 * hides the one interesting entry near the bottom.
 */
constexpr int MaxFindingsPerRule = 20;

/** Sizes and positions that agree to within this many points are the same. */
constexpr double GeometryTolerance = 1.0;

/** How deep to follow form XObjects that draw other form XObjects. */
constexpr int MaxContentDepth = 8;

// ---------------------------------------------------------------------------
// Facts gathered from the document
// ---------------------------------------------------------------------------

struct FontFact {
    QString label;
    QString resourceName;
    int page = -1;
    bool embedded = true;
    bool type3 = false;
    bool subsetWithoutToUnicode = false;
    bool licenceRestricted = false;
};

struct ImageFact {
    QString name;
    int page = -1;
    int pixelWidth = 0;
    int pixelHeight = 0;
    bool jpeg2000 = false;
    bool noColourSpace = false;
    /** Both zero until the picture is actually drawn somewhere. */
    double lowestDpi = 0.0;
    double highestDpi = 0.0;
};

struct PageFact {
    QSizeF mediaSize;
    int rotate = 0;
    bool hasTrimBox = false;
    bool cropDiffersFromMedia = false;
    bool marked = false;
    bool contentReadable = true;
    QStringList annotationsOutside;
    /** Negative when nothing on the page was stroked or shown. */
    double thinnestStroke = -1.0;
    double smallestText = -1.0;
};

struct Scan {
    bool encrypted = false;
    bool passwordNeeded = false;
    QStringList deniedPermissions;

    QVector<PageFact> pages;
    QVector<FontFact> fonts;
    QVector<ImageFact> images;

    QStringList spotColours;
    QStringList blendModes;
    bool usesRgb = false;
    bool usesCmyk = false;
    bool usesIcc = false;
    bool hasOutputIntent = false;
    bool transparency = false;

    bool javaScript = false;
    int embeddedFiles = 0;
    bool openAction = false;

    QString infoTitle;
    QString xmpTitle;

    bool tagged = false;
    QString language;
    QVector<QPair<int, QString>> figuresWithoutAlt;

    int drawnImages = 0;
    bool anyContentUnreadable = false;
};

// ---------------------------------------------------------------------------
// Small readers
// ---------------------------------------------------------------------------

std::string nameOf(QPDFObjectHandle dict, const char *key)
{
    if (!dict.isDictionary()) {
        return {};
    }
    QPDFObjectHandle value = dict.getKey(key);
    return value.isName() ? value.getName() : std::string();
}

QString stringValue(QPDFObjectHandle dict, const char *key)
{
    if (!dict.isDictionary() || !dict.hasKey(key)) {
        return {};
    }
    QPDFObjectHandle value = dict.getKey(key);
    return value.isString() ? QString::fromStdString(value.getUTF8Value()) : QString();
}

/** A resource name such as `/Im3` without the slash, for a report to quote. */
QString plainName(const std::string &name)
{
    QString text = QString::fromStdString(name);
    if (text.startsWith(QLatin1Char('/'))) {
        text.remove(0, 1);
    }
    return text;
}

bool hasFontFile(QPDFObjectHandle descriptor)
{
    if (!descriptor.isDictionary()) {
        return false;
    }
    // Type 1, TrueType and the compact/OpenType forms respectively; any one of
    // them means the glyphs travel with the document. Deliberately the same
    // test Archival makes, so the two never disagree about the same file.
    return descriptor.hasKey("/FontFile") || descriptor.hasKey("/FontFile2") || descriptor.hasKey("/FontFile3");
}

/** The font program itself, when the document carries one. */
QPDFObjectHandle fontProgram(QPDFObjectHandle descriptor)
{
    if (!descriptor.isDictionary()) {
        return QPDFObjectHandle::newNull();
    }
    for (const char *key : { "/FontFile2", "/FontFile3", "/FontFile" }) {
        QPDFObjectHandle stream = descriptor.getKey(key);
        if (stream.isStream()) {
            return stream;
        }
    }
    return QPDFObjectHandle::newNull();
}

/**
 * The `fsType` field of an sfnt font's OS/2 table, or -1 when there is none.
 *
 * This is the only machine-readable statement a font makes about whether it may
 * be carried inside a document, so it is the only thing a licence check can
 * honestly be built on. Type 1 and bare CFF fonts have no equivalent field at
 * all, which is why limitations() says so out loud rather than letting a clean
 * report imply the question was answered.
 */
int sfntEmbeddingPermission(const QByteArray &font)
{
    const auto uint16At = [&font](qsizetype at) -> int {
        return (static_cast<unsigned char>(font.at(at)) << 8) | static_cast<unsigned char>(font.at(at + 1));
    };
    const auto uint32At = [&font](qsizetype at) -> quint32 {
        return (static_cast<quint32>(static_cast<unsigned char>(font.at(at))) << 24)
            | (static_cast<quint32>(static_cast<unsigned char>(font.at(at + 1))) << 16)
            | (static_cast<quint32>(static_cast<unsigned char>(font.at(at + 2))) << 8)
            | static_cast<quint32>(static_cast<unsigned char>(font.at(at + 3)));
    };

    if (font.size() < 12) {
        return -1;
    }
    qsizetype base = 0;
    if (font.startsWith(QByteArrayLiteral("ttcf"))) {
        // A collection holds several fonts; the first one's licence is the one
        // that stands for the file, since a PDF only ever embeds one of them.
        if (font.size() < 16) {
            return -1;
        }
        base = qsizetype(uint32At(12));
        if (base < 0 || base + 12 > font.size()) {
            return -1;
        }
    }

    const int tableCount = uint16At(base + 4);
    for (int i = 0; i < tableCount; ++i) {
        const qsizetype entry = base + 12 + qsizetype(i) * 16;
        if (entry + 16 > font.size()) {
            return -1;
        }
        if (font.mid(entry, 4) != QByteArrayLiteral("OS/2")) {
            continue;
        }
        const qsizetype offset = qsizetype(uint32At(entry + 8));
        if (offset < 0 || offset + 10 > font.size()) {
            return -1;
        }
        return uint16At(offset + 8);
    }
    return -1;
}

/** Bit 1 of fsType forbids embedding outright; bit 9 permits only bitmaps. */
bool permissionIsRestricted(int fsType)
{
    return fsType >= 0 && ((fsType & 0x0002) != 0 || (fsType & 0x0200) != 0);
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

bool looksSubsetted(QPDFObjectHandle font)
{
    const QString name = QString::fromStdString(nameOf(font, "/BaseFont"));
    static const QRegularExpression subsetTag(QStringLiteral("^/[A-Z]{6}\\+"));
    return subsetTag.match(name).hasMatch();
}

/** The font dictionary whose descriptor matters, unwrapping a composite font. */
QPDFObjectHandle glyphSource(QPDFObjectHandle font)
{
    if (nameOf(font, "/Subtype") == "/Type0") {
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        if (descendants.isArray() && descendants.getArrayNItems() > 0) {
            return descendants.getArrayItem(0);
        }
    }
    return font;
}

FontFact readFont(QPDFObjectHandle font, const std::string &resourceName, int pageIndex)
{
    FontFact fact;
    fact.label = fontLabel(font);
    fact.resourceName = plainName(resourceName);
    fact.page = pageIndex;

    const std::string subtype = nameOf(font, "/Subtype");
    fact.type3 = subtype == "/Type3";

    QPDFObjectHandle source = glyphSource(font);
    QPDFObjectHandle descriptor
        = source.isDictionary() ? source.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();

    // A Type 3 font's glyphs are content streams inside the document already,
    // so there is nothing to embed and nothing to lose.
    fact.embedded = fact.type3 || hasFontFile(descriptor);

    // Without a /ToUnicode table a subset font's codes mean nothing outside the
    // document: the text is on the page but cannot be copied, searched or read
    // aloud. A full font with a standard encoding does not have that problem.
    fact.subsetWithoutToUnicode = looksSubsetted(font) && !font.hasKey("/ToUnicode");

    QPDFObjectHandle program = fontProgram(descriptor);
    if (program.isStream()) {
        try {
            std::shared_ptr<Buffer> buffer = program.getStreamData();
            const QByteArray bytes(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
            fact.licenceRestricted = permissionIsRestricted(sfntEmbeddingPermission(bytes));
        } catch (const std::exception &) {
            // A font program that will not decompress tells us nothing about its
            // licence, and is not worth failing the whole check over.
        }
    }
    return fact;
}

enum class SpaceKind {
    Unknown,
    Gray,
    Rgb,
    Cmyk,
    Lab,
    Pattern,
};

struct SpaceInfo {
    SpaceKind kind = SpaceKind::Unknown;
    bool icc = false;
    QStringList spotNames;
};

SpaceInfo classifySpace(QPDFObjectHandle space, QPDFObjectHandle resources, int depth = 0);

SpaceInfo classifyNamedSpace(const std::string &name, QPDFObjectHandle resources, int depth)
{
    SpaceInfo info;
    if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
        info.kind = SpaceKind::Gray;
    } else if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") {
        info.kind = SpaceKind::Rgb;
    } else if (name == "/DeviceCMYK" || name == "/CMYK") {
        info.kind = SpaceKind::Cmyk;
    } else if (name == "/Pattern") {
        info.kind = SpaceKind::Pattern;
    } else if (name == "/Lab") {
        info.kind = SpaceKind::Lab;
    } else if (resources.isDictionary() && depth < 4) {
        // Anything else is a name the page's resources have to define; the
        // abbreviations above are the only ones a reader knows by heart.
        QPDFObjectHandle table = resources.getKey("/ColorSpace");
        if (table.isDictionary() && table.hasKey(name)) {
            info = classifySpace(table.getKey(name), resources, depth + 1);
        }
    }
    return info;
}

SpaceInfo classifySpace(QPDFObjectHandle space, QPDFObjectHandle resources, int depth)
{
    SpaceInfo info;
    if (depth > 4) {
        return info;
    }
    if (space.isName()) {
        return classifyNamedSpace(space.getName(), resources, depth);
    }
    if (!space.isArray() || space.getArrayNItems() < 1) {
        return info;
    }

    QPDFObjectHandle head = space.getArrayItem(0);
    const std::string family = head.isName() ? head.getName() : std::string();

    if (family == "/ICCBased") {
        info.icc = true;
        if (space.getArrayNItems() > 1) {
            QPDFObjectHandle stream = space.getArrayItem(1);
            const int components = stream.isStream() ? int(numericValue(stream.getDict().getKey("/N"), 0.0)) : 0;
            if (components == 1) {
                info.kind = SpaceKind::Gray;
            } else if (components == 3) {
                info.kind = SpaceKind::Rgb;
            } else if (components == 4) {
                info.kind = SpaceKind::Cmyk;
            }
        }
        return info;
    }
    if (family == "/Indexed" || family == "/I") {
        // The palette's entries live in the base space, so that is what the
        // document is really printing with.
        if (space.getArrayNItems() > 1) {
            info = classifySpace(space.getArrayItem(1), resources, depth + 1);
        }
        return info;
    }
    if (family == "/Separation") {
        if (space.getArrayNItems() > 1 && space.getArrayItem(1).isName()) {
            info.spotNames.append(plainName(space.getArrayItem(1).getName()));
        }
        return info;
    }
    if (family == "/DeviceN") {
        if (space.getArrayNItems() > 1 && space.getArrayItem(1).isArray()) {
            QPDFObjectHandle names = space.getArrayItem(1);
            for (int i = 0; i < names.getArrayNItems(); ++i) {
                if (names.getArrayItem(i).isName()) {
                    info.spotNames.append(plainName(names.getArrayItem(i).getName()));
                }
            }
        }
        return info;
    }
    if (family == "/Pattern") {
        info.kind = SpaceKind::Pattern;
        return info;
    }
    return classifyNamedSpace(family, resources, depth);
}

bool streamUsesFilter(QPDFObjectHandle stream, const char *filter)
{
    if (!stream.isStream()) {
        return false;
    }
    QPDFObjectHandle filters = stream.getDict().getKey("/Filter");
    if (filters.isName()) {
        return filters.getName() == filter;
    }
    if (filters.isArray()) {
        for (int i = 0; i < filters.getArrayNItems(); ++i) {
            QPDFObjectHandle item = filters.getArrayItem(i);
            if (item.isName() && item.getName() == filter) {
                return true;
            }
        }
    }
    return false;
}

double transformScale(const QTransform &matrix)
{
    return std::sqrt(std::abs(matrix.determinant()));
}

// ---------------------------------------------------------------------------
// Walking a page's content
// ---------------------------------------------------------------------------

/** As much of the graphics state as any rule here depends on. */
struct GraphicsState {
    QTransform ctm;
    double lineWidth = 1.0;
    double fontSize = 0.0;
    int textRenderMode = 0;
};

/**
 * A ceiling on how much of one page's content is read.
 *
 * Form XObjects are re-read at every placement rather than once per document,
 * because a hairline inside a form is a hairline only at the scale that form is
 * drawn at, and a form that draws nothing has to leave the page it was drawn on
 * looking empty. That is the right answer and an unbounded one, so a page is
 * given a budget: a document that fans out beyond it is reported as unmeasured
 * rather than allowed to run for an hour.
 */
struct WalkBudget {
    int formsLeft = 4000;
};

/**
 * Reads a content stream for the facts that depend on where things land.
 *
 * A picture's resolution, a rule's width and a caption's size are all
 * properties of the placement rather than of the object: the same 300 dpi scan
 * is 600 dpi at half size and 75 dpi at double, and the same `0.1 w` is a
 * hairline on a page and a fat line inside a form scaled up ten times. Nothing
 * short of tracking the matrices can tell them apart, which is why this walks
 * the stream instead of reading the resource dictionary and guessing.
 */
class ContentScanner : public QPDFObjectHandle::ParserCallbacks
{
public:
    ContentScanner(Scan &scan, PageFact &page, int pageIndex, QPDFObjectHandle resources, const QTransform &initialCtm,
                   WalkBudget &budget, std::map<QPDFObjGen, int> &imageIndex, QVector<QPDFObjGen> ancestors)
        : m_scan(scan)
        , m_page(page)
        , m_pageIndex(pageIndex)
        , m_resources(std::move(resources))
        , m_budget(budget)
        , m_imageIndex(imageIndex)
        , m_ancestors(std::move(ancestors))
    {
        m_state.ctm = initialCtm;
    }

    void handleObject(QPDFObjectHandle object) override;
    void handleEOF() override { }

private:
    void handleOperator(const QString &op);
    void handleDo();
    void handleExtGState();
    void handleColourSpace();
    void noteImage(QPDFObjectHandle image, const std::string &resourceName);
    void noteForm(QPDFObjectHandle form);
    void noteFont();
    void noteSpace(const SpaceInfo &info);
    void showText();
    void nextLine(double leading);

    QPDFObjectHandle operandFromEnd(int back) const
    {
        const qsizetype index = m_operands.size() - 1 - back;
        return index >= 0 ? m_operands.at(index) : QPDFObjectHandle::newNull();
    }

    double numberFromEnd(int back) const { return numericValue(operandFromEnd(back), 0.0); }

    std::string nameFromEnd(int back) const
    {
        QPDFObjectHandle item = operandFromEnd(back);
        return item.isName() ? item.getName() : std::string();
    }

    QPDFObjectHandle lookUp(const char *category, const std::string &name) const
    {
        if (!m_resources.isDictionary()) {
            return QPDFObjectHandle::newNull();
        }
        QPDFObjectHandle table = m_resources.getKey(category);
        return table.isDictionary() && table.hasKey(name) ? table.getKey(name) : QPDFObjectHandle::newNull();
    }

    Scan &m_scan;
    PageFact &m_page;
    int m_pageIndex;
    QPDFObjectHandle m_resources;
    WalkBudget &m_budget;
    std::map<QPDFObjGen, int> &m_imageIndex;
    /** The forms this one is drawn inside, so a form that draws itself stops. */
    QVector<QPDFObjGen> m_ancestors;

    QVector<QPDFObjectHandle> m_operands;
    GraphicsState m_state;
    QVector<GraphicsState> m_stack;
    QTransform m_textMatrix;
    QTransform m_lineMatrix;
    double m_leading = 0.0;
};

void ContentScanner::handleObject(QPDFObjectHandle object)
{
    if (object.isOperator()) {
        handleOperator(QString::fromStdString(object.getOperatorValue()));
        m_operands.clear();
        return;
    }
    if (object.isInlineImage()) {
        // An inline image's dictionary is not handed over in a form this can
        // read, so it counts as ink on the page and nothing more.
        m_page.marked = true;
        m_operands.clear();
        return;
    }
    m_operands.append(object);
}

void ContentScanner::nextLine(double leading)
{
    m_lineMatrix = QTransform(1, 0, 0, 1, 0, -leading) * m_lineMatrix;
    m_textMatrix = m_lineMatrix;
}

void ContentScanner::noteSpace(const SpaceInfo &info)
{
    switch (info.kind) {
    case SpaceKind::Rgb:
        m_scan.usesRgb = true;
        break;
    case SpaceKind::Cmyk:
        m_scan.usesCmyk = true;
        break;
    default:
        break;
    }
    if (info.icc) {
        m_scan.usesIcc = true;
    }
    for (const QString &spot : info.spotNames) {
        // /All and /None are the specification's own placeholders rather than
        // inks a printer would have to mix.
        if (spot == QLatin1String("All") || spot == QLatin1String("None")) {
            continue;
        }
        if (!m_scan.spotColours.contains(spot)) {
            m_scan.spotColours.append(spot);
        }
    }
}

void ContentScanner::handleColourSpace()
{
    // Which of the two colours is being set makes no difference to any rule
    // here: a spot ink is a spot ink whether it fills or strokes.
    noteSpace(classifySpace(operandFromEnd(0), m_resources));
}

void ContentScanner::handleExtGState()
{
    QPDFObjectHandle state = lookUp("/ExtGState", nameFromEnd(0));
    if (!state.isDictionary()) {
        return;
    }

    const std::string blend = nameOf(state, "/BM");
    if (!blend.empty() && blend != "/Normal" && blend != "/Compatible") {
        const QString label = plainName(blend);
        if (!m_scan.blendModes.contains(label)) {
            m_scan.blendModes.append(label);
        }
        m_scan.transparency = true;
    }
    // A blend mode may also arrive as a one-element array, which readers accept.
    QPDFObjectHandle blendArray = state.getKey("/BM");
    if (blendArray.isArray() && blendArray.getArrayNItems() > 0 && blendArray.getArrayItem(0).isName()) {
        const std::string first = blendArray.getArrayItem(0).getName();
        if (first != "/Normal" && first != "/Compatible") {
            const QString label = plainName(first);
            if (!m_scan.blendModes.contains(label)) {
                m_scan.blendModes.append(label);
            }
            m_scan.transparency = true;
        }
    }

    for (const char *key : { "/ca", "/CA" }) {
        if (state.hasKey(key) && numericValue(state.getKey(key), 1.0) < 1.0) {
            m_scan.transparency = true;
        }
    }
    QPDFObjectHandle mask = state.getKey("/SMask");
    if (mask.isDictionary() || (mask.isName() && mask.getName() != "/None")) {
        m_scan.transparency = true;
    }

    if (state.hasKey("/LW")) {
        m_state.lineWidth = numericValue(state.getKey("/LW"), m_state.lineWidth);
    }
}

void ContentScanner::noteImage(QPDFObjectHandle image, const std::string &resourceName)
{
    m_page.marked = true;
    ++m_scan.drawnImages;

    const int pixelWidth = int(numericValue(image.getDict().getKey("/Width"), 0.0));
    const int pixelHeight = int(numericValue(image.getDict().getKey("/Height"), 0.0));

    // The unit square is where every image lives before the matrix moves it, so
    // the drawn size is the length of the matrix's own two axes.
    const double drawnWidth = std::hypot(m_state.ctm.m11(), m_state.ctm.m12());
    const double drawnHeight = std::hypot(m_state.ctm.m21(), m_state.ctm.m22());

    double lowest = 0.0;
    double highest = 0.0;
    if (pixelWidth > 0 && pixelHeight > 0 && drawnWidth > 0.01 && drawnHeight > 0.01) {
        const double dpiX = pixelWidth * 72.0 / drawnWidth;
        const double dpiY = pixelHeight * 72.0 / drawnHeight;
        lowest = std::min(dpiX, dpiY);
        highest = std::max(dpiX, dpiY);
    }

    const QPDFObjGen key = image.isIndirect() ? image.getObjGen() : QPDFObjGen();
    const auto existing = image.isIndirect() ? m_imageIndex.find(key) : m_imageIndex.end();
    if (existing != m_imageIndex.end()) {
        // The same picture placed twice at different sizes is worth judging at
        // its worst placement, not at whichever one happened to come first.
        ImageFact &fact = m_scan.images[existing->second];
        if (lowest > 0.0) {
            fact.lowestDpi = fact.lowestDpi > 0.0 ? std::min(fact.lowestDpi, lowest) : lowest;
            fact.highestDpi = std::max(fact.highestDpi, highest);
        }
        return;
    }

    ImageFact fact;
    fact.name = plainName(resourceName);
    fact.page = m_pageIndex;
    fact.pixelWidth = pixelWidth;
    fact.pixelHeight = pixelHeight;
    fact.jpeg2000 = streamUsesFilter(image, "/JPXDecode");
    fact.lowestDpi = lowest;
    fact.highestDpi = highest;

    const bool isMask
        = image.getDict().getKey("/ImageMask").isBool() && image.getDict().getKey("/ImageMask").getBoolValue();
    if (!image.getDict().hasKey("/ColorSpace") && !isMask) {
        // JPEG 2000 carries its colour space inside the codestream, so a
        // missing /ColorSpace there is legal rather than a fault.
        fact.noColourSpace = !fact.jpeg2000;
    } else if (image.getDict().hasKey("/ColorSpace")) {
        noteSpace(classifySpace(image.getDict().getKey("/ColorSpace"), m_resources));
    }

    m_scan.images.append(fact);
    if (image.isIndirect()) {
        m_imageIndex[key] = int(m_scan.images.size()) - 1;
    }
}

void ContentScanner::noteForm(QPDFObjectHandle form)
{
    if (m_ancestors.size() >= MaxContentDepth) {
        return;
    }
    QVector<QPDFObjGen> ancestors = m_ancestors;
    if (form.isIndirect()) {
        if (ancestors.contains(form.getObjGen())) {
            // A form that draws itself, directly or round a longer loop. No
            // reader would survive following that either.
            return;
        }
        ancestors.append(form.getObjGen());
    }
    if (m_budget.formsLeft <= 0) {
        m_scan.anyContentUnreadable = true;
        return;
    }
    --m_budget.formsLeft;

    QPDFObjectHandle dict = form.getDict();
    QPDFObjectHandle group = dict.getKey("/Group");
    if (nameOf(group, "/S") == "/Transparency") {
        m_scan.transparency = true;
    }

    QTransform ctm = m_state.ctm;
    QPDFObjectHandle matrix = dict.getKey("/Matrix");
    if (matrix.isArray() && matrix.getArrayNItems() == 6) {
        const QTransform own(PdfGeometry::numberAt(matrix, 0, 1.0), PdfGeometry::numberAt(matrix, 1, 0.0),
                             PdfGeometry::numberAt(matrix, 2, 0.0), PdfGeometry::numberAt(matrix, 3, 1.0),
                             PdfGeometry::numberAt(matrix, 4, 0.0), PdfGeometry::numberAt(matrix, 5, 0.0));
        ctm = own * ctm;
    }

    QPDFObjectHandle resources = dict.getKey("/Resources");
    if (!resources.isDictionary()) {
        // A form without resources of its own inherits the page's, which is
        // common in output from older generators.
        resources = m_resources;
    }

    ContentScanner inner(m_scan, m_page, m_pageIndex, resources, ctm, m_budget, m_imageIndex, ancestors);
    try {
        form.parseAsContents(&inner);
    } catch (const std::exception &) {
        m_page.contentReadable = false;
        m_scan.anyContentUnreadable = true;
    }
}

void ContentScanner::handleDo()
{
    const std::string name = nameFromEnd(0);
    QPDFObjectHandle object = lookUp("/XObject", name);
    if (!object.isStream()) {
        return;
    }
    const std::string subtype = nameOf(object.getDict(), "/Subtype");
    if (subtype == "/Image") {
        noteImage(object, name);
    } else if (subtype == "/Form") {
        noteForm(object);
    }
}

void ContentScanner::noteFont()
{
    m_state.fontSize = numberFromEnd(0);
    const std::string name = nameFromEnd(1);

    QPDFObjectHandle font = lookUp("/Font", name);
    if (!font.isDictionary()) {
        return;
    }

    const FontFact fact = readFont(font, name, m_pageIndex);
    const auto same = [&fact](const FontFact &other) {
        return other.label == fact.label && other.embedded == fact.embedded && other.type3 == fact.type3
            && other.subsetWithoutToUnicode == fact.subsetWithoutToUnicode
            && other.licenceRestricted == fact.licenceRestricted;
    };
    if (std::none_of(m_scan.fonts.cbegin(), m_scan.fonts.cend(), same)) {
        m_scan.fonts.append(fact);
    }
}

void ContentScanner::showText()
{
    // Mode 3 is the invisible text a scanner's OCR leaves behind and mode 7 only
    // clips. Neither puts a mark on the page, and calling an OCR layer "tiny
    // text" would flag every scanned book ever made.
    if (m_state.textRenderMode == 3 || m_state.textRenderMode == 7) {
        return;
    }
    m_page.marked = true;

    if (m_state.fontSize <= 0.0) {
        return;
    }
    const QTransform combined = m_textMatrix * m_state.ctm;
    const double scale = std::hypot(combined.m21(), combined.m22());
    const double effective = m_state.fontSize * scale;
    if (effective <= 0.0) {
        return;
    }
    if (m_page.smallestText < 0.0 || effective < m_page.smallestText) {
        m_page.smallestText = effective;
    }
}

void ContentScanner::handleOperator(const QString &op)
{
    if (op == QLatin1String("q")) {
        m_stack.append(m_state);
        return;
    }
    if (op == QLatin1String("Q")) {
        if (!m_stack.isEmpty()) {
            m_state = m_stack.takeLast();
        }
        return;
    }
    if (op == QLatin1String("cm")) {
        const QTransform step(numberFromEnd(5), numberFromEnd(4), numberFromEnd(3), numberFromEnd(2), numberFromEnd(1),
                              numberFromEnd(0));
        m_state.ctm = step * m_state.ctm;
        return;
    }
    if (op == QLatin1String("w")) {
        m_state.lineWidth = numberFromEnd(0);
        return;
    }
    if (op == QLatin1String("gs")) {
        handleExtGState();
        return;
    }
    if (op == QLatin1String("Do")) {
        handleDo();
        return;
    }
    if (op == QLatin1String("sh")) {
        m_page.marked = true;
        return;
    }

    if (op == QLatin1String("BT")) {
        m_textMatrix = QTransform();
        m_lineMatrix = QTransform();
        return;
    }
    if (op == QLatin1String("Tm")) {
        m_lineMatrix = QTransform(numberFromEnd(5), numberFromEnd(4), numberFromEnd(3), numberFromEnd(2),
                                  numberFromEnd(1), numberFromEnd(0));
        m_textMatrix = m_lineMatrix;
        return;
    }
    if (op == QLatin1String("Td")) {
        m_lineMatrix = QTransform(1, 0, 0, 1, numberFromEnd(1), numberFromEnd(0)) * m_lineMatrix;
        m_textMatrix = m_lineMatrix;
        return;
    }
    if (op == QLatin1String("TD")) {
        m_leading = -numberFromEnd(0);
        m_lineMatrix = QTransform(1, 0, 0, 1, numberFromEnd(1), numberFromEnd(0)) * m_lineMatrix;
        m_textMatrix = m_lineMatrix;
        return;
    }
    if (op == QLatin1String("TL")) {
        m_leading = numberFromEnd(0);
        return;
    }
    if (op == QLatin1String("T*")) {
        nextLine(m_leading);
        return;
    }
    if (op == QLatin1String("Tf")) {
        noteFont();
        return;
    }
    if (op == QLatin1String("Tr")) {
        m_state.textRenderMode = int(numberFromEnd(0));
        return;
    }
    if (op == QLatin1String("Tj") || op == QLatin1String("TJ")) {
        showText();
        return;
    }
    if (op == QLatin1String("'")) {
        nextLine(m_leading);
        showText();
        return;
    }
    if (op == QLatin1String("\"")) {
        nextLine(m_leading);
        showText();
        return;
    }

    if (op == QLatin1String("cs") || op == QLatin1String("CS")) {
        handleColourSpace();
        return;
    }
    if (op == QLatin1String("rg") || op == QLatin1String("RG")) {
        m_scan.usesRgb = true;
        return;
    }
    if (op == QLatin1String("k") || op == QLatin1String("K")) {
        m_scan.usesCmyk = true;
        return;
    }
    if (op == QLatin1String("scn") || op == QLatin1String("SCN") || op == QLatin1String("sc")
        || op == QLatin1String("SC")) {
        // The space was already recorded by cs/CS; only a pattern name adds
        // anything, and a pattern's own content is not walked.
        return;
    }

    static const QStringList strokingOperators { QStringLiteral("S"),  QStringLiteral("s"), QStringLiteral("B"),
                                                 QStringLiteral("B*"), QStringLiteral("b"), QStringLiteral("b*") };
    static const QStringList fillingOperators { QStringLiteral("f"), QStringLiteral("F"),  QStringLiteral("f*"),
                                                QStringLiteral("B"), QStringLiteral("B*"), QStringLiteral("b"),
                                                QStringLiteral("b*") };

    if (strokingOperators.contains(op)) {
        m_page.marked = true;
        const double effective = m_state.lineWidth * transformScale(m_state.ctm);
        if (m_page.thinnestStroke < 0.0 || effective < m_page.thinnestStroke) {
            m_page.thinnestStroke = effective;
        }
        return;
    }
    if (fillingOperators.contains(op)) {
        m_page.marked = true;
    }
}

// ---------------------------------------------------------------------------
// Document-level reading
// ---------------------------------------------------------------------------

bool documentHasScripts(QPDFObjectHandle root)
{
    QPDFObjectHandle names = root.getKey("/Names");
    if (names.isDictionary() && names.hasKey("/JavaScript")) {
        return true;
    }
    QPDFObjectHandle openAction = root.getKey("/OpenAction");
    if (openAction.isDictionary() && (openAction.hasKey("/JS") || nameOf(openAction, "/S") == "/JavaScript")) {
        return true;
    }
    return root.hasKey("/AA");
}

bool annotationHasScript(QPDFObjectHandle annotation)
{
    if (!annotation.isDictionary()) {
        return false;
    }
    if (annotation.hasKey("/AA")) {
        return true;
    }
    QPDFObjectHandle action = annotation.getKey("/A");
    return action.isDictionary() && (action.hasKey("/JS") || nameOf(action, "/S") == "/JavaScript");
}

int embeddedFileCount(QPDFObjectHandle root)
{
    QPDFObjectHandle names = root.getKey("/Names");
    if (!names.isDictionary()) {
        return 0;
    }
    QPDFObjectHandle files = names.getKey("/EmbeddedFiles");
    if (!files.isDictionary()) {
        return 0;
    }
    QPDFObjectHandle list = files.getKey("/Names");
    // The array alternates name, reference, so it holds half as many files as
    // it has entries.
    return list.isArray() ? list.getArrayNItems() / 2 : 0;
}

bool hasOutputIntentWithProfile(QPDFObjectHandle root)
{
    QPDFObjectHandle intents = root.getKey("/OutputIntents");
    if (!intents.isArray()) {
        return false;
    }
    for (int i = 0; i < intents.getArrayNItems(); ++i) {
        QPDFObjectHandle intent = intents.getArrayItem(i);
        if (intent.isDictionary() && intent.hasKey("/DestOutputProfile")) {
            return true;
        }
    }
    return false;
}

/** The XMP packet as text, or empty. */
QString xmpPacket(QPDFObjectHandle root)
{
    QPDFObjectHandle metadata = root.getKey("/Metadata");
    if (!metadata.isStream()) {
        return {};
    }
    try {
        std::shared_ptr<Buffer> buffer = metadata.getStreamData();
        return QString::fromUtf8(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
    } catch (const std::exception &) {
        return {};
    }
}

/** The `dc:title` of an XMP packet, whichever of the two spellings it uses. */
QString xmpTitle(const QString &packet)
{
    static const QRegularExpression titleBlock(QStringLiteral("<dc:title[^>]*>(.*?)</dc:title>"),
                                               QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch outer = titleBlock.match(packet);
    if (!outer.hasMatch()) {
        return {};
    }
    const QString inner = outer.captured(1);

    static const QRegularExpression item(QStringLiteral("<rdf:li[^>]*>(.*?)</rdf:li>"),
                                         QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch listed = item.match(inner);
    QString text = listed.hasMatch() ? listed.captured(1) : inner;

    // A title is text, not markup, so anything left in angle brackets is
    // structure this does not need to understand.
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    text.remove(tags);
    return text.trimmed();
}

/** Which of the standard permissions an encrypted document withholds. */
QStringList deniedPermissionsOf(QPDF &pdf)
{
    QStringList denied;
    if (!pdf.isEncrypted()) {
        return denied;
    }
    if (!pdf.allowPrintHighRes()) {
        denied.append(pdf.allowPrintLowRes() ? i18nc("@item a withheld PDF permission", "printing at full quality")
                                             : i18nc("@item a withheld PDF permission", "printing"));
    }
    if (!pdf.allowExtractAll()) {
        denied.append(i18nc("@item a withheld PDF permission", "copying text"));
    }
    if (!pdf.allowAccessibility()) {
        denied.append(i18nc("@item a withheld PDF permission", "reading by assistive software"));
    }
    if (!pdf.allowModifyAll()) {
        denied.append(i18nc("@item a withheld PDF permission", "editing"));
    }
    if (!pdf.allowModifyAnnotation()) {
        denied.append(i18nc("@item a withheld PDF permission", "commenting"));
    }
    if (!pdf.allowModifyForm()) {
        denied.append(i18nc("@item a withheld PDF permission", "filling in forms"));
    }
    if (!pdf.allowModifyAssembly()) {
        denied.append(i18nc("@item a withheld PDF permission", "rearranging pages"));
    }
    return denied;
}

/**
 * Walks the structure tree for pictures with nothing said about them.
 *
 * A `/Figure` is the only thing in a tagged document that stands in for an
 * image, and without `/Alt` or `/ActualText` it is a hole in the reading order:
 * a screen reader announces "graphic" and moves on.
 */
void collectFiguresWithoutAlt(QPDFObjectHandle node, const std::map<QPDFObjGen, int> &pageIndexes, Scan *scan,
                              std::set<QPDFObjGen> &visited, int depth = 0)
{
    if (depth > 64 || !node.isInitialized() || node.isNull()) {
        return;
    }
    if (node.isIndirect()) {
        if (visited.count(node.getObjGen()) > 0) {
            return;
        }
        visited.insert(node.getObjGen());
    }

    if (node.isArray()) {
        for (int i = 0; i < node.getArrayNItems(); ++i) {
            collectFiguresWithoutAlt(node.getArrayItem(i), pageIndexes, scan, visited, depth + 1);
        }
        return;
    }
    if (!node.isDictionary()) {
        return;
    }

    const std::string role = nameOf(node, "/S");
    if (role == "/Figure" || role == "/Formula") {
        const bool described = node.hasKey("/Alt") || node.hasKey("/ActualText");
        if (!described) {
            int page = -1;
            QPDFObjectHandle owner = node.getKey("/Pg");
            if (owner.isIndirect()) {
                const auto found = pageIndexes.find(owner.getObjGen());
                if (found != pageIndexes.end()) {
                    page = found->second;
                }
            }
            scan->figuresWithoutAlt.append(qMakePair(page, plainName(role)));
        }
    }

    collectFiguresWithoutAlt(node.getKey("/K"), pageIndexes, scan, visited, depth + 1);
}

void readCatalogue(QPDF &pdf, Scan *scan)
{
    QPDFObjectHandle root = pdf.getRoot();

    scan->javaScript = documentHasScripts(root);
    scan->embeddedFiles = embeddedFileCount(root);
    scan->openAction = root.hasKey("/OpenAction");
    scan->hasOutputIntent = hasOutputIntentWithProfile(root);
    if (scan->hasOutputIntent) {
        scan->usesIcc = true;
    }

    QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
    scan->infoTitle = stringValue(info, "/Title").trimmed();

    scan->xmpTitle = xmpTitle(xmpPacket(root));

    QPDFObjectHandle markInfo = root.getKey("/MarkInfo");
    const bool marked
        = markInfo.isDictionary() && markInfo.getKey("/Marked").isBool() && markInfo.getKey("/Marked").getBoolValue();
    scan->tagged = root.hasKey("/StructTreeRoot") && marked;

    QPDFObjectHandle language = root.getKey("/Lang");
    if (language.isString()) {
        scan->language = QString::fromStdString(language.getUTF8Value()).trimmed();
    }
}

/** Whether an annotation is one whose position is worth judging. */
bool annotationIsVisible(QPDFObjectHandle annotation)
{
    if (nameOf(annotation, "/Subtype") == "/Popup") {
        // A pop-up note lives wherever its parent put it and is not painted
        // until somebody clicks, so its rectangle proves nothing.
        return false;
    }
    const int flags = int(numericValue(annotation.getKey("/F"), 0.0));
    // Bit 2 is Hidden and bit 6 is NoView; neither will ever appear on paper.
    return (flags & 0x02) == 0 && (flags & 0x20) == 0;
}

void readPage(QPDFPageObjectHelper &page, Scan *scan)
{
    PageFact fact;
    const QRectF media = PdfGeometry::mediaBoxOf(page);
    fact.mediaSize = media.size();
    fact.rotate = PdfGeometry::rotationOf(page);
    fact.hasTrimBox = page.getObjectHandle().hasKey("/TrimBox");

    QPDFObjectHandle crop = page.getCropBox(false, false);
    QRectF cropRect = media;
    if (crop.isArray() && crop.getArrayNItems() == 4) {
        const double left = std::min(boxValue(crop, 0, media.left()), boxValue(crop, 2, media.right()));
        const double right = std::max(boxValue(crop, 0, media.left()), boxValue(crop, 2, media.right()));
        const double bottom = std::min(boxValue(crop, 1, media.top()), boxValue(crop, 3, media.bottom()));
        const double top = std::max(boxValue(crop, 1, media.top()), boxValue(crop, 3, media.bottom()));
        cropRect = QRectF(left, bottom, right - left, top - bottom);
    }
    fact.cropDiffersFromMedia = std::abs(cropRect.width() - media.width()) > GeometryTolerance
        || std::abs(cropRect.height() - media.height()) > GeometryTolerance
        || std::abs(cropRect.left() - media.left()) > GeometryTolerance
        || std::abs(cropRect.top() - media.top()) > GeometryTolerance;

    QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
    if (annotations.isArray()) {
        for (int i = 0; i < annotations.getArrayNItems(); ++i) {
            QPDFObjectHandle annotation = annotations.getArrayItem(i);
            if (!annotation.isDictionary()) {
                continue;
            }
            if (annotationHasScript(annotation)) {
                scan->javaScript = true;
            }
            if (nameOf(annotation, "/Subtype") == "/FileAttachment") {
                ++scan->embeddedFiles;
            }
            if (!annotationIsVisible(annotation)) {
                continue;
            }
            QPDFObjectHandle rect = annotation.getKey("/Rect");
            if (!rect.isArray() || rect.getArrayNItems() != 4) {
                continue;
            }
            const double left = std::min(boxValue(rect, 0, 0.0), boxValue(rect, 2, 0.0));
            const double right = std::max(boxValue(rect, 0, 0.0), boxValue(rect, 2, 0.0));
            const double bottom = std::min(boxValue(rect, 1, 0.0), boxValue(rect, 3, 0.0));
            const double top = std::max(boxValue(rect, 1, 0.0), boxValue(rect, 3, 0.0));
            const QRectF box(left, bottom, right - left, top - bottom);

            const QRectF allowed
                = cropRect.adjusted(-GeometryTolerance, -GeometryTolerance, GeometryTolerance, GeometryTolerance);
            if (!allowed.contains(box)) {
                QString label = plainName(nameOf(annotation, "/Subtype"));
                if (label.isEmpty()) {
                    label = i18nc("@item an annotation whose type the file does not give", "an annotation");
                }
                fact.annotationsOutside.append(label);
            }
        }
    }

    QPDFObjectHandle group = page.getObjectHandle().getKey("/Group");
    if (nameOf(group, "/S") == "/Transparency") {
        scan->transparency = true;
    }

    scan->pages.append(fact);
}

bool scanDocument(const QString &path, Scan *scan, QString *error)
{
    *scan = Scan {};

    QPDF pdf;
    try {
        PdfFile::open(pdf, path);
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password) {
            // A document that will not open without a password can still be
            // reported on ("it is encrypted" is the finding), but nothing
            // inside it can be examined, and run() says which rules that cost.
            scan->encrypted = true;
            scan->passwordNeeded = true;
            return true;
        }
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    try {
        scan->encrypted = pdf.isEncrypted();
        scan->deniedPermissions = deniedPermissionsOf(pdf);

        readCatalogue(pdf, scan);

        std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
        std::map<QPDFObjGen, int> pageIndexes;
        std::map<QPDFObjGen, int> imageIndex;

        for (int i = 0; i < int(pages.size()); ++i) {
            QPDFPageObjectHelper &page = pages[size_t(i)];
            if (page.getObjectHandle().isIndirect()) {
                pageIndexes[page.getObjectHandle().getObjGen()] = i;
            }
            readPage(page, scan);

            QPDFObjectHandle resources = page.getAttribute("/Resources", false);
            // A budget per page rather than per document: one page's tangle of
            // nested forms must not exhaust what is left for the next.
            WalkBudget budget;
            ContentScanner walker(*scan, scan->pages[i], i, resources, QTransform(), budget, imageIndex, {});
            try {
                page.parseContents(&walker);
            } catch (const std::exception &) {
                scan->pages[i].contentReadable = false;
                scan->anyContentUnreadable = true;
            }
        }

        if (scan->tagged) {
            std::set<QPDFObjGen> visited;
            collectFiguresWithoutAlt(pdf.getRoot().getKey("/StructTreeRoot").getKey("/K"), pageIndexes, scan, visited);
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Turning facts into findings
// ---------------------------------------------------------------------------

/** Which fix, if any, is offered for a rule. */
QString fixFor(QLatin1String rule)
{
    if (rule == Rule::FontNotEmbedded) {
        return Fix::EmbedFonts;
    }
    if (rule == Rule::InteractiveJavaScript) {
        return Fix::RemoveJavaScript;
    }
    if (rule == Rule::InteractiveEmbeddedFiles) {
        return Fix::RemoveEmbeddedFiles;
    }
    if (rule == Rule::InteractiveOpenAction) {
        return Fix::RemoveOpenAction;
    }
    if (rule == Rule::PageMissingTrimBox) {
        return Fix::SetTrimBox;
    }
    if (rule == Rule::MetadataNoTitle) {
        return Fix::SetTitle;
    }
    if (rule == Rule::SecurityEncrypted) {
        return Fix::Decrypt;
    }
    if (rule == Rule::ImageExcessiveResolution) {
        return Fix::DownsampleImages;
    }
    return {};
}

/**
 * Gathers findings, but only the ones the profile asked for.
 *
 * The gate lives here rather than at each call site so that a rule can be
 * written once as "what is wrong" and the profile decides separately whether
 * that is worth saying and how loudly.
 */
class Collector
{
public:
    explicit Collector(const Preflight::Profile &profile)
        : m_profile(profile)
    {
    }

    double threshold(QLatin1String rule, double fallback) const { return m_profile.thresholds.value(rule, fallback); }

    void add(QLatin1String rule, const QString &message, int page = -1, const QString &object = {})
    {
        const auto severity = m_profile.rules.constFind(rule);
        if (severity == m_profile.rules.constEnd()) {
            return;
        }
        const QString id = rule;
        if (m_counts[id] >= MaxFindingsPerRule) {
            ++m_suppressed[id];
            return;
        }
        ++m_counts[id];

        Preflight::Finding finding;
        finding.ruleId = id;
        finding.severity = *severity;
        finding.message = message;
        finding.page = page;
        finding.object = object;
        finding.fixId = fixFor(rule);
        if (!finding.fixId.isEmpty()) {
            finding.fixDescription = Preflight::describeFix(finding.fixId);
        }
        m_findings.append(finding);
    }

    /** Rounds off the capped rules and counts what was found. */
    void finish(Preflight::Report *report)
    {
        // Sorted, because a QHash hands its keys back in an order that changes
        // from one run to the next, and a report nobody can diff against
        // yesterday's is worth much less than one they can.
        QStringList capped = m_suppressed.keys();
        capped.sort();
        for (const QString &rule : std::as_const(capped)) {
            Preflight::Finding finding;
            finding.ruleId = rule;
            finding.severity = Preflight::Severity::Note;
            finding.message = i18np("One further place has the same problem; it is not listed individually.",
                                    "%1 further places have the same problem; they are not listed individually.",
                                    m_suppressed.value(rule));
            m_findings.append(finding);
        }

        report->findings = m_findings;
        for (const Preflight::Finding &finding : std::as_const(m_findings)) {
            if (finding.severity == Preflight::Severity::Error) {
                ++report->errors;
            } else if (finding.severity == Preflight::Severity::Warning) {
                ++report->warnings;
            }
        }
        report->passed = report->errors == 0;
    }

private:
    const Preflight::Profile &m_profile;
    QVector<Preflight::Finding> m_findings;
    QHash<QString, int> m_counts;
    QHash<QString, int> m_suppressed;
};

QString pageSizeText(const QSizeF &size)
{
    // Millimetres, because that is how paper is sold and how a printer will
    // answer. Formatted through QLocale so the decimal comma is right.
    return i18nc("@item a page size in millimetres", "%1 × %2 mm",
                 QLocale().toString(size.width() * 25.4 / 72.0, 'f', 1),
                 QLocale().toString(size.height() * 25.4 / 72.0, 'f', 1));
}

void applyFontRules(const Scan &scan, Collector *collector)
{
    for (const FontFact &font : scan.fonts) {
        if (!font.embedded) {
            collector->add(Rule::FontNotEmbedded,
                           i18n("The font “%1” is not embedded, so its text will be set in whatever the reader "
                                "happens to have instead.",
                                font.label),
                           font.page, font.label);
        }
        if (font.type3) {
            collector->add(Rule::FontType3,
                           i18n("“%1” is a Type 3 font: its letters are drawings rather than glyphs, which many "
                                "workflows cannot search, scale or trap properly.",
                                font.label),
                           font.page, font.label);
        }
        if (font.subsetWithoutToUnicode) {
            collector->add(Rule::FontSubsetWithoutToUnicode,
                           i18n("Only part of the font “%1” was embedded and it carries no /ToUnicode table, so its "
                                "text cannot be copied, searched or read aloud.",
                                font.label),
                           font.page, font.label);
        }
        if (font.licenceRestricted) {
            collector->add(Rule::FontLicenceRestricted,
                           i18n("The font “%1” states in its own licence field that it may not be embedded in a "
                                "document.",
                                font.label),
                           font.page, font.label);
        }
    }
}

void applyImageRules(const Scan &scan, Collector *collector)
{
    const double floorDpi = collector->threshold(Rule::ImageLowResolution, 300.0);
    const double ceilingDpi = collector->threshold(Rule::ImageExcessiveResolution, 900.0);

    for (const ImageFact &image : scan.images) {
        if (image.lowestDpi > 0.0 && image.lowestDpi < floorDpi) {
            collector->add(Rule::ImageLowResolution,
                           i18n("A picture is placed at %1 dpi, below the %2 dpi this profile asks for.",
                                QLocale().toString(qRound(image.lowestDpi)), QLocale().toString(qRound(floorDpi))),
                           image.page, image.name);
        }
        if (image.highestDpi > ceilingDpi) {
            collector->add(Rule::ImageExcessiveResolution,
                           i18n("A picture is placed at %1 dpi, far above the %2 dpi this profile asks for; it makes "
                                "the file large without making the print better.",
                                QLocale().toString(qRound(image.highestDpi)), QLocale().toString(qRound(ceilingDpi))),
                           image.page, image.name);
        }
        if (image.jpeg2000) {
            collector->add(Rule::ImageJpeg2000,
                           i18n("A picture is stored as JPEG 2000, which older readers and some printing systems "
                                "cannot decode."),
                           image.page, image.name);
        }
        if (image.noColourSpace) {
            collector->add(Rule::ImageNoColourSpace,
                           i18n("A picture names no colour space, so nothing in the file says what its numbers mean."),
                           image.page, image.name);
        }
    }
}

void applyColourRules(const Scan &scan, Collector *collector)
{
    if (scan.usesRgb) {
        collector->add(Rule::ColourRgbInPrint,
                       i18n("The document paints in RGB. A press works in CMYK, so those colours will be converted by "
                            "somebody else's settings unless they are converted here first."));
    }
    if (scan.usesCmyk) {
        collector->add(Rule::ColourCmykInWeb,
                       i18n("The document paints in CMYK, which browsers handle poorly and inconsistently; RGB is "
                            "what a screen actually shows."));
    }
    if (!scan.spotColours.isEmpty()) {
        collector->add(Rule::ColourSpot,
                       i18np("The document uses one spot colour (%2), which is a separate plate on press.",
                             "The document uses %1 spot colours (%2), each of which is a separate plate on press.",
                             int(scan.spotColours.size()),
                             scan.spotColours.join(i18nc("@item separator in a list of colour names", ", "))));
    }
    if (!scan.usesIcc) {
        collector->add(Rule::ColourNoIcc,
                       i18n("Nothing in the document defines its colours with an ICC profile, so every reader will "
                            "guess at them."));
    }
    if (!scan.hasOutputIntent) {
        collector->add(Rule::ColourNoOutputIntent,
                       i18n("The document has no output intent, so nothing in it says what its colours are meant to "
                            "look like."));
    }
}

void applyTransparencyRules(const Scan &scan, Collector *collector)
{
    if (scan.transparency) {
        collector->add(Rule::TransparencyPresent,
                       i18n("The document uses transparency. Older printing systems flatten it themselves, and the "
                            "result is not always what was drawn."));
    }
    if (!scan.blendModes.isEmpty()) {
        collector->add(Rule::TransparencyBlendModes,
                       i18np("The document uses the blend mode %2, which is not reproduced identically everywhere.",
                             "The document uses %1 blend modes (%2), which are not reproduced identically everywhere.",
                             int(scan.blendModes.size()),
                             scan.blendModes.join(i18nc("@item separator in a list of blend modes", ", "))));
    }
}

void applyPageRules(const Scan &scan, Collector *collector)
{
    for (int i = 0; i < scan.pages.size(); ++i) {
        const PageFact &page = scan.pages.at(i);
        if (!page.hasTrimBox) {
            collector->add(Rule::PageMissingTrimBox,
                           i18n("The page has no trim box, so nothing in the file says where the finished sheet is to "
                                "be cut."),
                           i);
        }
        if (page.cropDiffersFromMedia) {
            collector->add(Rule::PageMediaDiffersFromCrop,
                           i18n("The page's crop box differs from its media box, so what is shown on screen is not "
                                "the whole sheet."),
                           i);
        }
        if (!page.marked && page.contentReadable) {
            collector->add(Rule::PageEmpty, i18n("The page has nothing drawn on it."), i);
        }
        if (!page.annotationsOutside.isEmpty()) {
            // The count is of annotations and the list is of their kinds, so the
            // list is deduplicated: "2 annotations (Square, Square)" tells a
            // reader nothing the number did not already say.
            QStringList kinds = page.annotationsOutside;
            kinds.removeDuplicates();
            kinds.sort();
            collector->add(Rule::InteractiveAnnotationsOutside,
                           i18np("One annotation reaches outside the page (%2).",
                                 "%1 annotations reach outside the page (%2).", int(page.annotationsOutside.size()),
                                 kinds.join(i18nc("@item separator in a list of annotation types", ", "))),
                           i);
        }
    }

    if (scan.pages.size() > 1) {
        const QSizeF first = scan.pages.first().mediaSize;
        const int firstRotation = scan.pages.first().rotate;
        for (int i = 1; i < scan.pages.size(); ++i) {
            const PageFact &page = scan.pages.at(i);
            if (std::abs(page.mediaSize.width() - first.width()) > GeometryTolerance
                || std::abs(page.mediaSize.height() - first.height()) > GeometryTolerance) {
                collector->add(Rule::PageMixedSizes,
                               i18n("The page measures %1 while the first page measures %2.",
                                    pageSizeText(page.mediaSize), pageSizeText(first)),
                               i);
            }
            // Rotation is judged apart from size deliberately: two pages of the
            // same paper, one turned, are one sheet size and two orientations,
            // and a printer needs to hear about each separately.
            if (page.rotate != firstRotation) {
                collector->add(Rule::PageMixedRotation,
                               i18n("The page is turned by %1 degrees while the first page is turned by %2.",
                                    QLocale().toString(page.rotate), QLocale().toString(firstRotation)),
                               i);
            }
        }
    }
}

void applyInteractiveRules(const Scan &scan, Collector *collector)
{
    if (scan.javaScript) {
        collector->add(Rule::InteractiveJavaScript,
                       i18n("The document contains scripts, or an action that runs when it is opened."));
    }
    if (scan.embeddedFiles > 0) {
        collector->add(Rule::InteractiveEmbeddedFiles,
                       i18np("The document carries one other file inside it.",
                             "The document carries %1 other files inside it.", scan.embeddedFiles));
    }
    if (scan.openAction) {
        collector->add(Rule::InteractiveOpenAction,
                       i18n("The document does something of its own accord when it is opened."));
    }
}

void applySecurityRules(const Scan &scan, Collector *collector)
{
    if (scan.encrypted) {
        collector->add(Rule::SecurityEncrypted,
                       i18n("The document is encrypted, so anything that has to open it later needs the password to "
                            "still exist."));
    }
    if (!scan.deniedPermissions.isEmpty()) {
        collector->add(Rule::SecurityPermissions,
                       i18n("The document withholds permission for %1. Readers are free to ignore that, and many do, "
                            "but the ones that obey it will get in the way.",
                            scan.deniedPermissions.join(i18nc("@item separator in a list of permissions", ", "))));
    }
}

void applyMetadataRules(const Scan &scan, Collector *collector)
{
    if (scan.infoTitle.isEmpty() && scan.xmpTitle.isEmpty()) {
        collector->add(Rule::MetadataNoTitle,
                       i18n("The document has no title, so readers and archives will show its file name instead."));
    }
    if (!scan.infoTitle.isEmpty() && !scan.xmpTitle.isEmpty() && scan.infoTitle != scan.xmpTitle) {
        collector->add(Rule::MetadataXmpDisagrees,
                       i18n("The document's two sets of metadata disagree about its title: “%1” against “%2”.",
                            scan.infoTitle, scan.xmpTitle));
    }
}

void applyStructureRules(const Scan &scan, Collector *collector)
{
    if (!scan.tagged) {
        collector->add(Rule::StructureNoTags,
                       i18n("The document is not tagged, so nothing in it says what is a heading, a table or a "
                            "caption. Assistive software has only the reading order to go on."));
    }
    if (scan.language.isEmpty()) {
        collector->add(Rule::StructureNoLanguage,
                       i18n("The document does not say what language it is in, so a screen reader will pronounce it "
                            "in whatever language it was set up for."));
    }

    if (!scan.tagged) {
        if (scan.drawnImages > 0) {
            collector->add(Rule::StructureImagesWithoutAlt,
                           i18np("The document is not tagged, so its one picture has no text alternative.",
                                 "The document is not tagged, so none of its %1 pictures has a text alternative.",
                                 scan.drawnImages));
        }
        return;
    }
    for (const auto &figure : scan.figuresWithoutAlt) {
        collector->add(Rule::StructureImagesWithoutAlt,
                       i18n("A picture is tagged but has no text alternative, so a screen reader can only announce "
                            "that something is there."),
                       figure.first, figure.second);
    }
}

void applyInkRules(const Scan &scan, Collector *collector)
{
    const double hairline = collector->threshold(Rule::StrokeHairline, 0.25);
    const double tiny = collector->threshold(Rule::TextTiny, 5.0);

    for (int i = 0; i < scan.pages.size(); ++i) {
        const PageFact &page = scan.pages.at(i);
        if (page.thinnestStroke >= 0.0 && page.thinnestStroke < hairline) {
            if (page.thinnestStroke <= 0.0) {
                collector->add(Rule::StrokeHairline,
                               i18n("The page strokes a line of width zero, which means “as thin as this device can "
                                    "manage”: invisible on a press, a hairline on a laser printer."),
                               i);
            } else {
                collector->add(Rule::StrokeHairline,
                               i18n("The page strokes a line %1 pt wide, below the %2 pt this profile asks for; it "
                                    "may disappear in print.",
                                    QLocale().toString(page.thinnestStroke, 'f', 2),
                                    QLocale().toString(hairline, 'f', 2)),
                               i);
            }
        }
        if (page.smallestText >= 0.0 && page.smallestText < tiny) {
            collector->add(Rule::TextTiny,
                           i18n("The page sets text at %1 pt, below the %2 pt this profile asks for.",
                                QLocale().toString(page.smallestText, 'f', 1), QLocale().toString(tiny, 'f', 1)),
                           i);
        }
    }
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

using Severity = Preflight::Severity;

Preflight::Profile makeProfile(QLatin1String id, const QString &name,
                               const QVector<QPair<QLatin1String, Severity>> &rules,
                               const QVector<QPair<QLatin1String, double>> &thresholds = {})
{
    Preflight::Profile profile;
    profile.id = id;
    profile.name = name;
    for (const auto &rule : rules) {
        profile.rules.insert(rule.first, rule.second);
    }
    for (const auto &threshold : thresholds) {
        profile.thresholds.insert(threshold.first, threshold.second);
    }
    return profile;
}

/** The rules every archival profile shares, so the three stay in step. */
QVector<QPair<QLatin1String, Severity>> archivalCommon()
{
    return {
        { Rule::FontNotEmbedded, Severity::Error },         { Rule::FontSubsetWithoutToUnicode, Severity::Warning },
        { Rule::FontLicenceRestricted, Severity::Warning }, { Rule::ImageNoColourSpace, Severity::Error },
        { Rule::ColourNoOutputIntent, Severity::Error },    { Rule::ColourNoIcc, Severity::Warning },
        { Rule::InteractiveJavaScript, Severity::Error },   { Rule::InteractiveOpenAction, Severity::Warning },
        { Rule::SecurityEncrypted, Severity::Error },       { Rule::SecurityPermissions, Severity::Warning },
        { Rule::MetadataNoTitle, Severity::Warning },       { Rule::MetadataXmpDisagrees, Severity::Warning },
        { Rule::StructureNoLanguage, Severity::Note },      { Rule::PageEmpty, Severity::Note },
    };
}

QVector<QPair<QLatin1String, Severity>> exchangeCommon()
{
    return {
        { Rule::FontNotEmbedded, Severity::Error },
        { Rule::FontLicenceRestricted, Severity::Warning },
        { Rule::ImageNoColourSpace, Severity::Error },
        { Rule::ImageLowResolution, Severity::Warning },
        { Rule::ColourNoOutputIntent, Severity::Error },
        { Rule::ColourSpot, Severity::Note },
        { Rule::PageMissingTrimBox, Severity::Error },
        { Rule::ContentOutsideTrimBox, Severity::Warning },
        { Rule::InteractiveJavaScript, Severity::Error },
        { Rule::InteractiveEmbeddedFiles, Severity::Error },
        { Rule::InteractiveAnnotationsOutside, Severity::Warning },
        { Rule::SecurityEncrypted, Severity::Error },
        { Rule::MetadataNoTitle, Severity::Warning },
    };
}

} // namespace

QVector<Preflight::Profile> Preflight::builtinProfiles()
{
    QVector<Profile> profiles;

    profiles.append(makeProfile(QLatin1String("pdfa-1b"),
                                i18nc("@item preflight profile", "PDF/A-1b (long-term archiving, strictest)"),
                                archivalCommon()
                                    + QVector<QPair<QLatin1String, Severity>> {
                                        { Rule::TransparencyPresent, Severity::Error },
                                        { Rule::TransparencyBlendModes, Severity::Error },
                                        { Rule::ImageJpeg2000, Severity::Error },
                                        { Rule::InteractiveEmbeddedFiles, Severity::Error },
                                    }));

    profiles.append(makeProfile(QLatin1String("pdfa-2b"),
                                i18nc("@item preflight profile", "PDF/A-2b (long-term archiving, usual choice)"),
                                archivalCommon()
                                    + QVector<QPair<QLatin1String, Severity>> {
                                        { Rule::InteractiveEmbeddedFiles, Severity::Error },
                                    }));

    // PDF/A-3 exists precisely so a document may carry its source spreadsheet
    // along, so the attachment rule is the one thing it drops.
    profiles.append(makeProfile(QLatin1String("pdfa-3b"),
                                i18nc("@item preflight profile", "PDF/A-3b (archiving with attachments allowed)"),
                                archivalCommon()));

    profiles.append(makeProfile(QLatin1String("pdfx-1a"),
                                i18nc("@item preflight profile", "PDF/X-1a (CMYK-only print exchange)"),
                                exchangeCommon()
                                    + QVector<QPair<QLatin1String, Severity>> {
                                        { Rule::ColourRgbInPrint, Severity::Error },
                                        { Rule::TransparencyPresent, Severity::Error },
                                        { Rule::TransparencyBlendModes, Severity::Error },
                                        { Rule::ImageJpeg2000, Severity::Error },
                                    },
                                { { Rule::ImageLowResolution, 300.0 } }));

    profiles.append(makeProfile(QLatin1String("pdfx-3"),
                                i18nc("@item preflight profile", "PDF/X-3 (print exchange, managed colour allowed)"),
                                exchangeCommon()
                                    + QVector<QPair<QLatin1String, Severity>> {
                                        { Rule::ColourRgbInPrint, Severity::Warning },
                                        { Rule::ColourNoIcc, Severity::Warning },
                                        { Rule::TransparencyPresent, Severity::Error },
                                        { Rule::TransparencyBlendModes, Severity::Error },
                                        { Rule::ImageJpeg2000, Severity::Error },
                                    },
                                { { Rule::ImageLowResolution, 300.0 } }));

    profiles.append(makeProfile(QLatin1String("pdfx-4"),
                                i18nc("@item preflight profile", "PDF/X-4 (print exchange, transparency allowed)"),
                                exchangeCommon()
                                    + QVector<QPair<QLatin1String, Severity>> {
                                        { Rule::ColourRgbInPrint, Severity::Warning },
                                        { Rule::ColourNoIcc, Severity::Warning },
                                        { Rule::FontSubsetWithoutToUnicode, Severity::Note },
                                    },
                                { { Rule::ImageLowResolution, 300.0 } }));

    profiles.append(makeProfile(QLatin1String("print-ready"),
                                i18nc("@item preflight profile", "Print-ready (what a commercial printer wants)"),
                                {
                                    { Rule::FontNotEmbedded, Severity::Error },
                                    { Rule::FontType3, Severity::Note },
                                    { Rule::FontLicenceRestricted, Severity::Warning },
                                    { Rule::ImageLowResolution, Severity::Warning },
                                    { Rule::ImageExcessiveResolution, Severity::Note },
                                    { Rule::ImageNoColourSpace, Severity::Warning },
                                    { Rule::ColourRgbInPrint, Severity::Warning },
                                    { Rule::ColourSpot, Severity::Note },
                                    { Rule::ColourNoOutputIntent, Severity::Warning },
                                    { Rule::TransparencyPresent, Severity::Note },
                                    { Rule::PageMissingTrimBox, Severity::Warning },
                                    { Rule::PageMediaDiffersFromCrop, Severity::Note },
                                    { Rule::PageMixedSizes, Severity::Warning },
                                    { Rule::PageMixedRotation, Severity::Note },
                                    { Rule::PageEmpty, Severity::Note },
                                    { Rule::ContentOutsideTrimBox, Severity::Warning },
                                    { Rule::InteractiveAnnotationsOutside, Severity::Note },
                                    { Rule::SecurityEncrypted, Severity::Error },
                                    { Rule::StrokeHairline, Severity::Warning },
                                    { Rule::TextTiny, Severity::Note },
                                },
                                { { Rule::ImageLowResolution, 300.0 },
                                  { Rule::ImageExcessiveResolution, 900.0 },
                                  { Rule::StrokeHairline, 0.25 },
                                  { Rule::TextTiny, 5.0 } }));

    profiles.append(makeProfile(QLatin1String("web"),
                                i18nc("@item preflight profile", "Web (small, fast and readable in a browser)"),
                                {
                                    { Rule::FontNotEmbedded, Severity::Warning },
                                    { Rule::ImageExcessiveResolution, Severity::Warning },
                                    { Rule::ImageJpeg2000, Severity::Note },
                                    { Rule::ColourCmykInWeb, Severity::Warning },
                                    { Rule::InteractiveJavaScript, Severity::Warning },
                                    { Rule::InteractiveOpenAction, Severity::Note },
                                    { Rule::SecurityEncrypted, Severity::Warning },
                                    { Rule::MetadataNoTitle, Severity::Warning },
                                    { Rule::PageMixedSizes, Severity::Note },
                                    { Rule::StructureNoLanguage, Severity::Note },
                                },
                                { { Rule::ImageExcessiveResolution, 150.0 } }));

    profiles.append(makeProfile(QLatin1String("accessible"),
                                i18nc("@item preflight profile", "Accessible (usable with a screen reader)"),
                                {
                                    { Rule::StructureNoTags, Severity::Error },
                                    { Rule::StructureNoLanguage, Severity::Error },
                                    { Rule::StructureImagesWithoutAlt, Severity::Error },
                                    { Rule::MetadataNoTitle, Severity::Error },
                                    { Rule::FontNotEmbedded, Severity::Warning },
                                    { Rule::FontSubsetWithoutToUnicode, Severity::Error },
                                    { Rule::FontType3, Severity::Warning },
                                    { Rule::SecurityEncrypted, Severity::Warning },
                                    { Rule::SecurityPermissions, Severity::Warning },
                                    { Rule::TextTiny, Severity::Warning },
                                },
                                { { Rule::TextTiny, 6.0 } }));

    return profiles;
}

Preflight::Profile Preflight::profileById(const QString &id)
{
    const QVector<Profile> profiles = builtinProfiles();
    for (const Profile &profile : profiles) {
        if (profile.id == id) {
            return profile;
        }
    }
    return {};
}

QStringList Preflight::knownRules()
{
    return {
        Rule::FontNotEmbedded,
        Rule::FontSubsetWithoutToUnicode,
        Rule::FontType3,
        Rule::FontLicenceRestricted,
        Rule::ImageLowResolution,
        Rule::ImageExcessiveResolution,
        Rule::ImageJpeg2000,
        Rule::ImageNoColourSpace,
        Rule::ColourRgbInPrint,
        Rule::ColourCmykInWeb,
        Rule::ColourSpot,
        Rule::ColourNoIcc,
        Rule::ColourNoOutputIntent,
        Rule::TransparencyPresent,
        Rule::TransparencyBlendModes,
        Rule::PageMissingTrimBox,
        Rule::PageMediaDiffersFromCrop,
        Rule::PageMixedSizes,
        Rule::PageMixedRotation,
        Rule::PageEmpty,
        Rule::InteractiveJavaScript,
        Rule::InteractiveEmbeddedFiles,
        Rule::InteractiveOpenAction,
        Rule::InteractiveAnnotationsOutside,
        Rule::SecurityEncrypted,
        Rule::SecurityPermissions,
        Rule::MetadataNoTitle,
        Rule::MetadataXmpDisagrees,
        Rule::StructureNoTags,
        Rule::StructureNoLanguage,
        Rule::StructureImagesWithoutAlt,
        Rule::StrokeHairline,
        Rule::TextTiny,
        Rule::ContentOutsideTrimBox,
    };
}

QStringList Preflight::implementedRules()
{
    QStringList rules = knownRules();
    for (const QString &missing : unimplementedRules()) {
        rules.removeAll(missing);
    }
    return rules;
}

QString Preflight::describeRule(const QString &ruleId)
{
    if (ruleId == Rule::FontNotEmbedded) {
        return i18n("A font the document uses but does not carry, so the text is set in a substitute.");
    }
    if (ruleId == Rule::FontSubsetWithoutToUnicode) {
        return i18n("A partly embedded font with no /ToUnicode table, whose text cannot be copied or read aloud.");
    }
    if (ruleId == Rule::FontType3) {
        return i18n("A Type 3 font, whose letters are drawings rather than glyphs.");
    }
    if (ruleId == Rule::FontLicenceRestricted) {
        return i18n("A font whose own licence field forbids embedding it.");
    }
    if (ruleId == Rule::ImageLowResolution) {
        return i18n("A picture placed at fewer dots per inch than the profile asks for.");
    }
    if (ruleId == Rule::ImageExcessiveResolution) {
        return i18n("A picture placed at far more dots per inch than anything can use.");
    }
    if (ruleId == Rule::ImageJpeg2000) {
        return i18n("A picture stored as JPEG 2000, which not every reader can decode.");
    }
    if (ruleId == Rule::ImageNoColourSpace) {
        return i18n("A picture that names no colour space.");
    }
    if (ruleId == Rule::ColourRgbInPrint) {
        return i18n("RGB colour in a document meant for a press.");
    }
    if (ruleId == Rule::ColourCmykInWeb) {
        return i18n("CMYK colour in a document meant for a screen.");
    }
    if (ruleId == Rule::ColourSpot) {
        return i18n("Spot colours, each of which is a separate plate on press.");
    }
    if (ruleId == Rule::ColourNoIcc) {
        return i18n("No ICC profile anywhere, so the colours are undefined.");
    }
    if (ruleId == Rule::ColourNoOutputIntent) {
        return i18n("No output intent, so nothing states the intended printing condition.");
    }
    if (ruleId == Rule::TransparencyPresent) {
        return i18n("Transparency, which older printing systems have to flatten.");
    }
    if (ruleId == Rule::TransparencyBlendModes) {
        return i18n("Blend modes other than Normal, which are not reproduced identically everywhere.");
    }
    if (ruleId == Rule::PageMissingTrimBox) {
        return i18n("A page with no trim box, so the finished size is unstated.");
    }
    if (ruleId == Rule::PageMediaDiffersFromCrop) {
        return i18n("A page whose crop box hides part of the sheet.");
    }
    if (ruleId == Rule::PageMixedSizes) {
        return i18n("Pages of more than one size in one document.");
    }
    if (ruleId == Rule::PageMixedRotation) {
        return i18n("Pages turned by different amounts in one document.");
    }
    if (ruleId == Rule::PageEmpty) {
        return i18n("A page with nothing drawn on it.");
    }
    if (ruleId == Rule::InteractiveJavaScript) {
        return i18n("Scripts, or an action that runs when the document is opened.");
    }
    if (ruleId == Rule::InteractiveEmbeddedFiles) {
        return i18n("Other files carried inside the document.");
    }
    if (ruleId == Rule::InteractiveOpenAction) {
        return i18n("Anything the document does of its own accord when opened.");
    }
    if (ruleId == Rule::InteractiveAnnotationsOutside) {
        return i18n("An annotation that reaches outside the page.");
    }
    if (ruleId == Rule::SecurityEncrypted) {
        return i18n("A document that needs a password, or carries one.");
    }
    if (ruleId == Rule::SecurityPermissions) {
        return i18n("Permissions the document withholds, such as printing or copying.");
    }
    if (ruleId == Rule::MetadataNoTitle) {
        return i18n("A document with no title of its own.");
    }
    if (ruleId == Rule::MetadataXmpDisagrees) {
        return i18n("Two sets of metadata that disagree with each other.");
    }
    if (ruleId == Rule::StructureNoTags) {
        return i18n("An untagged document, which assistive software cannot navigate.");
    }
    if (ruleId == Rule::StructureNoLanguage) {
        return i18n("A document that does not say what language it is in.");
    }
    if (ruleId == Rule::StructureImagesWithoutAlt) {
        return i18n("A picture with no text alternative.");
    }
    if (ruleId == Rule::StrokeHairline) {
        return i18n("A line thin enough to vanish in print.");
    }
    if (ruleId == Rule::TextTiny) {
        return i18n("Text set smaller than the profile allows.");
    }
    if (ruleId == Rule::ContentOutsideTrimBox) {
        return i18n("Ink outside the trim box, which is either bleed or a mistake.");
    }
    return {};
}

QStringList Preflight::knownFixes()
{
    return {
        Fix::Decrypt,          Fix::EmbedFonts, Fix::DownsampleImages, Fix::RemoveJavaScript, Fix::RemoveEmbeddedFiles,
        Fix::RemoveOpenAction, Fix::SetTrimBox, Fix::SetTitle,         Fix::Linearise,
    };
}

QString Preflight::describeFix(const QString &fixId)
{
    if (fixId == Fix::EmbedFonts) {
        return i18n("Rewrites the whole document through Ghostscript so that every font travels with it. This also "
                    "converts the colours and adds an output intent, so it changes more than the fonts.");
    }
    if (fixId == Fix::RemoveJavaScript) {
        return i18n("Removes every script, the action that runs when the document is opened, and the actions "
                    "attached to pages and annotations. Form fields that calculated anything will stop.");
    }
    if (fixId == Fix::RemoveEmbeddedFiles) {
        return i18n("Drops the files carried inside the document, including file-attachment annotations. They cannot "
                    "be recovered from the result.");
    }
    if (fixId == Fix::RemoveOpenAction) {
        return i18n("Removes what the document does when it is opened, so it simply shows its first page.");
    }
    if (fixId == Fix::SetTrimBox) {
        return i18n("Gives every page that lacks one a trim box equal to its crop box, which states the finished size "
                    "as being exactly what is visible.");
    }
    if (fixId == Fix::SetTitle) {
        return i18n("Sets the document's title from its file name.");
    }
    if (fixId == Fix::Linearise) {
        return i18n("Reorders the file so a reader can show the first page before the rest has arrived.");
    }
    if (fixId == Fix::Decrypt) {
        return i18n("Writes the document out without its encryption. Only possible when it opens without a password.");
    }
    if (fixId == Fix::DownsampleImages) {
        return i18n("Re-encodes the pictures at 300 dpi through Ghostscript. Detail beyond that is lost for good.");
    }
    return {};
}

Preflight::Report Preflight::run(const QString &pdf, const Profile &profile, QString *error)
{
    Report report;

    Scan scan;
    if (!scanDocument(pdf, &scan, error)) {
        return report;
    }

    Collector collector(profile);

    if (scan.passwordNeeded) {
        // The one thing that can be said about a file that will not open is the
        // reason it will not open. Everything else the profile asked for is
        // unchecked, and the report is explicit about that rather than quietly
        // returning a clean bill of health.
        collector.add(Rule::SecurityEncrypted,
                      i18n("The document needs a password to be opened, so nothing inside it could be examined."));
        for (const QString &rule : profile.rules.keys()) {
            if (rule != Rule::SecurityEncrypted) {
                report.notChecked.append(rule);
            }
        }
        report.notChecked.sort();
        collector.finish(&report);
        return report;
    }

    applyFontRules(scan, &collector);
    applyImageRules(scan, &collector);
    applyColourRules(scan, &collector);
    applyTransparencyRules(scan, &collector);
    applyPageRules(scan, &collector);
    applyInteractiveRules(scan, &collector);
    applySecurityRules(scan, &collector);
    applyMetadataRules(scan, &collector);
    applyStructureRules(scan, &collector);
    applyInkRules(scan, &collector);

    const QStringList missing = unimplementedRules();
    for (const QString &rule : missing) {
        if (profile.rules.contains(rule)) {
            report.notChecked.append(rule);
        }
    }
    if (scan.anyContentUnreadable) {
        // A content stream that will not parse takes every rule that depends on
        // where things land with it, and saying so is the only honest answer.
        for (const QLatin1String rule :
             { Rule::ImageLowResolution, Rule::ImageExcessiveResolution, Rule::ImageJpeg2000, Rule::ImageNoColourSpace,
               Rule::ColourRgbInPrint, Rule::ColourCmykInWeb, Rule::ColourSpot, Rule::ColourNoIcc, Rule::StrokeHairline,
               Rule::TextTiny, Rule::PageEmpty, Rule::FontNotEmbedded, Rule::FontSubsetWithoutToUnicode,
               Rule::FontType3, Rule::FontLicenceRestricted }) {
            if (profile.rules.contains(rule) && !report.notChecked.contains(rule)) {
                report.notChecked.append(rule);
            }
        }
    }
    report.notChecked.sort();

    collector.finish(&report);
    return report;
}

// ---------------------------------------------------------------------------
// Profiles on disc
// ---------------------------------------------------------------------------

namespace {

QString severityName(Preflight::Severity severity)
{
    switch (severity) {
    case Preflight::Severity::Note:
        return QStringLiteral("note");
    case Preflight::Severity::Warning:
        return QStringLiteral("warning");
    case Preflight::Severity::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("warning");
}

bool severityFromName(const QString &name, Preflight::Severity *severity)
{
    if (name == QLatin1String("note")) {
        *severity = Preflight::Severity::Note;
        return true;
    }
    if (name == QLatin1String("warning")) {
        *severity = Preflight::Severity::Warning;
        return true;
    }
    if (name == QLatin1String("error")) {
        *severity = Preflight::Severity::Error;
        return true;
    }
    return false;
}

} // namespace

bool Preflight::saveProfile(const Profile &profile, const QString &path, QString *error)
{
    if (profile.id.trimmed().isEmpty()) {
        if (error) {
            *error = i18n("A profile needs an identifier before it can be saved.");
        }
        return false;
    }

    QJsonObject rules;
    // Sorted, so that a saved profile is stable from one save to the next and
    // can be kept in version control without spurious differences.
    QStringList ruleIds = profile.rules.keys();
    ruleIds.sort();
    for (const QString &id : std::as_const(ruleIds)) {
        rules.insert(id, severityName(profile.rules.value(id)));
    }

    QJsonObject thresholds;
    QStringList thresholdIds = profile.thresholds.keys();
    thresholdIds.sort();
    for (const QString &id : std::as_const(thresholdIds)) {
        thresholds.insert(id, profile.thresholds.value(id));
    }

    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("pdf-smithy-preflight-profile"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("id"), profile.id);
    root.insert(QStringLiteral("name"), profile.name);
    root.insert(QStringLiteral("rules"), rules);
    root.insert(QStringLiteral("thresholds"), thresholds);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = i18n("Could not write “%1”.", path);
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

Preflight::Profile Preflight::loadProfile(const QString &path, QString *error)
{
    Profile profile;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = i18n("Could not read “%1”.", path);
        }
        return profile;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parse {};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = i18n("“%1” is not a preflight profile.", QFileInfo(path).fileName());
        }
        return profile;
    }

    const QJsonObject root = document.object();
    if (!root.contains(QStringLiteral("id")) || !root.value(QStringLiteral("rules")).isObject()) {
        if (error) {
            *error = i18n("“%1” is not a preflight profile.", QFileInfo(path).fileName());
        }
        return profile;
    }

    const QStringList known = knownRules();
    Profile loaded;
    loaded.id = root.value(QStringLiteral("id")).toString();
    loaded.name = root.value(QStringLiteral("name")).toString();

    const QJsonObject rules = root.value(QStringLiteral("rules")).toObject();
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        // A rule id that does not exist is a typo, and a typo that silently
        // switches a check off is exactly the failure this refuses to allow.
        if (!known.contains(it.key())) {
            if (error) {
                *error
                    = i18n("“%1” names a rule this version does not have: %2.", QFileInfo(path).fileName(), it.key());
            }
            return profile;
        }
        Severity severity = Severity::Warning;
        if (!severityFromName(it.value().toString(), &severity)) {
            if (error) {
                *error = i18n("“%1” gives the rule %2 a severity this version does not have: %3.",
                              QFileInfo(path).fileName(), it.key(), it.value().toString());
            }
            return profile;
        }
        loaded.rules.insert(it.key(), severity);
    }

    const QJsonObject thresholds = root.value(QStringLiteral("thresholds")).toObject();
    for (auto it = thresholds.constBegin(); it != thresholds.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            if (error) {
                *error
                    = i18n("“%1” names a rule this version does not have: %2.", QFileInfo(path).fileName(), it.key());
            }
            return profile;
        }
        if (!it.value().isDouble()) {
            if (error) {
                *error = i18n("The threshold for %1 in “%2” is not a number.", it.key(), QFileInfo(path).fileName());
            }
            return profile;
        }
        loaded.thresholds.insert(it.key(), it.value().toDouble());
    }

    return loaded;
}

// ---------------------------------------------------------------------------
// Fixes
// ---------------------------------------------------------------------------

namespace {

/**
 * A `[llx lly urx ury]` array written the one way that ignores the locale.
 *
 * Spelled out as four doubles rather than a QRectF because Qt's rectangle has
 * its y axis pointing the other way, and a box written upside down puts the trim
 * marks somewhere nobody intended.
 */
QPDFObjectHandle boxArray(double left, double bottom, double right, double top)
{
    return QPDFObjectHandle::parse("[" + number(std::min(left, right)) + " " + number(std::min(bottom, top)) + " "
                                   + number(std::max(left, right)) + " " + number(std::max(bottom, top)) + "]");
}

bool removeDocumentScripts(QPDF &pdf)
{
    bool changed = false;
    QPDFObjectHandle root = pdf.getRoot();

    if (root.hasKey("/AA")) {
        root.removeKey("/AA");
        changed = true;
    }
    QPDFObjectHandle openAction = root.getKey("/OpenAction");
    if (openAction.isDictionary() && (openAction.hasKey("/JS") || nameOf(openAction, "/S") == "/JavaScript")) {
        root.removeKey("/OpenAction");
        changed = true;
    }
    QPDFObjectHandle names = root.getKey("/Names");
    if (names.isDictionary() && names.hasKey("/JavaScript")) {
        names.removeKey("/JavaScript");
        changed = true;
    }

    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        QPDFObjectHandle handle = page.getObjectHandle();
        if (handle.hasKey("/AA")) {
            handle.removeKey("/AA");
            changed = true;
        }
        QPDFObjectHandle annotations = handle.getKey("/Annots");
        if (!annotations.isArray()) {
            continue;
        }
        for (int i = 0; i < annotations.getArrayNItems(); ++i) {
            QPDFObjectHandle annotation = annotations.getArrayItem(i);
            if (!annotation.isDictionary()) {
                continue;
            }
            if (annotation.hasKey("/AA")) {
                annotation.removeKey("/AA");
                changed = true;
            }
            QPDFObjectHandle action = annotation.getKey("/A");
            if (action.isDictionary() && action.hasKey("/JS")) {
                annotation.removeKey("/A");
                changed = true;
            }
        }
    }
    return changed;
}

bool removeEmbeddedFiles(QPDF &pdf)
{
    bool changed = false;
    QPDFObjectHandle names = pdf.getRoot().getKey("/Names");
    if (names.isDictionary() && names.hasKey("/EmbeddedFiles")) {
        names.removeKey("/EmbeddedFiles");
        changed = true;
    }
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
        if (!annotations.isArray()) {
            continue;
        }
        QPDFObjectHandle kept = QPDFObjectHandle::newArray();
        bool dropped = false;
        for (int i = 0; i < annotations.getArrayNItems(); ++i) {
            QPDFObjectHandle annotation = annotations.getArrayItem(i);
            if (annotation.isDictionary() && nameOf(annotation, "/Subtype") == "/FileAttachment") {
                dropped = true;
                continue;
            }
            kept.appendItem(annotation);
        }
        if (dropped) {
            page.getObjectHandle().replaceKey("/Annots", kept);
            changed = true;
        }
    }
    return changed;
}

bool setTrimBoxes(QPDF &pdf)
{
    bool changed = false;
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
        if (page.getObjectHandle().hasKey("/TrimBox")) {
            continue;
        }
        // mediaBoxOf() hands back a rectangle whose top() is the PDF's lower
        // left, so the corners are named for the file rather than for Qt.
        const QRectF media = PdfGeometry::mediaBoxOf(page);
        QPDFObjectHandle crop = page.getCropBox(false, false);
        const double left = boxValue(crop, 0, media.left());
        const double bottom = boxValue(crop, 1, media.top());
        const double right = boxValue(crop, 2, media.right());
        const double top = boxValue(crop, 3, media.bottom());
        page.getObjectHandle().replaceKey("/TrimBox", boxArray(left, bottom, right, top));
        changed = true;
    }
    return changed;
}

bool setTitleFromName(QPDF &pdf, const QString &sourcePath)
{
    QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
    const QString existing = stringValue(info, "/Title").trimmed();
    if (!existing.isEmpty()) {
        return false;
    }

    // Underscores in a file name almost always stand in for spaces, and a title
    // is read by people rather than by a file system.
    QString title = QFileInfo(sourcePath).completeBaseName().replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
    if (title.isEmpty()) {
        return false;
    }

    if (!info.isDictionary()) {
        info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        pdf.getTrailer().replaceKey("/Info", info);
    }
    info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title.toStdString()));
    // The XMP packet says the same thing in a second place; leaving an old one
    // behind would mean the two disagree, which is a rule of its own here.
    pdf.getRoot().removeKey("/Metadata");
    return true;
}

/** Runs Ghostscript through Archival, and says what that cost. */
bool embedFontsStage(const QString &in, const QString &out, QStringList *applied, QString *error)
{
    Archival::Options options;
    options.level = Archival::Level::PdfA2b;

    Archival::Report report;
    if (!Archival::convert(in, out, options, &report, error)) {
        return false;
    }
    applied->append(i18n("Every font was embedded by rewriting the document through Ghostscript, which also "
                         "converted its colours and added an output intent."));
    for (const QString &warning : report.warnings) {
        applied->append(warning);
    }
    return true;
}

bool downsampleStage(const QString &in, const QString &out, QStringList *applied, QString *error)
{
    Compressor::Options options;
    options.level = Compressor::Level::Balanced;
    // 300 dpi, because the rule that offers this fix is about pictures far above
    // what a press can use; going lower would trade one complaint for another.
    options.imageDpi = 300;

    Compressor::Report report;
    if (!Compressor::compress(in, out, options, &report, error)) {
        return false;
    }
    if (report.outcome == Compressor::Outcome::Shrunk) {
        applied->append(i18n("The pictures were re-encoded at 300 dpi, which made the file %1% smaller.",
                             QLocale().toString(report.savedPercent())));
    } else {
        applied->append(i18n("The pictures were already at or below 300 dpi, so nothing was re-encoded."));
    }
    return true;
}

} // namespace

bool Preflight::applyFixes(const QString &in, const QString &out, const QStringList &fixIds, QStringList *applied,
                           QString *error)
{
    QStringList local;

    const QStringList known = knownFixes();
    for (const QString &id : fixIds) {
        if (!known.contains(id)) {
            if (error) {
                *error = i18n("There is no fix called “%1”.", id);
            }
            return false;
        }
    }
    if (!QFileInfo::exists(in)) {
        if (error) {
            *error = i18n("“%1” does not exist.", in);
        }
        return false;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        if (error) {
            *error = i18n("Could not create a temporary directory.");
        }
        return false;
    }

    QString current = in;
    int stage = 0;
    const auto nextPath
        = [&workspace, &stage] { return workspace.filePath(QStringLiteral("stage-%1.pdf").arg(++stage)); };

    // Ghostscript rewrites the whole file, so it has to run before anything that
    // edits objects: the other way round, every careful edit would be thrown
    // away and replaced by whatever the interpreter decided to emit.
    if (fixIds.contains(Fix::EmbedFonts)) {
        const QString target = nextPath();
        if (!embedFontsStage(current, target, &local, error)) {
            return false;
        }
        current = target;
    }
    if (fixIds.contains(Fix::DownsampleImages)) {
        const QString target = nextPath();
        if (!downsampleStage(current, target, &local, error)) {
            return false;
        }
        current = target;
    }

    const bool wantsDecrypt = fixIds.contains(Fix::Decrypt);
    const bool wantsLinearise = fixIds.contains(Fix::Linearise);
    const bool wantsObjectEdits = fixIds.contains(Fix::RemoveJavaScript) || fixIds.contains(Fix::RemoveEmbeddedFiles)
        || fixIds.contains(Fix::RemoveOpenAction) || fixIds.contains(Fix::SetTrimBox) || fixIds.contains(Fix::SetTitle);

    // One QPDF pass for everything QPDF can do, because each extra pass would
    // renumber the objects again for no gain.
    QTemporaryFile temp(QFileInfo(out).absolutePath() + QLatin1String("/.pdf-smithy-preflight-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(out).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        PdfFile::open(pdf, current);

        if (wantsDecrypt && !pdf.isEncrypted()) {
            local.append(i18n("The document was not encrypted, so there was nothing to remove."));
        }
        if (wantsDecrypt && pdf.isEncrypted()) {
            local.append(i18n("The encryption was removed, so the document opens without a password."));
        }

        if (wantsObjectEdits) {
            if (fixIds.contains(Fix::RemoveJavaScript)) {
                local.append(removeDocumentScripts(pdf)
                                 ? i18n("The scripts and the actions that ran on their own were removed.")
                                 : i18n("There were no scripts to remove."));
            }
            if (fixIds.contains(Fix::RemoveEmbeddedFiles)) {
                local.append(removeEmbeddedFiles(pdf) ? i18n("The files carried inside the document were dropped.")
                                                      : i18n("There were no files carried inside the document."));
            }
            if (fixIds.contains(Fix::RemoveOpenAction)) {
                if (pdf.getRoot().hasKey("/OpenAction")) {
                    pdf.getRoot().removeKey("/OpenAction");
                    local.append(i18n("The document no longer does anything of its own accord when opened."));
                } else {
                    local.append(i18n("The document already did nothing of its own accord when opened."));
                }
            }
            if (fixIds.contains(Fix::SetTrimBox)) {
                local.append(setTrimBoxes(pdf) ? i18n("Every page that lacked a trim box was given one equal to its "
                                                      "crop box.")
                                               : i18n("Every page already had a trim box."));
            }
            if (fixIds.contains(Fix::SetTitle)) {
                local.append(setTitleFromName(pdf, in) ? i18n("The document's title was set from its file name.")
                                                       : i18n("The document already had a title."));
            }
        }

        QPDFWriter writer(pdf);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        if (wantsDecrypt) {
            writer.setPreserveEncryption(false);
        }
        if (wantsLinearise) {
            writer.setLinearization(true);
            local.append(i18n("The file was reordered so that a reader can show the first page before the rest has "
                              "arrived."));
        }
        writer.write();
    } catch (const QPDFExc &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = e.getErrorCode() == qpdf_e_password
                ? i18n("The document needs a password, so it cannot be opened to be changed.")
                : QString::fromUtf8(e.what());
        }
        return false;
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(out).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not replace “%1”.", out);
        }
        return false;
    }

    if (applied) {
        *applied = local;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The report as a document
// ---------------------------------------------------------------------------

namespace {

constexpr double ReportPageWidth = 595.276;
constexpr double ReportPageHeight = 841.89;
constexpr double ReportMargin = 56.0;

/** One laid-out line of the report. */
struct ReportLine {
    QString text;
    bool bold = false;
    double size = 10.0;
    double indent = 0.0;
    /** Extra room above, for a heading that needs air around it. */
    double spaceAbove = 0.0;
};

/**
 * A string in WinAnsiEncoding, which is what the report's fonts declare.
 *
 * Deliberately not QString::toLatin1(): the punctuation this program writes
 * (curly quotes, dashes, ellipses) sits where Latin-1 keeps control characters,
 * and that conversion turns every one of them into a question mark. Since every
 * sentence in every class here is typeset with proper quotation marks, getting
 * this wrong would disfigure the whole report.
 */
QByteArray toWinAnsi(const QString &text)
{
    // The twenty-seven characters WinAnsiEncoding puts in the range where
    // Latin-1 has controls. Everything else in the two encodings agrees.
    static const QHash<ushort, uchar> extras {
        { 0x20AC, 0x80 }, { 0x201A, 0x82 }, { 0x0192, 0x83 }, { 0x201E, 0x84 }, { 0x2026, 0x85 }, { 0x2020, 0x86 },
        { 0x2021, 0x87 }, { 0x02C6, 0x88 }, { 0x2030, 0x89 }, { 0x0160, 0x8A }, { 0x2039, 0x8B }, { 0x0152, 0x8C },
        { 0x017D, 0x8E }, { 0x2018, 0x91 }, { 0x2019, 0x92 }, { 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 },
        { 0x2013, 0x96 }, { 0x2014, 0x97 }, { 0x02DC, 0x98 }, { 0x2122, 0x99 }, { 0x0161, 0x9A }, { 0x203A, 0x9B },
        { 0x0153, 0x9C }, { 0x017E, 0x9E }, { 0x0178, 0x9F },
    };

    QByteArray out;
    out.reserve(text.size());
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if ((code >= 32 && code <= 126) || (code >= 0xA0 && code <= 0xFF)) {
            out.append(char(uchar(code)));
        } else if (const auto found = extras.constFind(code); found != extras.constEnd()) {
            out.append(char(*found));
        } else {
            // A question mark rather than nothing: a character quietly dropped
            // makes a sentence read as though it had been written that way.
            out.append('?');
        }
    }
    return out;
}

double helveticaWidth(const QString &text, double size, bool bold)
{
    const quint16 *widths = bold ? Core14::HelveticaBold : Core14::Helvetica;
    const int average = bold ? 536 : 514;

    double thousandths = 0.0;
    const QByteArray encoded = toWinAnsi(text);
    for (const char raw : encoded) {
        const int code = static_cast<unsigned char>(raw);
        thousandths
            += (code >= Core14::firstCode && code <= Core14::lastCode) ? widths[code - Core14::firstCode] : average;
    }
    return thousandths * size / 1000.0;
}

/** Breaks @p text at spaces so that no line is wider than @p available. */
QStringList wrapText(const QString &text, double available, double size, bool bold)
{
    QStringList lines;
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString current;
    for (const QString &word : words) {
        const QString candidate = current.isEmpty() ? word : current + QLatin1Char(' ') + word;
        if (!current.isEmpty() && helveticaWidth(candidate, size, bold) > available) {
            lines.append(current);
            current = word;
        } else {
            current = candidate;
        }
    }
    if (!current.isEmpty()) {
        lines.append(current);
    }
    return lines.isEmpty() ? QStringList { QString() } : lines;
}

std::string escapeForPdf(const QString &text)
{
    const QByteArray encoded = toWinAnsi(text);
    std::string out;
    out.reserve(size_t(encoded.size()) + 8);
    for (const char raw : encoded) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '(' || byte == ')' || byte == '\\') {
            out.push_back('\\');
            out.push_back(char(byte));
        } else if (byte >= 32 && byte != 127) {
            out.push_back(char(byte));
        }
    }
    return out;
}

QString severityLabel(Preflight::Severity severity)
{
    switch (severity) {
    case Preflight::Severity::Note:
        return i18nc("@item preflight severity", "Note");
    case Preflight::Severity::Warning:
        return i18nc("@item preflight severity", "Warning");
    case Preflight::Severity::Error:
        return i18nc("@item preflight severity", "Error");
    }
    return {};
}

QVector<ReportLine> reportLines(const QString &pdf, const Preflight::Report &report)
{
    QVector<ReportLine> lines;
    const auto heading = [&lines](const QString &text, double size, double above) {
        lines.append(ReportLine { text, true, size, 0.0, above });
    };
    const auto body = [&lines](const QString &text, double indent = 0.0) {
        lines.append(ReportLine { text, false, 10.0, indent, 0.0 });
    };

    heading(i18n("Preflight report"), 18.0, 0.0);
    body(QFileInfo(pdf).fileName());
    body(i18n("Checked on %1", QLocale().toString(QDate::currentDate(), QLocale::LongFormat)));

    lines.append(ReportLine { report.passed ? i18n("No errors were found among the rules that were checked.")
                                            : i18np("One error was found.", "%1 errors were found.", report.errors),
                              true, 11.0, 0.0, 14.0 });
    body(i18np("One warning.", "%1 warnings.", report.warnings));

    if (report.findings.isEmpty()) {
        heading(i18n("Findings"), 13.0, 18.0);
        body(i18n("Nothing to report."));
    } else {
        heading(i18n("Findings"), 13.0, 18.0);
        for (const Preflight::Finding &finding : report.findings) {
            const QString where = finding.page >= 0
                ? i18nc("@info a finding's location", "page %1", QLocale().toString(finding.page + 1))
                : i18nc("@info a finding's location", "whole document");
            QString head = i18nc("@info severity, rule and place of a preflight finding", "%1 · %2 · %3",
                                 severityLabel(finding.severity), finding.ruleId, where);
            if (!finding.object.isEmpty()) {
                head += i18nc("@info the resource a finding is about", " · %1", finding.object);
            }
            lines.append(ReportLine { head, true, 10.0, 0.0, 8.0 });
            body(finding.message, 12.0);
            if (!finding.fixDescription.isEmpty()) {
                body(i18n("Fix available (%1): %2", finding.fixId, finding.fixDescription), 12.0);
            }
        }
    }

    if (!report.notChecked.isEmpty()) {
        heading(i18n("Asked for but not checked"), 13.0, 18.0);
        body(i18n("The profile names these rules, and this program cannot judge them. Nothing above says anything "
                  "about them either way."));
        for (const QString &rule : report.notChecked) {
            body(i18nc("@info a rule id followed by what it looks for", "%1: %2", rule, Preflight::describeRule(rule)),
                 12.0);
        }
    }

    heading(i18n("What this check cannot promise"), 13.0, 18.0);
    for (const QString &note : Preflight::limitations()) {
        body(note, 12.0);
    }

    return lines;
}

} // namespace

bool Preflight::writeReport(const QString &pdf, const Report &report, const QString &outPdf, QString *error)
{
    const QVector<ReportLine> lines = reportLines(pdf, report);
    const double available = ReportPageWidth - 2 * ReportMargin;

    QTemporaryFile temp(QFileInfo(outPdf).absolutePath() + QLatin1String("/.pdf-smithy-preflight-report-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(outPdf).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper pages(out);

        QPDFObjectHandle regular = out.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
        QPDFObjectHandle bold = out.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"));

        std::string content;
        double y = ReportPageHeight - ReportMargin;

        const auto startPage = [&content, &y] {
            content = "BT\n";
            y = ReportPageHeight - ReportMargin;
        };
        const auto finishPage = [&content, &out, &pages, &regular, &bold] {
            content += "ET\n";

            QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
            fonts.replaceKey("/F1", regular);
            fonts.replaceKey("/F2", bold);
            QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
            resources.replaceKey("/Font", fonts);

            QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
            page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
            page.replaceKey(
                "/MediaBox",
                QPDFObjectHandle::parse("[0 0 " + number(ReportPageWidth) + " " + number(ReportPageHeight) + "]"));
            page.replaceKey("/Resources", resources);
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&out, content));
            pages.addPage(QPDFPageObjectHelper(out.makeIndirectObject(page)), false);
        };

        startPage();
        for (const ReportLine &line : lines) {
            y -= line.spaceAbove;
            const double leading = line.size * 1.35;
            const QStringList wrapped = wrapText(line.text, available - line.indent, line.size, line.bold);
            for (const QString &piece : wrapped) {
                if (y - leading < ReportMargin) {
                    finishPage();
                    startPage();
                }
                y -= leading;
                content += std::string(line.bold ? "/F2 " : "/F1 ") + number(line.size) + " Tf\n";
                content += "1 0 0 1 " + number(ReportMargin + line.indent) + " " + number(y) + " Tm\n";
                content += "(" + escapeForPdf(piece) + ") Tj\n";
            }
        }
        finishPage();

        QPDFObjectHandle info = out.makeIndirectObject(QPDFObjectHandle::newDictionary());
        info.replaceKey("/Title",
                        QPDFObjectHandle::newUnicodeString(
                            i18n("Preflight report for %1", QFileInfo(pdf).fileName()).toStdString()));
        out.getTrailer().replaceKey("/Info", info);

        QPDFWriter writer(out);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setCompressStreams(true);
        writer.setDeterministicID(true);
        writer.write();
    } catch (const std::exception &e) {
        QFile::remove(tempPath);
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(outPdf).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not replace “%1”.", outPdf);
        }
        return false;
    }
    return true;
}

QStringList Preflight::limitations()
{
    return {
        i18n("This is a preflight check, not a certification. A file that passes here can still be refused by a "
             "strict validator such as veraPDF, which is the tool to reach for when somebody has to sign the file "
             "off."),
        i18n("Ink outside the trim box is not measured. Working out whether a mark is deliberate bleed or a mistake "
             "needs the exact outline of every glyph and path, which this does not compute, so the rule is named in "
             "the profile and reported as unchecked rather than quietly passed."),
        i18n("Only what is drawn on the pages themselves is examined. A font or a picture used solely inside an "
             "annotation's appearance, a pattern or a soft mask is not seen."),
        i18n("A font's licence is read from the embedding permission field that OpenType and TrueType fonts carry. "
             "Type 1 and bare CFF fonts have no such field, so nothing can be said about them either way."),
        i18n("Resolution is judged from where a picture is actually placed, so a picture that is never drawn is not "
             "judged at all. Pictures written directly into the page's instructions rather than stored as objects "
             "are counted as ink but not measured."),
        i18n("Whether a page is empty is decided by looking for drawing instructions, so a page that paints white "
             "on white, or draws entirely outside its own edges, counts as having something on it."),
        i18n("Tagging is checked for its presence and for pictures without a text alternative. Whether the reading "
             "order makes sense, or a heading is really a heading, is a judgement no program makes well."),
    };
}

} // namespace ps
