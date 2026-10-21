/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "livesessionstatusmodel.h"

#include <QSet>
#include <QStringList>
#include <algorithm>
#include <chrono>

#include "framework/global/translation.h"

using namespace au::project;

namespace {
QString roleLabel(const QString& role)
{
    if (role == "recording") { return muse::qtrc("project", "Recording"); }
    if (role == "ready") { return muse::qtrc("project", "Ready for REC"); }
    if (role == "recorded") { return muse::qtrc("project", "Recording stopped"); }
    if (role == "paused") { return muse::qtrc("project", "Local montage — sharing paused"); }
    if (role == "submitted") { return muse::qtrc("project", "Montage submitted"); }
    if (role == "frozen") { return muse::qtrc("project", "Montage frozen"); }
    if (role == "failed") { return muse::qtrc("project", "Live session error"); }
    return muse::qtrc("project", "Editing");
}
}

LiveSessionStatusModel::LiveSessionStatusModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    m_timer.setInterval(200);
    QObject::connect(&m_timer, &QTimer::timeout, [this]() { refresh(); });
}

void LiveSessionStatusModel::init()
{
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    refresh();
    m_timer.start();
}

void LiveSessionStatusModel::refresh()
{
    const auto session = projectFilesController()->liveSessionInfo();
    if (session.wavPath != m_currentSession) {
        m_currentSession = session.wavPath;
        m_active = !session.wavPath.isEmpty();
        m_available = true;
        m_recording = false;
        m_summary = m_active ? muse::qtrc("project", "Waiting for workstation presence…") : QString();
        m_details = m_summary;
        m_nextRead = 0;
        emit changed();
    }
    if (m_readWork.valid() && m_readWork.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const auto result = m_readWork.get();
        if (result.wavPath == session.wavPath) {
            applyPresence(session, result.participants, result.available);
        }
    }
    const qint64 elapsed = m_clock.elapsed();
    if (!session.wavPath.isEmpty() && !m_readWork.valid() && elapsed >= m_nextRead) {
        m_nextRead = elapsed + 2000;
        const QString path = session.wavPath;
        // Only the worker accesses the presence observer. UI properties and
        // dialogs are updated on the GUI thread after the result arrives.
        m_readWork = std::async(std::launch::async, [this, path, elapsed]() {
            m_reader.setSession(path);
            PresenceRead result;
            result.wavPath = path;
            QString error;
            result.available = m_reader.read(elapsed, result.participants, error);
            return result;
        });
    }
}

void LiveSessionStatusModel::applyPresence(const LiveSessionInfo& session,
                                          const QVector<LiveSessionPresence::Participant>& participants, bool available)
{
    const bool active = !session.wavPath.isEmpty();
    bool recording = false;
    QString summary;
    QString details;
    if (active) {
        if (!available) {
            summary = muse::qtrc("project", "Workstations unavailable");
            details = muse::qtrc("project", "The shared directory is unavailable. Workstation presence cannot be verified.");
        } else {
            auto sorted = participants;
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                return a.station != b.station ? a.station < b.station : a.role < b.role;
            });
            QSet<QString> editors;
            QStringList recorders;
            QStringList lines;
            bool ready = false;
            bool stopped = false;
            for (const auto& participant : sorted) {
                if (participant.role == "recording" && !recorders.contains(participant.station)) {
                    recorders.push_back(participant.station);
                }
                if (participant.role == "editing" || participant.role == "paused") { editors.insert(participant.machine); }
                ready = ready || participant.role == "ready";
                stopped = stopped || participant.role == "recorded";
                QString station = participant.station;
                if (participant.id == session.participantId) {
                    station += muse::qtrc("project", " (this window)");
                }
                QString line = station + QStringLiteral(" — ") + roleLabel(participant.role);
                if (participant.role == "editing" && !participant.sharedMontage) {
                    line += muse::qtrc("project", " (independent montage)");
                }
                lines.push_back(line);
            }
            recording = !recorders.isEmpty();
            const QString montage = editors.size() == 1 ? muse::qtrc("project", "Montage: 1 workstation")
                                    : muse::qtrc("project", "Montage: %1 workstations").arg(editors.size());
            summary = recording ? muse::qtrc("project", "REC: %1 · %2").arg(recorders.join(", "), montage)
                      : ready ? muse::qtrc("project", "Ready for REC · %1").arg(montage)
                      : stopped ? muse::qtrc("project", "REC stopped · %1").arg(montage) : montage;
            details = session.sharedMontage ? muse::qtrc("project", "Shared montage") : muse::qtrc("project", "Live session");
            details += QStringLiteral("\n\n") + (lines.isEmpty() ? muse::qtrc("project", "Waiting for workstation presence…")
                                                     : lines.join("\n"));
            details += QStringLiteral("\n\n") + muse::qtrc("project", "Presence is refreshed automatically. Closed or disconnected windows disappear after a short delay.");
        }
    }
    if (m_active != active || m_available != available || m_recording != recording
        || m_summary != summary || m_details != details) {
        m_active = active;
        m_available = available;
        m_recording = recording;
        m_summary = summary;
        m_details = details;
        emit changed();
    }
}

void LiveSessionStatusModel::showParticipants()
{
    refresh();
    if (m_active) {
        interactive()->info(muse::trc("project", "Workstations in this live session"), m_details.toStdString());
    }
}
