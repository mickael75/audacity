/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <QElapsedTimer>
#include <QString>

#include "framework/global/async/asyncable.h"
#include "framework/global/modularity/ioc.h"
#include "framework/global/io/path.h"

#include "record/irecord.h"
#include "project/iaudacityproject.h"

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

    void start(const IAudacityProjectPtr& project, const muse::io::path_t& targetPath);
    void finish();

private:
    struct Chunk {
        std::vector<float> samples; // interleaved
        int channels = 0;
        int rate = 0;
    };

    void copyNewAudio(bool force);
    void writeStatus(const QString& status) const;
    void writerLoop(QString wavPath);

    IAudacityProject* m_project = nullptr;
    QString m_targetPath;
    QString m_wavPath;
    QString m_statusPath;
    QString m_startTime;
    int64_t m_copiedSamples = 0;
    int m_channels = 0;
    int m_rate = 0;
    QElapsedTimer m_sinceLastCopy;
    bool m_started = false;

    //! NOTE: the (network) file is written by its own thread, so that a slow share never blocks the recording
    std::thread m_writer;
    std::mutex m_mutex;
    std::condition_variable m_hasWork;
    std::deque<Chunk> m_queue;
    bool m_stopping = false;
};
}
