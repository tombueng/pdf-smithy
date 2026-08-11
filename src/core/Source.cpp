/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Source.h"
#include "PdfFile.h"

#include <QFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

namespace ps {

Source::Source() = default;
Source::~Source() = default;

std::unique_ptr<Source> Source::open(const QString &path, QString *error, const QString &password, Trouble *trouble)
{
    if (trouble) {
        *trouble = Trouble::None;
    }

    // Not make_unique: the constructor is private on purpose, so that a Source
    // can only ever exist in a successfully-opened state.
    std::unique_ptr<Source> source(new Source);
    source->m_path = path;
    source->m_password = password;
    source->m_qpdf = std::make_unique<QPDF>();

    try {
        PdfFile::open(*source->m_qpdf, path, password);
        // Cached once: QPDF walks the page tree on every getAllPages() call,
        // which would turn writing a large document into a quadratic loop.
        source->m_pages = QPDFPageDocumentHelper(*source->m_qpdf).getAllPages();
        source->m_openWarning = PdfFile::warningSummary(*source->m_qpdf);
    } catch (const QPDFExc &e) {
        // Told apart by QPDF's own code rather than by reading its message,
        // which is neither stable nor translated.
        const bool wantsPassword = e.getErrorCode() == qpdf_e_password;
        if (trouble) {
            *trouble = wantsPassword ? Trouble::NeedsPassword : Trouble::Damaged;
        }
        if (error) {
            *error = wantsPassword && password.isEmpty() ? i18n("“%1” needs a password.", path)
                : wantsPassword                          ? i18n("That password does not open “%1”.", path)
                                                         : QString::fromUtf8(e.what());
        }
        return nullptr;
    } catch (const std::exception &e) {
        if (trouble) {
            *trouble = Trouble::Damaged;
        }
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return nullptr;
    }

    return source;
}

QPDFPageObjectHelper Source::page(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_pages.size())) {
        return QPDFPageObjectHelper(QPDFObjectHandle::newNull());
    }
    return m_pages[static_cast<size_t>(index)];
}

} // namespace ps
