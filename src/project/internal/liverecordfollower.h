/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <cstdint>
#include <vector>
#include <map>

#include <QString>
#include <QTimer>

#include "framework/global/async/asyncable.h"
#include "framework/global/modularity/ioc.h"
#include "framework/global/io/path.h"
#include "framework/toast/itoastservice.h"

#include "trackedit/iprojecthistory.h"
#include "project/iaudacityproject.h"
#include "au3wrap/iau3project.h"
#include "framework/interactive/iinteractive.h"
#include "playback/iplaybackcontroller.h"
#include "trackedit/iselectioncontroller.h"
#include "liverecordsession.h"

class WaveTrack;

namespace au::project {
//! NOTE: follows a live recording (see LiveRecordMirror) opened while it runs, e.g. on another computer: the audio
//! written to the growing WAV file since it was opened is appended to its clip every few seconds, until its
//! "<name>.json" status says it's done. Edit a copy of the live clip (or another track): the live clip grows at its end.
//! With --shared-montage, the raw source is kept outside the editable timeline,
//! incoming audio extends the montage, and native montage revisions are exchanged on the share.
class LiveRecordFollower : public muse::Contextable, public muse::async::Asyncable
{
    muse::ContextInject<trackedit::IProjectHistory> projectHistory { this };
    muse::GlobalInject<muse::toast::IToastService> toastService;
    muse::GlobalInject<au::au3::IAu3ProjectCreator> au3ProjectCreator;
    muse::ContextInject<muse::IInteractive> interactive { this };
    muse::ContextInject<playback::IPlaybackController> playbackController { this };
    muse::ContextInject<trackedit::ISelectionController> selectionController { this };

public:
    explicit LiveRecordFollower(const muse::modularity::ContextPtr& ctx);
    ~LiveRecordFollower() override;

    //! whether the file is a live recording which is still running
    static bool isRunningLiveRecording(const muse::io::path_t& wavPath);
    static bool isLiveRecording(const muse::io::path_t& wavPath);
    static bool sharedMontageEnabled();

    bool start(const IAudacityProjectPtr& project, const muse::io::path_t& wavPath, const muse::io::path_t& draftPath);
    void stop();
    void pause() { m_timer.stop(); m_syncTimer.stop(); }
    void resume() {
        if (m_project && !m_syncPaused) {
            if (!m_audioFinished) { m_timer.start(); }
            if (m_shared) { m_syncTimer.start(); }
        }
    }
    QString wavPath() const { return m_wavPath; }
    int64_t sourceTrackId() const { return m_sourceTrackId; }
    bool isSharedMontage() const { return m_shared; }
    QString presenceRole() const {
        if (m_finalFrozen) { return QStringLiteral("frozen"); }
        if (m_syncPaused) { return QStringLiteral("paused"); }
        return !m_project && !m_audioFinished ? QStringLiteral("failed") : QStringLiteral("editing");
    }
    bool leaveSession();
    bool prepareSubmission();
    bool submit(const QString& renderedPath, QString& error);

private:
    struct WavFormat {
        qint64 dataOffset = 0;
        int channels = 0;
        int bitsPerSample = 0;
        quint32 dataBytes = 0;
        int sampleRate = 0;
    };

    static bool readWavFormat(const QString& path, WavFormat& format);
    void follow();
    void reportError(const QString& error);
    void appendToMontage(const std::vector<std::vector<float>>& channels, size_t frames);
    bool synchronizeMontage();
    bool applyMontage(const LiveRecordSession::MontageSnapshot& snapshot);
    bool publishMontage();
    bool catchUpMontage(int64_t fromFrame);

    IAudacityProject* m_project = nullptr;
    QString m_wavPath;
    WavFormat m_format;
    int64_t m_framesRead = 0;
    int64_t m_sourceTrackId = -1;
    int64_t m_sourceClipId = -1;
    QTimer m_timer;
    QTimer m_syncTimer;
    bool m_shared = false;
    bool m_applying = false;
    bool m_dirty = false;
    bool m_syncPaused = false;
    bool m_audioFinished = false;
    bool m_finalFrozen = false;
    QString m_revision;
    QString m_editorId;
    bool m_leftSession = false;
    int64_t m_tailTrackId = -1;
    int64_t m_tailClipId = -1;
    int64_t m_tailSamples = 0;
    // The raw source is detached from the editable timeline, so undo and
    // montage operations cannot alter or delete the incoming recording.
    std::shared_ptr<WaveTrack> m_rawTrack;
    std::map<size_t, int64_t> m_stateFrames;
};
}
