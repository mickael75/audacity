/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <QString>
#include <QLockFile>
#include <QJsonObject>

namespace au::project {
// The recorder alone owns the Zetta path. Shared editors stay mutable until
// capture ends and every editor has synchronized and left the session.
class LiveRecordSession
{
public:
    enum class State { Recording, Done, Failed };
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

    // Immutable native-project snapshots, with compare-and-swap publication.
    // A stale editor must never overwrite another workstation's montage.
    struct MontageSnapshot {
        QString revision;
        QString projectPath;
        QJsonObject metadata;
    };
    enum class MontageUpdate { Published, Conflict, Frozen, Error };
    static QString sharedMontagePath(const QString& wavPath);
    static bool readMontage(const QString& wavPath, MontageSnapshot& snapshot, QString& error,
                            const QString& verifiedRevision = QString());
    static MontageUpdate publishMontage(const QString& wavPath, const QString& expectedRevision,
                                       const QString& localProject, const QJsonObject& metadata,
                                       MontageSnapshot& snapshot, QString& error);
    // Durable participation: only an explicit synchronized departure releases an editor.
    // A lost heartbeat is not proof that an editor has finished its work.
    static bool joinMontage(const QString& wavPath, const QString& editorId, const QString& station, QString& error);
    static bool leaveMontage(const QString& wavPath, const QString& editorId,
                             const QString& expectedRevision, QString& error);
    // Returns false with an empty error while capture/editors are still active.
    // Sealing and joining use the same lock, so a late join cannot race finalization.
    static bool sealMontage(const QString& wavPath, MontageSnapshot& snapshot, QString& error);
    static bool submitMontage(const QString& wavPath, const QString& expectedRevision,
                              const QString& renderedPath, QString& error);

private:
    static bool submitUnlocked(const QString& wavPath, const QString& renderedPath, QString& error);
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
