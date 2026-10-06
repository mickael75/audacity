/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QString>

#include "framework/global/async/asyncable.h"
#include "framework/global/modularity/ioc.h"
#include "framework/global/io/path.h"

#include "record/irecord.h"
#include "project/iaudacityproject.h"
#include "liverecordsession.h"

namespace au::project {
//! NOTE: while a quick edit records (e.g. launched by RCS Zetta to record into a new file), copy the recorded audio
//! into a WAV file of a shared (network) directory. The file grows during the recording and its header is kept
//! up to date, so that it can be opened at any time, from another computer too. A "<name>.json" file next to it
//! tells whether the recording is still running.
//! The directory is given with --live-dir "<directory>" or the AU_LIVE_RECORD_DIR environment variable.
class LiveRecordMirror : public muse::Contextable, public muse::async::Asyncable
{
    muse::ContextInject<record::IRecord> record { this };

public:
    explicit LiveRecordMirror(const muse::modularity::ContextPtr& ctx);
    ~LiveRecordMirror() override;

    //! the shared directory, or an empty string when live recording isn't configured
    static QString liveRecordDir();

    bool start(const IAudacityProjectPtr& project, const muse::io::path_t& targetPath);
    void finish();
    void cancel();
    QString wavPath() const { return m_wavPath; }
    bool isFinished() const { return m_finished; }
    bool hasFailed() const { return m_failed; }

private:
    struct Chunk {
        std::vector<float> samples; // interleaved
        int channels = 0;
        int rate = 0;
    };

    void copyNewAudio(bool force);
    bool writeStatus(const QString& status) const;
    void writerLoop(QString wavPath);

    IAudacityProject* m_project = nullptr;
    QString m_targetPath;
    QString m_wavPath;
    QString m_statusPath;
    QString m_startTime;
    std::unique_ptr<LiveRecordSession> m_session;
    int64_t m_copiedSamples = 0;
    int m_channels = 0;
    int m_rate = 0;
    QElapsedTimer m_sinceLastCopy;
    bool m_started = false;
    bool m_finished = false;
    bool m_recordingPublished = false;
    std::atomic<bool> m_failed { false };

    //! NOTE: the (network) file is written by its own thread, so that a slow share never blocks the recording
    std::thread m_writer;
    std::mutex m_mutex;
    std::condition_variable m_hasWork;
    std::deque<Chunk> m_queue;
    bool m_stopping = false;
};
}
