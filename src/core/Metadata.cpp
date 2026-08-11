/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#include "Metadata.h"
#include "PdfFile.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <KLocalizedString>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>
#include <functional>

namespace ps {

namespace {

QString stringValue(QPDFObjectHandle dict, const char *key)
{
    if (!dict.isDictionary() || !dict.hasKey(key)) {
        return {};
    }
    QPDFObjectHandle value = dict.getKey(key);
    return value.isString() ? QString::fromStdString(value.getUTF8Value()) : QString();
}

/** Opens, hands the document to @p work, writes the result atomically. */
bool rewrite(const QString &input, const QString &output, const std::function<void(QPDF &)> &work, QString *error)
{
    QTemporaryFile temp(QFileInfo(output).absolutePath() + QLatin1String("/.pdf-smithy-meta-XXXXXX.pdf"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        if (error) {
            *error = i18n("Cannot write to “%1”.", QFileInfo(output).absolutePath());
        }
        return false;
    }
    const QString tempPath = temp.fileName();
    temp.close();

    try {
        QPDF pdf;
        PdfFile::open(pdf, input);
        work(pdf);

        QPDFWriter writer(pdf);
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
    if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(output).constData()) != 0) {
        QFile::remove(tempPath);
        if (error) {
            *error = i18n("Could not write “%1”.", output);
        }
        return false;
    }
    return true;
}

} // namespace

bool Metadata::Fields::isEmpty() const
{
    return title.isEmpty() && author.isEmpty() && subject.isEmpty() && keywords.isEmpty() && creator.isEmpty()
        && producer.isEmpty() && !created.isValid() && !modified.isValid();
}

Metadata::Fields Metadata::readFrom(QPDF &pdf)
{
    Fields fields;
    QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");

    fields.title = stringValue(info, "/Title");
    fields.author = stringValue(info, "/Author");
    fields.subject = stringValue(info, "/Subject");
    fields.keywords = stringValue(info, "/Keywords");
    fields.creator = stringValue(info, "/Creator");
    fields.producer = stringValue(info, "/Producer");
    fields.created = PdfFile::parseDate(stringValue(info, "/CreationDate"));
    fields.modified = PdfFile::parseDate(stringValue(info, "/ModDate"));
    return fields;
}

void Metadata::applyTo(QPDF &pdf, const Fields &fields)
{
    if (fields.isEmpty()) {
        pdf.getTrailer().removeKey("/Info");
        return;
    }

    QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
    if (!info.isDictionary()) {
        info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        pdf.getTrailer().replaceKey("/Info", info);
    }

    const auto set = [&info](const char *key, const QString &value) {
        if (value.isEmpty()) {
            info.removeKey(key);
        } else {
            info.replaceKey(key, QPDFObjectHandle::newUnicodeString(value.toStdString()));
        }
    };

    set("/Title", fields.title);
    set("/Author", fields.author);
    set("/Subject", fields.subject);
    set("/Keywords", fields.keywords);
    set("/Creator", fields.creator);
    set("/Producer", fields.producer);
    set("/CreationDate", PdfFile::formatDate(fields.created));
    set("/ModDate", PdfFile::formatDate(fields.modified));
}

bool Metadata::read(const QString &path, Fields *fields, QString *error)
{
    if (!fields) {
        return false;
    }

    try {
        QPDF pdf;
        PdfFile::open(pdf, path);
        QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");

        fields->title = stringValue(info, "/Title");
        fields->author = stringValue(info, "/Author");
        fields->subject = stringValue(info, "/Subject");
        fields->keywords = stringValue(info, "/Keywords");
        fields->creator = stringValue(info, "/Creator");
        fields->producer = stringValue(info, "/Producer");
        fields->created = PdfFile::parseDate(stringValue(info, "/CreationDate"));
        fields->modified = PdfFile::parseDate(stringValue(info, "/ModDate"));
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

bool Metadata::write(const QString &inputPath, const QString &outputPath, const Fields &fields, QString *error)
{
    return rewrite(
        inputPath, outputPath,
        [&fields](QPDF &pdf) {
            QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
            if (!info.isDictionary()) {
                info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                pdf.getTrailer().replaceKey("/Info", info);
            }

            const auto set = [&info](const char *key, const QString &value) {
                if (value.isEmpty()) {
                    info.removeKey(key);
                } else {
                    info.replaceKey(key, QPDFObjectHandle::newUnicodeString(value.toStdString()));
                }
            };

            set("/Title", fields.title);
            set("/Author", fields.author);
            set("/Subject", fields.subject);
            set("/Keywords", fields.keywords);
            set("/Creator", fields.creator);
            set("/Producer", fields.producer);
            set("/CreationDate", PdfFile::formatDate(fields.created));
            set("/ModDate", PdfFile::formatDate(fields.modified));

            // The XMP packet says the same things in a second place. Leaving it
            // behind would mean the edit only half took.
            pdf.getRoot().removeKey("/Metadata");
        },
        error);
}

bool Metadata::inspect(const QString &path, Findings *findings, QString *error)
{
    if (!findings) {
        return false;
    }
    *findings = Findings {};

    try {
        QPDF pdf;
        PdfFile::open(pdf, path);

        QPDFObjectHandle info = pdf.getTrailer().getKey("/Info");
        if (info.isDictionary()) {
            for (const char *key : { "/Title", "/Author", "/Subject", "/Keywords", "/Creator", "/Producer",
                                     "/CreationDate", "/ModDate" }) {
                if (info.hasKey(key)) {
                    findings->hasDocumentInfo = true;
                    break;
                }
            }
        }

        QPDFObjectHandle root = pdf.getRoot();
        findings->hasXmp = root.hasKey("/Metadata");

        QPDFObjectHandle names = root.getKey("/Names");
        if (names.isDictionary()) {
            if (names.hasKey("/JavaScript")) {
                findings->hasJavaScript = true;
            }
            QPDFObjectHandle files = names.getKey("/EmbeddedFiles");
            if (files.isDictionary() && files.hasKey("/Names")) {
                QPDFObjectHandle list = files.getKey("/Names");
                if (list.isArray()) {
                    // The array alternates name, reference, so it holds half as
                    // many files as it has entries.
                    findings->embeddedFileCount = list.getArrayNItems() / 2;
                }
            }
        }
        if (root.hasKey("/OpenAction")) {
            QPDFObjectHandle action = root.getKey("/OpenAction");
            if (action.isDictionary() && action.hasKey("/JS")) {
                findings->hasJavaScript = true;
            }
        }
        if (root.hasKey("/AA")) {
            findings->hasJavaScript = true;
        }
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    return true;
}

bool Metadata::strip(const QString &inputPath, const QString &outputPath, const StripOptions &options, QString *error)
{
    return rewrite(
        inputPath, outputPath,
        [&options](QPDF &pdf) {
            QPDFObjectHandle root = pdf.getRoot();

            if (options.documentInfo) {
                pdf.getTrailer().removeKey("/Info");
                root.removeKey("/Metadata");
            }

            if (options.javaScript) {
                // Three separate hiding places, and a document that runs code
                // when opened is exactly the kind of thing people forward
                // without looking.
                root.removeKey("/OpenAction");
                root.removeKey("/AA");
                QPDFObjectHandle names = root.getKey("/Names");
                if (names.isDictionary()) {
                    names.removeKey("/JavaScript");
                }

                for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
                    QPDFObjectHandle handle = page.getObjectHandle();
                    handle.removeKey("/AA");
                    QPDFObjectHandle annots = handle.getKey("/Annots");
                    if (annots.isArray()) {
                        for (int i = 0; i < annots.getArrayNItems(); ++i) {
                            QPDFObjectHandle annot = annots.getArrayItem(i);
                            if (annot.isDictionary()) {
                                annot.removeKey("/AA");
                                QPDFObjectHandle action = annot.getKey("/A");
                                if (action.isDictionary() && action.hasKey("/JS")) {
                                    annot.removeKey("/A");
                                }
                            }
                        }
                    }
                }
            }

            if (options.embeddedFiles) {
                QPDFObjectHandle names = root.getKey("/Names");
                if (names.isDictionary()) {
                    names.removeKey("/EmbeddedFiles");
                }
                for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
                    QPDFObjectHandle annots = page.getObjectHandle().getKey("/Annots");
                    if (!annots.isArray()) {
                        continue;
                    }
                    QPDFObjectHandle kept = QPDFObjectHandle::newArray();
                    for (int i = 0; i < annots.getArrayNItems(); ++i) {
                        QPDFObjectHandle annot = annots.getArrayItem(i);
                        const bool isAttachment = annot.isDictionary() && annot.getKey("/Subtype").isName()
                            && annot.getKey("/Subtype").getName() == "/FileAttachment";
                        if (!isAttachment) {
                            kept.appendItem(annot);
                        }
                    }
                    page.getObjectHandle().replaceKey("/Annots", kept);
                }
            }
        },
        error);
}

} // namespace ps
