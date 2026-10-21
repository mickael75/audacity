/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <future>

#include "framework/global/modularity/ioc.h"
#include "framework/interactive/iinteractive.h"
#include "project/iprojectfilescontroller.h"
#include "project/internal/livesessionpresence.h"

namespace au::project {
class LiveSessionStatusModel : public QObject, public muse::Contextable
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)

    muse::ContextInject<IProjectFilesController> projectFilesController { this };
    muse::ContextInject<muse::IInteractive> interactive { this };

public:
    explicit LiveSessionStatusModel(QObject* parent = nullptr);
    Q_INVOKABLE void init();
    Q_INVOKABLE void showParticipants();
    bool active() const { return m_active; }
    bool available() const { return m_available; }
    bool recording() const { return m_recording; }
    QString summary() const { return m_summary; }
    QString details() const { return m_details; }

signals:
    void changed();

private:
    void refresh();
    void applyPresence(const LiveSessionInfo& session, const QVector<LiveSessionPresence::Participant>& participants, bool available);
    struct PresenceRead {
        QString wavPath;
        QVector<LiveSessionPresence::Participant> participants;
        bool available = false;
    };
    QTimer m_timer;
    QElapsedTimer m_clock;
    LiveSessionPresence m_reader;
    // The future is destroyed before the reader, waiting for any remaining I/O.
    std::future<PresenceRead> m_readWork;
    QString m_currentSession;
    qint64 m_nextRead = 0;
    bool m_active = false;
    bool m_available = true;
    bool m_recording = false;
    QString m_summary;
    QString m_details;
};
}
