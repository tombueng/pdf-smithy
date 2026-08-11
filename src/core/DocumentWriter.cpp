/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "DocumentWriter.h"

#include "Document.h"
#include "Encryption.h"
#include "Metadata.h"
#include "Outline.h"
#include "PageRef.h"
#include "Source.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <numeric>

namespace ps {

namespace {

/** What was actually added, in output order, so rotation can be applied after. */
struct AddedPage {
    int sourceId;
    int sourcePage;
    int rotation;
};

} // namespace

bool DocumentWriter::write(const Document &document, const QString &path, const Options &options, QString *error)
{
    QVector<int> all(document.pageCount());
    std::iota(all.begin(), all.end(), 0);
    return writeSelection(document, all, path, options, error);
}

namespace {

/**
 * Removes links whose destination page is not in the output.
 *
 * A dead destination shows up two ways. When the target page was copied along
 * with the rest, it is a real page dictionary that simply is not in the page
 * tree. When it was not, QPDF's copyForeignObject has already left a null in
 * its place, which is how it avoids dragging a whole document across for the
 * sake of one link, and is also the only trace that the target ever existed.
 * Both mean the same thing: the link goes nowhere, so it is not a link.
 *
 * Named destinations are left alone: resolving them needs the name tree, and
 * guessing wrong would break links that are perfectly good.
 */
int pruneDeadLinks(const std::vector<QPDFPageObjectHelper> &written)
{
    std::set<QPDFObjGen> alive;
    for (const QPDFPageObjectHelper &page : written) {
        alive.insert(page.getObjectHandle().getObjGen());
    }

    const auto destinationPage = [](QPDFObjectHandle annotation) {
        QPDFObjectHandle destination = annotation.getKey("/Dest");
        if (!destination.isArray()) {
            QPDFObjectHandle action = annotation.getKey("/A");
            const bool isGoTo
                = action.isDictionary() && action.getKey("/S").isName() && action.getKey("/S").getName() == "/GoTo";
            destination = isGoTo ? action.getKey("/D") : QPDFObjectHandle::newNull();
        }
        if (!destination.isArray() || destination.getArrayNItems() < 1) {
            return QPDFObjectHandle::newNull();
        }
        return destination.getArrayItem(0);
    };

    // A destination array exists but its page is null or missing from the tree.
    const auto isDead = [&alive](QPDFObjectHandle target) {
        if (target.isNull()) {
            return true;
        }
        if (!target.isDictionary()) {
            return false; // A page number or something exotic; leave it be.
        }
        return alive.find(target.getObjGen()) == alive.end();
    };

    int removed = 0;
    for (const QPDFPageObjectHelper &page : written) {
        QPDFObjectHandle handle = page.getObjectHandle();
        QPDFObjectHandle annotations = handle.getKey("/Annots");
        if (!annotations.isArray()) {
            continue;
        }

        QPDFObjectHandle kept = QPDFObjectHandle::newArray();
        for (int i = 0; i < annotations.getArrayNItems(); ++i) {
            QPDFObjectHandle annotation = annotations.getArrayItem(i);
            const bool isLink = annotation.isDictionary() && annotation.getKey("/Subtype").isName()
                && annotation.getKey("/Subtype").getName() == "/Link";

            if (isLink) {
                QPDFObjectHandle destination = annotation.getKey("/Dest");
                QPDFObjectHandle action = annotation.getKey("/A");
                const bool hasDestination
                    = destination.isArray() || (action.isDictionary() && action.getKey("/D").isArray());
                if (hasDestination && isDead(destinationPage(annotation))) {
                    ++removed;
                    continue;
                }
            }
            kept.appendItem(annotation);
        }

        if (kept.getArrayNItems() == 0) {
            handle.removeKey("/Annots");
        } else {
            handle.replaceKey("/Annots", kept);
        }
    }
    return removed;
}

/** Maps an outline from document rows onto the pages that were written. */
void renumber(QVector<OutlineItem> &items, const QHash<int, int> &outputRowOf)
{
    for (OutlineItem &item : items) {
        item.page = outputRowOf.value(item.page, -1);
        renumber(item.children, outputRowOf);
    }
}

} // namespace

bool DocumentWriter::writeSelection(const Document &document, const QVector<int> &pageIndexes, const QString &path,
                                    const Options &options, QString *error)
{
    if (pageIndexes.isEmpty()) {
        if (error) {
            *error = i18n("There are no pages to write.");
        }
        return false;
    }

    // Never write straight to the destination: the source file is very often
    // the destination, and QPDF still has it open for reading. Write beside it
    // and rename, which is atomic and leaves the original intact on failure.
    const QFileInfo targetInfo(path);
    QTemporaryFile temp(targetInfo.absolutePath() + QLatin1String("/.pdf-smithy-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”: %2", targetInfo.absolutePath(), temp.errorString());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    auto abandonTemp = [&tempPath] { QFile::remove(tempPath); };

    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper outPages(out);

        std::vector<AddedPage> added;
        added.reserve(static_cast<size_t>(pageIndexes.size()));

        for (const int index : pageIndexes) {
            const PageRef ref = document.pageAt(index);
            Source *src = document.source(ref.sourceId);
            if (!src) {
                continue;
            }
            QPDFPageObjectHelper from = src->page(ref.sourcePage);
            if (from.getObjectHandle().isNull()) {
                continue;
            }

            // addPage copies from the foreign document itself, and makes a
            // shallow copy when the same page is added twice, which is
            // exactly what page duplication needs.
            outPages.addPage(from, false);
            added.push_back({ ref.sourceId, ref.sourcePage, ref.rotation });
        }

        if (added.empty()) {
            abandonTemp();
            if (error) {
                *error = i18n("None of the selected pages could be read.");
            }
            return false;
        }

        // The handle passed to addPage is not necessarily the object that ended
        // up in the tree, so post-processing has to work off the real page list.
        // Fetched once, because getAllPages() walks the tree on every call.
        std::vector<QPDFPageObjectHelper> written = outPages.getAllPages();

        QPDFAcroFormDocumentHelper outForms(out);
        std::map<int, std::unique_ptr<QPDFAcroFormDocumentHelper>> sourceForms;

        for (size_t i = 0; i < added.size() && i < written.size(); ++i) {
            const AddedPage &info = added.at(i);

            if (info.rotation != 0) {
                written[i].rotatePage(info.rotation, /*relative=*/true);
            }

            // Copied pages otherwise share form fields with their originals,
            // which silently breaks interactive forms in merged documents.
            Source *src = document.source(info.sourceId);
            if (!src) {
                continue;
            }
            auto it = sourceForms.find(info.sourceId);
            if (it == sourceForms.end()) {
                it = sourceForms.emplace(info.sourceId, std::make_unique<QPDFAcroFormDocumentHelper>(src->qpdf()))
                         .first;
            }
            outForms.fixCopiedAnnotations(written[i].getObjectHandle(), src->page(info.sourcePage).getObjectHandle(),
                                          *it->second);

            if (options.stripInteractivity) {
                QPDFObjectHandle handle = written[i].getObjectHandle();
                handle.removeKey("/AA");
                QPDFObjectHandle annots = handle.getKey("/Annots");
                if (annots.isArray()) {
                    QPDFObjectHandle kept = QPDFObjectHandle::newArray();
                    for (int a = 0; a < annots.getArrayNItems(); ++a) {
                        QPDFObjectHandle annot = annots.getArrayItem(a);
                        if (!annot.isDictionary()) {
                            continue;
                        }
                        const bool attachment = annot.getKey("/Subtype").isName()
                            && annot.getKey("/Subtype").getName() == "/FileAttachment";
                        if (attachment) {
                            continue;
                        }
                        annot.removeKey("/AA");
                        QPDFObjectHandle action = annot.getKey("/A");
                        if (action.isDictionary() && action.hasKey("/JS")) {
                            annot.removeKey("/A");
                        }
                        kept.appendItem(annot);
                    }
                    handle.replaceKey("/Annots", kept);
                }
            }
        }

        pruneDeadLinks(written);

        Metadata::applyTo(out, document.metadata());

        // The table of contents, renumbered for the pages that were actually
        // written. Every save dropped it until this line existed, which turned
        // "rotate one page of the manual" into "lose its navigation".
        if (options.keepOutline) {
            QHash<int, int> outputRowOf;
            for (int i = 0; i < pageIndexes.size() && i < int(added.size()); ++i) {
                // First occurrence wins, so a duplicated page does not move a
                // bookmark onto the copy.
                if (!outputRowOf.contains(pageIndexes.at(i))) {
                    outputRowOf.insert(pageIndexes.at(i), i);
                }
            }
            QVector<OutlineItem> items = document.outline();
            renumber(items, outputRowOf);
            Outline::applyTo(out, items);
        }

        QPDFWriter writer(out);
        writer.setOutputFilename(QFile::encodeName(tempPath).constData());
        writer.setObjectStreamMode(options.objectStreams ? qpdf_o_generate : qpdf_o_disable);
        writer.setCompressStreams(options.compressStreams);
        writer.setLinearization(options.linearize);
        if (options.userPassword.isEmpty()) {
            writer.setDeterministicID(options.deterministicId);
        } else {
            // Deliberately no deterministic ID alongside encryption: the file
            // ID feeds the encryption key, so a reproducible ID would mean a
            // reproducible key. QPDF refuses the combination outright and is
            // right to.
            const QByteArray user = options.userPassword.toUtf8();
            // An empty owner password leaves the permissions trivially
            // removable, so it falls back to the user password.
            const QByteArray owner
                = (options.ownerPassword.isEmpty() ? options.userPassword : options.ownerPassword).toUtf8();
            const Encryption::Permissions &permitted = options.permissions;
            writer.setR6EncryptionParameters(user.constData(), owner.constData(), permitted.allowAccessibility,
                                             permitted.allowExtractText, permitted.allowAssemble,
                                             permitted.allowAnnotate, permitted.allowFillForms, permitted.allowModify,
                                             static_cast<qpdf_r3_print_e>(Encryption::printingFlagFor(permitted.printing)),
                                             /*encrypt_metadata_aes=*/true);
        }
        writer.write();
    } catch (const std::exception &e) {
        abandonTemp();
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }

    // Match the permissions of whatever we are replacing; a fresh temporary
    // file is 0600 and would silently tighten access on an existing document.
    if (targetInfo.exists()) {
        QFile::setPermissions(tempPath, QFile::permissions(path));
    } else {
        QFile::setPermissions(tempPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    }

    // QFile::rename refuses to clobber, so go through POSIX rename, which
    // replaces atomically. Both paths are in the same directory by construction.
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(path).constData()) != 0) {
        abandonTemp();
        if (error) {
            *error = i18n("Could not replace “%1”: %2", path, QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    return true;
}

} // namespace ps
