/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "CompareDialog.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ps {

CompareDialog::CompareDialog(RenderBackend *backend, const QString &leftPdf, const QString &rightPdf,
                             const Compare::Report &report, QWidget *parent)
    : QDialog(parent)
    , m_backend(backend)
    , m_left(leftPdf)
    , m_right(rightPdf)
    , m_report(report)
    , m_list(new QListWidget(this))
    , m_sheet(new QLabel(this))
    , m_words(new QLabel(this))
    , m_scroll(new QScrollArea(this))
{
    setWindowTitle(i18nc("@title:window", "What Changed"));
    resize(1200, 860);

    for (int i = 0; i < m_report.pages.size(); ++i) {
        const Compare::PageResult &page = m_report.pages.at(i);
        if (page.change == Compare::Change::Same) {
            continue; // Only what changed, as any comparison tool shows.
        }
        const int number = (page.leftPage >= 0 ? page.leftPage : page.rightPage) + 1;
        m_list->addItem(i18nc("@item one changed page: number and what changed", "Page %1: %2", number,
                              Compare::describe(page.change)));
        m_rows.append(i);
    }

    m_sheet->setAlignment(Qt::AlignCenter);
    m_scroll->setWidget(m_sheet);
    m_scroll->setWidgetResizable(true);
    m_scroll->setMinimumWidth(600);

    m_words->setWordWrap(true);
    m_words->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_words->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    auto *summary = new QLabel(i18ncp("@info", "%1 page changed, out of %2.", "%1 pages changed, out of %2.",
                                      m_report.changedPages, m_report.pages.size()),
                               this);

    auto *left = new QVBoxLayout;
    left->addWidget(summary);
    left->addWidget(m_list, 2);
    left->addWidget(m_words, 1);

    auto *columns = new QHBoxLayout;
    columns->addLayout(left, 2);
    columns->addWidget(m_scroll, 3);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &CompareDialog::showEntry);
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

void CompareDialog::showEntry(int index)
{
    if (index < 0 || index >= m_rows.size()) {
        return;
    }
    const Compare::PageResult &page = m_report.pages.at(m_rows.at(index));

    // Rendered on demand rather than all at once: a two-hundred-page comparison
    // would otherwise spend a minute drawing sheets nobody looks at.
    const QImage sheet = Compare::visualise(m_backend, m_left, m_right, page.leftPage, page.rightPage, {});
    if (sheet.isNull()) {
        m_sheet->setText(i18n("This page could not be drawn."));
        m_sheet->setPixmap({});
    } else {
        m_sheet->setPixmap(QPixmap::fromImage(sheet));
        m_sheet->resize(sheet.size());
    }

    QString description;
    if (!page.removedWords.isEmpty()) {
        description += QStringLiteral("<p><b>%1</b><br>%2</p>")
                           .arg(i18nc("@label words that are no longer there", "Gone:"),
                                page.removedWords.join(QLatin1Char(' ')).toHtmlEscaped());
    }
    if (!page.addedWords.isEmpty()) {
        description += QStringLiteral("<p><b>%1</b><br>%2</p>")
                           .arg(i18nc("@label words that are new", "New:"),
                                page.addedWords.join(QLatin1Char(' ')).toHtmlEscaped());
    }
    if (page.change == Compare::Change::VisualOnly) {
        description += QStringLiteral("<p>%1</p>")
                           .arg(i18n("The words are the same but the page looks different: a picture changed, or a "
                                     "font was swapped. %1% of the page differs.",
                                     QString::number(page.pixelDifference * 100.0, 'f', 1)));
    }
    m_words->setText(description);
}

} // namespace ps
