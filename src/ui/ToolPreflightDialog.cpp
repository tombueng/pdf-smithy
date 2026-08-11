/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ToolDialogs.h"

#include "ToolSupport.h"
#include "core/Preflight.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace ps::tools {

namespace {

/** Where a finding's fix id is kept on its row. */
constexpr int FixRole = Qt::UserRole + 1;

QIcon iconFor(Preflight::Severity severity)
{
    switch (severity) {
    case Preflight::Severity::Error:
        return QIcon::fromTheme(u"dialog-error"_s);
    case Preflight::Severity::Warning:
        return QIcon::fromTheme(u"dialog-warning"_s);
    case Preflight::Severity::Note:
        break;
    }
    return QIcon::fromTheme(u"dialog-information"_s);
}

QString headingFor(Preflight::Severity severity, int count)
{
    switch (severity) {
    case Preflight::Severity::Error:
        return i18ncp("@item:inlistbox group heading", "%1 error", "%1 errors", count);
    case Preflight::Severity::Warning:
        return i18ncp("@item:inlistbox group heading", "%1 warning", "%1 warnings", count);
    case Preflight::Severity::Note:
        break;
    }
    return i18ncp("@item:inlistbox group heading", "%1 note", "%1 notes", count);
}

/**
 * The preflight report, its fixes, and what it did not look at.
 *
 * Deliberately three parts, not two. Every preflight tool shows problems and
 * offers to mend them; the third part (the rules the profile named and the run
 * could not judge) is the one that decides whether the other two can be
 * trusted. It gets its own box on the dialog rather than a line in a log,
 * because a user who reads "no problems found" without reading "and here are
 * the six things nobody checked" will send the file.
 */
class PreflightDialog : public QDialog
{
public:
    PreflightDialog(Document *document, const QString &pdf, QWidget *parent);

private:
    void check();
    void fill();
    void loadProfileFile();
    void applySelected();
    void saveReport();
    void updateButtons();

    Document *m_document = nullptr;
    QString m_pdf;
    QVector<Preflight::Profile> m_profiles;
    Preflight::Report m_report;

    QComboBox *m_profile = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_verdict = nullptr;
    QLabel *m_unchecked = nullptr;
    QPushButton *m_fix = nullptr;
    QPushButton *m_report_ = nullptr;
};

PreflightDialog::PreflightDialog(Document *document, const QString &pdf, QWidget *parent)
    : QDialog(parent)
    , m_document(document)
    , m_pdf(pdf)
    , m_profiles(Preflight::builtinProfiles())
    , m_profile(new QComboBox(this))
    , m_tree(new QTreeWidget(this))
    , m_verdict(new QLabel(this))
    , m_unchecked(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Preflight"));
    resize(1080, 780);

    for (const Preflight::Profile &profile : std::as_const(m_profiles)) {
        m_profile->addItem(profile.name);
    }

    auto *open = new QPushButton(QIcon::fromTheme(u"document-open"_s), i18nc("@action:button", "Profile…"), this);
    open->setToolTip(i18nc("@info:tooltip", "Use a profile saved in a file"));

    auto *again = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18nc("@action:button", "Check Again"), this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(i18nc("@label:listbox", "Profile:"), this));
    top->addWidget(m_profile, 1);
    top->addWidget(open);
    top->addWidget(again);

    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({ i18nc("@title:column", "Problem"), i18nc("@title:column", "Page"),
                              i18nc("@title:column", "Where"), i18nc("@title:column", "A fix would") });
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);

    m_verdict->setWordWrap(true);

    m_unchecked->setWordWrap(true);
    m_unchecked->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *honesty = new QGroupBox(i18nc("@title:group", "What was not checked"), this);
    auto *honestyLayout = new QVBoxLayout(honesty);
    honestyLayout->addWidget(m_unchecked);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_fix = buttons->addButton(i18nc("@action:button", "Fix the Ticked Problems"), QDialogButtonBox::ApplyRole);
    m_report_ = buttons->addButton(i18nc("@action:button", "Report as PDF…"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_fix, &QPushButton::clicked, this, [this] { applySelected(); });
    connect(m_report_, &QPushButton::clicked, this, [this] { saveReport(); });

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_verdict);
    layout->addWidget(honesty);
    layout->addWidget(buttons);

    connect(m_profile, &QComboBox::currentIndexChanged, this, [this] { check(); });
    connect(open, &QPushButton::clicked, this, [this] { loadProfileFile(); });
    connect(again, &QPushButton::clicked, this, [this] { check(); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { updateButtons(); });

    check();
}

void PreflightDialog::check()
{
    const int index = m_profile->currentIndex();
    if (index < 0 || index >= m_profiles.size()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const Preflight::Report report = Preflight::run(m_pdf, m_profiles.at(index), &error);
    QApplication::restoreOverrideCursor();

    if (!error.isEmpty()) {
        KMessageBox::error(this, error, i18nc("@title:window", "Preflight"));
        return;
    }
    m_report = report;
    fill();
}

void PreflightDialog::fill()
{
    m_tree->clear();

    // Grouped by severity and in this order, so that the thing which will get
    // the document rejected is the thing at the top.
    const Preflight::Severity order[]
        = { Preflight::Severity::Error, Preflight::Severity::Warning, Preflight::Severity::Note };
    for (Preflight::Severity severity : order) {
        QVector<const Preflight::Finding *> mine;
        for (const Preflight::Finding &finding : std::as_const(m_report.findings)) {
            if (finding.severity == severity) {
                mine.append(&finding);
            }
        }
        if (mine.isEmpty()) {
            continue;
        }

        auto *group = new QTreeWidgetItem(m_tree, { headingFor(severity, mine.size()) });
        group->setIcon(0, iconFor(severity));
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        group->setFirstColumnSpanned(false);

        for (const Preflight::Finding *finding : std::as_const(mine)) {
            const QString where = finding->page >= 0 ? QLocale().toString(finding->page + 1) : QString();
            auto *row
                = new QTreeWidgetItem(group, { finding->message, where, finding->object, finding->fixDescription });
            row->setToolTip(0, Preflight::describeRule(finding->ruleId));
            row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            if (!finding->fixId.isEmpty()) {
                row->setData(0, FixRole, finding->fixId);
                row->setCheckState(0, Qt::Unchecked);
            }
        }
        group->setExpanded(true);
    }
    m_tree->resizeColumnToContents(1);
    m_tree->resizeColumnToContents(2);

    if (m_report.findings.isEmpty()) {
        m_verdict->setText(i18n("Nothing to report against this profile."));
    } else {
        m_verdict->setText(i18nc("@info a preflight verdict: counts, then whether it passed", "%1, %2. %3",
                                 i18ncp("@info", "%1 error", "%1 errors", m_report.errors),
                                 i18ncp("@info", "%1 warning", "%1 warnings", m_report.warnings),
                                 m_report.passed ? i18n("No errors among the rules that were checked.")
                                                 : i18n("This would be rejected.")));
    }

    QStringList honest;
    if (!m_report.notChecked.isEmpty()) {
        QStringList named;
        for (const QString &rule : std::as_const(m_report.notChecked)) {
            const QString what = Preflight::describeRule(rule);
            named += what.isEmpty() ? rule : i18nc("@item rule id and what it looks for", "%1: %2", rule, what);
        }
        honest += i18np("This profile names one rule that is not implemented here, so it was not looked at:",
                        "This profile names %1 rules that are not implemented here, so they were not looked at:",
                        m_report.notChecked.size());
        honest += named.join(u"\n"_s);
    }
    honest += Preflight::limitations();
    m_unchecked->setText(honest.isEmpty() ? i18n("Every rule in this profile was checked.") : honest.join(u"\n"_s));

    updateButtons();
}

void PreflightDialog::loadProfileFile()
{
    const QString path = QFileDialog::getOpenFileName(this, i18nc("@title:window", "Open a Preflight Profile"),
                                                      QString(), i18n("Preflight profiles (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    const Preflight::Profile profile = Preflight::loadProfile(path, &error);
    if (!error.isEmpty()) {
        KMessageBox::error(this, error, i18nc("@title:window", "Preflight"));
        return;
    }

    m_profiles.append(profile);
    m_profile->addItem(profile.name.isEmpty() ? QFileInfo(path).completeBaseName() : profile.name);
    m_profile->setCurrentIndex(m_profile->count() - 1); // re-runs the check
}

void PreflightDialog::updateButtons()
{
    int ticked = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, FixRole).isValid() && (*it)->checkState(0) == Qt::Checked) {
            ++ticked;
        }
    }
    m_fix->setEnabled(ticked > 0);
    m_report_->setEnabled(!m_report.findings.isEmpty() || !m_report.notChecked.isEmpty());
}

void PreflightDialog::applySelected()
{
    // One fix mends every finding of its kind, so the same id can be ticked
    // several times over. Running it twice would be harmless but slow, and on a
    // fix that rewrites the whole document through Ghostscript, not that
    // harmless either.
    QStringList ids;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QVariant fix = (*it)->data(0, FixRole);
        if (fix.isValid() && (*it)->checkState(0) == Qt::Checked && !ids.contains(fix.toString())) {
            ids.append(fix.toString());
        }
    }
    if (ids.isEmpty()) {
        return;
    }

    const bool done = runProducing(m_document, this, i18nc("@title:window", "Preflight"), u"-fixed.pdf"_s,
                                   [this, &ids](const QString &output, QStringList *summary, QString *error) {
                                       QStringList applied;
                                       QApplication::setOverrideCursor(Qt::WaitCursor);
                                       const bool ok = Preflight::applyFixes(m_pdf, output, ids, &applied, error);
                                       QApplication::restoreOverrideCursor();
                                       if (!ok) {
                                           return false;
                                       }
                                       *summary += applied.isEmpty()
                                           ? i18n("Nothing needed changing after all.")
                                           : i18np("One thing was changed:", "%1 things were changed:", applied.size())
                                               + u"\n"_s + applied.join(u"\n"_s);
                                       return true;
                                   });
    if (done) {
        // The fixed file is a different document. Leaving this report open over
        // it would invite the user to tick a second fix and have it applied to
        // the file they have just superseded.
        accept();
    }
}

void PreflightDialog::saveReport()
{
    const QFileInfo file(m_pdf);
    const QString suggested = file.absolutePath() + u'/' + file.completeBaseName() + u"-preflight.pdf"_s;
    const QString path = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Save the Report"), suggested,
                                                      i18n("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = Preflight::writeReport(m_pdf, m_report, path, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        KMessageBox::error(this, error.isEmpty() ? i18n("The report could not be written.") : error,
                           i18nc("@title:window", "Preflight"));
        return;
    }
    KMessageBox::information(this, i18n("Written to %1.", QFileInfo(path).fileName()),
                             i18nc("@title:window", "Preflight"));
}

} // namespace

void showPreflight(Document *document, QWidget *parent)
{
    const QString pdf = savedPath(document, parent);
    if (pdf.isEmpty()) {
        return;
    }
    PreflightDialog(document, pdf, parent).exec();
}

} // namespace ps::tools
