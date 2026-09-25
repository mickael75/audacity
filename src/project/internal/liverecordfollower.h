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

    //! where the edit of a live recording is saved: "<name>_montage.wav" next to it
    static muse::io::path_t montagePath(const muse::io::path_t& wavPath);

    void start(const IAudacityProjectPtr& project, const muse::io::path_t& wavPath);
    void stop();

private:
    struct WavFormat {
        qint64 dataOffset = 0;
        int channels = 0;
        int bitsPerSample = 0;
    };

    static bool readWavFormat(const QString& path, WavFormat& format);
    static QString statusPath(const QString& wavPath);

    void follow();

    IAudacityProject* m_project = nullptr;
    QString m_wavPath;
    WavFormat m_format;
    int64_t m_framesRead = 0;
    QTimer m_timer;
};
}
