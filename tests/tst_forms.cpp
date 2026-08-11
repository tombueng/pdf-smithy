/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TestPdf.h"

#include "core/Forms.h"
#include "render/PopplerBackend.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

using namespace ps;

namespace {

const FormField *fieldNamed(const QVector<FormField> &fields, const QString &name)
{
    for (const FormField &field : fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

/**
 * A form whose fields say how they want to look, in every form the syntax allows.
 *
 * Built here rather than in the shared fixtures because nothing else needs it:
 * a /BG in each of its four lengths, an empty one, a /DA with a fractional size,
 * a comb field, the four value scripts, and one field that states none of it.
 */
bool writeStyledFormPdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper documents(pdf);

        QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
        page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
        documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        QPDFPageObjectHelper first = documents.getAllPages().at(0);

        QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /Font << >> >>");
        resources.getKey("/Font").replaceKey("/Helv", font);

        const char *widgets[] = {
            // Everything at once: a fractional size, an RGB text colour, a grey
            // background, a red border, a dashed two-point edge and all four
            // scripts a value can carry.
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Betrag) /MaxLen 12 /Q 2"
            "   /Rect [72 700 272 724] /F 4 /DA (/Helv 10.5 Tf 0.2 0.4 0.6 rg)"
            "   /MK << /BG [0.75] /BC [1 0 0] >> /BS << /W 2 /S /D >>"
            "   /AA << /F << /S /JavaScript /JS (AFNumber_Format(2, 0, 0, 0, \"EUR \", true);) >>"
            "         /K << /S /JavaScript /JS (AFNumber_Keystroke(2, 0, 0, 0, \"EUR \", true);) >>"
            "         /V << /S /JavaScript /JS (if (event.value < 0) event.rc = false;) >>"
            "         /C << /S /JavaScript /JS (event.value = this.getField(\"Betrag\").value * 1.19;) >> >> >>",
            // CMYK, on both sides: half black ink in /MK and a quarter in /DA.
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Grau)"
            "   /Rect [72 660 272 684] /F 4 /DA (/Helv 8 Tf 0 0 0 0.25 k)"
            "   /MK << /BG [0 0 0 0.5] >> >>",
            // An empty /BC is a border the document declines to colour, which is
            // not the same as a black one, and a /Border of width 0 is the only
            // way a document written before /BS existed could say "no border".
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Bunt)"
            "   /Rect [72 620 272 644] /F 4 /DA (/Helv 9 Tf 0.5 g)"
            "   /MK << /BG [1 0.5 0] /BC [] >> /Border [0 0 0] >>",
            // States nothing of its own; the size has to come back as the 0 the
            // form's own /DA asks for, not as a guess.
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Schlicht) /Rect [72 580 272 604] /F 4 >>",
            // /Ff bit 25 with a /MaxLen: eight cells, one character each.
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Kasten) /Ff 16777216 /MaxLen 8 /Q 1"
            "   /Rect [72 540 272 564] /F 4 /DA (/Helv 12 Tf 0 g) >>",
            // The comb flag on its own, with no /MaxLen to say how many cells.
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Ohne Kaesten) /Ff 16777216"
            "   /Rect [72 500 272 524] /F 4 /DA (/Helv 12 Tf 0 g) >>",
            // A push button, which is the only field that carries a caption.
            "<< /Type /Annot /Subtype /Widget /FT /Btn /T (Senden) /Ff 65536"
            "   /Rect [72 460 172 484] /F 4 /MK << /CA (Absenden) /BG [0.9 0.9 0.9] >> >>",
        };

        QPDFObjectHandle annots = QPDFObjectHandle::newArray();
        QPDFObjectHandle fields = QPDFObjectHandle::newArray();
        QPDFObjectHandle button;
        for (const char *definition : widgets) {
            QPDFObjectHandle widget = pdf.makeIndirectObject(QPDFObjectHandle::parse(definition));
            widget.replaceKey("/P", first.getObjectHandle());
            annots.appendItem(widget);
            fields.appendItem(widget);
            button = widget;
        }
        first.getObjectHandle().replaceKey("/Annots", annots);

        // Only the button gets an appearance stream, so that the two answers to
        // "has this field one?" are both represented.
        QPDFObjectHandle drawn = QPDFObjectHandle::newStream(&pdf, "0.9 g 0 0 100 24 re f\n");
        drawn.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
        drawn.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
        drawn.getDict().replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 100 24]"));
        QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
        appearance.replaceKey("/N", drawn);
        button.replaceKey("/AP", appearance);

        QPDFObjectHandle acroForm = QPDFObjectHandle::newDictionary();
        acroForm.replaceKey("/Fields", fields);
        acroForm.replaceKey("/DR", resources);
        // 0 Tf: the form leaves the size to whoever draws the field.
        acroForm.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 0 Tf 0 g"));
        pdf.getRoot().replaceKey("/AcroForm", pdf.makeIndirectObject(acroForm));

        QPDFWriter writer(pdf, QFile::encodeName(path).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

/**
 * A form of the two shapes a field can take that are not one box on one page.
 *
 * A radio group (one field, one value, three widgets, each with its own
 * rectangle and its own name for "chosen"), and a field drawn once on each of
 * two pages, which is how a reference number is repeated at the head of every
 * sheet. Both are ordinary; both are invisible to anything that believes a field
 * has a rectangle.
 */
bool writeGroupedFormPdf(const QString &path)
{
    try {
        QPDF pdf;
        pdf.emptyPDF();
        QPDFPageDocumentHelper documents(pdf);

        for (int i = 0; i < 2; ++i) {
            QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>");
            page.replaceKey("/Resources", QPDFObjectHandle::parse("<< >>"));
            page.replaceKey("/Contents", QPDFObjectHandle::newStream(&pdf, "\n"));
            documents.addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
        }
        std::vector<QPDFPageObjectHelper> pages = documents.getAllPages();

        QPDFObjectHandle font = pdf.makeIndirectObject(QPDFObjectHandle::parse(
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
        QPDFObjectHandle resources = QPDFObjectHandle::parse("<< /Font << >> >>");
        resources.getKey("/Font").replaceKey("/Helv", font);

        // /Ff bit 16, 32768: the group is a set of radio buttons rather than a
        // set of tick boxes, which is what makes its one value exclusive.
        QPDFObjectHandle group
            = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /FT /Btn /Ff 32768 /T (Anrede) /V /Off >>"));

        const char *buttons[] = {
            "<< /Type /Annot /Subtype /Widget /Rect [72 700 88 716] /F 4 /AS /Off"
            "   /AP << /N << /Frau << >> /Off << >> >> >> >>",
            "<< /Type /Annot /Subtype /Widget /Rect [160 700 176 716] /F 4 /AS /Off"
            "   /AP << /N << /Herr << >> /Off << >> >> >> >>",
            "<< /Type /Annot /Subtype /Widget /Rect [250 700 266 716] /F 4 /AS /Off"
            "   /AP << /N << /Divers << >> /Off << >> >> >> >>",
        };

        QPDFObjectHandle firstAnnots = QPDFObjectHandle::newArray();
        QPDFObjectHandle kids = QPDFObjectHandle::newArray();
        for (const char *definition : buttons) {
            QPDFObjectHandle widget = pdf.makeIndirectObject(QPDFObjectHandle::parse(definition));
            widget.replaceKey("/P", pages.at(0).getObjectHandle());
            widget.replaceKey("/Parent", group);
            kids.appendItem(widget);
            firstAnnots.appendItem(widget);
        }
        group.replaceKey("/Kids", kids);

        // One field, two widgets, one on each page: the same answer printed at
        // the head of every sheet.
        QPDFObjectHandle repeated = pdf.makeIndirectObject(
            QPDFObjectHandle::parse("<< /FT /Tx /T (Kopfzeile) /V (Muster) /DA (/Helv 9 Tf 0 g) >>"));
        QPDFObjectHandle heads = QPDFObjectHandle::newArray();
        QPDFObjectHandle secondAnnots = QPDFObjectHandle::newArray();
        for (int i = 0; i < 2; ++i) {
            QPDFObjectHandle widget = pdf.makeIndirectObject(QPDFObjectHandle::parse(
                "<< /Type /Annot /Subtype /Widget /Rect [400 740 560 760] /F 4 >>"));
            widget.replaceKey("/P", pages.at(size_t(i)).getObjectHandle());
            widget.replaceKey("/Parent", repeated);
            heads.appendItem(widget);
            (i == 0 ? firstAnnots : secondAnnots).appendItem(widget);
        }
        repeated.replaceKey("/Kids", heads);

        pages.at(0).getObjectHandle().replaceKey("/Annots", firstAnnots);
        pages.at(1).getObjectHandle().replaceKey("/Annots", secondAnnots);

        QPDFObjectHandle fields = QPDFObjectHandle::newArray();
        fields.appendItem(group);
        fields.appendItem(repeated);

        QPDFObjectHandle acroForm = QPDFObjectHandle::newDictionary();
        acroForm.replaceKey("/Fields", fields);
        acroForm.replaceKey("/DR", resources);
        acroForm.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 9 Tf 0 g"));
        pdf.getRoot().replaceKey("/AcroForm", pdf.makeIndirectObject(acroForm));

        QPDFWriter writer(pdf, QFile::encodeName(path).constData());
        writer.write();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

/**
 * What each widget of @p path is showing, keyed by the state it turns on with.
 *
 * /AS is the key the widget draws itself from, and it is the difference between
 * a group that holds the right answer and a group that holds the right answer
 * and looks blank, or looks as though every button in it were chosen.
 */
QHash<QString, QString> appearanceStates(const QString &path)
{
    QHash<QString, QString> shown;
    try {
        QPDF pdf;
        pdf.processFile(QFile::encodeName(path).constData());
        for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
            for (int a = 0; annots.isArray() && a < annots.getArrayNItems(); ++a) {
                QPDFObjectHandle widget = annots.getArrayItem(a);
                QPDFObjectHandle appearance = widget.getKey("/AP");
                QPDFObjectHandle normal
                    = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
                if (!normal.isDictionary()) {
                    continue;
                }
                for (const auto &[name, value] : normal.getDictAsMap()) {
                    Q_UNUSED(value)
                    if (name == "/Off") {
                        continue;
                    }
                    QPDFObjectHandle state = widget.getKey("/AS");
                    shown.insert(QString::fromStdString(name),
                                 state.isName() ? QString::fromStdString(state.getName()) : QString());
                }
            }
        }
    } catch (const std::exception &) {
        return {};
    }
    return shown;
}

} // namespace

/**
 * Filling in forms, checked by reading the answers back out of the file.
 *
 * The failure that matters here is quiet: a value set without an appearance
 * stream is invisible in about half the readers in use, printing ones included,
 * so a form that looks filled in on screen arrives blank. Every case that can
 * be checked by extracting the text of the page is checked that way.
 */
class TestForms : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void findsEveryKindOfField();
    void fillsTextAndShowsIt();
    void ticksABoxWithAnUnusualOnState();
    void refusesToTouchALockedField();
    void saysWhenAFieldNameIsWrong();
    void flatteningKeepsTheAnswersAndDropsTheForm();
    void leavesADocumentWithoutAFormAlone();

    void readsTheDefaultAppearance();
    void readsABackgroundInEveryColourSpace();
    void leavesUnstatedStylingUnset();
    void readsTheBorder();
    void readsACombField();
    void reportsTheScriptsWithoutRunningThem();
    void saysWhetherAFieldHasAnAppearance();

    void reportsEveryButtonOfARadioGroup();
    void choosingOneOfAGroupLeavesTheOthersOff();
    void reportsAFieldDrawnOnTwoPages();
    void reportsOneWidgetForAnOrdinaryField();

private:
    QTemporaryDir m_dir;
    QString m_form;
    QString m_styled;
    QString m_grouped;
};

void TestForms::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_form = m_dir.filePath(QStringLiteral("form.pdf"));
    QVERIFY(test::writeFormPdf(m_form));
    m_styled = m_dir.filePath(QStringLiteral("styled.pdf"));
    QVERIFY(writeStyledFormPdf(m_styled));
    m_grouped = m_dir.filePath(QStringLiteral("grouped.pdf"));
    QVERIFY(writeGroupedFormPdf(m_grouped));
}

void TestForms::findsEveryKindOfField()
{
    QVERIFY(Forms::hasForm(m_form));

    QString error;
    const QVector<FormField> fields = Forms::read(m_form, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(fields.size(), 5);

    const auto byName = [&fields](const QString &name) -> const FormField * {
        for (const FormField &field : fields) {
            if (field.name == name) {
                return &field;
            }
        }
        return nullptr;
    };

    const FormField *name = byName(QStringLiteral("Name"));
    QVERIFY(name);
    QCOMPARE(name->kind, FormField::Kind::Text);
    QVERIFY2(name->required, "the required flag was not read");
    QVERIFY(!name->multiline);
    QCOMPARE(name->label, QStringLiteral("Ihr Name")); // /TU, not the field name
    QCOMPARE(name->page, 0);
    QVERIFY2(qAbs(name->rect.x() - 72.0) < 1.0, qPrintable(QString::number(name->rect.x())));

    QVERIFY(byName(QStringLiteral("Bemerkung"))->multiline);

    const FormField *tick = byName(QStringLiteral("Einverstanden"));
    QCOMPARE(tick->kind, FormField::Kind::Checkbox);
    QCOMPARE(tick->value, QStringLiteral("/Off"));
    // The state this box actually understands, which is not /Yes.
    QCOMPARE(tick->options, QStringList { QStringLiteral("/Ja") });

    const FormField *country = byName(QStringLiteral("Land"));
    QCOMPARE(country->kind, FormField::Kind::Choice);
    QCOMPARE(country->options.size(), 3);
    QVERIFY(country->options.contains(QStringLiteral("Schweiz")));

    QVERIFY(byName(QStringLiteral("Aktenzeichen"))->readOnly);
}

void TestForms::fillsTextAndShowsIt()
{
    const QString out = m_dir.filePath(QStringLiteral("filled.pdf"));

    int filled = 0;
    QStringList notes;
    QString error;
    QVERIFY2(Forms::fill(m_form, out,
                         { { QStringLiteral("Name"), QStringLiteral("Tom Bueng") },
                           { QStringLiteral("Land"), QStringLiteral("Schweiz") },
                           { QStringLiteral("Bemerkung"), QStringLiteral("Zwei Worte") } },
                         &filled, &notes, &error),
             qPrintable(error));
    QCOMPARE(filled, 3);

    // Read back through the file, not through our own memory.
    const QVector<FormField> after = Forms::read(out, &error);
    for (const FormField &field : after) {
        if (field.name == QLatin1String("Name")) {
            QCOMPARE(field.value, QStringLiteral("Tom Bueng"));
        }
        if (field.name == QLatin1String("Land")) {
            QCOMPARE(field.value, QStringLiteral("Schweiz"));
        }
    }

    // And the answer is actually drawn. Without an appearance stream the value
    // is in the file and invisible, which is the whole trap.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, out, nullptr));
    const QString shown = backend.extractText(1, 0);
    QVERIFY2(shown.contains(QStringLiteral("Tom Bueng")), qPrintable(shown));
    QVERIFY2(shown.contains(QStringLiteral("Schweiz")), qPrintable(shown));
}

void TestForms::ticksABoxWithAnUnusualOnState()
{
    const QString out = m_dir.filePath(QStringLiteral("ticked.pdf"));

    int filled = 0;
    QString error;
    QVERIFY2(Forms::fill(m_form, out, { { QStringLiteral("Einverstanden"), QStringLiteral("ja") } }, &filled, nullptr,
                         &error),
             qPrintable(error));
    QCOMPARE(filled, 1);

    // Writing /Yes into a box whose appearance only knows /Ja sets a value the
    // box cannot show, which looks like a broken reader rather than a bug here.
    const QVector<FormField> after = Forms::read(out, &error);
    for (const FormField &field : after) {
        if (field.name == QLatin1String("Einverstanden")) {
            QCOMPARE(field.value, QStringLiteral("/Ja"));
        }
    }

    // And unticking again.
    const QString off = m_dir.filePath(QStringLiteral("unticked.pdf"));
    QVERIFY2(Forms::fill(out, off, { { QStringLiteral("Einverstanden"), QStringLiteral("nein") } }, &filled, nullptr,
                         &error),
             qPrintable(error));
    for (const FormField &field : Forms::read(off, &error)) {
        if (field.name == QLatin1String("Einverstanden")) {
            QCOMPARE(field.value, QStringLiteral("/Off"));
        }
    }
}

void TestForms::refusesToTouchALockedField()
{
    const QString out = m_dir.filePath(QStringLiteral("locked.pdf"));

    int filled = 0;
    QStringList notes;
    QString error;
    QVERIFY2(Forms::fill(m_form, out, { { QStringLiteral("Aktenzeichen"), QStringLiteral("Versuch") } }, &filled,
                         &notes, &error),
             qPrintable(error));

    QCOMPARE(filled, 0);
    QCOMPARE(notes.size(), 1);
    QVERIFY2(!notes.constFirst().isEmpty(), "the locked field was skipped without a word");

    for (const FormField &field : Forms::read(out, &error)) {
        if (field.name == QLatin1String("Aktenzeichen")) {
            QCOMPARE(field.value, QStringLiteral("AZ-2026-001"));
        }
    }
}

void TestForms::saysWhenAFieldNameIsWrong()
{
    const QString out = m_dir.filePath(QStringLiteral("typo.pdf"));

    int filled = 0;
    QStringList notes;
    QString error;
    QVERIFY2(Forms::fill(m_form, out,
                         { { QStringLiteral("Name"), QStringLiteral("Tom") },
                           { QStringLiteral("Naem"), QStringLiteral("Tom") } },
                         &filled, &notes, &error),
             qPrintable(error));

    // One filled, one reported. A typo that fills nothing and says nothing
    // looks exactly like success.
    QCOMPARE(filled, 1);
    QCOMPARE(notes.size(), 1);
    QVERIFY(notes.constFirst().contains(QStringLiteral("Naem")));
}

void TestForms::flatteningKeepsTheAnswersAndDropsTheForm()
{
    const QString filledPath = m_dir.filePath(QStringLiteral("toflatten.pdf"));
    int filled = 0;
    QString error;
    QVERIFY2(Forms::fill(m_form, filledPath, { { QStringLiteral("Name"), QStringLiteral("Tom Bueng") } }, &filled,
                         nullptr, &error),
             qPrintable(error));

    const QString flat = m_dir.filePath(QStringLiteral("flat.pdf"));
    int flattened = 0;
    QVERIFY2(Forms::flatten(filledPath, flat, &flattened, &error), qPrintable(error));
    QCOMPARE(flattened, 5);

    // No form left…
    QVERIFY2(!Forms::hasForm(flat), "the form dictionary survived flattening");
    QVERIFY(Forms::read(flat, &error).isEmpty());

    // …and the answer is still on the page.
    PopplerBackend backend;
    QVERIFY(backend.addDocument(1, flat, nullptr));
    QVERIFY2(backend.extractText(1, 0).contains(QStringLiteral("Tom Bueng")),
             "flattening lost the answer it was supposed to make permanent");
}

void TestForms::leavesADocumentWithoutAFormAlone()
{
    const QString plain = m_dir.filePath(QStringLiteral("plain.pdf"));
    QVERIFY(test::writeSamplePdf(plain, 2));

    QVERIFY(!Forms::hasForm(plain));
    QString error;
    QVERIFY(Forms::read(plain, &error).isEmpty());
    QVERIFY(error.isEmpty()); // No form is not a failure.

    QVERIFY(!Forms::fill(plain, m_dir.filePath(QStringLiteral("nope.pdf")),
                         { { QStringLiteral("Name"), QStringLiteral("x") } }, nullptr, nullptr, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!Forms::flatten(plain, m_dir.filePath(QStringLiteral("nope2.pdf")), nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void TestForms::readsTheDefaultAppearance()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *amount = fieldNamed(fields, QStringLiteral("Betrag"));
    QVERIFY(amount);
    QCOMPARE(amount->fontResource, QStringLiteral("Helv"));
    // The one number in this file that a strtod under a comma locale would read
    // as 10, and nothing would warn.
    QCOMPARE(amount->fontSize, 10.5);
    QCOMPARE(amount->textColour, QColor::fromRgbF(0.2f, 0.4f, 0.6f));
    QCOMPARE(amount->alignment, 2);

    // A CMYK /DA, where the same trap costs three quarters of the ink.
    const FormField *grey = fieldNamed(fields, QStringLiteral("Grau"));
    QVERIFY(grey);
    QCOMPARE(grey->textColour, QColor::fromRgbF(0.75f, 0.75f, 0.75f));

    QCOMPARE(fieldNamed(fields, QStringLiteral("Bunt"))->textColour, QColor::fromRgbF(0.5f, 0.5f, 0.5f));

    // No /DA of its own, so the form's own applies, and its size of 0 means
    // "fit it to the box", which has to survive as 0 rather than become a guess.
    const FormField *plain = fieldNamed(fields, QStringLiteral("Schlicht"));
    QVERIFY(plain);
    QCOMPARE(plain->fontResource, QStringLiteral("Helv"));
    QCOMPARE(plain->fontSize, 0.0);
    QCOMPARE(plain->textColour, QColor::fromRgbF(0.0f, 0.0f, 0.0f));
    QCOMPARE(plain->alignment, 0);
}

void TestForms::readsABackgroundInEveryColourSpace()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // One number is grey…
    QCOMPARE(fieldNamed(fields, QStringLiteral("Betrag"))->background, QColor::fromRgbF(0.75f, 0.75f, 0.75f));
    // …three are RGB…
    QCOMPARE(fieldNamed(fields, QStringLiteral("Bunt"))->background, QColor::fromRgbF(1.0f, 0.5f, 0.0f));
    // …and four are CMYK, which is not three with one ignored.
    QCOMPARE(fieldNamed(fields, QStringLiteral("Grau"))->background, QColor::fromRgbF(0.5f, 0.5f, 0.5f));

    QCOMPARE(fieldNamed(fields, QStringLiteral("Betrag"))->border, QColor::fromRgbF(1.0f, 0.0f, 0.0f));
    QCOMPARE(fieldNamed(fields, QStringLiteral("Senden"))->background, QColor::fromRgbF(0.9f, 0.9f, 0.9f));
}

void TestForms::leavesUnstatedStylingUnset()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A field with no /MK asks for no colours at all. Answering "black" here
    // would paint a box over a form that never wanted one, and nothing
    // downstream could tell the difference.
    const FormField *plain = fieldNamed(fields, QStringLiteral("Schlicht"));
    QVERIFY(plain);
    QVERIFY2(!plain->background.isValid(), "a field with no /MK came back with a background");
    QVERIFY2(!plain->border.isValid(), "a field with no /MK came back with a border colour");
    QVERIFY(plain->caption.isEmpty());
    QCOMPARE(plain->maxLength, -1);

    // An empty /BC is the same statement made explicitly: transparent.
    const FormField *colourful = fieldNamed(fields, QStringLiteral("Bunt"));
    QVERIFY(colourful);
    QVERIFY2(!colourful->border.isValid(), "an empty /BC array was read as black");
    QVERIFY(colourful->background.isValid());

    QCOMPARE(fieldNamed(fields, QStringLiteral("Senden"))->caption, QStringLiteral("Absenden"));
    QVERIFY(fieldNamed(fields, QStringLiteral("Betrag"))->caption.isEmpty());
}

void TestForms::readsTheBorder()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *amount = fieldNamed(fields, QStringLiteral("Betrag"));
    QCOMPARE(amount->borderWidth, 2.0);
    QCOMPARE(amount->borderStyle, FormField::BorderStyle::Dashed);

    // No /BS: the specification's default is a solid one-point border.
    const FormField *plain = fieldNamed(fields, QStringLiteral("Schlicht"));
    QCOMPARE(plain->borderWidth, 1.0);
    QCOMPARE(plain->borderStyle, FormField::BorderStyle::Solid);

    // /Border [0 0 0], which is how a document older than /BS says "none".
    QCOMPARE(fieldNamed(fields, QStringLiteral("Bunt"))->borderWidth, 0.0);
}

void TestForms::readsACombField()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *boxes = fieldNamed(fields, QStringLiteral("Kasten"));
    QVERIFY(boxes);
    QVERIFY2(boxes->comb, "the comb flag was not read");
    QCOMPARE(boxes->maxLength, 8);
    QCOMPARE(boxes->alignment, 1);

    // The flag without a /MaxLen says nothing: there is no number of cells to
    // draw, and the specification tells a reader to ignore it.
    QVERIFY2(!fieldNamed(fields, QStringLiteral("Ohne Kaesten"))->comb,
             "a comb flag with no /MaxLen was reported as a comb field");

    // And a length without the flag is a limit, not a row of boxes.
    const FormField *amount = fieldNamed(fields, QStringLiteral("Betrag"));
    QCOMPARE(amount->maxLength, 12);
    QVERIFY(!amount->comb);
}

void TestForms::reportsTheScriptsWithoutRunningThem()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *amount = fieldNamed(fields, QStringLiteral("Betrag"));
    QVERIFY(amount);
    QVERIFY2(amount->formatScript.contains(QStringLiteral("AFNumber_Format")), qPrintable(amount->formatScript));
    QVERIFY2(amount->keystrokeScript.contains(QStringLiteral("AFNumber_Keystroke")),
             qPrintable(amount->keystrokeScript));
    QVERIFY2(amount->validateScript.contains(QStringLiteral("event.rc")), qPrintable(amount->validateScript));
    QVERIFY2(amount->calculateScript.contains(QStringLiteral("1.19")), qPrintable(amount->calculateScript));

    // Reported verbatim, down to the punctuation: anything else would be an
    // interpretation, and interpreting these is not this class's job.
    QCOMPARE(amount->validateScript, QStringLiteral("if (event.value < 0) event.rc = false;"));

    const FormField *plain = fieldNamed(fields, QStringLiteral("Schlicht"));
    QVERIFY(plain->formatScript.isEmpty());
    QVERIFY(plain->validateScript.isEmpty());
}

void TestForms::saysWhetherAFieldHasAnAppearance()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_styled, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY2(fieldNamed(fields, QStringLiteral("Senden"))->hasAppearance, "an /AP /N stream went unnoticed");
    QVERIFY2(!fieldNamed(fields, QStringLiteral("Schlicht"))->hasAppearance,
             "a field with no /AP was reported as having one, so nothing will draw a substitute for it");

    // A tick box keeps its appearances in a subdictionary, one per state, which
    // is still an appearance.
    const QVector<FormField> plainForm = Forms::read(m_form, &error);
    QVERIFY2(fieldNamed(plainForm, QStringLiteral("Einverstanden"))->hasAppearance,
             "a per-state /AP /N subdictionary was not recognised");
}

void TestForms::reportsEveryButtonOfARadioGroup()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_grouped, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *group = fieldNamed(fields, QStringLiteral("Anrede"));
    QVERIFY(group);
    QCOMPARE(group->kind, FormField::Kind::Radio);

    // Three buttons, not one. A group reported as a single box is drawn as a
    // single box, and then one click sets the value of all three.
    QCOMPARE(group->widgets.size(), 3);

    QStringList states;
    for (const FormField::Widget &widget : group->widgets) {
        QCOMPARE(widget.page, 0);
        QVERIFY2(!widget.rect.isEmpty(), "a button of the group came back with no rectangle");
        QVERIFY2(widget.hasAppearance, "a button of the group lost its /AP");
        states << widget.onState;
    }
    QCOMPARE(states, QStringList({ QStringLiteral("/Frau"), QStringLiteral("/Herr"), QStringLiteral("/Divers") }));

    // Each button stands somewhere else on the page, which is the whole reason
    // one rectangle for the group cannot be right.
    QVERIFY(group->widgets.at(0).rect != group->widgets.at(1).rect);
    QVERIFY(group->widgets.at(1).rect != group->widgets.at(2).rect);

    // And what the field says about itself is still the first of them, because
    // everything that only ever wanted one box reads that.
    QCOMPARE(group->page, group->widgets.constFirst().page);
    QCOMPARE(group->rect, group->widgets.constFirst().rect);
    QCOMPARE(group->hasAppearance, group->widgets.constFirst().hasAppearance);
}

void TestForms::choosingOneOfAGroupLeavesTheOthersOff()
{
    const QString out = m_dir.filePath(QStringLiteral("chosen.pdf"));

    int filled = 0;
    QString error;
    QVERIFY2(Forms::fill(m_grouped, out, { { QStringLiteral("Anrede"), QStringLiteral("/Herr") } }, &filled, nullptr,
                         &error),
             qPrintable(error));
    QCOMPARE(filled, 1);

    QVector<FormField> after = Forms::read(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(fieldNamed(after, QStringLiteral("Anrede"))->value, QStringLiteral("/Herr"));

    // The value is only half of it: without an /AS per widget the group holds
    // the right answer and every button of it looks blank; or, drawn from the
    // group's value alone, every button looks chosen.
    QHash<QString, QString> shown = appearanceStates(out);
    QCOMPARE(shown.value(QStringLiteral("/Herr")), QStringLiteral("/Herr"));
    QCOMPARE(shown.value(QStringLiteral("/Frau")), QStringLiteral("/Off"));
    QCOMPARE(shown.value(QStringLiteral("/Divers")), QStringLiteral("/Off"));

    // Changing one's mind moves the answer rather than adding to it.
    const QString again = m_dir.filePath(QStringLiteral("chosen-again.pdf"));
    QVERIFY2(Forms::fill(out, again, { { QStringLiteral("Anrede"), QStringLiteral("Divers") } }, &filled, nullptr,
                         &error),
             qPrintable(error));

    after = Forms::read(again, &error);
    QCOMPARE(fieldNamed(after, QStringLiteral("Anrede"))->value, QStringLiteral("/Divers"));

    shown = appearanceStates(again);
    QCOMPARE(shown.value(QStringLiteral("/Divers")), QStringLiteral("/Divers"));
    QCOMPARE(shown.value(QStringLiteral("/Herr")), QStringLiteral("/Off"));
    QCOMPARE(shown.value(QStringLiteral("/Frau")), QStringLiteral("/Off"));
}

void TestForms::reportsAFieldDrawnOnTwoPages()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_grouped, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const FormField *repeated = fieldNamed(fields, QStringLiteral("Kopfzeile"));
    QVERIFY(repeated);
    QCOMPARE(repeated->kind, FormField::Kind::Text);
    QCOMPARE(repeated->widgets.size(), 2);
    QCOMPARE(repeated->widgets.at(0).page, 0);
    QCOMPARE(repeated->widgets.at(1).page, 1);

    // One field, one answer, drawn twice.
    QCOMPARE(repeated->value, QStringLiteral("Muster"));
    QCOMPARE(repeated->page, 0);

    // Nothing to choose, so nothing to name: an on state on a text field would
    // be a state its appearance does not have.
    QVERIFY(repeated->widgets.at(0).onState.isEmpty());
    QVERIFY(repeated->widgets.at(1).onState.isEmpty());
}

void TestForms::reportsOneWidgetForAnOrdinaryField()
{
    QString error;
    const QVector<FormField> fields = Forms::read(m_form, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // Every field has widgets, not only the ones drawn more than once, so that
    // nothing has to ask which of the two ways to read a field it is holding.
    for (const FormField &field : fields) {
        QCOMPARE(field.widgets.size(), 1);
        QCOMPARE(field.widgets.constFirst().page, field.page);
        QCOMPARE(field.widgets.constFirst().rect, field.rect);
        QCOMPARE(field.widgets.constFirst().hasAppearance, field.hasAppearance);
    }

    const FormField *tick = fieldNamed(fields, QStringLiteral("Einverstanden"));
    QVERIFY(tick);
    QCOMPARE(tick->widgets.constFirst().onState, QStringLiteral("/Ja"));
    QVERIFY(fieldNamed(fields, QStringLiteral("Name"))->widgets.constFirst().onState.isEmpty());
}

QTEST_MAIN(TestForms)

#include "tst_forms.moc"
