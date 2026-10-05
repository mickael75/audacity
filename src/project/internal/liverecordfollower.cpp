/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "liverecordfollower.h"
#include "liverecordsession.h"

#include <algorithm>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "framework/global/log.h"
#include "framework/global/translation.h"
#include "framework/ui/view/iconcodes.h"

#include "au3wrap/au3types.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "au3-track/Track.h"

using namespace au::project;

static constexpr int FOLLOW_INTERVAL_MS = 2000;

LiveRecordFollower::LiveRecordFollower(const muse::modularity::ContextPtr& ctx)
    : muse::Contextable(ctx)
{
    m_timer.setInterval(FOLLOW_INTERVAL_MS);
    QObject::connect(&m_timer, &QTimer::timeout, [this]() {
        follow();
    });
}

LiveRecordFollower::~LiveRecordFollower()
{
    stop();
}

bool LiveRecordFollower::isRunningLiveRecording(const muse::io::path_t& wavPath)
{
    LiveRecordSession::State state;
    QString error;
    return LiveRecordSession::readState(wavPath.toQString(), state, error) && state == LiveRecordSession::State::Recording;
}

bool LiveRecordFollower::isLiveRecording(const muse::io::path_t& wavPath)
{
    LiveRecordSession::State state;
    QString error;
    return LiveRecordSession::readState(wavPath.toQString(), state, error);
}

bool LiveRecordFollower::readWavFormat(const QString& path, WavFormat& format)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray riff = file.read(12);
    if (riff.size() < 12 || !riff.startsWith("RIFF") || riff.mid(8, 4) != "WAVE") {
        return false;
    }

    auto le16 = [](const QByteArray& b, int i) { return int(uchar(b[i])) | (int(uchar(b[i + 1])) << 8); };
    auto le32 = [&le16](const QByteArray& b, int i) { return quint32(le16(b, i)) | (quint32(le16(b, i + 2)) << 16); };

    //! NOTE: walk the chunks up to "data"
    while (true) {
        const QByteArray chunk = file.read(8);
        if (chunk.size() < 8) {
            return false;
        }

        const QByteArray id = chunk.left(4);
        const quint32 size = le32(chunk, 4);
        if (id == "fmt ") {
            if (size < 16 || size > 65536) {
                return false;
            }
            const QByteArray fmt = file.read(size);
            if (fmt.size() != size || le16(fmt, 0) != 1) {
                return false;
            }
            format.channels = le16(fmt, 2);
            format.bitsPerSample = le16(fmt, 14);
            const quint32 sampleRate = le32(fmt, 4);
            if (sampleRate == 0 || sampleRate > 768000) {
                return false;
            }
            format.sampleRate = int(sampleRate);
            if (size % 2) {
                file.read(1);
            }
        } else if (id == "data") {
            format.dataOffset = file.pos();
            format.dataBytes = size;
            return format.channels > 0 && format.channels <= 2 && (format.bitsPerSample == 16 || format.bitsPerSample == 24);
        } else if (!file.seek(file.pos() + size + (size % 2))) {
            return false;
        }
    }
}

bool LiveRecordFollower::start(const IAudacityProjectPtr& project, const muse::io::path_t& wavPath,
                               const muse::io::path_t& draftPath)
{
    if (!project || !readWavFormat(wavPath.toQString(), m_format)) {
        reportError("Cannot follow the live WAV: " + wavPath.toQString());
        return false;
    }

    auto* au3Project = reinterpret_cast<au::au3::Au3Project*>(project->au3ProjectPtr());
    auto tracks = au::au3::Au3TrackList::Get(*au3Project).Any<au::au3::Au3WaveTrack>();
    if (tracks.begin() == tracks.end()) {
        reportError("The live recording has no audio track.");
        return false;
    }

    //! NOTE: the import read the audio written so far
    const auto clip = (*tracks.begin())->GetRightmostClip();
    if (!clip || static_cast<int>(clip->NChannels()) != m_format.channels) {
        reportError("The live recording has no compatible audio clip.");
        return false;
    }

    m_project = project.get();
    m_wavPath = wavPath.toQString();
    m_framesRead = clip->GetSequenceSamplesCount().as_long_long();
    m_sourceTrackId = (*tracks.begin())->GetId();
    m_sourceClipId = clip->GetId();

    // Keep an untouched live source and give the editor an independent working copy.
    auto copy = (*tracks.begin())->Duplicate();
    au::au3::Au3TrackList::Get(*au3Project).Add(copy);
    (*tracks.begin())->SetMute(true);
    project->trackeditProject()->reload();
    projectHistory()->modifyState();

    toastService()->show(muse::trc("project", "Live recording"),
                         muse::mtrc("project", "Following the live recording \"%1\": its end is added as it is recorded. "
                                               "The muted source grows; edit the working copy. Save writes \"%2\". "
                                               "Use File > Montage finished to send the frozen edit to Zetta.")
                         .arg(muse::String::fromQString(QFileInfo(m_wavPath).fileName()))
                         .arg(draftPath.toString()).toStdString(),
                         muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});

    m_timer.start();
    return true;
}

void LiveRecordFollower::stop()
{
    m_timer.stop();
    m_project = nullptr;
}

void LiveRecordFollower::follow()
{
    if (!m_project) {
        return;
    }

    LiveRecordSession::State state;
    QString error;
    if (!LiveRecordSession::readState(m_wavPath, state, error)) {
        reportError(error);
        return;
    }
    if (state == LiveRecordSession::State::Failed) {
        reportError("The live recording failed. It cannot be published to Zetta.");
        return;
    }
    const bool done = state == LiveRecordSession::State::Done;

    QFile file(m_wavPath);
    if (!file.open(QIODevice::ReadOnly)) {
        reportError(m_wavPath + ": " + file.errorString());
        return;
    }

    WavFormat currentFormat;
    if (!readWavFormat(m_wavPath, currentFormat) || currentFormat.channels != m_format.channels
        || currentFormat.bitsPerSample != m_format.bitsPerSample || currentFormat.dataOffset != m_format.dataOffset
        || currentFormat.sampleRate != m_format.sampleRate) {
        reportError("The live WAV format changed or could not be read.");
        return;
    }
    const int bytesPerSample = m_format.bitsPerSample / 8;
    const int blockAlign = bytesPerSample * m_format.channels;
    if (done && file.size() < m_format.dataOffset + currentFormat.dataBytes) {
        reportError("The completed live recording is incomplete.");
        return;
    }
    const int64_t availableFrames = std::min<qint64>(file.size() - m_format.dataOffset, currentFormat.dataBytes) / blockAlign;
    const int64_t newFrames = availableFrames - m_framesRead;
    if (newFrames < 0) {
        reportError("The live recording was truncated.");
        return;
    }

    if (newFrames > 0) {
        if (!file.seek(m_format.dataOffset + m_framesRead * blockAlign)) {
            reportError(file.errorString());
            return;
        }
        const int64_t framesToRead = std::min<int64_t>(newFrames, int64_t(m_format.sampleRate) * 4);
        const QByteArray data = file.read(framesToRead * blockAlign);
        if (data.size() != framesToRead * blockAlign) {
            reportError("Cannot read the committed live audio: " + file.errorString());
            return;
        }
        const int64_t frames = data.size() / blockAlign;

        auto* au3Project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
        auto* track = au::au3::DomAccessor::findWaveTrack(*au3Project, au::au3::Au3TrackId(m_sourceTrackId));
        const auto clip = track ? au::au3::DomAccessor::findWaveClip(track, m_sourceClipId) : nullptr;
        if (!clip || clip->GetSequenceSamplesCount().as_long_long() != m_framesRead || frames == 0
            || static_cast<int>(clip->NChannels()) != m_format.channels) {
            reportError("The live source was edited or removed. Edit the working copy, not the live source.");
            return;
        }

        if (clip && frames > 0 && static_cast<int>(clip->NChannels()) == m_format.channels) {
            std::vector<std::vector<float> > channels(m_format.channels, std::vector<float>(size_t(frames)));
            const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
            for (int64_t f = 0; f < frames; ++f) {
                for (int c = 0; c < m_format.channels; ++c) {
                    const uchar* s = bytes + f * blockAlign + c * bytesPerSample;
                    const float value = bytesPerSample == 2
                                        ? float(qint16(quint16(s[0]) | (quint16(s[1]) << 8))) / 32768.0f
                                        : float(qint32((quint32(s[0]) << 8) | (quint32(s[1]) << 16) | (quint32(s[2]) << 24)) >> 8)
                                        / 8388608.0f;
                    channels[c][size_t(f)] = value;
                }
            }

            std::vector<constSamplePtr> buffers;
            for (const auto& channel : channels) {
                buffers.push_back(reinterpret_cast<constSamplePtr>(channel.data()));
            }

            clip->Append(buffers.data(), floatSample, size_t(frames), 1, bytesPerSample == 2 ? int16Sample : int24Sample);
            clip->Flush();
            clip->MarkChanged();
            m_framesRead += frames;

            if (const auto trackeditProject = m_project->trackeditProject()) {
                trackeditProject->notifyAboutClipChanged(au::au3::DomConverter::clip(track, clip.get()));
            }
            //! NOTE: keep the new audio in the current undo state
            projectHistory()->modifyState();
        }
    }

    if (done && m_framesRead == availableFrames) {
        stop();
        toastService()->show(muse::trc("project", "Live recording"),
                             muse::mtrc("project", "The live recording \"%1\" is finished.")
                             .arg(muse::String::fromQString(QFileInfo(m_wavPath).fileName())).toStdString(),
                             muse::ui::IconCode::Code::TICK, true /*dismissable*/, {});
    }
}

void LiveRecordFollower::reportError(const QString& error)
{
    LOGE() << "live recording: " << error;
    stop();
    toastService()->show(muse::trc("project", "Live recording error"), error.toStdString(),
                         muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});
}
