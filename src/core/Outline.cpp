/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Outline.h"

#include "PdfFile.h"
#include "PdfGeometry.h"

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFOutlineObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <map>
#include <utility>

namespace ps {

namespace {

/** Page object to index, so a destination costs a lookup rather than a search. */
std::map<QPDFObjGen, int> pageIndexOf(QPDF &pdf)
{
    std::map<QPDFObjGen, int> index;
    const std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();
    for (size_t i = 0; i < pages.size(); ++i) {
        index[pages[i].getObjectHandle().getObjGen()] = int(i);
    }
    return index;
}

QVector<OutlineItem> readLevel(std::vector<QPDFOutlineObjectHelper> entries, const std::map<QPDFObjGen, int> &index,
                               int depth)
{
    QVector<OutlineItem> items;
    // A malformed file can point an entry at its own ancestor. Depth is the
    // cheap way to refuse to follow it forever.
    if (depth > 32) {
        return items;
    }

    for (QPDFOutlineObjectHelper &entry : entries) {
        OutlineItem item;
        item.title = QString::fromStdString(entry.getTitle());

        QPDFObjectHandle destination = entry.getDestPage();
        if (!destination.isNull()) {
            const auto found = index.find(destination.getObjGen());
            if (found != index.end()) {
                item.page = found->second;
            }
        }

        QPDFObjectHandle count = entry.getObjectHandle().getKey("/Count");
        item.open = !count.isInteger() || count.getIntValueAsInt() >= 0;

        QPDFObjectHandle flags = entry.getObjectHandle().getKey("/F");
        if (flags.isInteger()) {
            const int bits = flags.getIntValueAsInt();
            item.italic = (bits & 1) != 0;
            item.bold = (bits & 2) != 0;
        }

        QPDFObjectHandle colour = entry.getObjectHandle().getKey("/C");
        if (colour.isArray() && colour.getArrayNItems() == 3) {
            // Three components in 0..1, read through numberAt() rather than
            // QPDF's own accessor: strtod follows LC_NUMERIC, so on a German
            // desktop [0.2 0.4 0.6] would come back as pure black.
            const double red = qBound(0.0, PdfGeometry::numberAt(colour, 0, 0.0), 1.0);
            const double green = qBound(0.0, PdfGeometry::numberAt(colour, 1, 0.0), 1.0);
            const double blue = qBound(0.0, PdfGeometry::numberAt(colour, 2, 0.0), 1.0);
            item.colour = QColor::fromRgbF(float(red), float(green), float(blue));
        }

        item.children = readLevel(entry.getKids(), index, depth + 1);

        // An entry with neither a destination nor children is decoration at
        // best; keeping it would put a dead line in the sidebar.
        if (item.page >= 0 || !item.children.isEmpty()) {
            items.append(item);
        }
    }
    return items;
}

/** Builds the /Outlines subtree for one level, returning first, last and count. */
struct Built {
    QPDFObjectHandle first;
    QPDFObjectHandle last;
    int visible = 0;
};

Built buildLevel(QPDF &pdf, const QVector<OutlineItem> &items, QPDFObjectHandle parent,
                 const std::vector<QPDFPageObjectHelper> &pages)
{
    Built built;
    QPDFObjectHandle previous;

    for (const OutlineItem &item : items) {
        const bool hasPage = item.page >= 0 && item.page < int(pages.size());
        if (!hasPage && item.children.isEmpty()) {
            continue;
        }

        QPDFObjectHandle object = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        object.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(item.title.toStdString()));
        object.replaceKey("/Parent", parent);

        const int flags = (item.italic ? 1 : 0) | (item.bold ? 2 : 0);
        if (flags != 0) {
            object.replaceKey("/F", QPDFObjectHandle::newInteger(flags));
        }

        if (item.colour.isValid()) {
            QPDFObjectHandle colour = QPDFObjectHandle::newArray();
            // QPDF's newReal(double) formats through snprintf, which follows
            // LC_NUMERIC and would write "0,4" into the file.
            for (const double component :
                 { double(item.colour.redF()), double(item.colour.greenF()), double(item.colour.blueF()) }) {
                colour.appendItem(QPDFObjectHandle::newReal(PdfGeometry::number(component)));
            }
            object.replaceKey("/C", colour);
        }

        if (hasPage) {
            // /Fit rather than /XYZ: a bookmark should show the page, not
            // restore whatever zoom the last editor happened to be using.
            QPDFObjectHandle destination = QPDFObjectHandle::newArray();
            destination.appendItem(pages.at(size_t(item.page)).getObjectHandle());
            destination.appendItem(QPDFObjectHandle::newName("/Fit"));
            object.replaceKey("/Dest", destination);
        }

        const Built children = buildLevel(pdf, item.children, object, pages);
        if (children.first.isInitialized()) {
            object.replaceKey("/First", children.first);
            object.replaceKey("/Last", children.last);
            // Negative means closed, and the magnitude is how many would show.
            object.replaceKey("/Count", QPDFObjectHandle::newInteger(item.open ? children.visible : -children.visible));
        }

        if (previous.isInitialized()) {
            previous.replaceKey("/Next", object);
            object.replaceKey("/Prev", previous);
        } else {
            built.first = object;
        }
        previous = object;
        built.last = object;
        built.visible += 1 + (item.open ? children.visible : 0);
    }

    return built;
}

} // namespace

QVector<OutlineItem> Outline::readFrom(QPDF &pdf)
{
    const std::map<QPDFObjGen, int> index = pageIndexOf(pdf);
    QPDFOutlineDocumentHelper outlines(pdf);
    return readLevel(outlines.getTopLevelOutlines(), index, 0);
}

QVector<OutlineItem> Outline::read(const QString &pdf, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        return readFrom(document);
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return {};
    }
}

QVector<OutlineItem> Outline::fromHeadings(const QVector<Convert::Heading> &headings)
{
    QVector<OutlineItem> items;

    // The chain of child indexes leading to the entry last added, and the
    // heading level each link of it was made from. Indexes rather than
    // pointers, because appending to a QVector moves what is already in it and
    // a pointer held from one heading to the next would be left dangling.
    QVector<int> path;
    QVector<int> levels;

    for (const Convert::Heading &heading : headings) {
        const QString title = heading.text.simplified();
        if (title.isEmpty() || heading.page < 0) {
            continue;
        }

        // Climb out of everything at this level or deeper: the new entry
        // belongs under the nearest heading above it that was shallower.
        while (!levels.isEmpty() && levels.constLast() >= heading.level) {
            levels.removeLast();
            path.removeLast();
        }

        QVector<OutlineItem> *siblings = &items;
        for (const int index : std::as_const(path)) {
            siblings = &(*siblings)[index].children;
        }

        OutlineItem item;
        item.title = title;
        item.page = heading.page;
        siblings->append(item);

        levels.append(heading.level);
        path.append(int(siblings->size()) - 1);
    }

    return items;
}

void Outline::applyTo(QPDF &pdf, const QVector<OutlineItem> &items)
{
    QPDFObjectHandle root = pdf.getRoot();
    if (items.isEmpty()) {
        root.removeKey("/Outlines");
        root.removeKey("/PageMode");
        return;
    }

    const std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(pdf).getAllPages();

    QPDFObjectHandle outlines = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));

    const Built built = buildLevel(pdf, items, outlines, pages);
    if (!built.first.isInitialized()) {
        root.removeKey("/Outlines");
        return;
    }

    outlines.replaceKey("/First", built.first);
    outlines.replaceKey("/Last", built.last);
    outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(built.visible));
    root.replaceKey("/Outlines", outlines);

    // Ask readers to show the panel. A table of contents nobody can see is the
    // same as none at all for most people.
    root.replaceKey("/PageMode", QPDFObjectHandle::newName("/UseOutlines"));
}

bool Outline::write(const QString &inputPdf, const QString &outputPdf, const QVector<OutlineItem> &items,
                    QString *error)
{
    try {
        QPDF pdf;
        PdfFile::open(pdf, inputPdf);
        applyTo(pdf, items);

        QPDFWriter writer(pdf, outputPdf.toUtf8().constData());
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_preserve);
        writer.write();
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

int Outline::count(const QVector<OutlineItem> &items)
{
    int total = 0;
    for (const OutlineItem &item : items) {
        total += 1 + count(item.children);
    }
    return total;
}

QVector<QPair<const OutlineItem *, int>> Outline::flatten(const QVector<OutlineItem> &items)
{
    QVector<QPair<const OutlineItem *, int>> flat;
    // An explicit stack rather than recursion, so the returned pointers stay
    // valid and the order is exactly what a sidebar shows.
    struct Frame {
        const QVector<OutlineItem> *items;
        int index;
        int depth;
    };
    QVector<Frame> stack { { &items, 0, 0 } };

    while (!stack.isEmpty()) {
        Frame &frame = stack.last();
        if (frame.index >= frame.items->size()) {
            stack.removeLast();
            continue;
        }
        const OutlineItem &item = frame.items->at(frame.index);
        ++frame.index;
        flat.append({ &item, frame.depth });
        if (!item.children.isEmpty()) {
            stack.append({ &item.children, 0, frame.depth + 1 });
        }
    }
    return flat;
}

} // namespace ps
