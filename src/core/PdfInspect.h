/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ps {

/**
 * The document as it is actually stored: indirect objects and the graph between
 * them, not the page a reader sees.
 *
 * `objects list` answers "what is painted on this page"; this answers "what is
 * in the file". They are different questions. A PDF is a set of numbered
 * indirect objects — dictionaries, arrays, streams and scalars — referring to
 * one another from a root, and every real problem (a wrong `/Type`, a dangling
 * reference, a resource that is not what it claims) lives at that level, below
 * anything the page shows. This is the layer iText's RUPS exposes; the aim here
 * is the same view, read more clearly.
 *
 * It is read-only. Changing an attribute is a separate, deliberate act with its
 * own guardrails, built on top of this once the seeing is solid.
 */
namespace PdfInspect {

/** The PDF object kinds, as the file distinguishes them. */
enum class Type {
    Null,
    Boolean,
    Integer,
    Real,
    String,
    Name,
    Array,
    Dictionary,
    Stream,
    Operator,
    InlineImage,
    Other,
};

/**
 * One object, or one element inside one, as a node of a tree.
 *
 * A node that is an indirect object carries its @c obj / @c gen number. A node
 * reached a second time, or past the requested depth, is left @c truncated with
 * its summary only, so the graph is shown as a tree without looping forever.
 */
struct Node {
    Type type = Type::Null;

    QString label; //!< a /Key for a dictionary entry, "[i]" for an array element; empty at the top

    bool indirect = false; //!< this node is an indirect object with a number
    int obj = 0;
    int gen = 0;

    QString summary; //!< a one-line description, always set
    QString value; //!< the scalar value, for leaves (numbers, names, strings)

    // Streams only.
    QString streamFilters; //!< the /Filter, verbatim, or empty
    long long streamLength = -1; //!< the /Length when it is a plain integer, else -1

    bool truncated = false; //!< had children, not expanded (depth or a cycle)

    QVector<Node> children;
};

/** One line of the flat object table. */
struct ObjectSummary {
    int obj = 0;
    int gen = 0;
    QString typeName; //!< "dictionary", "stream", "array", "integer"…
    QString pdfType; //!< the /Type and /Subtype when present, e.g. "/Page", "/XObject /Image"
    QString summary; //!< the same one-liner the tree uses
};

/** The facts worth knowing before looking closer. */
struct Overview {
    int objectCount = 0;
    int pageCount = -1; //!< -1 when the page tree could not be walked
    QString version; //!< the header version, e.g. "1.7"
    bool encrypted = false;
    bool linearized = false;
    QString producer; //!< /Info /Producer, when set
    QString warnings; //!< one sentence on what QPDF had to recover from, or empty
};

/** Top-level facts about @p pdf. False, with @p error set, when it will not open. */
bool overview(const QString &pdf, Overview *out, QString *error);

/** Every indirect object, lowest number first. */
bool objects(const QString &pdf, QVector<ObjectSummary> *out, QString *error);

/** The trailer dictionary, expanded to @p depth. */
bool trailer(const QString &pdf, int depth, Node *out, QString *error);

/** The document catalogue (the trailer's /Root), expanded to @p depth. */
bool root(const QString &pdf, int depth, Node *out, QString *error);

/**
 * One object by number, expanded to @p depth.
 *
 * False, with @p error set, when the file will not open or holds no such object.
 */
bool object(const QString &pdf, int obj, int gen, int depth, Node *out, QString *error);

/** The short type word for a node, for a column that must not follow the UI language. */
QString typeToken(Type type);

/** What this view does not (yet) show or let you change. */
QStringList limitations();

} // namespace PdfInspect

} // namespace ps
