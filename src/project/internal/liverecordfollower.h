/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <cstdint>

#include <QString>
#include <QTimer>

#include "framework/global/async/asyncable.h"
#include "framework/global/modularity/ioc.h"
#include "framework/global/io/path.h"
#include "framework/toast/itoastservice.h"

#include "trackedit/iprojecthistory.h"
#include "project/iaudacityproject.h"

namespace au::project {
//! NOTE: follows a live recording (see LiveRecordMirror) opened while it runs, e.g. on another computer: the audio
//! written to the growing WAV file since it was opened is appended to its clip every few seconds, until its
//! "<name>.json" status says it's done. Edit a copy of the live clip (or another track): the live clip grows at its end.
class LiveRecordFollower : public muse::Contextable, public muse::async::Asyncable
{
    muse::ContextInject<trackedit::IProjectHistory> projectHistory { this };
    muse::GlobalInject<muse::toast::IToastService> toastService;

public:
    explicit LiveRecordFollower(const muse::modularity::ContextPtr& ctx);
    ~LiveRecordFollower() override;

    //! whether the file is a live recording which is still running
    static bool isRunningLiveRecording(const muse::io::path_t& wavPath);
    static bool isLiveRecording(const muse::io::path_t& wavPath);

    bool start(const IAudacityProjectPtr& project, const muse::io::path_t& wavPath, const muse::io::path_t& draftPath);
    void stop();
    void pause() { m_timer.stop(); }
    void resume() { if (m_project) { m_timer.start(); } }
    QString wavPath() const { return m_wavPath; }
    int64_t sourceTrackId() const { return m_sourceTrackId; }

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

    IAudacityProject* m_project = nullptr;
    QString m_wavPath;
    WavFormat m_format;
    int64_t m_framesRead = 0;
    int64_t m_sourceTrackId = -1;
    int64_t m_sourceClipId = -1;
    QTimer m_timer;
};
}
