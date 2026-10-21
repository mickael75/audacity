/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace au::project {
// Presence is informational. It never grants recording ownership or writes Zetta audio.
class LiveSessionPresence
{
public:
    struct Participant {
        QString id;
        QString station;
        QString role;
        bool sharedMontage = false;
        QString machine;
    };
    static constexpr qint64 TIMEOUT_MS = 15000;

    explicit LiveSessionPresence(const QString& station = QString());
    ~LiveSessionPresence();
    LiveSessionPresence(const LiveSessionPresence&) = delete;
    LiveSessionPresence& operator=(const LiveSessionPresence&) = delete;

    static QString stationName();
    QString id() const { return m_id; }
    void setSession(const QString& wavPath);
    bool publish(const QString& role, bool sharedMontage, QString& error);
    // Observe pulse changes using the reader's monotonic clock, rather than
    // comparing wall clocks on different workstations.
    bool read(qint64 elapsedMs, QVector<Participant>& participants, QString& error);
    void clear();

private:
    struct Observation { QString pulse; qint64 lastChange = 0; };
    QString m_id;
    QString m_station;
    QString m_machine;
    QString m_directory;
    bool m_published = false;
    QHash<QString, Observation> m_observed;
};
}
