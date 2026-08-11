/*
    SPDX-FileCopyrightText: 2026 Tom Bueng <tombueng@gmail.com>
    SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

class QCommandLineParser;
class QTextStream;

/**
 * The pieces every command group shares.
 *
 * The command line started as one file, which was fine while it was one file.
 * With the editing modules arriving it would become a single place that every
 * change has to pass through, so each area now owns a translation unit and
 * declares itself here. A group registers the options only it needs and is
 * offered each verb in turn; returning nothing means "not mine", which keeps
 * the dispatcher from having to know the whole vocabulary.
 */
namespace ps::cli {

enum ExitCode {
    Success = 0,
    /** Differences were found. Chosen so that `if compare a b; then` reads like diff. */
    DifferencesFound = 1,
    UsageError = 2,
    InputError = 3,
    OutputError = 4,
};

QTextStream &out();
QTextStream &err();

/** Prints @p message to standard error with the usual prefix and returns @p code. */
int fail(const QString &message, int code = InputError);

/** A size a person can read, in their own locale. */
QString humanSize(qint64 bytes);

/**
 * Checks that @p arguments holds exactly one readable file, else explains why not.
 *
 * @p verb names the command in the message, because "Error: give one file" is
 * no help when the script that produced it ran six of them.
 */
bool oneInputFile(const QStringList &arguments, const QString &verb, QString *path, int *code);

/** Where to write, falling back to @p fallback beside the input when unset. */
QString outputOr(const QString &given, const QString &input, const QString &suffix);

/**
 * One area of the command line.
 *
 * @c options is called once before parsing, @c run once per invocation with the
 * verb already taken off the front of @p arguments. Returning std::nullopt means
 * the verb belongs to someone else.
 */
struct Group {
    void (*options)(QCommandLineParser &parser) = nullptr;
    std::optional<int> (*run)(const QString &verb, const QStringList &arguments,
                              const QCommandLineParser &parser) = nullptr;
    /** One line per command, its usage and what it does, for the help text. */
    QStringList (*help)() = nullptr;
};

// Each area, defined in its own file. Adding one means adding it here and to
// the list in main.cpp; nothing else in the dispatcher needs to change.
Group colourGroup();
Group fontGroup();
Group imageGroup();
Group layoutGroup();
Group preflightGroup();
Group formGroup();
Group typesetGroup();
Group objectGroup();
Group convertGroup();

} // namespace ps::cli
