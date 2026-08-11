/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "ColourTools.h"
#include "PdfFile.h"

#include "PdfGeometry.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTransform>

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#ifdef PS_WITH_LCMS
#include <lcms2.h>
#endif

#include <qpdf/Buffer.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

constexpr double HairlineLimit = 0.25;
constexpr int GhostscriptTimeoutMs = 15 * 60 * 1000;
constexpr int MaxFormDepth = 12;

/** How many pages Ghostscript rasterises before the results are read and thrown away. */
constexpr int InkCoverageBatch = 16;

/** Ink coverage is measured at this resolution unless a caller says otherwise. */
constexpr int InkCoverageDpi = 72;

// ══ colour arithmetic ═════════════════════════════════════════════════════
//
// Everything in this section is either defined by the PDF specification or
// defined by a standard, and is marked as such. Nothing here invents a
// conversion.

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

/** PDF 32000-1, 10.4: the weighting DeviceRGB to DeviceGray is defined with. */
double luminanceOf(const Rgb &colour)
{
    return clamp01(0.3 * colour.r + 0.59 * colour.g + 0.11 * colour.b);
}

/** PDF 32000-1, 10.4: DeviceCMYK to DeviceRGB, additive subtraction of the inks. */
Rgb rgbFromCmyk(double c, double m, double y, double k)
{
    return { clamp01(1.0 - std::min(1.0, c + k)), clamp01(1.0 - std::min(1.0, m + k)),
             clamp01(1.0 - std::min(1.0, y + k)) };
}

/**
 * CIELAB to sRGB, through XYZ and the Bradford-adapted D50 matrix.
 *
 * A /Lab space states its own white point, so the conversion honours it rather
 * than assuming D50, because a document that declares D65 would otherwise come out
 * with a cool cast that no one could account for.
 */
Rgb rgbFromLab(double lStar, double aStar, double bStar, const QVector<double> &whitePoint)
{
    const double xw = whitePoint.value(0, 0.9642);
    const double yw = whitePoint.value(1, 1.0);
    const double zw = whitePoint.value(2, 0.8249);

    const double m = (lStar + 16.0) / 116.0;
    const auto inverseF = [](double t) {
        return t >= 6.0 / 29.0 ? t * t * t : (108.0 / 841.0) * (t - 4.0 / 29.0);
    };
    const double x = xw * inverseF(m + aStar / 500.0);
    const double y = yw * inverseF(m);
    const double z = zw * inverseF(m - bStar / 200.0);

    const double linearR = 3.1338561 * x - 1.6168667 * y - 0.4906146 * z;
    const double linearG = -0.9787684 * x + 1.9161415 * y + 0.0334540 * z;
    const double linearB = 0.0719453 * x - 0.2289914 * y + 1.4052427 * z;

    const auto encode = [](double v) {
        v = clamp01(v);
        return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };
    return { encode(linearR), encode(linearG), encode(linearB) };
}

// ══ ICC, through LittleCMS ════════════════════════════════════════════════

/**
 * A colour-managed transform between sRGB and one CMYK profile.
 *
 * Deliberately the only place a conversion into CMYK can come from. Where this
 * class cannot be built there is no CMYK conversion to be had in process, and
 * the caller falls back to Ghostscript and says so, rather than reaching for
 * `C = 1 - R`, which is not a conversion but a rumour of one.
 */
class Managed
{
public:
    static std::unique_ptr<Managed> create(const QString &profilePath, QString *error);
    ~Managed();

    Managed(const Managed &) = delete;
    Managed &operator=(const Managed &) = delete;

    /** What the profile calls itself, for the report. */
    QString profileName() const { return m_name; }

    bool rgbToCmyk(const Rgb &in, double out[4]) const;
    bool cmykToRgb(const double in[4], Rgb *out) const;

    /** A whole scanline at a time, because a million single conversions is a stall. */
    void rgbLineToCmyk(const uchar *rgb, uchar *cmyk, int count) const;

private:
    Managed() = default;

    QString m_name;
#ifdef PS_WITH_LCMS
    cmsHPROFILE m_srgb = nullptr;
    cmsHPROFILE m_cmyk = nullptr;
    cmsHTRANSFORM m_toCmykDouble = nullptr;
    cmsHTRANSFORM m_toRgbDouble = nullptr;
    cmsHTRANSFORM m_toCmykBytes = nullptr;
#endif
};

#ifdef PS_WITH_LCMS

Managed::~Managed()
{
    if (m_toCmykDouble) {
        cmsDeleteTransform(m_toCmykDouble);
    }
    if (m_toRgbDouble) {
        cmsDeleteTransform(m_toRgbDouble);
    }
    if (m_toCmykBytes) {
        cmsDeleteTransform(m_toCmykBytes);
    }
    if (m_srgb) {
        cmsCloseProfile(m_srgb);
    }
    if (m_cmyk) {
        cmsCloseProfile(m_cmyk);
    }
}

std::unique_ptr<Managed> Managed::create(const QString &profilePath, QString *error)
{
    std::unique_ptr<Managed> managed(new Managed);
    managed->m_cmyk = cmsOpenProfileFromFile(QFile::encodeName(profilePath).constData(), "r");
    if (!managed->m_cmyk) {
        if (error) {
            *error = i18n("The ICC profile “%1” could not be opened.", profilePath);
        }
        return {};
    }
    if (cmsGetColorSpace(managed->m_cmyk) != cmsSigCmykData) {
        if (error) {
            *error = i18n("“%1” is not a CMYK ICC profile.", QFileInfo(profilePath).fileName());
        }
        return {};
    }

    managed->m_srgb = cmsCreate_sRGBProfile();
    if (!managed->m_srgb) {
        return {};
    }

    // Black point compensation, because without it the darkest tones of a
    // photograph all land on the same near-black and the shadows go flat.
    const cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION;
    managed->m_toCmykDouble = cmsCreateTransform(managed->m_srgb, TYPE_RGB_DBL, managed->m_cmyk, TYPE_CMYK_DBL,
                                                 INTENT_RELATIVE_COLORIMETRIC, flags);
    managed->m_toRgbDouble = cmsCreateTransform(managed->m_cmyk, TYPE_CMYK_DBL, managed->m_srgb, TYPE_RGB_DBL,
                                                INTENT_RELATIVE_COLORIMETRIC, flags);
    managed->m_toCmykBytes = cmsCreateTransform(managed->m_srgb, TYPE_RGB_8, managed->m_cmyk, TYPE_CMYK_8,
                                                INTENT_PERCEPTUAL, flags);
    if (!managed->m_toCmykDouble || !managed->m_toRgbDouble || !managed->m_toCmykBytes) {
        if (error) {
            *error = i18n("A colour transform could not be built from “%1”.", QFileInfo(profilePath).fileName());
        }
        return {};
    }

    char description[256] = { 0 };
    if (cmsGetProfileInfoASCII(managed->m_cmyk, cmsInfoDescription, "en", "US", description, sizeof(description) - 1)
        > 0) {
        managed->m_name = QString::fromLatin1(description).trimmed();
    }
    if (managed->m_name.isEmpty()) {
        managed->m_name = QFileInfo(profilePath).fileName();
    }
    return managed;
}

bool Managed::rgbToCmyk(const Rgb &in, double out[4]) const
{
    const double source[3] = { clamp01(in.r), clamp01(in.g), clamp01(in.b) };
    double percent[4] = { 0, 0, 0, 0 };
    cmsDoTransform(m_toCmykDouble, source, percent, 1);
    // LittleCMS states CMYK doubles as percentages; PDF wants unit fractions.
    for (int i = 0; i < 4; ++i) {
        out[i] = clamp01(percent[i] / 100.0);
    }
    return true;
}

bool Managed::cmykToRgb(const double in[4], Rgb *out) const
{
    const double percent[4] = { clamp01(in[0]) * 100.0, clamp01(in[1]) * 100.0, clamp01(in[2]) * 100.0,
                                clamp01(in[3]) * 100.0 };
    double rgb[3] = { 0, 0, 0 };
    cmsDoTransform(m_toRgbDouble, percent, rgb, 1);
    *out = { clamp01(rgb[0]), clamp01(rgb[1]), clamp01(rgb[2]) };
    return true;
}

void Managed::rgbLineToCmyk(const uchar *rgb, uchar *cmyk, int count) const
{
    cmsDoTransform(m_toCmykBytes, rgb, cmyk, cmsUInt32Number(count));
}

#else

Managed::~Managed() = default;

std::unique_ptr<Managed> Managed::create(const QString &, QString *error)
{
    if (error) {
        *error = i18n("This build has no colour management, so it cannot convert into CMYK itself.");
    }
    return {};
}

bool Managed::rgbToCmyk(const Rgb &, double[4]) const
{
    return false;
}

bool Managed::cmykToRgb(const double[4], Rgb *) const
{
    return false;
}

void Managed::rgbLineToCmyk(const uchar *, uchar *, int) const { }

#endif

// ══ PDF functions ═════════════════════════════════════════════════════════

/** One step of a Type 4 function's little stack machine. */
struct PsStep {
    enum class Kind { Number, Word, Block };
    Kind kind = Kind::Number;
    double number = 0.0;
    QString word;
    // std::vector tolerates an incomplete element type, which QList does not.
    std::vector<PsStep> block;
};

std::vector<PsStep> parseCalculator(const QByteArray &text, qsizetype &at)
{
    std::vector<PsStep> steps;
    while (at < text.size()) {
        const char ch = text.at(at);
        if (ch == '}') {
            ++at;
            return steps;
        }
        if (ch == '{') {
            ++at;
            PsStep step;
            step.kind = PsStep::Kind::Block;
            step.block = parseCalculator(text, at);
            steps.push_back(std::move(step));
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            ++at;
            continue;
        }
        if (ch == '%') {
            while (at < text.size() && text.at(at) != '\n') {
                ++at;
            }
            continue;
        }

        qsizetype end = at;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text.at(end))) == 0 && text.at(end) != '{'
               && text.at(end) != '}') {
            ++end;
        }
        const QByteArray word = text.mid(at, end - at);
        at = end;

        PsStep step;
        bool numeric = false;
        // QByteArray::toDouble is always C-locale, which is the only reason it
        // is safe to read a PDF number with.
        const double value = word.toDouble(&numeric);
        if (numeric) {
            step.kind = PsStep::Kind::Number;
            step.number = value;
        } else {
            step.kind = PsStep::Kind::Word;
            step.word = QString::fromLatin1(word);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

bool runCalculator(const std::vector<PsStep> &program, QVector<double> &stack, int depth);

/**
 * A PDF function, as far as a tint transform or a shading needs one.
 *
 * Types 2 and 3 are exact. Type 0 interpolates linearly for one input and takes
 * the nearest sample for more than one, which is where a multi-ink DeviceN
 * transform loses a little; Type 4 runs the document's own PostScript. All four
 * exist here because a spot colour's meaning is written in the file rather than
 * in a profile, and refusing to read it would turn an exact conversion into a
 * guess.
 */
class Function
{
public:
    static std::shared_ptr<Function> load(QPDFObjectHandle object, int depth = 0);

    /** Evaluates the function, clipping to /Domain and /Range as the spec requires. */
    bool evaluate(const QVector<double> &inputs, QVector<double> *outputs) const;

    int outputCount() const { return m_outputs; }

private:
    bool evaluateType0(const QVector<double> &inputs, QVector<double> *outputs) const;
    bool evaluateType2(double x, QVector<double> *outputs) const;
    bool evaluateType3(double x, QVector<double> *outputs) const;
    bool evaluateType4(const QVector<double> &inputs, QVector<double> *outputs) const;
    double sampleAt(qsizetype index) const;

    int m_type = -1;
    int m_inputs = 1;
    int m_outputs = 0;
    QVector<double> m_domain;
    QVector<double> m_range;

    QVector<double> m_c0;
    QVector<double> m_c1;
    double m_exponent = 1.0;

    QVector<std::shared_ptr<Function>> m_children;
    QVector<double> m_bounds;
    QVector<double> m_encode;

    QVector<int> m_size;
    int m_bitsPerSample = 8;
    QVector<double> m_decode;
    QByteArray m_samples;

    std::vector<PsStep> m_program;

    /** An array of single-output functions, which is legal wherever one is. */
    bool m_isArray = false;
};

QVector<double> numbersOf(QPDFObjectHandle array)
{
    QVector<double> values;
    if (!array.isArray()) {
        return values;
    }
    const int count = array.getArrayNItems();
    values.reserve(count);
    for (int i = 0; i < count; ++i) {
        values.append(PdfGeometry::numberAt(array, i, 0.0));
    }
    return values;
}

std::shared_ptr<Function> Function::load(QPDFObjectHandle object, int depth)
{
    if (depth > 8) {
        return {};
    }

    auto function = std::make_shared<Function>();

    if (object.isArray()) {
        function->m_isArray = true;
        for (int i = 0; i < object.getArrayNItems(); ++i) {
            auto child = load(object.getArrayItem(i), depth + 1);
            if (!child) {
                return {};
            }
            function->m_children.append(child);
        }
        function->m_outputs = function->m_children.size();
        return function->m_outputs > 0 ? function : nullptr;
    }

    QPDFObjectHandle dict = object.isStream() ? object.getDict() : object;
    if (!dict.isDictionary()) {
        return {};
    }
    QPDFObjectHandle type = dict.getKey("/FunctionType");
    if (!type.isInteger()) {
        return {};
    }
    function->m_type = type.getIntValueAsInt();
    function->m_domain = numbersOf(dict.getKey("/Domain"));
    function->m_range = numbersOf(dict.getKey("/Range"));
    function->m_inputs = std::max<qsizetype>(1, function->m_domain.size() / 2);
    function->m_outputs = int(function->m_range.size() / 2);

    switch (function->m_type) {
    case 0: {
        if (!object.isStream()) {
            return {};
        }
        QPDFObjectHandle size = dict.getKey("/Size");
        for (int i = 0; size.isArray() && i < size.getArrayNItems(); ++i) {
            QPDFObjectHandle item = size.getArrayItem(i);
            function->m_size.append(item.isInteger() ? item.getIntValueAsInt() : 0);
        }
        QPDFObjectHandle bits = dict.getKey("/BitsPerSample");
        function->m_bitsPerSample = bits.isInteger() ? bits.getIntValueAsInt() : 8;
        function->m_encode = numbersOf(dict.getKey("/Encode"));
        function->m_decode = numbersOf(dict.getKey("/Decode"));
        if (function->m_size.isEmpty() || function->m_outputs <= 0) {
            return {};
        }
        try {
            const std::shared_ptr<Buffer> buffer = object.getStreamData();
            function->m_samples = QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()),
                                            qsizetype(buffer->getSize()));
        } catch (const std::exception &) {
            return {};
        }
        return function;
    }
    case 2:
        function->m_c0 = numbersOf(dict.getKey("/C0"));
        function->m_c1 = numbersOf(dict.getKey("/C1"));
        if (function->m_c0.isEmpty()) {
            function->m_c0 = { 0.0 };
        }
        if (function->m_c1.isEmpty()) {
            function->m_c1 = { 1.0 };
        }
        function->m_exponent = PdfGeometry::numericValue(dict.getKey("/N"), 1.0);
        function->m_outputs = int(std::min(function->m_c0.size(), function->m_c1.size()));
        return function->m_outputs > 0 ? function : nullptr;
    case 3: {
        QPDFObjectHandle functions = dict.getKey("/Functions");
        for (int i = 0; functions.isArray() && i < functions.getArrayNItems(); ++i) {
            auto child = load(functions.getArrayItem(i), depth + 1);
            if (!child) {
                return {};
            }
            function->m_children.append(child);
        }
        function->m_bounds = numbersOf(dict.getKey("/Bounds"));
        function->m_encode = numbersOf(dict.getKey("/Encode"));
        if (function->m_children.isEmpty()) {
            return {};
        }
        if (function->m_outputs <= 0) {
            function->m_outputs = function->m_children.first()->outputCount();
        }
        return function;
    }
    case 4: {
        if (!object.isStream() || function->m_outputs <= 0) {
            return {};
        }
        try {
            const std::shared_ptr<Buffer> buffer = object.getStreamData();
            const QByteArray text(reinterpret_cast<const char *>(buffer->getBuffer()), qsizetype(buffer->getSize()));
            qsizetype at = text.indexOf('{');
            if (at < 0) {
                return {};
            }
            ++at;
            function->m_program = parseCalculator(text, at);
        } catch (const std::exception &) {
            return {};
        }
        return function;
    }
    default:
        return {};
    }
}

double Function::sampleAt(qsizetype index) const
{
    const double maximum = std::pow(2.0, m_bitsPerSample) - 1.0;
    switch (m_bitsPerSample) {
    case 8:
        return index < m_samples.size() ? double(static_cast<uchar>(m_samples.at(index))) / 255.0 : 0.0;
    case 16: {
        const qsizetype at = index * 2;
        if (at + 1 >= m_samples.size()) {
            return 0.0;
        }
        const int value = (static_cast<uchar>(m_samples.at(at)) << 8) | static_cast<uchar>(m_samples.at(at + 1));
        return double(value) / 65535.0;
    }
    default: {
        const qsizetype firstBit = index * m_bitsPerSample;
        int value = 0;
        for (int i = 0; i < m_bitsPerSample; ++i) {
            const qsizetype bit = firstBit + i;
            const qsizetype byte = bit / 8;
            if (byte >= m_samples.size()) {
                return 0.0;
            }
            const int shift = 7 - int(bit % 8);
            value = (value << 1) | ((static_cast<uchar>(m_samples.at(byte)) >> shift) & 1);
        }
        return maximum > 0.0 ? double(value) / maximum : 0.0;
    }
    }
}

bool Function::evaluateType0(const QVector<double> &inputs, QVector<double> *outputs) const
{
    const int dimensions = int(m_size.size());
    if (dimensions <= 0 || inputs.size() < dimensions) {
        return false;
    }

    QVector<double> positions;
    positions.reserve(dimensions);
    for (int i = 0; i < dimensions; ++i) {
        const double lower = m_domain.value(2 * i, 0.0);
        const double upper = m_domain.value(2 * i + 1, 1.0);
        const double encodeLow = m_encode.value(2 * i, 0.0);
        const double encodeHigh = m_encode.value(2 * i + 1, double(m_size.at(i) - 1));
        double x = std::clamp(inputs.at(i), std::min(lower, upper), std::max(lower, upper));
        const double span = upper - lower;
        x = qFuzzyIsNull(span) ? encodeLow : encodeLow + (x - lower) * (encodeHigh - encodeLow) / span;
        positions.append(std::clamp(x, 0.0, double(m_size.at(i) - 1)));
    }

    const auto decodeOutput = [this](int output, double raw) {
        const double low = m_decode.value(2 * output, m_range.value(2 * output, 0.0));
        const double high = m_decode.value(2 * output + 1, m_range.value(2 * output + 1, 1.0));
        return low + raw * (high - low);
    };

    outputs->clear();
    outputs->reserve(m_outputs);

    if (dimensions == 1) {
        const double x = positions.first();
        const auto lower = qsizetype(std::floor(x));
        const qsizetype upper = std::min<qsizetype>(lower + 1, m_size.first() - 1);
        const double t = x - double(lower);
        for (int output = 0; output < m_outputs; ++output) {
            const double a = sampleAt(lower * m_outputs + output);
            const double b = sampleAt(upper * m_outputs + output);
            outputs->append(decodeOutput(output, a + t * (b - a)));
        }
        return true;
    }

    // More than one input, so the nearest sample it is: multilinear
    // interpolation across n dimensions buys very little on the tint transforms
    // this is actually used for, and the limitation is stated rather than hidden.
    qsizetype index = 0;
    qsizetype stride = 1;
    for (int i = 0; i < dimensions; ++i) {
        const auto nearest = qsizetype(std::llround(positions.at(i)));
        index += std::clamp<qsizetype>(nearest, 0, m_size.at(i) - 1) * stride;
        stride *= m_size.at(i);
    }
    for (int output = 0; output < m_outputs; ++output) {
        outputs->append(decodeOutput(output, sampleAt(index * m_outputs + output)));
    }
    return true;
}

bool Function::evaluateType2(double x, QVector<double> *outputs) const
{
    outputs->clear();
    outputs->reserve(m_outputs);
    const double raised = qFuzzyCompare(m_exponent, 1.0) ? x : std::pow(std::max(0.0, x), m_exponent);
    for (int i = 0; i < m_outputs; ++i) {
        const double c0 = m_c0.value(i, 0.0);
        const double c1 = m_c1.value(i, 1.0);
        outputs->append(c0 + raised * (c1 - c0));
    }
    return true;
}

bool Function::evaluateType3(double x, QVector<double> *outputs) const
{
    const double lower = m_domain.value(0, 0.0);
    const double upper = m_domain.value(1, 1.0);
    x = std::clamp(x, std::min(lower, upper), std::max(lower, upper));

    qsizetype which = 0;
    while (which < m_bounds.size() && x >= m_bounds.at(which)) {
        ++which;
    }
    which = std::min(which, qsizetype(m_children.size() - 1));

    const double low = which == 0 ? lower : m_bounds.at(which - 1);
    const double high = which == m_bounds.size() ? upper : m_bounds.at(which);
    const double encodeLow = m_encode.value(2 * which, 0.0);
    const double encodeHigh = m_encode.value(2 * which + 1, 1.0);
    const double span = high - low;
    const double encoded = qFuzzyIsNull(span) ? encodeLow : encodeLow + (x - low) * (encodeHigh - encodeLow) / span;

    return m_children.at(which)->evaluate({ encoded }, outputs);
}

bool runCalculator(const std::vector<PsStep> &program, QVector<double> &stack, int depth)
{
    if (depth > 32) {
        return false;
    }

    std::vector<const std::vector<PsStep> *> procedures;
    const auto pop = [&stack](double *value) {
        if (stack.isEmpty()) {
            return false;
        }
        *value = stack.takeLast();
        return true;
    };

    for (const PsStep &step : program) {
        if (step.kind == PsStep::Kind::Number) {
            stack.append(step.number);
            continue;
        }
        if (step.kind == PsStep::Kind::Block) {
            procedures.push_back(&step.block);
            continue;
        }

        const QString &word = step.word;
        double a = 0.0;
        double b = 0.0;

        if (word == u"if"_s) {
            if (procedures.empty() || !pop(&a)) {
                return false;
            }
            const std::vector<PsStep> *body = procedures.back();
            procedures.pop_back();
            if (a != 0.0 && !runCalculator(*body, stack, depth + 1)) {
                return false;
            }
        } else if (word == u"ifelse"_s) {
            if (procedures.size() < 2 || !pop(&a)) {
                return false;
            }
            const std::vector<PsStep> *elseBody = procedures.back();
            procedures.pop_back();
            const std::vector<PsStep> *thenBody = procedures.back();
            procedures.pop_back();
            if (!runCalculator(a != 0.0 ? *thenBody : *elseBody, stack, depth + 1)) {
                return false;
            }
        } else if (word == u"add"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(a + b);
        } else if (word == u"sub"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(a - b);
        } else if (word == u"mul"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(a * b);
        } else if (word == u"div"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(qFuzzyIsNull(b) ? 0.0 : a / b);
        } else if (word == u"idiv"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            const int divisor = int(b);
            stack.append(divisor == 0 ? 0.0 : double(int(a) / divisor));
        } else if (word == u"mod"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            const int divisor = int(b);
            stack.append(divisor == 0 ? 0.0 : double(int(a) % divisor));
        } else if (word == u"neg"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(-a);
        } else if (word == u"abs"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::abs(a));
        } else if (word == u"sqrt"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::sqrt(std::max(0.0, a)));
        } else if (word == u"sin"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::sin(a * M_PI / 180.0));
        } else if (word == u"cos"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::cos(a * M_PI / 180.0));
        } else if (word == u"atan"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            double degrees = std::atan2(a, b) * 180.0 / M_PI;
            if (degrees < 0.0) {
                degrees += 360.0;
            }
            stack.append(degrees);
        } else if (word == u"exp"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(std::pow(a, b));
        } else if (word == u"ln"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(a > 0.0 ? std::log(a) : 0.0);
        } else if (word == u"log"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(a > 0.0 ? std::log10(a) : 0.0);
        } else if (word == u"cvi"_s || word == u"truncate"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::trunc(a));
        } else if (word == u"cvr"_s) {
            // Already a real; the operator exists to change the type, not the value.
        } else if (word == u"floor"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::floor(a));
        } else if (word == u"ceiling"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::ceil(a));
        } else if (word == u"round"_s) {
            if (!pop(&a)) {
                return false;
            }
            stack.append(std::round(a));
        } else if (word == u"dup"_s) {
            if (stack.isEmpty()) {
                return false;
            }
            stack.append(stack.constLast());
        } else if (word == u"pop"_s) {
            if (!pop(&a)) {
                return false;
            }
        } else if (word == u"exch"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(b);
            stack.append(a);
        } else if (word == u"copy"_s) {
            if (!pop(&a)) {
                return false;
            }
            const auto count = qsizetype(a);
            if (count < 0 || count > stack.size()) {
                return false;
            }
            const qsizetype from = stack.size() - count;
            for (qsizetype i = 0; i < count; ++i) {
                stack.append(stack.at(from + i));
            }
        } else if (word == u"index"_s) {
            if (!pop(&a)) {
                return false;
            }
            const auto back = qsizetype(a);
            if (back < 0 || back >= stack.size()) {
                return false;
            }
            stack.append(stack.at(stack.size() - 1 - back));
        } else if (word == u"roll"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            const auto count = qsizetype(a);
            if (count <= 0 || count > stack.size()) {
                return false;
            }
            auto shift = qsizetype(b) % count;
            if (shift < 0) {
                shift += count;
            }
            const qsizetype from = stack.size() - count;
            std::rotate(stack.begin() + from, stack.begin() + (stack.size() - shift), stack.end());
        } else if (word == u"eq"_s || word == u"ne"_s || word == u"gt"_s || word == u"ge"_s || word == u"lt"_s
                   || word == u"le"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            bool result = false;
            if (word == u"eq"_s) {
                result = qFuzzyCompare(a + 1.0, b + 1.0);
            } else if (word == u"ne"_s) {
                result = !qFuzzyCompare(a + 1.0, b + 1.0);
            } else if (word == u"gt"_s) {
                result = a > b;
            } else if (word == u"ge"_s) {
                result = a >= b;
            } else if (word == u"lt"_s) {
                result = a < b;
            } else {
                result = a <= b;
            }
            stack.append(result ? 1.0 : 0.0);
        } else if (word == u"and"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(double(int(a) & int(b)));
        } else if (word == u"or"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(double(int(a) | int(b)));
        } else if (word == u"xor"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            stack.append(double(int(a) ^ int(b)));
        } else if (word == u"not"_s) {
            if (!pop(&a)) {
                return false;
            }
            // Doubles as a logical and a bitwise operator, exactly as PostScript does.
            stack.append(a == 0.0 ? 1.0 : (a == 1.0 ? 0.0 : double(~int(a))));
        } else if (word == u"bitshift"_s) {
            if (!pop(&b) || !pop(&a)) {
                return false;
            }
            const int by = int(b);
            stack.append(by >= 0 ? double(int(a) << by) : double(int(a) >> -by));
        } else if (word == u"true"_s) {
            stack.append(1.0);
        } else if (word == u"false"_s) {
            stack.append(0.0);
        } else {
            return false;
        }
    }
    return true;
}

bool Function::evaluateType4(const QVector<double> &inputs, QVector<double> *outputs) const
{
    QVector<double> stack = inputs;
    if (!runCalculator(m_program, stack, 0) || stack.size() < qsizetype(m_outputs)) {
        return false;
    }
    *outputs = stack.mid(stack.size() - m_outputs);
    return true;
}

bool Function::evaluate(const QVector<double> &inputs, QVector<double> *outputs) const
{
    if (!outputs) {
        return false;
    }

    if (m_isArray) {
        outputs->clear();
        for (const auto &child : m_children) {
            QVector<double> one;
            if (!child->evaluate(inputs, &one) || one.isEmpty()) {
                return false;
            }
            outputs->append(one.first());
        }
        return true;
    }

    QVector<double> clipped = inputs;
    for (qsizetype i = 0; i < clipped.size() && 2 * i + 1 < m_domain.size(); ++i) {
        const double low = m_domain.at(2 * i);
        const double high = m_domain.at(2 * i + 1);
        clipped[i] = std::clamp(clipped.at(i), std::min(low, high), std::max(low, high));
    }

    bool done = false;
    switch (m_type) {
    case 0:
        done = evaluateType0(clipped, outputs);
        break;
    case 2:
        done = !clipped.isEmpty() && evaluateType2(clipped.first(), outputs);
        break;
    case 3:
        done = !clipped.isEmpty() && evaluateType3(clipped.first(), outputs);
        break;
    case 4:
        done = evaluateType4(clipped, outputs);
        break;
    default:
        return false;
    }
    if (!done) {
        return false;
    }

    for (qsizetype i = 0; i < outputs->size() && 2 * i + 1 < m_range.size(); ++i) {
        const double low = m_range.at(2 * i);
        const double high = m_range.at(2 * i + 1);
        (*outputs)[i] = std::clamp(outputs->at(i), std::min(low, high), std::max(low, high));
    }
    return true;
}

// ══ colour spaces ═════════════════════════════════════════════════════════

enum class Family { Unknown, Gray, Rgb, Cmyk, Lab, Indexed, Separation, DeviceN, Pattern };

struct SpaceInfo {
    Family family = Family::Unknown;
    int components = 0;

    /** The file states an ICC profile for this space, so it is colour-managed already. */
    bool viaIcc = false;

    QStringList inkNames;
    std::shared_ptr<SpaceInfo> alternate;
    std::shared_ptr<Function> tint;

    QVector<double> whitePoint;
    QVector<double> labRange;

    int hival = 0;
    QByteArray lookup;

    /** What inspect() should call it. */
    QString label() const
    {
        switch (family) {
        case Family::Gray:
            return viaIcc ? u"ICCBased (1 component)"_s : u"DeviceGray"_s;
        case Family::Rgb:
            return viaIcc ? u"ICCBased (3 components)"_s : u"DeviceRGB"_s;
        case Family::Cmyk:
            return viaIcc ? u"ICCBased (4 components)"_s : u"DeviceCMYK"_s;
        case Family::Lab:
            return u"Lab"_s;
        case Family::Indexed:
            return u"Indexed"_s;
        case Family::Separation:
            return u"Separation"_s;
        case Family::DeviceN:
            return u"DeviceN"_s;
        case Family::Pattern:
            return u"Pattern"_s;
        case Family::Unknown:
            break;
        }
        return u"unrecognised"_s;
    }
};

using Space = std::shared_ptr<SpaceInfo>;

Space makeDevice(Family family, int components)
{
    auto space = std::make_shared<SpaceInfo>();
    space->family = family;
    space->components = components;
    return space;
}

const Space &deviceGray()
{
    static const Space space = makeDevice(Family::Gray, 1);
    return space;
}

const Space &deviceRgb()
{
    static const Space space = makeDevice(Family::Rgb, 3);
    return space;
}

const Space &deviceCmyk()
{
    static const Space space = makeDevice(Family::Cmyk, 4);
    return space;
}

/** The name of a dictionary key, without the slash, or an empty string. */
QString nameValue(QPDFObjectHandle object)
{
    if (!object.isName()) {
        return {};
    }
    QString name = QString::fromStdString(object.getName());
    if (name.startsWith(u'/')) {
        name.remove(0, 1);
    }
    return name;
}

Space resolveSpace(QPDFObjectHandle object, QPDFObjectHandle resources, int depth = 0);

/** Resolves the abbreviated and full names a colour space can appear under. */
Space resolveSpaceName(const std::string &name, QPDFObjectHandle resources, int depth)
{
    if (name == "/DeviceGray" || name == "/G" || name == "/CalGray") {
        return deviceGray();
    }
    if (name == "/DeviceRGB" || name == "/RGB" || name == "/CalRGB") {
        return deviceRgb();
    }
    if (name == "/DeviceCMYK" || name == "/CMYK") {
        return deviceCmyk();
    }
    if (name == "/Pattern") {
        return makeDevice(Family::Pattern, 1);
    }

    // Anything else is a name in the resource dictionary, which is how every
    // separation and every ICC space actually reaches a content stream.
    QPDFObjectHandle table
        = resources.isDictionary() ? resources.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
    if (table.isDictionary() && table.hasKey(name)) {
        return resolveSpace(table.getKey(name), resources, depth + 1);
    }
    return {};
}

Space resolveSpace(QPDFObjectHandle object, QPDFObjectHandle resources, int depth)
{
    if (depth > 8) {
        return {};
    }
    if (object.isName()) {
        return resolveSpaceName(object.getName(), resources, depth);
    }
    if (!object.isArray() || object.getArrayNItems() == 0) {
        return {};
    }

    QPDFObjectHandle head = object.getArrayItem(0);
    if (!head.isName()) {
        return {};
    }
    const std::string family = head.getName();
    const int items = object.getArrayNItems();

    if (family == "/CalGray") {
        return deviceGray();
    }
    if (family == "/CalRGB") {
        return deviceRgb();
    }
    if (family == "/DeviceGray" || family == "/DeviceRGB" || family == "/DeviceCMYK" || family == "/G"
        || family == "/RGB" || family == "/CMYK") {
        return resolveSpaceName(family, resources, depth);
    }

    if (family == "/Lab" && items >= 2) {
        auto space = makeDevice(Family::Lab, 3);
        QPDFObjectHandle dict = object.getArrayItem(1);
        if (dict.isDictionary()) {
            space->whitePoint = numbersOf(dict.getKey("/WhitePoint"));
            space->labRange = numbersOf(dict.getKey("/Range"));
        }
        return space;
    }

    if (family == "/ICCBased" && items >= 2) {
        QPDFObjectHandle stream = object.getArrayItem(1);
        QPDFObjectHandle count = stream.isStream() ? stream.getDict().getKey("/N") : QPDFObjectHandle::newNull();
        const int components = count.isInteger() ? count.getIntValueAsInt() : 0;
        Space space;
        if (components == 1) {
            space = makeDevice(Family::Gray, 1);
        } else if (components == 4) {
            space = makeDevice(Family::Cmyk, 4);
        } else if (components == 3) {
            space = makeDevice(Family::Rgb, 3);
        } else if (stream.isStream()) {
            // No /N, which is illegal; the /Alternate is the only thing left.
            space = resolveSpace(stream.getDict().getKey("/Alternate"), resources, depth + 1);
        }
        if (space) {
            space = std::make_shared<SpaceInfo>(*space);
            space->viaIcc = true;
        }
        return space;
    }

    if ((family == "/Indexed" || family == "/I") && items >= 4) {
        auto space = makeDevice(Family::Indexed, 1);
        space->alternate = resolveSpace(object.getArrayItem(1), resources, depth + 1);
        QPDFObjectHandle hival = object.getArrayItem(2);
        space->hival = hival.isInteger() ? hival.getIntValueAsInt() : 0;
        QPDFObjectHandle lookup = object.getArrayItem(3);
        if (lookup.isString()) {
            const std::string bytes = lookup.getStringValue();
            space->lookup = QByteArray(bytes.data(), qsizetype(bytes.size()));
        } else if (lookup.isStream()) {
            try {
                const std::shared_ptr<Buffer> buffer = lookup.getStreamData();
                space->lookup = QByteArray(reinterpret_cast<const char *>(buffer->getBuffer()),
                                           qsizetype(buffer->getSize()));
            } catch (const std::exception &) {
                return {};
            }
        }
        return space->alternate ? space : nullptr;
    }

    if (family == "/Separation" && items >= 3) {
        auto space = makeDevice(Family::Separation, 1);
        space->inkNames << nameValue(object.getArrayItem(1));
        space->alternate = resolveSpace(object.getArrayItem(2), resources, depth + 1);
        if (items >= 4) {
            space->tint = Function::load(object.getArrayItem(3));
        }
        return space;
    }

    if (family == "/DeviceN" && items >= 3) {
        auto space = makeDevice(Family::DeviceN, 1);
        QPDFObjectHandle names = object.getArrayItem(1);
        for (int i = 0; names.isArray() && i < names.getArrayNItems(); ++i) {
            space->inkNames << nameValue(names.getArrayItem(i));
        }
        space->components = std::max<qsizetype>(1, space->inkNames.size());
        space->alternate = resolveSpace(object.getArrayItem(2), resources, depth + 1);
        if (items >= 4) {
            space->tint = Function::load(object.getArrayItem(3));
        }
        return space;
    }

    if (family == "/Pattern") {
        auto space = makeDevice(Family::Pattern, 1);
        if (items >= 2) {
            space->alternate = resolveSpace(object.getArrayItem(1), resources, depth + 1);
        }
        return space;
    }

    return {};
}

/** The colour a space starts at, per PDF 32000-1 8.6.8. */
QVector<double> initialColourOf(const Space &space)
{
    if (!space) {
        return { 0.0 };
    }
    switch (space->family) {
    case Family::Cmyk:
        return { 0.0, 0.0, 0.0, 1.0 };
    case Family::Separation:
    case Family::DeviceN:
        return QVector<double>(space->components, 1.0);
    case Family::Lab:
        return { 0.0, 0.0, 0.0 };
    default:
        return QVector<double>(std::max(1, space->components), 0.0);
    }
}

bool toRgb(const Space &space, const QVector<double> &values, Rgb *out, int depth = 0);

/** One component of an /Indexed palette entry, decoded from its byte. */
double paletteComponent(const Space &base, int component, uchar byte)
{
    const double unit = double(byte) / 255.0;
    if (base && base->family == Family::Lab) {
        if (component == 0) {
            return unit * 100.0;
        }
        const double low = base->labRange.value(2 * (component - 1), -100.0);
        const double high = base->labRange.value(2 * (component - 1) + 1, 100.0);
        return low + unit * (high - low);
    }
    return unit;
}

bool toRgb(const Space &space, const QVector<double> &values, Rgb *out, int depth)
{
    if (!space || !out || depth > 8) {
        return false;
    }

    switch (space->family) {
    case Family::Gray: {
        const double v = clamp01(values.value(0, 0.0));
        *out = { v, v, v };
        return true;
    }
    case Family::Rgb:
        *out = { clamp01(values.value(0, 0.0)), clamp01(values.value(1, 0.0)), clamp01(values.value(2, 0.0)) };
        return true;
    case Family::Cmyk:
        *out = rgbFromCmyk(values.value(0, 0.0), values.value(1, 0.0), values.value(2, 0.0), values.value(3, 0.0));
        return true;
    case Family::Lab:
        *out = rgbFromLab(values.value(0, 0.0), values.value(1, 0.0), values.value(2, 0.0), space->whitePoint);
        return true;
    case Family::Indexed: {
        const Space &base = space->alternate;
        if (!base) {
            return false;
        }
        const int components = std::max(1, base->components);
        const auto index = qsizetype(std::llround(std::clamp(values.value(0, 0.0), 0.0, double(space->hival))));
        const qsizetype at = index * components;
        if (at + components > space->lookup.size()) {
            return false;
        }
        QVector<double> baseValues;
        baseValues.reserve(components);
        for (int i = 0; i < components; ++i) {
            baseValues.append(paletteComponent(base, i, static_cast<uchar>(space->lookup.at(at + i))));
        }
        return toRgb(base, baseValues, out, depth + 1);
    }
    case Family::Separation:
    case Family::DeviceN: {
        if (!space->tint || !space->alternate) {
            return false;
        }
        QVector<double> alternateValues;
        if (!space->tint->evaluate(values, &alternateValues)) {
            return false;
        }
        return toRgb(space->alternate, alternateValues, out, depth + 1);
    }
    case Family::Pattern:
    case Family::Unknown:
        break;
    }
    return false;
}

bool isPrintersBlack(const QVector<double> &components, int family)
{
    switch (static_cast<Family>(family)) {
    case Family::Gray:
        return qFuzzyIsNull(components.value(0, 1.0));
    case Family::Rgb:
        return qFuzzyIsNull(components.value(0, 1.0)) && qFuzzyIsNull(components.value(1, 1.0))
            && qFuzzyIsNull(components.value(2, 1.0));
    case Family::Cmyk:
        // Only pure black counts. A rich black is four inks by intention, and
        // overprinting it is the mistake this feature is supposed to prevent.
        return qFuzzyIsNull(components.value(0, 1.0)) && qFuzzyIsNull(components.value(1, 1.0))
            && qFuzzyIsNull(components.value(2, 1.0)) && components.value(3, 0.0) > 0.5;
    default:
        return false;
    }
}

// ══ image plumbing ════════════════════════════════════════════════════════

QStringList filtersOf(QPDFObjectHandle dict)
{
    QStringList names;
    QPDFObjectHandle filter = dict.isDictionary() ? dict.getKey("/Filter") : QPDFObjectHandle::newNull();
    if (filter.isName()) {
        names << QString::fromStdString(filter.getName());
    } else if (filter.isArray()) {
        for (int i = 0; i < filter.getArrayNItems(); ++i) {
            QPDFObjectHandle item = filter.getArrayItem(i);
            if (item.isName()) {
                names << QString::fromStdString(item.getName());
            }
        }
    }
    return names;
}

/** How many components a JPEG declares, read from its frame header, or 0. */
int jpegComponents(const QByteArray &jpeg)
{
    for (qsizetype i = 2; i + 9 < jpeg.size();) {
        if (static_cast<uchar>(jpeg.at(i)) != 0xFF) {
            ++i;
            continue;
        }
        const uchar marker = static_cast<uchar>(jpeg.at(i + 1));
        if (marker == 0xFF) {
            ++i;
            continue;
        }
        const bool isFrame = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7)
            || (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
        if (isFrame) {
            return static_cast<uchar>(jpeg.at(i + 9));
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const int length = (static_cast<uchar>(jpeg.at(i + 2)) << 8) | static_cast<uchar>(jpeg.at(i + 3));
        i += 2 + std::max(2, length);
    }
    return 0;
}

/** Reads one sample of @p bits width out of a packed row. */
int sampleFromRow(const uchar *row, qsizetype available, qsizetype index, int bits)
{
    switch (bits) {
    case 8:
        return index < available ? row[index] : 0;
    case 16: {
        const qsizetype at = index * 2;
        return at + 1 < available ? ((row[at] << 8) | row[at + 1]) : 0;
    }
    default: {
        const qsizetype firstBit = index * bits;
        int value = 0;
        for (int i = 0; i < bits; ++i) {
            const qsizetype bit = firstBit + i;
            const qsizetype byte = bit / 8;
            const int shift = 7 - int(bit % 8);
            const int one = byte < available ? ((row[byte] >> shift) & 1) : 0;
            value = (value << 1) | one;
        }
        return value;
    }
    }
}

/**
 * Decodes an image's pixels into RGB, or fails honestly.
 *
 * Fax and JPEG 2000 are not readable here and neither is a four-component
 * JPEG, because Qt's decoder does not handle Adobe's inverted CMYK convention
 * and a wrong guess would turn a photograph inside out.
 */
bool decodeImage(QPDFObjectHandle image, const Space &space, QImage *out, bool *wasJpeg)
{
    *wasJpeg = false;
    QPDFObjectHandle dict = image.getDict();
    QPDFObjectHandle widthKey = dict.getKey("/Width");
    QPDFObjectHandle heightKey = dict.getKey("/Height");
    const int width = widthKey.isInteger() ? widthKey.getIntValueAsInt() : 0;
    const int height = heightKey.isInteger() ? heightKey.getIntValueAsInt() : 0;
    if (width <= 0 || height <= 0 || qint64(width) * height > 200'000'000LL) {
        return false;
    }

    const QStringList filters = filtersOf(dict);
    if (filters.contains(u"/JPXDecode"_s) || filters.contains(u"/CCITTFaxDecode"_s)
        || filters.contains(u"/JBIG2Decode"_s)) {
        return false;
    }

    if (filters.contains(u"/DCTDecode"_s)) {
        if (space && space->components == 4) {
            return false;
        }
        // The six-argument overload: the shorter one reports whether filtering
        // was attempted rather than whether it worked, and a JPEG deliberately
        // left alone would look like a failure.
        Pl_Buffer buffer("jpeg");
        bool attempted = false;
        if (!image.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_specialized, true, false)) {
            return false;
        }
        const auto data = buffer.getBufferSharedPointer();
        const QImage decoded = QImage::fromData(
            QByteArray(reinterpret_cast<const char *>(data->getBuffer()), qsizetype(data->getSize())));
        if (decoded.isNull()) {
            return false;
        }
        *wasJpeg = true;
        *out = decoded.convertToFormat(QImage::Format_RGB32);
        return !out->isNull();
    }

    if (!space) {
        return false;
    }
    QPDFObjectHandle bitsKey = dict.getKey("/BitsPerComponent");
    const int bits = bitsKey.isInteger() ? bitsKey.getIntValueAsInt() : 8;
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) {
        return false;
    }
    const int components = std::max(1, space->components);

    Pl_Buffer buffer("samples");
    bool attempted = false;
    if (!image.pipeStreamData(&buffer, &attempted, 0, qpdf_dl_all, true, false)) {
        return false;
    }
    if (!attempted && !filters.isEmpty()) {
        // Something is still wrapped around the samples, so what came out is
        // not pixels.
        return false;
    }
    const auto data = buffer.getBufferSharedPointer();
    const auto *bytes = reinterpret_cast<const uchar *>(data->getBuffer());
    const qsizetype size = qsizetype(data->getSize());
    const qsizetype stride = (qsizetype(width) * components * bits + 7) / 8;
    if (size < stride * height) {
        return false;
    }

    const QVector<double> decode = numbersOf(dict.getKey("/Decode"));
    const double maximum = std::pow(2.0, bits) - 1.0;

    QImage canvas(width, height, QImage::Format_RGB32);
    if (canvas.isNull()) {
        return false;
    }

    QVector<double> values(components, 0.0);
    for (int y = 0; y < height; ++y) {
        const uchar *row = bytes + stride * y;
        auto *line = reinterpret_cast<QRgb *>(canvas.scanLine(y));
        for (int x = 0; x < width; ++x) {
            for (int component = 0; component < components; ++component) {
                const int raw = sampleFromRow(row, stride, qsizetype(x) * components + component, bits);
                double unit = maximum > 0.0 ? double(raw) / maximum : 0.0;
                if (2 * component + 1 < decode.size()) {
                    const double low = decode.at(2 * component);
                    const double high = decode.at(2 * component + 1);
                    unit = low + unit * (high - low);
                }
                // An indexed image's sample is the index itself, not a fraction.
                values[component] = space->family == Family::Indexed ? double(raw) : unit;
            }
            Rgb colour;
            if (!toRgb(space, values, &colour)) {
                return false;
            }
            line[x] = qRgb(int(std::lround(colour.r * 255.0)), int(std::lround(colour.g * 255.0)),
                           int(std::lround(colour.b * 255.0)));
        }
    }

    *out = canvas;
    return true;
}

/** Whether an image needed converting, and whether that was possible. */
enum class ImageOutcome { NotNeeded, Planned, Refused };

/** What is to be written back over an image, once it has been converted. */
struct ImagePlan {
    bool paletteOnly = false;
    QPDFObjectHandle colourSpace;

    std::string data;
    QPDFObjectHandle filter;
    QPDFObjectHandle decodeParms;
    std::string space;
    int bits = 8;
    int width = 0;
    int height = 0;
};

} // namespace

// ══ the traversal ═════════════════════════════════════════════════════════

namespace {

enum class Job { Inspect, Convert, Replace, Hairline, Overprint, Spot };

/** Everything the jobs share, so the walk over a document is written once. */
struct Work {
    QPDF *pdf = nullptr;
    Job job = Job::Inspect;

    ColourTools::Target target = ColourTools::Target::Grayscale;
    double threshold = 0.5;
    bool recompressPhotographs = true;
    int jpegQuality = 92;
    const Managed *managed = nullptr;

    Rgb from;
    Rgb to;
    double tolerance = 0.0;
    bool replacementIsGrey = false;

    double minimumWidth = HairlineLimit;

    bool overprintOn = true;

    QStringList toProcess;

    int operatorsChanged = 0;
    int imagesConverted = 0;
    int palettesConverted = 0;
    int imagesRefused = 0;
    int inlineImages = 0;
    int hairlinesFixed = 0;
    int hairlinesFound = 0;
    int strokesSeen = 0;
    double thinnestStroke = std::numeric_limits<double>::infinity();
    int replaced = 0;
    int overprintInserted = 0;
    int spotsRewritten = 0;

    QSet<QString> spaces;
    QSet<QString> spots;
    bool sawIcc = false;
    bool sawTransparency = false;
    bool sawOverprint = false;
    bool pageHasRgb = false;
    bool pageHasCmyk = false;

    std::set<QPDFObjGen> sharedWithUntouched;
    std::set<QPDFObjGen> formsDone;
    std::set<QPDFObjGen> imagesDone;
    std::set<QPDFObjGen> imagesFailed;
    std::map<QPDFObjGen, QPDFObjectHandle> imageCopies;

    QStringList notes;

    void note(const QString &text)
    {
        if (!notes.contains(text)) {
            notes << text;
        }
    }
};

/** The graphics state, as far as colour and line width are concerned. */
struct GraphicsState {
    QTransform ctm;
    double lineWidth = 1.0;

    Space fillSpace;
    Space strokeSpace;
    QVector<double> fillColour { 0.0 };
    QVector<double> strokeColour { 0.0 };

    /** The name that selected the space, so it can be re-issued when needed. */
    std::string fillSpaceName;
    std::string strokeSpaceName;
    bool fillSpaceStale = false;
    bool strokeSpaceStale = false;

    /** -1 not set, 0 off, 1 on. Tracked so a run of black does not emit a state per glyph. */
    int overprintWritten = -1;
};

class ColourFilter : public QPDFObjectHandle::TokenFilter
{
public:
    ColourFilter(Work &work, QPDFObjectHandle resources, int depth)
        : m_work(work)
        , m_resources(std::move(resources))
        , m_depth(depth)
    {
        m_state.fillSpace = deviceGray();
        m_state.strokeSpace = deviceGray();
    }

    void handleToken(QPDFTokenizer::Token const &token) override;

    /** True when the output differs from the input, which is what licenses a rewrite. */
    bool changed() const { return m_changed; }

    /**
     * Colour spaces this page selected and no longer does.
     *
     * A separation dissolved into process colour leaves nothing behind in the
     * content stream, but the resource entry naming the ink survives, and an
     * ink named in the resources is a plate as far as a print shop is
     * concerned, whether or not anything paints with it. Only names that were
     * dropped everywhere they appeared are listed: one place where the tint
     * transform could not be evaluated is enough to need the space kept.
     */
    QVector<std::string> retiredSpaces() const
    {
        QVector<std::string> names;
        for (const std::string &name : m_spacesDropped) {
            if (!m_spacesKept.contains(name)) {
                names.append(name);
            }
        }
        return names;
    }

private:
    void handleOperator(const QString &op);
    void handleColour(const QString &op);
    void handleSetSpace(bool stroking);
    void handleWidth();
    void handleStroke(const QString &op);
    void handleExtGState();
    void handleDo();
    void handleInlineImage();
    void writeOperands(const QString &op);
    void writeThrough(const QString &op);

    double operandNumber(int index) const
    {
        if (index < 0 || index >= m_operands.size()) {
            return 0.0;
        }
        // QString::toDouble never consults the locale, which is the only reason
        // reading a PDF number this way is safe.
        return QString::fromStdString(m_operands.at(index).getValue()).toDouble();
    }

    QVector<double> numericOperands() const
    {
        QVector<double> values;
        for (const QPDFTokenizer::Token &token : m_operands) {
            const auto type = token.getType();
            if (type == QPDFTokenizer::tt_integer || type == QPDFTokenizer::tt_real) {
                values.append(QString::fromStdString(token.getValue()).toDouble());
            }
        }
        return values;
    }

    QTransform operandMatrix() const
    {
        return QTransform(operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3), operandNumber(4),
                          operandNumber(5));
    }

    /** How much the current matrix magnifies a stroke, for hairline judgements. */
    double scaleOfCtm() const
    {
        const double determinant = std::abs(m_state.ctm.m11() * m_state.ctm.m22() - m_state.ctm.m12() * m_state.ctm.m21());
        const double scale = std::sqrt(determinant);
        return scale > 1e-9 ? scale : 1.0;
    }

    void recordSpace(const Space &space);
    bool emitTarget(const Rgb &colour, const Space &source, const QVector<double> &values, bool stroking);
    QString freshName(QPDFObjectHandle dictionary, const QString &stem) const;

    Work &m_work;
    QPDFObjectHandle m_resources;
    int m_depth = 0;

    QVector<QPDFTokenizer::Token> m_operands;
    GraphicsState m_state;
    QVector<GraphicsState> m_stack;
    QHash<QString, Space> m_spaceCache;
    bool m_changed = false;
    QSet<std::string> m_spacesDropped;
    QSet<std::string> m_spacesKept;
};

ImageOutcome convertImageInto(Work &work, QPDFObjectHandle image, const Space &space, ImagePlan *plan);
bool applyImagePlan(QPDFObjectHandle image, const ImagePlan &plan);
QPDFObjectHandle cloneStream(QPDF &pdf, QPDFObjectHandle original);

void ColourFilter::writeOperands(const QString &op)
{
    for (const QPDFTokenizer::Token &token : std::as_const(m_operands)) {
        writeToken(token);
        write(" ");
    }
    write(op.toStdString());
    write("\n");
}

void ColourFilter::writeThrough(const QString &op)
{
    // Passing an unconverted colour operator through means the space this filter
    // last wrote is not the one the file selected, so it has to be re-selected
    // or the operands would be read against the wrong space.
    if (op == u"sc"_s || op == u"scn"_s) {
        if (m_state.fillSpaceStale && !m_state.fillSpaceName.empty()) {
            write(m_state.fillSpaceName + " cs\n");
            m_state.fillSpaceStale = false;
            m_spacesKept.insert(m_state.fillSpaceName);
        }
    } else if (op == u"SC"_s || op == u"SCN"_s) {
        if (m_state.strokeSpaceStale && !m_state.strokeSpaceName.empty()) {
            write(m_state.strokeSpaceName + " CS\n");
            m_state.strokeSpaceStale = false;
            m_spacesKept.insert(m_state.strokeSpaceName);
        }
    }
    writeOperands(op);
}

void ColourFilter::handleToken(QPDFTokenizer::Token const &token)
{
    switch (token.getType()) {
    case QPDFTokenizer::tt_space:
    case QPDFTokenizer::tt_comment:
        if (m_operands.isEmpty()) {
            writeToken(token);
        }
        return;

    case QPDFTokenizer::tt_word:
        handleOperator(QString::fromStdString(token.getValue()));
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_inline_image:
        handleInlineImage();
        writeToken(token);
        m_operands.clear();
        return;

    case QPDFTokenizer::tt_eof:
        return;

    default:
        m_operands.append(token);
        return;
    }
}

void ColourFilter::recordSpace(const Space &space)
{
    if (!space) {
        return;
    }
    m_work.spaces.insert(space->label());
    if (space->viaIcc) {
        m_work.sawIcc = true;
    }
    for (const QString &ink : space->inkNames) {
        if (ink != u"All"_s && ink != u"None"_s) {
            m_work.spots.insert(ink);
        }
    }
    if (space->alternate) {
        recordSpace(space->alternate);
    }
}

QString ColourFilter::freshName(QPDFObjectHandle dictionary, const QString &stem) const
{
    int suffix = 0;
    QString candidate;
    do {
        candidate = u"/%1%2"_s.arg(stem).arg(suffix++);
    } while (dictionary.isDictionary() && dictionary.hasKey(candidate.toStdString()));
    return candidate;
}

void ColourFilter::handleSetSpace(bool stroking)
{
    if (m_operands.isEmpty() || m_operands.constLast().getType() != QPDFTokenizer::tt_name) {
        writeOperands(stroking ? u"CS"_s : u"cs"_s);
        return;
    }
    const std::string name = m_operands.constLast().getValue();
    const QString key = QString::fromStdString(name);

    Space space;
    const auto cached = m_spaceCache.constFind(key);
    if (cached != m_spaceCache.constEnd()) {
        space = *cached;
    } else {
        space = resolveSpaceName(name, m_resources, 0);
        m_spaceCache.insert(key, space);
    }
    if (space) {
        recordSpace(space);
    } else {
        m_work.note(i18n("A colour space named in a page could not be resolved, so colour set through it "
                         "was left as it is."));
    }

    // A separation on its way to process colour must stop being named here, or
    // the plate outlives the ink: every `scn` through it becomes a `k`, nothing
    // references the space any more, and yet the page still selects it, so it
    // stays in the resources and the file still lists a Pantone that no longer
    // prints anything. Marking it stale rather than dropping it outright keeps
    // the existing safety net: if some colour through this space turns out not
    // to be convertible after all, writeThrough() selects the space again
    // before passing the operator on.
    bool dissolving = false;
    if (m_work.job == Job::Spot && space
        && (space->family == Family::Separation || space->family == Family::DeviceN)) {
        for (const QString &ink : space->inkNames) {
            dissolving = dissolving || m_work.toProcess.contains(ink);
        }
        dissolving = dissolving && space->tint && space->alternate;
    }

    if (stroking) {
        m_state.strokeSpace = space;
        m_state.strokeColour = initialColourOf(space);
        m_state.strokeSpaceName = name;
        m_state.strokeSpaceStale = dissolving;
    } else {
        m_state.fillSpace = space;
        m_state.fillColour = initialColourOf(space);
        m_state.fillSpaceName = name;
        m_state.fillSpaceStale = dissolving;
    }

    if (dissolving) {
        m_spacesDropped.insert(name);
        m_changed = true;
        return;
    }
    writeOperands(stroking ? u"CS"_s : u"cs"_s);
}

bool ColourFilter::emitTarget(const Rgb &colour, const Space &source, const QVector<double> &values, bool stroking)
{
    std::string text;
    switch (m_work.target) {
    case ColourTools::Target::Grayscale: {
        const double grey = luminanceOf(colour);
        text = PdfGeometry::number(grey) + (stroking ? " G" : " g");
        break;
    }
    case ColourTools::Target::BlackWhite: {
        const double grey = luminanceOf(colour) > m_work.threshold ? 1.0 : 0.0;
        text = PdfGeometry::number(grey) + (stroking ? " G" : " g");
        break;
    }
    case ColourTools::Target::Rgb: {
        Rgb result = colour;
        if (source && source->family == Family::Cmyk && m_work.managed) {
            const double cmyk[4] = { values.value(0, 0.0), values.value(1, 0.0), values.value(2, 0.0),
                                     values.value(3, 0.0) };
            m_work.managed->cmykToRgb(cmyk, &result);
        }
        text = PdfGeometry::number(result.r) + " " + PdfGeometry::number(result.g) + " "
            + PdfGeometry::number(result.b) + (stroking ? " RG" : " rg");
        break;
    }
    case ColourTools::Target::Cmyk: {
        double cmyk[4] = { 0.0, 0.0, 0.0, 0.0 };
        if (source && source->family == Family::Gray) {
            // A grey is one ink on press. Sending it through an RGB profile
            // would build it from all four, which is exactly what a printer
            // rings up to complain about.
            cmyk[3] = clamp01(1.0 - values.value(0, 0.0));
        } else if (m_work.managed) {
            m_work.managed->rgbToCmyk(colour, cmyk);
        } else {
            return false;
        }
        text = PdfGeometry::number(cmyk[0]) + " " + PdfGeometry::number(cmyk[1]) + " " + PdfGeometry::number(cmyk[2])
            + " " + PdfGeometry::number(cmyk[3]) + (stroking ? " K" : " k");
        break;
    }
    }

    write(text + "\n");
    ++m_work.operatorsChanged;
    m_changed = true;
    if (stroking) {
        m_state.strokeSpaceStale = true;
    } else {
        m_state.fillSpaceStale = true;
    }
    return true;
}

void ColourFilter::handleColour(const QString &op)
{
    const bool stroking = op == u"G"_s || op == u"RG"_s || op == u"K"_s || op == u"SC"_s || op == u"SCN"_s;

    Space space;
    QVector<double> values;

    if (op == u"g"_s || op == u"G"_s) {
        space = deviceGray();
        values = { operandNumber(0) };
    } else if (op == u"rg"_s || op == u"RG"_s) {
        space = deviceRgb();
        values = { operandNumber(0), operandNumber(1), operandNumber(2) };
    } else if (op == u"k"_s || op == u"K"_s) {
        space = deviceCmyk();
        values = { operandNumber(0), operandNumber(1), operandNumber(2), operandNumber(3) };
    } else {
        space = stroking ? m_state.strokeSpace : m_state.fillSpace;
        values = numericOperands();
    }

    if (stroking) {
        m_state.strokeSpace = space;
        m_state.strokeColour = values;
    } else {
        m_state.fillSpace = space;
        m_state.fillColour = values;
    }
    recordSpace(space);

    const bool isPattern = space && space->family == Family::Pattern;
    if (space && !isPattern) {
        if (space->family == Family::Rgb || (space->alternate && space->alternate->family == Family::Rgb)) {
            m_work.pageHasRgb = true;
        }
        if (space->family == Family::Cmyk || (space->alternate && space->alternate->family == Family::Cmyk)) {
            m_work.pageHasCmyk = true;
        }
    }

    if (m_work.job == Job::Inspect) {
        return;
    }

    Rgb colour;
    const bool readable = !isPattern && toRgb(space, values, &colour);
    if (!readable && !isPattern && space) {
        m_work.note(i18n("A colour could not be read back through its own colour space, most likely a "
                         "separation without a usable tint transform, and was left alone."));
    }

    switch (m_work.job) {
    case Job::Convert: {
        if (!readable) {
            writeThrough(op);
            return;
        }
        const bool alreadyThere = (m_work.target == ColourTools::Target::Grayscale && space->family == Family::Gray)
            || (m_work.target == ColourTools::Target::Rgb && space->family == Family::Rgb)
            || (m_work.target == ColourTools::Target::Cmyk && space->family == Family::Cmyk)
            || (m_work.target == ColourTools::Target::BlackWhite && space->family == Family::Gray
                && (qFuzzyIsNull(values.value(0, 0.0)) || qFuzzyCompare(values.value(0, 0.0), 1.0)));
        if (alreadyThere) {
            writeThrough(op);
            return;
        }
        if (!emitTarget(colour, space, values, stroking)) {
            writeThrough(op);
        }
        return;
    }

    case Job::Replace: {
        if (!readable) {
            writeThrough(op);
            return;
        }
        const double distance = std::sqrt(std::pow(colour.r - m_work.from.r, 2.0)
                                          + std::pow(colour.g - m_work.from.g, 2.0)
                                          + std::pow(colour.b - m_work.from.b, 2.0));
        if (distance > m_work.tolerance + 1e-9) {
            writeThrough(op);
            return;
        }
        std::string text;
        if (m_work.replacementIsGrey && space->family == Family::Gray) {
            text = PdfGeometry::number(m_work.to.r) + (stroking ? " G" : " g");
        } else {
            text = PdfGeometry::number(m_work.to.r) + " " + PdfGeometry::number(m_work.to.g) + " "
                + PdfGeometry::number(m_work.to.b) + (stroking ? " RG" : " rg");
        }
        write(text + "\n");
        ++m_work.replaced;
        m_changed = true;
        if (stroking) {
            m_state.strokeSpaceStale = true;
        } else {
            m_state.fillSpaceStale = true;
        }
        return;
    }

    case Job::Spot: {
        const bool separated = space && (space->family == Family::Separation || space->family == Family::DeviceN);
        bool wanted = false;
        if (separated) {
            for (const QString &ink : space->inkNames) {
                wanted = wanted || m_work.toProcess.contains(ink);
            }
        }
        if (!separated || !wanted || !space->tint || !space->alternate) {
            if (separated && wanted) {
                m_work.note(i18n("A separation has no tint transform this can evaluate, so it was left as "
                                 "a spot colour."));
            }
            writeThrough(op);
            return;
        }
        QVector<double> alternateValues;
        if (!space->tint->evaluate(values, &alternateValues)) {
            writeThrough(op);
            return;
        }
        const Space &alternate = space->alternate;
        std::string text;
        if (alternate->family == Family::Cmyk) {
            text = PdfGeometry::number(alternateValues.value(0, 0.0)) + " "
                + PdfGeometry::number(alternateValues.value(1, 0.0)) + " "
                + PdfGeometry::number(alternateValues.value(2, 0.0)) + " "
                + PdfGeometry::number(alternateValues.value(3, 0.0)) + (stroking ? " K" : " k");
        } else if (alternate->family == Family::Gray) {
            text = PdfGeometry::number(alternateValues.value(0, 0.0)) + (stroking ? " G" : " g");
        } else {
            Rgb result;
            if (!toRgb(alternate, alternateValues, &result)) {
                writeThrough(op);
                return;
            }
            text = PdfGeometry::number(result.r) + " " + PdfGeometry::number(result.g) + " "
                + PdfGeometry::number(result.b) + (stroking ? " RG" : " rg");
        }
        write(text + "\n");
        ++m_work.spotsRewritten;
        m_changed = true;
        if (stroking) {
            m_state.strokeSpaceStale = true;
        } else {
            m_state.fillSpaceStale = true;
        }
        return;
    }

    case Job::Overprint: {
        writeThrough(op);
        if (stroking) {
            return;
        }
        const bool black = space && isPrintersBlack(values, int(space->family));

        int wanted = m_state.overprintWritten;
        if (black) {
            wanted = m_work.overprintOn ? 1 : 0;
        } else if (m_work.overprintOn && m_state.overprintWritten == 1) {
            // Overprinting stays in force until something turns it off, so a
            // colour set after a black inherits it. Leaving it alone would mean
            // every colour following a black overprints too, which is the exact
            // opposite of what was asked for, and it would show up not as a
            // wrong number but as a red that vanishes into what is under it.
            // Only what this filter switched on is switched off again.
            wanted = 0;
        }
        if (wanted == m_state.overprintWritten) {
            return;
        }

        write(wanted == 1 ? "/PsOverprintOn gs\n" : "/PsOverprintOff gs\n");
        m_state.overprintWritten = wanted;
        if (black) {
            // The blacks are the change that was asked for; switching back off
            // afterwards is bookkeeping and would double the count.
            ++m_work.overprintInserted;
        }
        m_changed = true;
        return;
    }

    case Job::Hairline:
    case Job::Inspect:
        writeThrough(op);
        return;
    }
}

void ColourFilter::handleWidth()
{
    const double stated = operandNumber(0);
    m_state.lineWidth = stated;

    if (m_work.job != Job::Hairline) {
        writeOperands(u"w"_s);
        return;
    }

    const double scale = scaleOfCtm();
    if (stated * scale >= m_work.minimumWidth - 1e-9) {
        writeOperands(u"w"_s);
        return;
    }
    const double replacement = m_work.minimumWidth / scale;
    write(PdfGeometry::number(replacement) + " w\n");
    m_state.lineWidth = replacement;
    ++m_work.hairlinesFixed;
    m_changed = true;
}

void ColourFilter::handleStroke(const QString &op)
{
    const double scale = scaleOfCtm();
    const double onPaper = m_state.lineWidth * scale;

    if (m_work.job == Job::Inspect) {
        ++m_work.strokesSeen;
        m_work.thinnestStroke = qMin(m_work.thinnestStroke, onPaper);
        if (onPaper < HairlineLimit) {
            ++m_work.hairlinesFound;
        }
        writeOperands(op);
        return;
    }

    if (m_work.job == Job::Hairline && onPaper < m_work.minimumWidth - 1e-9) {
        // The width was set before the matrix that shrank it, which is common
        // enough in generated content that fixing only the `w` operator would
        // miss most real hairlines.
        const double replacement = m_work.minimumWidth / scale;
        write(PdfGeometry::number(replacement) + " w\n");
        m_state.lineWidth = replacement;
        ++m_work.hairlinesFixed;
        m_changed = true;
    }
    writeOperands(op);
}

void ColourFilter::handleExtGState()
{
    if (m_operands.isEmpty() || m_operands.constLast().getType() != QPDFTokenizer::tt_name) {
        writeOperands(u"gs"_s);
        return;
    }
    QPDFObjectHandle table
        = m_resources.isDictionary() ? m_resources.getKey("/ExtGState") : QPDFObjectHandle::newNull();
    QPDFObjectHandle state
        = table.isDictionary() ? table.getKey(m_operands.constLast().getValue()) : QPDFObjectHandle::newNull();

    if (state.isDictionary()) {
        QPDFObjectHandle width = state.getKey("/LW");
        if (width.isNumber()) {
            m_state.lineWidth = PdfGeometry::numericValue(width, m_state.lineWidth);
        }
        QPDFObjectHandle fillOverprint = state.getKey("/op");
        QPDFObjectHandle strokeOverprint = state.getKey("/OP");
        if ((fillOverprint.isBool() && fillOverprint.getBoolValue())
            || (strokeOverprint.isBool() && strokeOverprint.getBoolValue())) {
            m_work.sawOverprint = true;
        }
        if (m_work.job == Job::Overprint && fillOverprint.isBool()) {
            // The file set overprint itself, so what this filter wrote earlier
            // no longer holds and the next black run must state it again.
            m_state.overprintWritten = -1;
        }
        QPDFObjectHandle softMask = state.getKey("/SMask");
        if (softMask.isDictionary() || (softMask.isName() && softMask.getName() != "/None")) {
            m_work.sawTransparency = true;
        }
        QPDFObjectHandle blend = state.getKey("/BM");
        const QString blendName = blend.isName() ? nameValue(blend) : QString();
        if (!blendName.isEmpty() && blendName != u"Normal"_s && blendName != u"Compatible"_s) {
            m_work.sawTransparency = true;
        }
        for (const char *key : { "/CA", "/ca" }) {
            QPDFObjectHandle alpha = state.getKey(key);
            if (alpha.isNumber() && PdfGeometry::numericValue(alpha, 1.0) < 0.999) {
                m_work.sawTransparency = true;
            }
        }
    }
    writeOperands(u"gs"_s);
}

void ColourFilter::handleInlineImage()
{
    ++m_work.inlineImages;
    if (m_work.job == Job::Convert) {
        m_work.note(i18n("Inline images are carried across in their original colour; they are rare and "
                         "small, and rewriting one means rebuilding the operator around it."));
    }
}

void ColourFilter::handleDo()
{
    if (m_operands.isEmpty() || m_operands.constLast().getType() != QPDFTokenizer::tt_name) {
        writeOperands(u"Do"_s);
        return;
    }

    QPDFObjectHandle table = m_resources.isDictionary() ? m_resources.getKey("/XObject") : QPDFObjectHandle::newNull();
    QPDFObjectHandle xobject
        = table.isDictionary() ? table.getKey(m_operands.constLast().getValue()) : QPDFObjectHandle::newNull();
    if (!xobject.isStream()) {
        writeOperands(u"Do"_s);
        return;
    }

    QPDFObjectHandle dict = xobject.getDict();
    QPDFObjectHandle subtype = dict.getKey("/Subtype");
    const QString kind = subtype.isName() ? nameValue(subtype) : QString();

    if (kind == u"Image"_s) {
        QPDFObjectHandle maskFlag = dict.getKey("/ImageMask");
        const bool stencil = maskFlag.isBool() && maskFlag.getBoolValue();
        Space space = stencil ? Space() : resolveSpace(dict.getKey("/ColorSpace"), m_resources, 0);

        if (m_work.job == Job::Inspect) {
            if (space) {
                recordSpace(space);
                const Family family = space->family == Family::Indexed && space->alternate ? space->alternate->family
                                                                                           : space->family;
                if (family == Family::Rgb) {
                    m_work.pageHasRgb = true;
                } else if (family == Family::Cmyk) {
                    m_work.pageHasCmyk = true;
                }
            }
            if (dict.hasKey("/SMask")) {
                m_work.sawTransparency = true;
            }
            writeOperands(u"Do"_s);
            return;
        }

        if (m_work.job != Job::Convert || stencil) {
            writeOperands(u"Do"_s);
            return;
        }

        const QPDFObjGen generation = xobject.getObjGen();
        if (m_work.imagesFailed.count(generation) > 0 || m_work.imagesDone.count(generation) > 0) {
            writeOperands(u"Do"_s);
            return;
        }

        const auto existing = m_work.imageCopies.find(generation);
        if (existing != m_work.imageCopies.end()) {
            QPDFObjectHandle own = m_resources.getKey("/XObject");
            const QString name = freshName(own, u"PsColour"_s);
            own.replaceKey(name.toStdString(), existing->second);
            write(name.toStdString() + " Do\n");
            m_changed = true;
            return;
        }

        ImagePlan plan;
        const ImageOutcome outcome = convertImageInto(m_work, xobject, space, &plan);
        if (outcome == ImageOutcome::NotNeeded) {
            m_work.imagesDone.insert(generation);
            writeOperands(u"Do"_s);
            return;
        }
        if (outcome == ImageOutcome::Refused) {
            m_work.imagesFailed.insert(generation);
            ++m_work.imagesRefused;
            writeOperands(u"Do"_s);
            return;
        }

        const bool shared = m_work.sharedWithUntouched.count(generation) > 0;
        if (!shared) {
            if (!applyImagePlan(xobject, plan)) {
                m_work.imagesFailed.insert(generation);
                ++m_work.imagesRefused;
                writeOperands(u"Do"_s);
                return;
            }
            m_work.imagesDone.insert(generation);
            if (plan.paletteOnly) {
                ++m_work.palettesConverted;
            } else {
                ++m_work.imagesConverted;
            }
            // The stream changed but the operator did not, so the content stream
            // stays byte for byte what it was.
            writeOperands(u"Do"_s);
            return;
        }

        QPDFObjectHandle copy = cloneStream(*m_work.pdf, xobject);
        if (!copy.isStream() || !applyImagePlan(copy, plan)) {
            m_work.imagesFailed.insert(generation);
            ++m_work.imagesRefused;
            writeOperands(u"Do"_s);
            return;
        }
        m_work.imageCopies.emplace(generation, copy);
        QPDFObjectHandle own = m_resources.getKey("/XObject");
        const QString name = freshName(own, u"PsColour"_s);
        own.replaceKey(name.toStdString(), copy);
        if (plan.paletteOnly) {
            ++m_work.palettesConverted;
        } else {
            ++m_work.imagesConverted;
        }
        write(name.toStdString() + " Do\n");
        m_changed = true;
        return;
    }

    if (kind != u"Form"_s || m_depth >= MaxFormDepth) {
        writeOperands(u"Do"_s);
        return;
    }

    QPDFObjectHandle formResources = dict.getKey("/Resources");
    if (!formResources.isDictionary()) {
        formResources = m_resources;
    }
    const QPDFObjGen generation = xobject.getObjGen();
    const bool shared = m_work.sharedWithUntouched.count(generation) > 0;

    if (m_work.job == Job::Inspect) {
        // Not memoised: a drawing placed on ten pages contributes to all ten,
        // and the per-page counts would be wrong if it were counted once.
        ColourFilter inner(m_work, formResources, m_depth + 1);
        xobject.filterAsContents(&inner, nullptr);
        writeOperands(u"Do"_s);
        return;
    }

    if (!shared) {
        if (m_work.formsDone.count(generation) == 0) {
            m_work.formsDone.insert(generation);
            Pl_Buffer buffer("form");
            ColourFilter inner(m_work, formResources, m_depth + 1);
            xobject.filterAsContents(&inner, &buffer);
            if (inner.changed()) {
                const auto data = buffer.getBufferSharedPointer();
                xobject.replaceStreamData(
                    std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize()),
                    QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
            }
        }
        writeOperands(u"Do"_s);
        return;
    }

    // Shared with a page that was not selected, so it gets a converted twin and
    // the untouched page keeps the original.
    QPDFObjectHandle ownResources = formResources.shallowCopy();
    QPDFObjectHandle nestedXObjects = ownResources.isDictionary() ? ownResources.getKey("/XObject")
                                                                 : QPDFObjectHandle::newNull();
    if (nestedXObjects.isDictionary()) {
        ownResources.replaceKey("/XObject", nestedXObjects.shallowCopy());
    }

    Pl_Buffer buffer("form");
    ColourFilter inner(m_work, ownResources, m_depth + 1);
    xobject.filterAsContents(&inner, &buffer);
    if (!inner.changed()) {
        writeOperands(u"Do"_s);
        return;
    }

    const auto data = buffer.getBufferSharedPointer();
    QPDFObjectHandle copy = QPDFObjectHandle::newStream(
        m_work.pdf, std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize()));
    QPDFObjectHandle copyDict = copy.getDict();
    for (const auto &[key, value] : dict.getDictAsMap()) {
        if (key == "/Length" || key == "/Filter" || key == "/DecodeParms") {
            continue;
        }
        copyDict.replaceKey(key, value);
    }
    copyDict.replaceKey("/Resources", ownResources);

    QPDFObjectHandle own = m_resources.getKey("/XObject");
    const QString name = freshName(own, u"PsColourForm"_s);
    own.replaceKey(name.toStdString(), copy);
    write(name.toStdString() + " Do\n");
    m_changed = true;
}

void ColourFilter::handleOperator(const QString &op)
{
    if (op == u"q"_s) {
        m_stack.append(m_state);
        writeOperands(op);
        return;
    }
    if (op == u"Q"_s) {
        if (!m_stack.isEmpty()) {
            m_state = m_stack.takeLast();
        }
        writeOperands(op);
        return;
    }
    if (op == u"cm"_s && m_operands.size() >= 6) {
        m_state.ctm = operandMatrix() * m_state.ctm;
        writeOperands(op);
        return;
    }
    if (op == u"w"_s && !m_operands.isEmpty()) {
        handleWidth();
        return;
    }
    if (op == u"gs"_s) {
        handleExtGState();
        return;
    }
    if (op == u"cs"_s || op == u"CS"_s) {
        handleSetSpace(op == u"CS"_s);
        return;
    }
    if (op == u"g"_s || op == u"G"_s || op == u"rg"_s || op == u"RG"_s || op == u"k"_s || op == u"K"_s
        || op == u"sc"_s || op == u"SC"_s || op == u"scn"_s || op == u"SCN"_s) {
        handleColour(op);
        return;
    }
    if (op == u"S"_s || op == u"s"_s || op == u"B"_s || op == u"B*"_s || op == u"b"_s || op == u"b*"_s) {
        handleStroke(op);
        return;
    }
    if (op == u"Do"_s) {
        handleDo();
        return;
    }
    if (op == u"sh"_s) {
        m_work.note(i18n("A shading pattern paints from its own colour function, which is carried across "
                         "unchanged. Its colours are not converted."));
        writeOperands(op);
        return;
    }
    writeOperands(op);
}

// ══ images, in detail ═════════════════════════════════════════════════════

/** Packs a threshold of @p image into one bit per pixel, the way PDF wants it. */
std::string packOneBit(const QImage &grey, double threshold)
{
    const int width = grey.width();
    const int height = grey.height();
    const qsizetype stride = (qsizetype(width) + 7) / 8;
    const auto cut = uchar(std::clamp(std::lround(threshold * 255.0), 0L, 255L));

    std::string packed(size_t(stride * height), '\0');
    for (int y = 0; y < height; ++y) {
        const uchar *line = grey.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            if (line[x] > cut) {
                // One is white in /DeviceGray, and the high bit is the leftmost pixel.
                packed[size_t(stride * y + x / 8)] |= char(0x80 >> (x % 8));
            }
        }
    }
    return packed;
}

QPDFObjectHandle cloneStream(QPDF &pdf, QPDFObjectHandle original)
{
    try {
        const std::shared_ptr<Buffer> raw = original.getRawStreamData();
        QPDFObjectHandle copy = QPDFObjectHandle::newStream(&pdf);
        QPDFObjectHandle dict = original.getDict();
        QPDFObjectHandle copyDict = copy.getDict();
        for (const auto &[key, value] : dict.getDictAsMap()) {
            if (key == "/Length") {
                continue;
            }
            copyDict.replaceKey(key, value);
        }
        copy.replaceStreamData(std::string(reinterpret_cast<const char *>(raw->getBuffer()), raw->getSize()),
                               dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
        return copy;
    } catch (const std::exception &) {
        return QPDFObjectHandle::newNull();
    }
}

/** Rewrites an /Indexed palette rather than its pixels, which keeps them exactly as they are. */
bool convertPalette(Work &work, const Space &space, ImagePlan *plan)
{
    const Space &base = space->alternate;
    if (!base) {
        return false;
    }
    const int inComponents = std::max(1, base->components);
    const int entries = space->hival + 1;
    if (space->lookup.size() < qsizetype(inComponents) * entries) {
        return false;
    }

    std::string outSpace = "/DeviceGray";
    switch (work.target) {
    case ColourTools::Target::Grayscale:
    case ColourTools::Target::BlackWhite:
        break;
    case ColourTools::Target::Rgb:
        outSpace = "/DeviceRGB";
        break;
    case ColourTools::Target::Cmyk:
        outSpace = "/DeviceCMYK";
        break;
    }

    std::string lookup;
    lookup.reserve(size_t(entries) * 4);
    QVector<double> values(inComponents, 0.0);
    for (int entry = 0; entry < entries; ++entry) {
        for (int i = 0; i < inComponents; ++i) {
            values[i] = paletteComponent(base, i, static_cast<uchar>(space->lookup.at(qsizetype(entry) * inComponents + i)));
        }
        Rgb colour;
        if (!toRgb(base, values, &colour)) {
            return false;
        }
        const auto push = [&lookup](double unit) {
            lookup.push_back(char(uchar(std::clamp(std::lround(unit * 255.0), 0L, 255L))));
        };
        switch (work.target) {
        case ColourTools::Target::Grayscale:
            push(luminanceOf(colour));
            break;
        case ColourTools::Target::BlackWhite:
            push(luminanceOf(colour) > work.threshold ? 1.0 : 0.0);
            break;
        case ColourTools::Target::Rgb:
            push(colour.r);
            push(colour.g);
            push(colour.b);
            break;
        case ColourTools::Target::Cmyk: {
            double cmyk[4] = { 0, 0, 0, 0 };
            if (base->family == Family::Gray) {
                cmyk[3] = clamp01(1.0 - values.value(0, 0.0));
            } else if (work.managed) {
                work.managed->rgbToCmyk(colour, cmyk);
            } else {
                return false;
            }
            for (const double ink : cmyk) {
                push(ink);
            }
            break;
        }
        }
    }

    QPDFObjectHandle replacement = QPDFObjectHandle::newArray();
    replacement.appendItem(QPDFObjectHandle::newName("/Indexed"));
    replacement.appendItem(QPDFObjectHandle::newName(outSpace));
    replacement.appendItem(QPDFObjectHandle::newInteger(space->hival));
    replacement.appendItem(QPDFObjectHandle::newString(lookup));

    plan->paletteOnly = true;
    plan->colourSpace = replacement;
    plan->space = outSpace;
    return true;
}

ImageOutcome convertImageInto(Work &work, QPDFObjectHandle image, const Space &space, ImagePlan *plan)
{
    if (!space) {
        return ImageOutcome::Refused;
    }

    const Family effective = space->family == Family::Indexed && space->alternate ? space->alternate->family
                                                                                 : space->family;
    const bool alreadyThere = (work.target == ColourTools::Target::Grayscale && effective == Family::Gray)
        || (work.target == ColourTools::Target::Rgb && effective == Family::Rgb)
        || (work.target == ColourTools::Target::Cmyk && effective == Family::Cmyk);
    if (alreadyThere && space->family != Family::Indexed) {
        return ImageOutcome::NotNeeded;
    }
    if (work.target == ColourTools::Target::Cmyk && !work.managed && effective != Family::Gray) {
        return ImageOutcome::Refused;
    }

    if (space->family == Family::Indexed) {
        return convertPalette(work, space, plan) ? ImageOutcome::Planned : ImageOutcome::Refused;
    }

    bool wasJpeg = false;
    QImage decoded;
    if (!decodeImage(image, space, &decoded, &wasJpeg) || decoded.isNull()) {
        return ImageOutcome::Refused;
    }

    plan->width = decoded.width();
    plan->height = decoded.height();
    plan->filter = QPDFObjectHandle::newNull();
    plan->decodeParms = QPDFObjectHandle::newNull();

    switch (work.target) {
    case ColourTools::Target::Grayscale: {
        const QImage grey = decoded.convertToFormat(QImage::Format_Grayscale8);
        if (grey.isNull()) {
            return ImageOutcome::Refused;
        }
        plan->space = "/DeviceGray";
        plan->bits = 8;
        if (wasJpeg && work.recompressPhotographs) {
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            if (grey.save(&buffer, "JPEG", work.jpegQuality) && jpegComponents(encoded) == 1) {
                plan->data.assign(encoded.constData(), size_t(encoded.size()));
                plan->filter = QPDFObjectHandle::newName("/DCTDecode");
                return ImageOutcome::Planned;
            }
        }
        plan->data.reserve(size_t(grey.width()) * grey.height());
        for (int y = 0; y < grey.height(); ++y) {
            plan->data.append(reinterpret_cast<const char *>(grey.constScanLine(y)), size_t(grey.width()));
        }
        return ImageOutcome::Planned;
    }

    case ColourTools::Target::BlackWhite: {
        const QImage grey = decoded.convertToFormat(QImage::Format_Grayscale8);
        if (grey.isNull()) {
            return ImageOutcome::Refused;
        }
        plan->space = "/DeviceGray";
        plan->bits = 1;
        plan->data = packOneBit(grey, work.threshold);
        return ImageOutcome::Planned;
    }

    case ColourTools::Target::Rgb: {
        const QImage rgb = decoded.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) {
            return ImageOutcome::Refused;
        }
        plan->space = "/DeviceRGB";
        plan->bits = 8;
        if (work.recompressPhotographs) {
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            if (rgb.save(&buffer, "JPEG", work.jpegQuality) && jpegComponents(encoded) == 3) {
                plan->data.assign(encoded.constData(), size_t(encoded.size()));
                plan->filter = QPDFObjectHandle::newName("/DCTDecode");
                return ImageOutcome::Planned;
            }
        }
        for (int y = 0; y < rgb.height(); ++y) {
            plan->data.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), size_t(rgb.width()) * 3);
        }
        return ImageOutcome::Planned;
    }

    case ColourTools::Target::Cmyk: {
        if (!work.managed) {
            return ImageOutcome::Refused;
        }
        const QImage rgb = decoded.convertToFormat(QImage::Format_RGB888);
        if (rgb.isNull()) {
            return ImageOutcome::Refused;
        }
        plan->space = "/DeviceCMYK";
        plan->bits = 8;
        std::string row(size_t(rgb.width()) * 4, '\0');
        plan->data.reserve(size_t(rgb.width()) * rgb.height() * 4);
        for (int y = 0; y < rgb.height(); ++y) {
            work.managed->rgbLineToCmyk(rgb.constScanLine(y), reinterpret_cast<uchar *>(row.data()), rgb.width());
            plan->data.append(row);
        }
        return ImageOutcome::Planned;
    }
    }
    return ImageOutcome::Refused;
}

bool applyImagePlan(QPDFObjectHandle image, const ImagePlan &plan)
{
    QPDFObjectHandle dict = image.getDict();
    if (plan.paletteOnly) {
        dict.replaceKey("/ColorSpace", plan.colourSpace);
        return true;
    }

    try {
        image.replaceStreamData(plan.data, plan.filter, plan.decodeParms);
    } catch (const std::exception &) {
        return false;
    }
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName(plan.space));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(plan.bits));
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(plan.width));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(plan.height));
    // A /Decode written for the old space would invert or clip the new samples.
    dict.removeKey("/Decode");
    return true;
}

// ══ document-wide plumbing ════════════════════════════════════════════════

void collectReachable(QPDFObjectHandle resources, std::set<QPDFObjGen> &found, int depth = 0)
{
    if (depth > MaxFormDepth || !resources.isDictionary()) {
        return;
    }
    QPDFObjectHandle table = resources.getKey("/XObject");
    if (!table.isDictionary()) {
        return;
    }
    for (const auto &[name, object] : table.getDictAsMap()) {
        Q_UNUSED(name)
        if (!object.isStream() || found.count(object.getObjGen()) > 0) {
            continue;
        }
        found.insert(object.getObjGen());
        collectReachable(object.getDict().getKey("/Resources"), found, depth + 1);
    }
}

/** The pages a caller selected, as indexes into the document, cleaned up. */
std::set<int> selectionOf(const QVector<int> &wanted, size_t pageCount)
{
    std::set<int> selected;
    if (wanted.isEmpty()) {
        for (size_t i = 0; i < pageCount; ++i) {
            selected.insert(int(i));
        }
        return selected;
    }
    for (const int page : wanted) {
        if (page >= 0 && size_t(page) < pageCount) {
            selected.insert(page);
        }
    }
    return selected;
}

/** Runs one filter job over the appearance streams of a page's annotations. */
void filterAppearances(Work &work, QPDFPageObjectHelper &page)
{
    QPDFObjectHandle annotations = page.getObjectHandle().getKey("/Annots");
    if (!annotations.isArray()) {
        return;
    }
    for (int i = 0; i < annotations.getArrayNItems(); ++i) {
        QPDFObjectHandle annotation = annotations.getArrayItem(i);
        QPDFObjectHandle appearances = annotation.isDictionary() ? annotation.getKey("/AP")
                                                                : QPDFObjectHandle::newNull();
        if (!appearances.isDictionary()) {
            continue;
        }
        QVector<QPDFObjectHandle> streams;
        for (const auto &[key, value] : appearances.getDictAsMap()) {
            Q_UNUSED(key)
            if (value.isStream()) {
                streams.append(value);
            } else if (value.isDictionary()) {
                for (const auto &[state, inner] : value.getDictAsMap()) {
                    Q_UNUSED(state)
                    if (inner.isStream()) {
                        streams.append(inner);
                    }
                }
            }
        }
        for (QPDFObjectHandle stream : streams) {
            if (work.formsDone.count(stream.getObjGen()) > 0) {
                continue;
            }
            work.formsDone.insert(stream.getObjGen());
            QPDFObjectHandle resources = stream.getDict().getKey("/Resources");
            Pl_Buffer buffer("appearance");
            ColourFilter filter(work, resources, 1);
            stream.filterAsContents(&filter, &buffer);
            if (!filter.changed()) {
                continue;
            }
            const auto data = buffer.getBufferSharedPointer();
            stream.replaceStreamData(std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize()),
                                     QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        }
    }
}

/**
 * The graphics state pair that setBlackOverprint() needs on a page.
 *
 * Both are always written, whichever direction was asked for, because switching
 * overprinting on for the blacks means switching it off again for whatever
 * follows them, and a `gs` naming a state the page does not carry is a broken
 * page, not a slightly larger one.
 */
void addOverprintStates(QPDF &pdf, QPDFObjectHandle resources)
{
    QPDFObjectHandle table = resources.getKey("/ExtGState");
    if (!table.isDictionary()) {
        table = QPDFObjectHandle::newDictionary();
        resources.replaceKey("/ExtGState", table);
    }
    for (const bool on : { true, false }) {
        QPDFObjectHandle state = QPDFObjectHandle::newDictionary();
        state.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
        state.replaceKey("/OP", QPDFObjectHandle::newBool(on));
        state.replaceKey("/op", QPDFObjectHandle::newBool(on));
        // Overprint mode 1 is what makes a zero component mean "leave the plate
        // alone" rather than "knock it out", which is the whole point for black.
        state.replaceKey("/OPM", QPDFObjectHandle::newInteger(on ? 1 : 0));
        table.replaceKey(on ? "/PsOverprintOn" : "/PsOverprintOff", pdf.makeIndirectObject(state));
    }
}

/** Applies one filter job across a selection of pages and writes the result. */
bool runOverPages(const QString &in, const QString &out, Work &work, const QVector<int> &pages,
                  const std::function<void(QPDF &, QPDFObjectHandle)> &prepare, QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, in);
        work.pdf = &pdf;

        QPDFPageDocumentHelper documents(pdf);
        std::vector<QPDFPageObjectHelper> all = documents.getAllPages();
        const std::set<int> selected = selectionOf(pages, all.size());
        if (selected.empty()) {
            if (error) {
                *error = i18n("None of the pages asked for are in this document.");
            }
            return false;
        }

        if (selected.size() < all.size()) {
            for (size_t i = 0; i < all.size(); ++i) {
                if (selected.count(int(i)) == 0) {
                    collectReachable(all.at(i).getAttribute("/Resources", false), work.sharedWithUntouched);
                }
            }
        }

        for (const int index : selected) {
            QPDFPageObjectHelper page = all.at(size_t(index));
            // The resources have to be the page's own before anything is added
            // to them, or the addition would land on every page that shares them.
            QPDFObjectHandle resources = page.getAttribute("/Resources", true);
            if (!resources.isDictionary()) {
                resources = QPDFObjectHandle::newDictionary();
                page.getObjectHandle().replaceKey("/Resources", resources);
            }
            if (prepare) {
                prepare(pdf, resources);
            }

            Pl_Buffer buffer("colour");
            ColourFilter filter(work, resources, 0);
            page.filterContents(&filter, &buffer);

            if (filter.changed()) {
                const auto data = buffer.getBufferSharedPointer();
                page.getObjectHandle().replaceKey(
                    "/Contents", QPDFObjectHandle::newStream(
                                     &pdf, std::string(reinterpret_cast<const char *>(data->getBuffer()),
                                                       data->getSize())));
                page.removeUnreferencedResources();

                // Done by name rather than left to the sweep above, because that
                // one keeps a colour space that nothing selects any more, so a
                // Pantone dissolved into process ink would still be listed as a
                // plate, and the file would still cost a run through the press.
                QPDFObjectHandle table = resources.getKey("/ColorSpace");
                if (table.isDictionary()) {
                    for (const std::string &name : filter.retiredSpaces()) {
                        table.removeKey(name);
                    }
                    if (table.getKeys().empty()) {
                        resources.removeKey("/ColorSpace");
                    }
                }
            }

            // An annotation's own appearance stream is a content stream too, and
            // a highlight left yellow in a greyscaled document is exactly the
            // kind of miss that makes a tool untrustworthy. Overprint is the
            // exception: its graphics state is added to the page's resources,
            // not to every appearance's.
            if (work.job != Job::Inspect && work.job != Job::Overprint) {
                filterAppearances(work, page);
            }
        }

        QPDFWriter writer(pdf, QFile::encodeName(out).constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

// ══ Ghostscript ═══════════════════════════════════════════════════════════

QString ghostscriptPath()
{
    return QStandardPaths::findExecutable(u"gs"_s);
}

bool runGhostscript(const QStringList &arguments, QByteArray *standardOutput, QString *error)
{
    const QString gs = ghostscriptPath();
    if (gs.isEmpty()) {
        if (error) {
            *error = i18n("Ghostscript is not installed, so this cannot be measured. "
                          "Install the “ghostscript” package.");
        }
        return false;
    }

    QProcess process;
    process.start(gs, arguments);
    if (!process.waitForStarted(10000)) {
        if (error) {
            *error = i18n("Ghostscript could not be started.");
        }
        return false;
    }
    if (!process.waitForFinished(GhostscriptTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        if (error) {
            *error = i18n("Ghostscript took too long and was stopped.");
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (error) {
            *error = details.isEmpty() ? i18n("Ghostscript reported an error.") : details;
        }
        return false;
    }
    if (standardOutput) {
        *standardOutput = process.readAllStandardOutput();
    }
    return true;
}

/** A Ghostscript PAM raster: the header fields that matter, and the samples. */
struct Pam {
    int width = 0;
    int height = 0;
    int depth = 0;
    int maxValue = 255;
    QByteArray samples;
};

bool readPam(const QString &path, Pam *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray whole = file.readAll();
    const qsizetype end = whole.indexOf("ENDHDR\n");
    if (end < 0) {
        return false;
    }

    const QList<QByteArray> lines = whole.left(end).split('\n');
    for (const QByteArray &raw : lines) {
        QByteArray line = raw;
        const qsizetype comment = line.indexOf('#');
        if (comment >= 0) {
            line = line.left(comment);
        }
        line = line.trimmed();
        const qsizetype space = line.indexOf(' ');
        if (space < 0) {
            continue;
        }
        const QByteArray key = line.left(space);
        const int value = line.mid(space + 1).trimmed().toInt();
        if (key == "WIDTH") {
            out->width = value;
        } else if (key == "HEIGHT") {
            out->height = value;
        } else if (key == "DEPTH") {
            out->depth = value;
        } else if (key == "MAXVAL") {
            out->maxValue = value;
        }
    }

    out->samples = whole.mid(end + 7);
    return out->width > 0 && out->height > 0 && out->depth > 0
        && out->samples.size() >= qsizetype(out->width) * out->height * out->depth;
}

int pageCountOf(const QString &pdf, QString *error)
{
    try {
        QPDF file;
        PdfFile::open(file, pdf);
        return int(QPDFPageDocumentHelper(file).getAllPages().size());
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return -1;
    }
}

} // namespace

// ══ the public face ═══════════════════════════════════════════════════════

ColourTools::Inventory ColourTools::inspect(const QString &pdf, QString *error)
{
    Inventory inventory;
    Work work;
    work.job = Job::Inspect;

    try {
        QPDF file;
        PdfFile::open(file, pdf);
        work.pdf = &file;

        QPDFObjectHandle intents = file.getRoot().getKey("/OutputIntents");
        inventory.hasOutputIntent = intents.isArray() && intents.getArrayNItems() > 0;

        QPDFPageDocumentHelper documents(file);
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();
        inventory.pages = int(pages.size());

        for (QPDFPageObjectHelper &page : pages) {
            work.pageHasRgb = false;
            work.pageHasCmyk = false;

            QPDFObjectHandle group = page.getObjectHandle().getKey("/Group");
            if (group.isDictionary()) {
                QPDFObjectHandle kind = group.getKey("/S");
                if (kind.isName() && kind.getName() == "/Transparency") {
                    work.sawTransparency = true;
                }
            }

            QPDFObjectHandle resources = page.getAttribute("/Resources", false);

            // The resource dictionary is worth reading on its own: a separation
            // that is defined but never painted still needs a plate, and a
            // print shop that finds out at the press has found out too late.
            QPDFObjectHandle table = resources.isDictionary() ? resources.getKey("/ColorSpace")
                                                             : QPDFObjectHandle::newNull();
            if (table.isDictionary()) {
                for (const auto &[name, value] : table.getDictAsMap()) {
                    Q_UNUSED(name)
                    Space space = resolveSpace(value, resources, 0);
                    if (!space) {
                        continue;
                    }
                    work.spaces.insert(space->label());
                    if (space->viaIcc) {
                        work.sawIcc = true;
                    }
                    for (const QString &ink : space->inkNames) {
                        if (ink != u"All"_s && ink != u"None"_s) {
                            work.spots.insert(ink);
                        }
                    }
                }
            }

            ColourFilter filter(work, resources, 0);
            page.filterContents(&filter, nullptr);

            if (work.pageHasRgb) {
                ++inventory.pagesWithRgb;
            }
            if (work.pageHasCmyk) {
                ++inventory.pagesWithCmyk;
            }
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }

    inventory.spaces = QStringList(work.spaces.cbegin(), work.spaces.cend());
    inventory.spaces.sort();
    inventory.spotColours = QStringList(work.spots.cbegin(), work.spots.cend());
    inventory.spotColours.sort();
    inventory.hasIccProfile = work.sawIcc;
    inventory.usesTransparency = work.sawTransparency;
    inventory.usesOverprint = work.sawOverprint;
    inventory.hairlines = work.hairlinesFound;
    inventory.strokes = work.strokesSeen;
    inventory.thinnestStroke = work.thinnestStroke;
    inventory.notes = work.notes;
    if (work.inlineImages > 0) {
        inventory.notes << i18np("The document has one inline image, whose colour is not counted here.",
                                 "The document has %1 inline images, whose colour is not counted here.",
                                 work.inlineImages);
    }
    return inventory;
}

bool ColourTools::hasColourManagement()
{
#ifdef PS_WITH_LCMS
    return true;
#else
    return false;
#endif
}

QString ColourTools::defaultIccProfile(Target target)
{
    if (target != Target::Cmyk) {
        return {};
    }

    // Ordered by how likely a European print shop is to ask for it, then by how
    // generic it is. A real job names its profile; this is only a starting point.
    static const QStringList candidates {
        u"/usr/share/color/icc/ISOcoated_v2_300_bas.ICC"_s,
        u"/usr/share/color/icc/ISOcoated_v2_bas.ICC"_s,
        u"/usr/share/color/icc/colord/FOGRA39L_coated.icc"_s,
        u"/usr/share/color/icc/colord/GRACoL_TR006_coated.icc"_s,
        u"/usr/share/color/icc/colord/FOGRA47L_uncoated.icc"_s,
        u"/usr/share/color/icc/ghostscript/default_cmyk.icc"_s,
        u"/usr/local/share/color/icc/ISOcoated_v2_300_bas.ICC"_s,
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    // Nothing familiar, so ask every profile on the system what it is.
    const QStringList directories { u"/usr/share/color/icc"_s, u"/usr/share/color/icc/colord"_s,
                                    u"/usr/share/color/icc/ghostscript"_s,
                                    QDir::homePath() + u"/.local/share/icc"_s };
    for (const QString &directory : directories) {
        const QStringList files
            = QDir(directory).entryList({ u"*.icc"_s, u"*.ICC"_s, u"*.icm"_s, u"*.ICM"_s }, QDir::Files, QDir::Name);
        for (const QString &file : files) {
            const QString path = directory + u'/' + file;
#ifdef PS_WITH_LCMS
            cmsHPROFILE profile = cmsOpenProfileFromFile(QFile::encodeName(path).constData(), "r");
            if (!profile) {
                continue;
            }
            const bool isCmyk = cmsGetColorSpace(profile) == cmsSigCmykData;
            cmsCloseProfile(profile);
            if (isCmyk) {
                return path;
            }
#else
            Q_UNUSED(path)
#endif
        }
    }
    return {};
}

namespace {

/** The whole-document route, for when there is no profile to convert with in process. */
bool convertWithGhostscript(const QString &in, const QString &out, ColourTools::Target target, QStringList *changes,
                            QString *error)
{
    QString strategy;
    switch (target) {
    case ColourTools::Target::Cmyk:
        strategy = u"CMYK"_s;
        break;
    case ColourTools::Target::Rgb:
        strategy = u"RGB"_s;
        break;
    case ColourTools::Target::Grayscale:
    case ColourTools::Target::BlackWhite:
        strategy = u"Gray"_s;
        break;
    }

    const QStringList arguments {
        u"-sDEVICE=pdfwrite"_s,
        u"-dCompatibilityLevel=1.7"_s,
        u"-dNOPAUSE"_s,
        u"-dBATCH"_s,
        u"-dQUIET"_s,
        u"-dSAFER"_s,
        u"-dAutoRotatePages=/None"_s,
        u"-sColorConversionStrategy=%1"_s.arg(strategy),
        u"-sOutputFile=%1"_s.arg(out),
        in,
    };
    if (!runGhostscript(arguments, nullptr, error)) {
        return false;
    }
    if (changes) {
        *changes << i18n("Ghostscript converted the whole document, because no ICC profile was available "
                         "to do it in place. Every page was rewritten, the result is Ghostscript's own "
                         "conversion rather than a profile you chose, and the file has been rebuilt from "
                         "end to end rather than edited.");
    }
    return true;
}

} // namespace

bool ColourTools::convert(const QString &in, const QString &out, const ConvertOptions &options, QStringList *changes,
                          QString *error)
{
    QStringList collected;
    std::unique_ptr<Managed> managed;

    if (options.target == Target::Cmyk) {
        const QString profile = options.iccProfilePath.isEmpty() ? defaultIccProfile(Target::Cmyk)
                                                                 : options.iccProfilePath;
        if (hasColourManagement() && !profile.isEmpty()) {
            QString reason;
            managed = Managed::create(profile, &reason);
            if (!managed && !options.iccProfilePath.isEmpty()) {
                // An explicitly named profile that cannot be used is a mistake
                // worth reporting rather than quietly working around.
                if (error) {
                    *error = reason;
                }
                return false;
            }
        }
        if (!managed) {
            if (!options.pages.isEmpty()) {
                if (error) {
                    *error = i18n("Converting only some pages to CMYK needs an ICC profile, because the "
                                  "only other route is Ghostscript and it rewrites the whole document. "
                                  "Name a CMYK profile, or convert every page.");
                }
                return false;
            }
            const bool done = convertWithGhostscript(in, out, options.target, &collected, error);
            if (changes) {
                *changes = collected;
            }
            return done;
        }
        collected << i18n("Colour was converted to CMYK through the ICC profile “%1”.", managed->profileName());
        collected << i18n("Grey was written as black ink alone rather than as a four-colour build, which "
                          "is what a press wants.");
    } else if (options.target == Target::Rgb && !options.iccProfilePath.isEmpty()) {
        QString reason;
        managed = Managed::create(options.iccProfilePath, &reason);
        if (!managed) {
            if (error) {
                *error = reason;
            }
            return false;
        }
        collected << i18n("CMYK was converted to RGB through the ICC profile “%1”.", managed->profileName());
    } else if (options.target == Target::Rgb) {
        collected << i18n("CMYK was converted to RGB with the formula the PDF specification defines. It is "
                          "predictable and needs no profile, but it is not colour-managed. Name an ICC "
                          "profile for that.");
    } else {
        collected << i18n("Grey was computed with the weighting the PDF specification defines for it, "
                          "which is exact and needs no profile.");
    }

    Work work;
    work.job = Job::Convert;
    work.target = options.target;
    work.threshold = std::clamp(options.threshold, 0.0, 1.0);
    work.recompressPhotographs = options.recompressPhotographs;
    work.jpegQuality = std::clamp(options.jpegQuality, 1, 100);
    work.managed = managed.get();

    if (!runOverPages(in, out, work, options.pages, {}, error)) {
        return false;
    }

    if (work.operatorsChanged > 0) {
        collected << i18np("One colour operator was rewritten in place, leaving everything else in the "
                           "page untouched.",
                           "%1 colour operators were rewritten in place, leaving everything else in the "
                           "pages untouched.",
                           work.operatorsChanged);
    }
    if (work.imagesConverted > 0) {
        collected << i18np("One image had its pixels converted and re-encoded.",
                           "%1 images had their pixels converted and re-encoded.", work.imagesConverted);
    }
    if (work.palettesConverted > 0) {
        collected << i18np("One indexed image had only its palette rewritten, so its pixels and their "
                           "compression are exactly as they were.",
                           "%1 indexed images had only their palettes rewritten, so their pixels and their "
                           "compression are exactly as they were.",
                           work.palettesConverted);
    }
    if (work.imagesRefused > 0) {
        collected << i18np("One image could not be decoded and was left in its original colour.",
                           "%1 images could not be decoded and were left in their original colour.",
                           work.imagesRefused);
    }
    if (work.operatorsChanged == 0 && work.imagesConverted == 0 && work.palettesConverted == 0) {
        collected << i18n("Nothing needed changing: the pages were already in the colour asked for.");
    }
    collected += work.notes;

    if (changes) {
        *changes = collected;
    }
    return true;
}

bool ColourTools::replaceColour(const QString &in, const QString &out, const QColor &from, const QColor &to,
                                double tolerance, int *replaced, QString *error)
{
    if (!from.isValid() || !to.isValid()) {
        if (error) {
            *error = i18n("Both colours have to be valid.");
        }
        return false;
    }

    Work work;
    work.job = Job::Replace;
    work.from = { from.redF(), from.greenF(), from.blueF() };
    work.to = { to.redF(), to.greenF(), to.blueF() };
    work.tolerance = std::max(0.0, tolerance);
    work.replacementIsGrey = qFuzzyCompare(to.redF() + 1.0, to.greenF() + 1.0)
        && qFuzzyCompare(to.greenF() + 1.0, to.blueF() + 1.0);

    if (!runOverPages(in, out, work, {}, {}, error)) {
        return false;
    }
    if (replaced) {
        *replaced = work.replaced;
    }
    return true;
}

bool ColourTools::mapSpotColours(const QString &in, const QString &out, const QHash<QString, QString> &renames,
                                 const QStringList &toProcess, int *changed, QString *error)
{
    if (renames.isEmpty() && toProcess.isEmpty()) {
        if (error) {
            *error = i18n("Nothing was asked for: name an ink to rename or one to turn into process colour.");
        }
        return false;
    }

    Work work;
    work.job = Job::Spot;
    work.toProcess = toProcess;

    // A rename lives in the colour space array rather than in any content
    // stream, so it is done as a walk over the objects. Walking every object
    // rather than every page's resources is deliberate: a separation reached
    // only from a pattern or an annotation still needs its plate renamed.
    // Shared state, because the hook runs inside the document runOverPages
    // opens, so one write produces both halves of the change.
    struct Renaming {
        QHash<QString, QString> renames;
        int applied = 0;
        bool done = false;
    };
    auto renaming = std::make_shared<Renaming>();
    renaming->renames = renames;

    const auto prepare = [renaming](QPDF &pdf, QPDFObjectHandle) {
        if (renaming->done || renaming->renames.isEmpty()) {
            return;
        }
        renaming->done = true;

        const auto renameArray = [&renaming](QPDFObjectHandle object) {
            if (object.getArrayNItems() < 2) {
                return;
            }
            QPDFObjectHandle head = object.getArrayItem(0);
            if (!head.isName()) {
                return;
            }
            const std::string family = head.getName();
            if (family == "/Separation") {
                const auto found = renaming->renames.constFind(nameValue(object.getArrayItem(1)));
                if (found != renaming->renames.constEnd()) {
                    object.setArrayItem(1, QPDFObjectHandle::newName("/" + found->toStdString()));
                    ++renaming->applied;
                }
            } else if (family == "/DeviceN") {
                QPDFObjectHandle names = object.getArrayItem(1);
                for (int i = 0; names.isArray() && i < names.getArrayNItems(); ++i) {
                    const auto found = renaming->renames.constFind(nameValue(names.getArrayItem(i)));
                    if (found != renaming->renames.constEnd()) {
                        names.setArrayItem(i, QPDFObjectHandle::newName("/" + found->toStdString()));
                        ++renaming->applied;
                    }
                }
            }
        };

        // getAllObjects() lists the indirect objects only, and a colour space is
        // very often written straight into the resource dictionary that uses it,
        // so walking that list alone renames nothing at all in the commonest
        // file there is. Each indirect object is therefore descended into, but
        // only through its direct children: anything behind a reference is an
        // entry in the list in its own right and would otherwise be walked twice
        // or, where the file has a cycle, for ever.
        std::function<void(QPDFObjectHandle, int)> descend = [&](QPDFObjectHandle object, int depth) {
            if (depth > 32) {
                return;
            }
            if (object.isStream()) {
                descend(object.getDict(), depth + 1);
                return;
            }
            if (object.isArray()) {
                renameArray(object);
                for (int i = 0; i < object.getArrayNItems(); ++i) {
                    QPDFObjectHandle child = object.getArrayItem(i);
                    if (!child.isIndirect()) {
                        descend(child, depth + 1);
                    }
                }
                return;
            }
            if (object.isDictionary()) {
                for (const auto &[key, value] : object.getDictAsMap()) {
                    Q_UNUSED(key)
                    if (!value.isIndirect()) {
                        descend(value, depth + 1);
                    }
                }
            }
        };

        for (QPDFObjectHandle object : pdf.getAllObjects()) {
            descend(object, 0);
        }
    };

    if (!runOverPages(in, out, work, {}, prepare, error)) {
        return false;
    }
    if (changed) {
        *changed = renaming->applied + work.spotsRewritten;
    }
    return true;
}

bool ColourTools::fixHairlines(const QString &in, const QString &out, double minimumWidth, int *fixed, QString *error)
{
    if (minimumWidth <= 0.0) {
        if (error) {
            *error = i18n("The minimum width has to be greater than zero.");
        }
        return false;
    }

    Work work;
    work.job = Job::Hairline;
    work.minimumWidth = minimumWidth;

    if (!runOverPages(in, out, work, {}, {}, error)) {
        return false;
    }
    if (fixed) {
        *fixed = work.hairlinesFixed;
    }
    return true;
}

bool ColourTools::setBlackOverprint(const QString &in, const QString &out, bool on, int *changed, QString *error)
{
    Work work;
    work.job = Job::Overprint;
    work.overprintOn = on;

    const auto prepare = [](QPDF &pdf, QPDFObjectHandle resources) { addOverprintStates(pdf, resources); };

    if (!runOverPages(in, out, work, {}, prepare, error)) {
        return false;
    }
    if (changed) {
        *changed = work.overprintInserted;
    }
    return true;
}

QVector<double> ColourTools::inkCoverage(const QString &pdf, QString *error)
{
    const int pages = pageCountOf(pdf, error);
    if (pages <= 0) {
        if (pages == 0 && error) {
            *error = i18n("The document has no pages.");
        }
        return {};
    }
    if (ghostscriptPath().isEmpty()) {
        if (error) {
            *error = i18n("Ink coverage is measured by rendering each page in CMYK, which needs "
                          "Ghostscript. Install the “ghostscript” package.");
        }
        return {};
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        if (error) {
            *error = i18n("A temporary folder could not be created.");
        }
        return {};
    }

    QVector<double> coverage(pages, 0.0);

    // Rendered in batches, because a two-hundred-page magazine at this
    // resolution is a third of a gigabyte of raster if it is all written first.
    for (int first = 1; first <= pages; first += InkCoverageBatch) {
        const int last = std::min(pages, first + InkCoverageBatch - 1);
        const QStringList arguments {
            u"-q"_s,
            u"-dNOPAUSE"_s,
            u"-dBATCH"_s,
            u"-dSAFER"_s,
            u"-sDEVICE=pamcmyk32"_s,
            u"-r%1"_s.arg(InkCoverageDpi),
            u"-dFirstPage=%1"_s.arg(first),
            u"-dLastPage=%1"_s.arg(last),
            u"-sOutputFile=%1/page-%d.pam"_s.arg(directory.path()),
            pdf,
        };
        if (!runGhostscript(arguments, nullptr, error)) {
            return {};
        }

        for (int page = first; page <= last; ++page) {
            const QString path = u"%1/page-%2.pam"_s.arg(directory.path()).arg(page);
            Pam raster;
            if (!readPam(path, &raster) || raster.depth != 4) {
                QFile::remove(path);
                continue;
            }
            const auto *samples = reinterpret_cast<const uchar *>(raster.samples.constData());
            const qsizetype pixels = qsizetype(raster.width) * raster.height;
            int highest = 0;
            for (qsizetype i = 0; i < pixels; ++i) {
                const int total = samples[i * 4] + samples[i * 4 + 1] + samples[i * 4 + 2] + samples[i * 4 + 3];
                highest = std::max(highest, total);
            }
            const double scale = raster.maxValue > 0 ? double(raster.maxValue) : 255.0;
            coverage[page - 1] = 100.0 * double(highest) / scale;
            QFile::remove(path);
        }
    }

    return coverage;
}

QImage ColourTools::separation(const QString &pdf, int page, const QString &inkName, double dpi, QString *error)
{
    const int pages = pageCountOf(pdf, error);
    if (pages <= 0) {
        return {};
    }
    if (page < 0 || page >= pages) {
        if (error) {
            *error = i18n("Page %1 is not in this document.", page + 1);
        }
        return {};
    }
    if (ghostscriptPath().isEmpty()) {
        if (error) {
            *error = i18n("A separations preview is produced by Ghostscript. Install the "
                          "“ghostscript” package.");
        }
        return {};
    }

    const int resolution = int(std::clamp(dpi > 0.0 ? dpi : 72.0, 12.0, 1200.0));
    QTemporaryDir directory;
    if (!directory.isValid()) {
        if (error) {
            *error = i18n("A temporary folder could not be created.");
        }
        return {};
    }

    static const QStringList process { u"Cyan"_s, u"Magenta"_s, u"Yellow"_s, u"Black"_s };
    const qsizetype channel = process.indexOf(inkName);

    if (channel >= 0) {
        const QString path = directory.filePath(u"plate.pam"_s);
        const QStringList arguments {
            u"-q"_s,          u"-dNOPAUSE"_s,                    u"-dBATCH"_s,
            u"-dSAFER"_s,     u"-sDEVICE=pamcmyk32"_s,           u"-r%1"_s.arg(resolution),
            u"-dFirstPage=%1"_s.arg(page + 1),                   u"-dLastPage=%1"_s.arg(page + 1),
            u"-sOutputFile=%1"_s.arg(path),                      pdf,
        };
        if (!runGhostscript(arguments, nullptr, error)) {
            return {};
        }
        Pam raster;
        if (!readPam(path, &raster) || raster.depth != 4) {
            if (error) {
                *error = i18n("Ghostscript did not produce a readable separation.");
            }
            return {};
        }

        QImage plate(raster.width, raster.height, QImage::Format_Grayscale8);
        if (plate.isNull()) {
            return {};
        }
        const auto *samples = reinterpret_cast<const uchar *>(raster.samples.constData());
        for (int y = 0; y < raster.height; ++y) {
            uchar *line = plate.scanLine(y);
            for (int x = 0; x < raster.width; ++x) {
                const qsizetype at = (qsizetype(y) * raster.width + x) * 4 + channel;
                // Inverted, so the result reads as the plate does on paper:
                // black is a full covering of ink.
                line[x] = uchar(255 - samples[at]);
            }
        }
        return plate;
    }

    // A named ink has to stay its own plate, which is what tiffsep is for.
    const QString stem = directory.filePath(u"sep"_s);
    const QStringList arguments {
        u"-q"_s,
        u"-dNOPAUSE"_s,
        u"-dBATCH"_s,
        u"-dSAFER"_s,
        u"-sDEVICE=tiffsep"_s,
        u"-r%1"_s.arg(resolution),
        u"-dFirstPage=%1"_s.arg(page + 1),
        u"-dLastPage=%1"_s.arg(page + 1),
        u"-sOutputFile=%1-%d.tif"_s.arg(stem),
        pdf,
    };
    if (!runGhostscript(arguments, nullptr, error)) {
        return {};
    }

    const QString plate = u"%1-%2(%3).tif"_s.arg(stem).arg(page + 1).arg(inkName);
    QImage image(plate);
    if (image.isNull()) {
        if (error) {
            *error = i18n("There is no separation called “%1” on page %2.", inkName, page + 1);
        }
        return {};
    }
    // tiffsep already writes plates the way they print: nothing to invert.
    return image.convertToFormat(QImage::Format_Grayscale8);
}

QStringList ColourTools::limitations()
{
    QStringList notes {
        i18n("Text and vector colour is rewritten exactly, operator by operator. Everything else in "
             "those pages (the glyphs, the paths, the fonts, the structure) comes out untouched."),
        i18n("Conversion into CMYK needs an ICC profile. Without one, Ghostscript converts the whole "
             "document instead, which cannot honour a page selection and gives Ghostscript's "
             "conversion rather than one you chose."),
        i18n("Greyscale and RGB conversions use the arithmetic the PDF specification defines, so they "
             "are exact and repeatable, but they are not colour-managed."),
        i18n("Photographs are decoded, converted and re-encoded, so a JPEG loses a little each time. "
             "Indexed pictures have only their palette rewritten and lose nothing at all."),
        i18n("Fax, JPEG 2000 and four-colour JPEG images cannot be decoded here and are left in their "
             "original colour rather than guessed at."),
        i18n("Shading patterns and tiling patterns paint from their own colour functions, which are "
             "carried across unchanged."),
        i18n("Inline images keep their original colour."),
        i18n("A colour replaced by name is written in RGB, or in grey when the replacement is a grey and "
             "the original was one. Putting an arbitrary colour back into CMYK or into a spot ink would "
             "need a profile this has not been given."),
        i18n("Replacing a colour searches the drawing operators, not the pixels of photographs."),
        i18n("Hairline widths are judged after the page's own scaling, but a width set before the "
             "matrix that shrinks it is only caught at the stroke itself, where the fix is applied to "
             "everything drawn afterwards at that width."),
        i18n("Ink coverage and separation previews are measured through Ghostscript's own conversion to "
             "CMYK, which is not colour-managed unless the document carries an output intent."),
        i18n("Ink coverage is the highest total found on the page at 72 dots per inch, which is the "
             "number an ink limit is about; it is not an average."),
        i18n("A multi-ink DeviceN transform stored as a sampled function is read at its nearest sample "
             "rather than interpolated across every ink."),
        i18n("Colour inside a Type 3 font's glyph procedures is not converted."),
    };
    if (!hasColourManagement()) {
        notes.prepend(i18n("This build has no colour management, so conversion into CMYK can only go "
                           "through Ghostscript."));
    }
    return notes;
}

} // namespace ps
