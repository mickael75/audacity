/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "liverecordfollower.h"

#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

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

QString LiveRecordFollower::statusPath(const QString& wavPath)
{
    const QFileInfo info(wavPath);
    return info.dir().filePath(info.completeBaseName() + ".json");
}

bool LiveRecordFollower::isRunningLiveRecording(const muse::io::path_t& wavPath)
{
    QFile status(statusPath(wavPath.toQString()));
    if (!status.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonObject json = QJsonDocument::fromJson(status.readAll()).object();
    return json.value("status").toString() == "recording";
}

muse::io::path_t LiveRecordFollower::montagePath(const muse::io::path_t& wavPath)
{
    const QFileInfo info(wavPath.toQString());
    return muse::io::path_t(info.dir().filePath(info.completeBaseName() + "_montage.wav"));
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
            const QByteArray fmt = file.read(size);
            if (fmt.size() < 16) {
                return false;
            }
            format.channels = le16(fmt, 2);
            format.bitsPerSample = le16(fmt, 14);
            if (size % 2) {
                file.read(1);
            }
        } else if (id == "data") {
            format.dataOffset = file.pos();
            return format.channels > 0 && (format.bitsPerSample == 16 || format.bitsPerSample == 24);
        } else if (!file.seek(file.pos() + size + (size % 2))) {
            return false;
        }
    }
}

void LiveRecordFollower::start(const IAudacityProjectPtr& project, const muse::io::path_t& wavPath)
{
    if (!project || !readWavFormat(wavPath.toQString(), m_format)) {
        LOGW() << "live recording: can't follow " << wavPath.toQString();
        return;
    }

    auto* au3Project = reinterpret_cast<au::au3::Au3Project*>(project->au3ProjectPtr());
    auto tracks = au::au3::Au3TrackList::Get(*au3Project).Any<au::au3::Au3WaveTrack>();
    if (tracks.begin() == tracks.end()) {
        return;
    }

    //! NOTE: the import read the audio written so far
    const auto clip = (*tracks.begin())->GetRightmostClip();
    if (!clip || static_cast<int>(clip->NChannels()) != m_format.channels) {
        return;
    }

    m_project = project.get();
    m_wavPath = wavPath.toQString();
    m_framesRead = clip->GetSequenceSamplesCount().as_long_long();

    toastService()->show(muse::trc("project", "Live recording"),
                         muse::mtrc("project", "Following the live recording \"%1\": its end is added as it is recorded. "
                                               "Edit a copy of it; saving writes \"%2\".")
                         .arg(muse::String::fromQString(QFileInfo(m_wavPath).fileName()))
                         .arg(montagePath(wavPath).toString()).toStdString(),
                         muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});

    m_timer.start();
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

    //! NOTE: check the status first: once done, the last audio is read, then the following stops
    const bool done = !isRunningLiveRecording(muse::io::path_t(m_wavPath));

    QFile file(m_wavPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const int bytesPerSample = m_format.bitsPerSample / 8;
    const int blockAlign = bytesPerSample * m_format.channels;
    const int64_t availableFrames = (file.size() - m_format.dataOffset) / blockAlign;
    const int64_t newFrames = availableFrames - m_framesRead;

    if (newFrames > 0 && file.seek(m_format.dataOffset + m_framesRead * blockAlign)) {
        const QByteArray data = file.read(newFrames * blockAlign);
        const int64_t frames = data.size() / blockAlign;

        auto* au3Project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
        auto tracks = au::au3::Au3TrackList::Get(*au3Project).Any<au::au3::Au3WaveTrack>();
        au::au3::Au3WaveTrack* track = tracks.begin() != tracks.end() ? *tracks.begin() : nullptr;
        const auto clip = track ? track->GetRightmostClip() : nullptr;

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

    if (done) {
        stop();
        toastService()->show(muse::trc("project", "Live recording"),
                             muse::mtrc("project", "The live recording \"%1\" is finished.")
                             .arg(muse::String::fromQString(QFileInfo(m_wavPath).fileName())).toStdString(),
                             muse::ui::IconCode::Code::TICK, true /*dismissable*/, {});
    }
}
