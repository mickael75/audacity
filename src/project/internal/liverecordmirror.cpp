/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "liverecordmirror.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

#include "framework/global/async/async.h"
#include "framework/global/log.h"

#include "au3wrap/au3types.h"
#include "au3wrap/internal/domaccessor.h"

using namespace au::project;

//! NOTE: copy the recorded audio at most this often (ms), the file doesn't need to follow the audio more closely
static constexpr qint64 COPY_INTERVAL_MS = 1000;
static constexpr size_t COPY_BLOCK_FRAMES = 65536;

//! NOTE: a 16-bit PCM WAV header; the sizes are rewritten after each block so that the file is always valid
static QByteArray wavHeader(int channels, int rate, quint32 dataBytes)
{
    QByteArray header;
    auto put16 = [&header](quint16 v) { header.append(char(v & 0xFF)).append(char((v >> 8) & 0xFF)); };
    auto put32 = [&put16](quint32 v) { put16(quint16(v & 0xFFFF)); put16(quint16(v >> 16)); };

    const quint16 blockAlign = quint16(channels * 2);
    header.append("RIFF");
    put32(36 + dataBytes);
    header.append("WAVE");
    header.append("fmt ");
    put32(16);
    put16(1); // PCM
    put16(quint16(channels));
    put32(quint32(rate));
    put32(quint32(rate) * blockAlign);
    put16(blockAlign);
    put16(16);
    header.append("data");
    put32(dataBytes);
    return header;
}

LiveRecordMirror::LiveRecordMirror(const muse::modularity::ContextPtr& ctx)
    : muse::Contextable(ctx)
{
}

LiveRecordMirror::~LiveRecordMirror()
{
    finish();
}

QString LiveRecordMirror::liveRecordDir()
{
    //! NOTE: read from the arguments (Unicode on Windows), the option is declared by the command line parser
    const QStringList args = QCoreApplication::arguments();
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--live-dir") {
            return args.at(i + 1);
        }
    }

    return qEnvironmentVariable("AU_LIVE_RECORD_DIR");
}

void LiveRecordMirror::start(const IAudacityProjectPtr& project, const muse::io::path_t& targetPath)
{
    const QString dir = liveRecordDir();
    if (m_started || dir.isEmpty() || !project) {
        return;
    }

    if (!QDir().mkpath(dir)) {
        LOGE() << "live recording: can't create " << dir;
        return;
    }

    m_project = project.get();
    m_targetPath = targetPath.toQString();
    m_startTime = QDateTime::currentDateTime().toString(Qt::ISODate);

    const QString baseName = QFileInfo(m_targetPath).completeBaseName();
    const QString name = baseName + "_" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    m_wavPath = QDir(dir).filePath(name + ".wav");
    m_statusPath = QDir(dir).filePath(name + ".json");
    m_copiedSamples = 0;
    m_channels = 0;
    m_rate = 0;
    m_stopping = false;
    m_started = true;
    m_sinceLastCopy.start();

    writeStatus("recording");
    LOGI() << "live recording to " << m_wavPath;

    m_writer = std::thread(&LiveRecordMirror::writerLoop, this, m_wavPath);

    record()->recordPositionChanged().onReceive(this, [this](const muse::secs_t&) {
        copyNewAudio(false);
    });

    //! NOTE: finish on the next event loop, not while the notification is being sent
    record()->recordingFinished().onNotify(this, [this]() {
        muse::async::Async::call(this, [this]() {
            finish();
        });
    });
}

void LiveRecordMirror::finish()
{
    if (!m_started) {
        return;
    }

    copyNewAudio(true);

    record()->recordPositionChanged().disconnect(this);
    record()->recordingFinished().disconnect(this);

    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_hasWork.notify_one();
    if (m_writer.joinable()) {
        m_writer.join();
    }

    writeStatus("done");
    LOGI() << "live recording done: " << m_wavPath;

    m_started = false;
    m_project = nullptr;
}

void LiveRecordMirror::copyNewAudio(bool force)
{
    if (!m_project || (!force && m_sinceLastCopy.elapsed() < COPY_INTERVAL_MS)) {
        return;
    }
    m_sinceLastCopy.restart();

    const std::vector<au::trackedit::ClipKey>& keys = record()->recordingClipKeys();
    if (keys.empty() || !keys.front().isValid()) {
        return;
    }

    auto* au3Project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
    if (!au3Project) {
        return;
    }

    au::au3::Au3WaveTrack* track = au::au3::DomAccessor::findWaveTrack(*au3Project, au::au3::Au3TrackId(keys.front().trackId));
    if (!track) {
        return;
    }

    const std::shared_ptr<au::au3::Au3WaveClip> clip = au::au3::DomAccessor::findWaveClip(track, keys.front().itemId);
    if (!clip) {
        return;
    }

    const int64_t total = clip->GetVisibleSampleCount().as_long_long();
    const int channels = static_cast<int>(clip->NChannels());
    if (m_channels == 0) {
        m_channels = channels;
        m_rate = clip->GetRate();
    }

    //! NOTE: the file keeps its first layout (a recording doesn't change it)
    if (channels != m_channels || total <= m_copiedSamples) {
        return;
    }

    std::vector<std::vector<float> > buffers(channels);
    while (m_copiedSamples < total) {
        const size_t frames = static_cast<size_t>(std::min<int64_t>(COPY_BLOCK_FRAMES, total - m_copiedSamples));

        for (int c = 0; c < channels; ++c) {
            buffers[c].assign(frames, 0.0f);
            clip->GetSamples(size_t(c), reinterpret_cast<samplePtr>(buffers[c].data()), floatSample,
                             sampleCount(m_copiedSamples), frames, false /*mayThrow*/);
        }

        Chunk chunk;
        chunk.channels = channels;
        chunk.rate = m_rate;
        chunk.samples.resize(frames * channels);
        for (size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < channels; ++c) {
                chunk.samples[f * channels + c] = buffers[c][f];
            }
        }

        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(chunk));
        }
        m_hasWork.notify_one();

        m_copiedSamples += static_cast<int64_t>(frames);
    }
}

void LiveRecordMirror::writeStatus(const QString& status) const
{
    QJsonObject json;
    json["status"] = status;
    json["target"] = m_targetPath;
    json["audio"] = QFileInfo(m_wavPath).fileName();
    json["host"] = QSysInfo::machineHostName();
    json["started"] = m_startTime;
    json["updated"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    QFile file(m_statusPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOGW() << "live recording: can't write " << m_statusPath;
        return;
    }
    file.write(QJsonDocument(json).toJson());
}

void LiveRecordMirror::writerLoop(QString wavPath)
{
    //! NOTE: the file is shared for reading, so that other computers can open it while it grows
    QFile file(wavPath);
    quint32 dataBytes = 0;
    bool opened = false;
    bool failed = false;

    while (true) {
        Chunk chunk;
        {
            std::unique_lock lock(m_mutex);
            m_hasWork.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                break; // stopping, everything written
            }
            chunk = std::move(m_queue.front());
            m_queue.pop_front();
        }

        if (failed) {
            continue;
        }

        if (!opened) {
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                LOGE() << "live recording: can't write " << wavPath << ": " << file.errorString();
                failed = true;
                continue;
            }
            file.write(wavHeader(chunk.channels, chunk.rate, 0));
            opened = true;
        }

        QByteArray pcm;
        pcm.resize(int(chunk.samples.size() * 2));
        for (size_t i = 0; i < chunk.samples.size(); ++i) {
            const float clamped = std::clamp(chunk.samples[i], -1.0f, 1.0f);
            const qint16 value = static_cast<qint16>(std::lround(clamped * 32767.0f));
            pcm[int(i * 2)] = char(value & 0xFF);
            pcm[int(i * 2 + 1)] = char((value >> 8) & 0xFF);
        }

        if (file.write(pcm) != pcm.size()) {
            LOGE() << "live recording: write error on " << wavPath << ": " << file.errorString();
            failed = true;
            continue;
        }
        dataBytes += quint32(pcm.size());

        //! NOTE: keep the header sizes up to date, then go back to the end for the next block
        file.seek(0);
        file.write(wavHeader(chunk.channels, chunk.rate, dataBytes));
        file.seek(file.size());
        file.flush();
    }

    if (opened) {
        file.close();
    }
}
