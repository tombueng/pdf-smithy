/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: MIT
*/
#pragma once

#include <QString>
#include <memory>
#include <vector>

// Included rather than forward-declared: the cached page vector needs the
// complete type to instantiate, and every user of Source needs its layout.
#include <qpdf/QPDFPageObjectHelper.hh>

class QPDF;

namespace ps {

/**
 * One PDF file the user has opened, held for the lifetime of the session.
 *
 * A Document may hold several of these at once, and that is what makes merging
 * work: pages from three files coexist in one page list, and each PageRef
 * remembers which Source it came from. Sources are read-only; all edits live
 * in the Document's page list until the writer assembles the result.
 */
class Source
{
public:
    ~Source();

    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;

    /**
     * Why an open did not work, so that a caller can do something about it.
     *
     * The distinction earns its keep: a file that merely needs a password is
     * not a failure at all, it is a question, and answering it with the same
     * red banner a damaged file gets leaves the user with a document they own
     * and cannot open. This program can itself put a password on a file, so
     * that was a door it could lock behind you.
     */
    enum class Trouble {
        None,
        NeedsPassword, //!< the password given, if any, does not open it
        Damaged, //!< anything else QPDF refused
    };

    /**
     * Opens @p path, with @p password if the file wants one.
     *
     * Returns nullptr and fills @p error on failure: a damaged or
     * password-protected file must never take the application down with it.
     * When @p trouble is given it says which kind of failure it was, which is
     * what lets a caller ask for a password rather than give up.
     */
    static std::unique_ptr<Source> open(const QString &path, QString *error, const QString &password = QString(),
                                        Trouble *trouble = nullptr);

    /** The password the file was opened with, empty when it needed none. */
    const QString &password() const { return m_password; }

    const QString &path() const { return m_path; }

    int pageCount() const { return static_cast<int>(m_pages.size()); }

    /**
     * A note about anything QPDF had to work around while reading, or empty.
     *
     * Worth surfacing once: a file that needed recovery is a file worth saving
     * a clean copy of. Worth surfacing only once, which is why it is captured
     * here rather than left to shout from stderr per object.
     */
    const QString &openWarning() const { return m_openWarning; }

    /** The underlying QPDF, used by DocumentWriter to assemble output. */
    QPDF &qpdf() const { return *m_qpdf; }

    /**
     * Page helper for @p index, or a null helper when out of range.
     * The page vector is cached because QPDF rebuilds it on every call, which
     * turns saving a large document into an accidental quadratic loop.
     */
    QPDFPageObjectHelper page(int index) const;

private:
    Source();

    QString m_path;
    QString m_password;
    QString m_openWarning;
    std::unique_ptr<QPDF> m_qpdf;
    std::vector<QPDFPageObjectHelper> m_pages;
};

} // namespace ps
