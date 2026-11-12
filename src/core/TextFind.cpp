/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "TextFind.h"

#include "PdfFile.h"
#include "TextEdit.h"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

int pageCount(const QString &pdf, QString *error)
{
    try {
        QPDF document;
        PdfFile::open(document, pdf);
        return static_cast<int>(QPDFPageDocumentHelper(document).getAllPages().size());
    } catch (const std::exception &problem) {
        if (error) {
            *error = QString::fromUtf8(problem.what());
        }
        return -1;
    }
}

/** A little of @p text either side of the match at @p pos, for a result line. */
QString snippetAround(const QString &text, int pos, int length)
{
    constexpr int margin = 32;
    const int start = qMax(0, pos - margin);
    const int end = qMin<int>(text.size(), pos + length + margin);
    QString piece = text.mid(start, end - start).simplified();
    if (start > 0) {
        piece.prepend(QChar(0x2026));
    }
    if (end < text.size()) {
        piece.append(QChar(0x2026));
    }
    return piece;
}

} // namespace

namespace TextFind {

QVector<Hit> search(const QString &pdf, const QString &term, Qt::CaseSensitivity cs, QString *error)
{
    QVector<Hit> hits;
    if (term.isEmpty()) {
        return hits;
    }

    const int pages = pageCount(pdf, error);
    if (pages < 0) {
        return hits;
    }

    for (int page = 0; page < pages; ++page) {
        QString pageError;
        const QVector<TextEdit::Run> runs = TextEdit::runsOn(pdf, page, &pageError);

        QString text;
        for (const TextEdit::Run &run : runs) {
            if (!text.isEmpty()) {
                text += u' ';
            }
            text += run.text;
        }

        int count = 0;
        int first = -1;
        int from = 0;
        while (true) {
            const int at = text.indexOf(term, from, cs);
            if (at < 0) {
                break;
            }
            if (first < 0) {
                first = at;
            }
            ++count;
            from = at + term.size();
        }

        if (count > 0) {
            Hit hit;
            hit.page = page;
            hit.count = count;
            hit.snippet = snippetAround(text, first, term.size());
            hits.push_back(hit);
        }
    }

    return hits;
}

} // namespace TextFind

} // namespace ps
