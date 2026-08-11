/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "OutlineDock.h"

#include "core/Convert.h"
#include "core/Document.h"
#include "core/PageRef.h"
#include "core/Source.h"
#include "core/commands/PageCommands.h"

#include <KGuiItem>
#include <KLocalizedString>
#include <KMessageBox>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDropEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using namespace Qt::Literals::StringLiterals;

namespace ps {

namespace {

/** Where an entry's own fields live on the tree item that shows it. */
constexpr int pageRole = Qt::UserRole + 1;
constexpr int openRole = Qt::UserRole + 2;
constexpr int boldRole = Qt::UserRole + 3;
constexpr int italicRole = Qt::UserRole + 4;
constexpr int colourRole = Qt::UserRole + 5;
constexpr int sourceIdRole = Qt::UserRole + 6;
constexpr int sourcePageRole = Qt::UserRole + 7;

/**
 * A tree that says when a drag has finished moving something.
 *
 * QTreeWidget carries out an internal move by taking the items out and putting
 * them back rather than by asking the model to move rows, so rowsMoved() never
 * arrives and a drop would otherwise reach the document only by the accident of
 * whatever the user edited next. Which item is being dragged has to be noted
 * before the base class runs, because the view has no current item while it is
 * out of the tree.
 */
class DropAwareTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

    std::function<void(QTreeWidgetItem *)> dropped;

protected:
    void dropEvent(QDropEvent *event) override
    {
        QTreeWidgetItem *moved = currentItem();
        QTreeWidget::dropEvent(event);
        if (event->isAccepted() && dropped) {
            dropped(moved);
        }
    }
};

/** Takes @p item out of wherever it sits, without destroying it. */
QTreeWidgetItem *takeOut(QTreeWidget *tree, QTreeWidgetItem *item)
{
    if (QTreeWidgetItem *parent = item->parent()) {
        return parent->takeChild(parent->indexOfChild(item));
    }
    return tree->takeTopLevelItem(tree->indexOfTopLevelItem(item));
}

/** Puts @p item back at @p index under @p parent, or at the top level for null. */
void putBack(QTreeWidget *tree, QTreeWidgetItem *parent, int index, QTreeWidgetItem *item)
{
    if (parent) {
        parent->insertChild(index, item);
    } else {
        tree->insertTopLevelItem(index, item);
    }
}

/** How many entries sit under @p item, at every depth. */
int descendantsOf(const QTreeWidgetItem *item)
{
    int total = 0;
    for (int i = 0; i < item->childCount(); ++i) {
        total += 1 + descendantsOf(item->child(i));
    }
    return total;
}

/** A button that shows the colour it holds and picks a new one when pressed. */
class Swatch : public QPushButton
{
public:
    Swatch(const QColor &initial, QWidget *parent, const std::function<void(const QColor &)> &picked)
        : QPushButton(parent)
        , m_colour(initial)
    {
        setAutoFillBackground(true);
        refresh();
        connect(this, &QPushButton::clicked, this, [this, picked] {
            const QColor chosen = QColorDialog::getColor(m_colour.isValid() ? m_colour : QColor(Qt::black), this,
                                                         i18nc("@title:window", "Pick a Colour"));
            if (chosen.isValid()) {
                m_colour = chosen;
                refresh();
                picked(chosen);
            }
        });
    }

    void forget()
    {
        m_colour = QColor();
        refresh();
    }

private:
    void refresh()
    {
        setText(m_colour.isValid()
                    ? m_colour.name()
                    : i18nc("@info:whatsthis the colour a bookmark is drawn in, when the file names none", "Default"));
        if (!m_colour.isValid()) {
            setStyleSheet(QString());
            return;
        }
        // Black text on a dark swatch is unreadable, so the label follows the
        // swatch rather than the theme.
        const QString ink = m_colour.lightnessF() > 0.55 ? u"#101010"_s : u"#f0f0f0"_s;
        setStyleSheet(u"background-color: %1; color: %2;"_s.arg(m_colour.name(), ink));
    }

    QColor m_colour;
};

} // namespace

OutlineDock::OutlineDock(Document *document, QWidget *parent)
    : QDockWidget(i18nc("@title:window", "Contents"), parent)
    , m_document(document)
    , m_tree(new DropAwareTree(this))
    , m_empty(new QWidget(this))
{
    setObjectName(QStringLiteral("outline_dock"));

    m_tree->setHeaderLabels({ i18nc("@title:column", "Bookmark"), i18nc("@title:column", "Page") });
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    // Double click renames and single click goes to the page. Not the other way
    // round: going somewhere is the thing done a hundred times a session, and
    // renaming the thing done once. SelectedClicked, which the panel used
    // before, would start a rename every time the user clicked the entry they
    // were already on.
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->setDropIndicatorShown(true);
    m_tree->setExpandsOnDoubleClick(false);

    // A document with no table of contents would otherwise show an empty panel
    // and no hint that anything can be done about it.
    auto *hint = new QLabel(i18n("This document has no table of contents. Build one from the headings the text "
                                 "appears to have, or add a bookmark for the page you are looking at and drag "
                                 "entries onto one another to nest them."),
                            m_empty);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    auto *addFirst = new QPushButton(QIcon::fromTheme(QStringLiteral("bookmark-new")),
                                     i18nc("@action:button", "Add a Bookmark for This Page"), m_empty);
    connect(addFirst, &QPushButton::clicked, this, &OutlineDock::addForCurrentPage);

    auto *fromHeadings = new QPushButton(QIcon::fromTheme(QStringLiteral("bookmark-new-list")),
                                         i18nc("@action:button", "Build From Headings"), m_empty);
    // Said on the button rather than discovered afterwards: somebody about to
    // do this to a two-hundred-page manual should know beforehand that it is a
    // guess from type size and nothing more.
    fromHeadings->setToolTip(i18nc("@info:tooltip",
                                   "Makes a bookmark of every paragraph set larger than the text around it. Nothing "
                                   "in a PDF says which paragraph is a heading, so this is worked out from type "
                                   "size alone and is worth reading through before you save."));
    connect(fromHeadings, &QPushButton::clicked, this, &OutlineDock::buildFromHeadings);

    auto *emptyLayout = new QVBoxLayout(m_empty);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->addWidget(hint);
    emptyLayout->addWidget(fromHeadings);
    emptyLayout->addWidget(addFirst);

    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_empty);
    layout->addWidget(m_tree, 1);
    setWidget(body);

    auto *remove = new QAction(this);
    remove->setShortcut(QKeySequence::Delete);
    remove->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(remove, &QAction::triggered, this, &OutlineDock::removeSelected);
    m_tree->addAction(remove);

    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        const int row = item->data(0, pageRole).toInt();
        if (row >= 0) {
            Q_EMIT pageRequested(row);
        }
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        // Enter on a highlighted entry, and on a desktop that activates with a
        // single click that click as well. Asking for the same page twice costs
        // nothing, and leaving this out would strand anyone using the keyboard.
        const int row = item->data(0, pageRole).toInt();
        if (row >= 0) {
            Q_EMIT pageRequested(row);
        }
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] {
        if (!m_loading) {
            commit(i18nc("@action:inmenu undo step", "Rename bookmark"));
        }
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { Q_EMIT inspectionChanged(); });

    // Opening and closing an entry is remembered on the entry itself rather
    // than read back off the view, because taking an item out of a tree and
    // putting it somewhere else loses the view's idea of what was open, which
    // would quietly close every chapter the user dragged.
    //
    // Twiddling a disclosure triangle is deliberately not an undo step: it is
    // how the panel is read, not how it is edited, and an undo stack full of
    // "opened a chapter" is an undo stack nobody dares press. What the saved
    // file says is set on purpose through the properties panel instead, and a
    // triangle turned in passing travels along with the next real edit.
    const auto remember = [this](QTreeWidgetItem *node, bool open) {
        if (node->data(0, openRole).toBool() == open) {
            return;
        }
        const bool wasLoading = m_loading;
        m_loading = true;
        node->setData(0, openRole, open);
        m_loading = wasLoading;
    };
    connect(m_tree, &QTreeWidget::itemExpanded, this, [remember](QTreeWidgetItem *node) { remember(node, true); });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [remember](QTreeWidgetItem *node) { remember(node, false); });

    static_cast<DropAwareTree *>(m_tree)->dropped = [this](QTreeWidgetItem *moved) { finishDrop(moved); };

    connect(m_tree, &QWidget::customContextMenuRequested, this, &OutlineDock::showMenu);
    connect(m_document, &Document::outlineChanged, this, &OutlineDock::refresh);
    connect(m_document, &Document::wasReset, this, &OutlineDock::refresh);
    connect(m_document, &Document::pagesRemoved, this, &OutlineDock::refresh);
    connect(m_document, &Document::pagesInserted, this, &OutlineDock::refresh);

    refresh();
}

OutlineDock::~OutlineDock() = default;

// ── Showing what the document holds ───────────────────────────────────────

void OutlineDock::decorate(QTreeWidgetItem *node, const OutlineItem &item)
{
    node->setText(0, item.title);
    node->setData(0, pageRole, item.page);
    node->setData(0, openRole, item.open);
    node->setData(0, boldRole, item.bold);
    node->setData(0, italicRole, item.italic);
    node->setData(0, colourRole, item.colour);
    node->setData(0, sourceIdRole, item.sourceId);
    node->setData(0, sourcePageRole, item.sourcePage);
    node->setText(
        1, item.page >= 0 ? QString::number(item.page + 1) : i18nc("@item a bookmark that points at no page", "none"));
    node->setFlags(node->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);

    QFont font = m_tree->font();
    font.setBold(item.bold);
    font.setItalic(item.italic);
    node->setFont(0, font);

    // An entry anchored to a page that is no longer here is not the same thing
    // as one that never had a page: the first is broken and says so, the second
    // is a heading that groups the entries under it, which is legitimate.
    if (item.page < 0 && item.sourceId >= 0) {
        node->setToolTip(0, i18n("The page this pointed at is no longer in the document."));
        node->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        return;
    }

    node->setToolTip(
        0, item.page < 0 ? i18n("A heading with no page of its own, grouping the entries under it.") : QString());
    if (item.colour.isValid()) {
        // The reader will draw it in the file's own colour, so the sidebar does
        // too. A colour chosen for a white page can be hard to read against a
        // dark panel, but showing something other than what the document says
        // would be worse: the user could not tell what they had set.
        node->setForeground(0, item.colour);
    } else {
        node->setData(0, Qt::ForegroundRole, QVariant());
    }
}

OutlineItem OutlineDock::attributesOf(const QTreeWidgetItem *node)
{
    OutlineItem item;
    item.title = node->text(0);
    item.page = node->data(0, pageRole).toInt();
    item.open = node->data(0, openRole).toBool();
    item.bold = node->data(0, boldRole).toBool();
    item.italic = node->data(0, italicRole).toBool();
    item.colour = node->data(0, colourRole).value<QColor>();
    item.sourceId = node->data(0, sourceIdRole).toInt();
    item.sourcePage = node->data(0, sourcePageRole).toInt();
    return item;
}

void OutlineDock::fill(QTreeWidgetItem *parent, const QVector<OutlineItem> &items)
{
    for (const OutlineItem &item : items) {
        auto *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        decorate(node, item);
        fill(node, item.children);
        node->setExpanded(item.open);
    }
}

void OutlineDock::refresh()
{
    if (m_committing) {
        // Our own change coming back. The tree already shows it, and rebuilding
        // now would destroy the item whose editor is still closing above us.
        return;
    }

    m_loading = true;
    m_tree->clear();
    fill(nullptr, m_document->outline());
    m_loading = false;

    updateHint();
    Q_EMIT inspectionChanged();
}

void OutlineDock::updateHint()
{
    m_empty->setVisible(m_tree->topLevelItemCount() == 0);
}

void OutlineDock::restoreExpansion(QTreeWidgetItem *node)
{
    node->setExpanded(node->data(0, openRole).toBool());
    for (int i = 0; i < node->childCount(); ++i) {
        restoreExpansion(node->child(i));
    }
}

void OutlineDock::followPage(int row)
{
    m_currentRow = row;

    // The entry whose page is the last one at or before this row: the chapter
    // the reader is currently inside.
    QTreeWidgetItemIterator it(m_tree);
    QTreeWidgetItem *best = nullptr;
    int bestPage = -1;
    for (; *it; ++it) {
        const int page = (*it)->data(0, pageRole).toInt();
        if (page >= 0 && page <= row && page > bestPage) {
            bestPage = page;
            best = *it;
        }
    }

    if (!best) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    m_tree->setCurrentItem(best);
    m_tree->scrollToItem(best);
}

// ── Getting it back into the document ─────────────────────────────────────

QVector<OutlineItem> OutlineDock::harvest() const
{
    const std::function<QVector<OutlineItem>(QTreeWidgetItem *)> gather
        = [&](QTreeWidgetItem *parent) -> QVector<OutlineItem> {
        QVector<OutlineItem> items;
        const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            QTreeWidgetItem *node = parent ? parent->child(i) : m_tree->topLevelItem(i);
            OutlineItem item = attributesOf(node);
            item.children = gather(node);
            items.append(item);
        }
        return items;
    };
    return gather(nullptr);
}

void OutlineDock::commit(const QString &undoText)
{
    // Pushing runs the command straight away, and the document answers by
    // announcing a new table of contents, which this panel would ordinarily
    // rebuild from. It must not do so here: the caller is still holding the
    // item it just changed, and a rename is still unwinding through the item
    // delegate. refresh() sees the flag and leaves the tree alone.
    m_committing = true;
    m_document->undoStack()->push(new SetOutlineCommand(m_document, harvest(), undoText));
    m_committing = false;

    updateHint();
    Q_EMIT inspectionChanged();
}

// ── The edits themselves ──────────────────────────────────────────────────

void OutlineDock::addForCurrentPage()
{
    if (m_document->pageCount() == 0) {
        return; // Nothing to point at yet.
    }

    OutlineItem fresh;
    fresh.title = i18n("New bookmark");
    fresh.page = qBound(0, m_currentRow, m_document->pageCount() - 1);

    m_loading = true;
    auto *node = new QTreeWidgetItem;
    if (QTreeWidgetItem *beside = m_tree->currentItem()) {
        // Beside whatever the user has hold of rather than at the very end: a
        // bookmark added while a chapter is selected belongs next to it.
        QTreeWidgetItem *parent = beside->parent();
        const int index = parent ? parent->indexOfChild(beside) : m_tree->indexOfTopLevelItem(beside);
        putBack(m_tree, parent, index + 1, node);
    } else {
        m_tree->addTopLevelItem(node);
    }
    decorate(node, fresh);
    m_loading = false;

    commit(i18nc("@action:inmenu undo step", "Add bookmark"));

    // Straight into renaming: nobody wants a bookmark called "New bookmark".
    m_tree->setCurrentItem(node);
    m_tree->editItem(node, 0);
}

void OutlineDock::buildFromHeadings()
{
    if (m_document->pageCount() == 0) {
        return; // Nothing to read.
    }

    // Which pages of which opened file this document is made of. The inference
    // reads a file, and a document assembled from three of them is three files;
    // asking each separately is also what keeps each file's body size its own,
    // and the body size is what decides what counts as a heading in it.
    QHash<QPair<int, int>, int> rowOf;
    QMap<int, QVector<int>> pagesOfSource;
    for (int row = m_document->pageCount() - 1; row >= 0; --row) {
        const PageRef ref = m_document->pageAt(row);
        if (ref.sourceId < 0) {
            continue;
        }
        // Counting downwards leaves the first row holding the key, so a page
        // the user duplicated puts its heading on the copy read first.
        rowOf.insert({ ref.sourceId, ref.sourcePage }, row);
        pagesOfSource[ref.sourceId].append(ref.sourcePage);
    }

    QVector<Convert::Heading> found;
    QString trouble;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (auto it = pagesOfSource.cbegin(); it != pagesOfSource.cend(); ++it) {
        Source *source = m_document->source(it.key());
        if (!source) {
            continue;
        }

        QVector<int> pages = it.value();
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

        QString failure;
        const QVector<Convert::Heading> headings = Convert::findHeadings(source->path(), pages, &failure);
        if (!failure.isEmpty() && trouble.isEmpty()) {
            trouble = failure;
        }
        for (Convert::Heading heading : headings) {
            const auto row = rowOf.constFind({ it.key(), heading.page });
            if (row == rowOf.constEnd()) {
                continue;
            }
            heading.page = *row;
            found.append(heading);
        }
    }
    QApplication::restoreOverrideCursor();

    // Back into the order the document reads in, which is not the order the
    // files were read in. Two headings can only share a row when they came off
    // the same page, so a stable sort leaves those in the order found on it.
    std::stable_sort(found.begin(), found.end(),
                     [](const Convert::Heading &a, const Convert::Heading &b) { return a.page < b.page; });

    const QVector<OutlineItem> built = Outline::fromHeadings(found);
    if (built.isEmpty()) {
        // Plainly, and with the reason: an empty tree or one entry called
        // nothing would leave the user guessing whether it had even run.
        KMessageBox::information(
            this,
            trouble.isEmpty()
                ? i18n("No headings were found. A heading is recognised only by being set larger than the text "
                       "around it, so a document set in one size throughout (or one whose words are part of a "
                       "scanned picture rather than text) offers nothing to go on. The bookmarks will have to be "
                       "added one at a time.")
                : trouble,
            i18nc("@title:window", "No Headings Found"));
        return;
    }

    m_loading = true;
    m_tree->clear();
    fill(nullptr, built);
    m_loading = false;

    commit(i18nc("@action:inmenu undo step", "Build bookmarks from headings"));

    KMessageBox::information(
        this,
        i18np("One bookmark was made from what looks like a heading. Read it before saving: which paragraph is a "
              "heading was worked out from type size alone, because nothing in the file says.",
              "%1 bookmarks were made from what look like headings. Read them before saving: which paragraph is a "
              "heading was worked out from type size alone, because nothing in the file says.",
              Outline::count(built)),
        i18nc("@title:window", "Bookmarks Built"), QStringLiteral("outline_built_from_headings"));
}

void OutlineDock::renameSelected()
{
    if (QTreeWidgetItem *item = m_tree->currentItem()) {
        m_tree->editItem(item, 0);
    }
}

void OutlineDock::removeSelected()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        return;
    }

    // Taking a chapter's sub-entries with it is one of the two things the user
    // might mean and never the one they expect, so it is asked rather than
    // assumed. Nothing is undone here (the question comes first, the edit
    // after), so cancelling costs nothing.
    bool keepChildren = false;
    if (item->childCount() > 0) {
        const auto answer = KMessageBox::questionTwoActionsCancel(
            this,
            i18ncp("@info", "“%2” has one entry under it. Deleting it need not take that one with it.",
                   "“%2” has %1 entries under it. Deleting it need not take them with it.", descendantsOf(item),
                   item->text(0)),
            i18nc("@title:window", "Delete Bookmark"),
            KGuiItem(i18nc("@action:button", "Keep Them"), QIcon::fromTheme(QStringLiteral("format-indent-less"))),
            KGuiItem(i18nc("@action:button", "Delete Them Too"), QIcon::fromTheme(QStringLiteral("edit-delete"))));
        if (answer == KMessageBox::Cancel) {
            return;
        }
        keepChildren = answer == KMessageBox::PrimaryAction;
    }

    m_loading = true;
    QTreeWidgetItem *parent = item->parent();
    const int index = parent ? parent->indexOfChild(item) : m_tree->indexOfTopLevelItem(item);
    if (keepChildren) {
        // Into the hole their parent leaves, in their own order, so that they
        // keep their place in the document rather than moving to the end of it.
        const QList<QTreeWidgetItem *> orphans = item->takeChildren();
        int at = index + 1;
        for (QTreeWidgetItem *orphan : orphans) {
            putBack(m_tree, parent, at++, orphan);
            restoreExpansion(orphan);
        }
    }
    delete item;
    m_loading = false;

    commit(i18nc("@action:inmenu undo step", "Delete bookmark"));
}

void OutlineDock::nestSelected()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        return;
    }
    QTreeWidgetItem *parent = item->parent();
    const int index = parent ? parent->indexOfChild(item) : m_tree->indexOfTopLevelItem(item);
    if (index <= 0) {
        return; // Nothing above it to become its parent.
    }

    m_loading = true;
    QTreeWidgetItem *above = parent ? parent->child(index - 1) : m_tree->topLevelItem(index - 1);
    takeOut(m_tree, item);
    above->addChild(item);
    above->setData(0, openRole, true);
    above->setExpanded(true);
    restoreExpansion(item);
    m_loading = false;

    m_tree->setCurrentItem(item);
    commit(i18nc("@action:inmenu undo step", "Make a sub-entry"));
}

void OutlineDock::unnestSelected()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || !item->parent()) {
        return;
    }
    QTreeWidgetItem *parent = item->parent();
    QTreeWidgetItem *grandparent = parent->parent();
    const int after = grandparent ? grandparent->indexOfChild(parent) : m_tree->indexOfTopLevelItem(parent);

    m_loading = true;
    takeOut(m_tree, item);
    putBack(m_tree, grandparent, after + 1, item);
    restoreExpansion(item);
    m_loading = false;

    m_tree->setCurrentItem(item);
    commit(i18nc("@action:inmenu undo step", "Move bookmark out one level"));
}

void OutlineDock::moveSelected(int delta)
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        return;
    }
    QTreeWidgetItem *parent = item->parent();
    const int index = parent ? parent->indexOfChild(item) : m_tree->indexOfTopLevelItem(item);
    const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
    const int target = index + delta;
    if (target < 0 || target >= count) {
        return;
    }

    m_loading = true;
    takeOut(m_tree, item);
    putBack(m_tree, parent, target, item);
    restoreExpansion(item);
    m_loading = false;

    m_tree->setCurrentItem(item);
    commit(i18nc("@action:inmenu undo step", "Move bookmark"));
}

void OutlineDock::finishDrop(QTreeWidgetItem *moved)
{
    if (m_loading || !moved) {
        return;
    }

    m_loading = true;
    restoreExpansion(moved);
    // An entry dropped into a closed one would otherwise appear to have
    // vanished, so its new parents are opened, and told that they are open,
    // since that is what the document will be asked to save.
    for (QTreeWidgetItem *up = moved->parent(); up; up = up->parent()) {
        up->setData(0, openRole, true);
        up->setExpanded(true);
    }
    m_loading = false;

    m_tree->setCurrentItem(moved);
    commit(i18nc("@action:inmenu undo step", "Move bookmark"));
}

void OutlineDock::showMenu(const QPoint &where)
{
    QMenu menu(this);
    QTreeWidgetItem *current = m_tree->currentItem();
    const bool hasItem = current != nullptr;

    QAction *add = menu.addAction(QIcon::fromTheme(QStringLiteral("bookmark-new")),
                                  i18nc("@action:inmenu", "Add for Page %1", m_currentRow + 1));
    connect(add, &QAction::triggered, this, &OutlineDock::addForCurrentPage);

    menu.addSeparator();
    QAction *rename
        = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "Rename"));
    rename->setEnabled(hasItem);
    connect(rename, &QAction::triggered, this, &OutlineDock::renameSelected);

    QAction *remove
        = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete"));
    remove->setEnabled(hasItem);
    remove->setShortcut(QKeySequence::Delete);
    connect(remove, &QAction::triggered, this, &OutlineDock::removeSelected);

    menu.addSeparator();
    QAction *up = menu.addAction(QIcon::fromTheme(QStringLiteral("arrow-up")), i18nc("@action:inmenu", "Move Up"));
    up->setEnabled(hasItem);
    connect(up, &QAction::triggered, this, [this] { moveSelected(-1); });

    QAction *down
        = menu.addAction(QIcon::fromTheme(QStringLiteral("arrow-down")), i18nc("@action:inmenu", "Move Down"));
    down->setEnabled(hasItem);
    connect(down, &QAction::triggered, this, [this] { moveSelected(1); });

    QAction *nest = menu.addAction(QIcon::fromTheme(QStringLiteral("format-indent-more")),
                                   i18nc("@action:inmenu", "Make a Sub-entry"));
    nest->setEnabled(hasItem);
    connect(nest, &QAction::triggered, this, &OutlineDock::nestSelected);

    QAction *unnest = menu.addAction(QIcon::fromTheme(QStringLiteral("format-indent-less")),
                                     i18nc("@action:inmenu", "Move Out One Level"));
    unnest->setEnabled(hasItem && current->parent() != nullptr);
    connect(unnest, &QAction::triggered, this, &OutlineDock::unnestSelected);

    menu.addSeparator();
    QAction *expand = menu.addAction(i18nc("@action:inmenu", "Expand All"));
    connect(expand, &QAction::triggered, m_tree, &QTreeWidget::expandAll);
    QAction *collapse = menu.addAction(i18nc("@action:inmenu", "Collapse All"));
    connect(collapse, &QAction::triggered, m_tree, &QTreeWidget::collapseAll);

    menu.exec(m_tree->viewport()->mapToGlobal(where));
}

// ── What the properties panel sees ────────────────────────────────────────

Inspectable *OutlineDock::inspected()
{
    // A panel the user has closed must not go on claiming the properties view
    // from the overlay they are actually working in.
    if (!isVisible() || !m_tree->currentItem()) {
        return nullptr;
    }
    if (!m_inspectable) {
        m_inspectable = std::make_unique<BookmarkInspectable>(this);
    }
    return m_inspectable.get();
}

OutlineItem OutlineDock::selectedAttributes() const
{
    if (QTreeWidgetItem *item = m_tree->currentItem()) {
        return attributesOf(item);
    }
    return {};
}

bool OutlineDock::selectedHasChildren() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    return item && item->childCount() > 0;
}

int OutlineDock::pageCount() const
{
    return m_document->pageCount();
}

void OutlineDock::applyToSelected(const OutlineItem &attributes)
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        return;
    }

    OutlineItem edited = attributes;
    // Where it came from is not the panel's to change: a bookmark whose page
    // was deleted keeps its anchor, so undoing that deletion brings it back
    // pointing at the same page rather than at nothing.
    const OutlineItem before = attributesOf(item);
    edited.sourceId = before.sourceId;
    edited.sourcePage = before.sourcePage;
    if (edited.page != before.page) {
        // Except when the panel has just pointed it somewhere else, which is a
        // new anchor by definition; the document works the real one out.
        edited.sourceId = -1;
        edited.sourcePage = -1;
    }

    m_loading = true;
    decorate(item, edited);
    m_loading = false;

    commit(i18nc("@action:inmenu undo step", "Edit bookmark"));
}

// ── BookmarkInspectable ───────────────────────────────────────────────────

BookmarkInspectable::BookmarkInspectable(OutlineDock *dock)
    : m_dock(dock)
{
}

QString BookmarkInspectable::kindName() const
{
    return i18nc("@title the kind of thing the properties panel is showing", "Bookmark");
}

QString BookmarkInspectable::description() const
{
    const QString title = m_dock->selectedAttributes().title.simplified();
    if (title.isEmpty()) {
        return i18nc("@info a bookmark whose title is empty", "untitled");
    }
    return title.size() > 42 ? title.left(41) + u'…' : title;
}

QWidget *BookmarkInspectable::buildEditor(QWidget *parent)
{
    const OutlineItem now = m_dock->selectedAttributes();

    auto *box = new QWidget(parent);
    auto *form = new QFormLayout(box);
    form->setContentsMargins(0, 0, 0, 0);

    auto *title = new QLineEdit(now.title, box);
    form->addRow(i18nc("@label:textbox what a bookmark says", "Title:"), title);
    // On finishing rather than on every keystroke: one undo step per rename is
    // what Ctrl+Z is expected to walk back, not one per letter.
    QObject::connect(title, &QLineEdit::editingFinished, box, [this, title] {
        OutlineItem edited = m_dock->selectedAttributes();
        if (edited.title == title->text()) {
            return;
        }
        edited.title = title->text();
        m_dock->applyToSelected(edited);
    });

    auto *page = new QSpinBox(box);
    page->setRange(0, m_dock->pageCount());
    page->setSpecialValueText(i18nc("@item a bookmark that points at no page of its own", "None"));
    page->setValue(now.page >= 0 ? now.page + 1 : 0);
    page->setToolTip(i18nc("@info:tooltip",
                           "Which page the bookmark opens. An entry with no page of its own is a "
                           "heading for the entries under it."));
    form->addRow(i18nc("@label:spinbox which page a bookmark opens", "Page:"), page);
    QObject::connect(page, &QSpinBox::valueChanged, box, [this](int value) {
        OutlineItem edited = m_dock->selectedAttributes();
        if (edited.page == value - 1) {
            return;
        }
        edited.page = value - 1;
        m_dock->applyToSelected(edited);
    });

    auto *open = new QCheckBox(i18nc("@option:check", "Opens showing its sub-entries"), box);
    open->setChecked(now.open);
    open->setEnabled(m_dock->selectedHasChildren());
    form->addRow(QString(), open);
    QObject::connect(open, &QCheckBox::toggled, box, [this](bool checked) {
        OutlineItem edited = m_dock->selectedAttributes();
        if (edited.open == checked) {
            return;
        }
        edited.open = checked;
        m_dock->applyToSelected(edited);
    });

    auto *bold = new QCheckBox(i18nc("@option:check", "Bold"), box);
    bold->setChecked(now.bold);
    auto *italic = new QCheckBox(i18nc("@option:check", "Italic"), box);
    italic->setChecked(now.italic);
    auto *style = new QHBoxLayout;
    style->addWidget(bold);
    style->addWidget(italic);
    style->addStretch(1);
    form->addRow(i18nc("@label how a bookmark is drawn", "Style:"), style);

    const auto restyle = [this, bold, italic] {
        OutlineItem edited = m_dock->selectedAttributes();
        if (edited.bold == bold->isChecked() && edited.italic == italic->isChecked()) {
            return;
        }
        edited.bold = bold->isChecked();
        edited.italic = italic->isChecked();
        m_dock->applyToSelected(edited);
    };
    QObject::connect(bold, &QCheckBox::toggled, box, restyle);
    QObject::connect(italic, &QCheckBox::toggled, box, restyle);

    auto *swatch = new Swatch(now.colour, box, [this](const QColor &chosen) {
        OutlineItem edited = m_dock->selectedAttributes();
        edited.colour = chosen;
        m_dock->applyToSelected(edited);
    });
    auto *clear
        = new QPushButton(i18nc("@action:button put a bookmark's colour back to the reader's own", "Default"), box);
    clear->setEnabled(now.colour.isValid());
    QObject::connect(clear, &QPushButton::clicked, box, [this, swatch, clear] {
        OutlineItem edited = m_dock->selectedAttributes();
        if (!edited.colour.isValid()) {
            return;
        }
        edited.colour = QColor();
        swatch->forget();
        clear->setEnabled(false);
        m_dock->applyToSelected(edited);
    });

    auto *colour = new QHBoxLayout;
    colour->addWidget(swatch, 1);
    colour->addWidget(clear);
    form->addRow(i18nc("@label:chooser the colour a bookmark is drawn in", "Colour:"), colour);

    return box;
}

} // namespace ps
