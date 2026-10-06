/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <QString>
#include <QLockFile>

namespace au::project {
// The recorder alone owns the Zetta path. Editors exchange a frozen render on the share.
class LiveRecordSession
{
public:
    enum class State { Ready, Recording, Done, Failed };
    enum class Lookup { Absent, Pending, Found, Error };

    LiveRecordSession(const QString& directory, const QString& target);
    ~LiveRecordSession();
    bool claimTarget(QString& error);
    bool publishTarget(const QString& wavPath, QString& error);
    static Lookup findTarget(const QString& directory, const QString& target, QString& wavPath, QString& error);

    static QString statusPath(const QString& wavPath);
    static QString submissionPath(const QString& wavPath);
    static QString resultPath(const QString& wavPath);
    static QString finalAudioPath(const QString& wavPath);

    static bool readState(const QString& wavPath, State& state, QString& error);
    static bool submit(const QString& wavPath, const QString& renderedPath, QString& error);
    static bool readSubmission(const QString& wavPath, QString& renderedPath, QString& error);
    static bool publicationReady(const QString& wavPath, QString& renderedPath, QString& error);
    static bool acknowledge(const QString& wavPath, QString& error);
    static bool isAcknowledged(const QString& wavPath, QString& error);

private:
    static QString targetKey(const QString& target);
    static QString targetIndex(const QString& directory, const QString& target);
    QString m_directory;
    QString m_target;
    QString m_index;
    QLockFile m_targetLock;
    bool m_claimed = false;
    bool m_indexPublished = false;
};
}
