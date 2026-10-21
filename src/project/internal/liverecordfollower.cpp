/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "liverecordfollower.h"
#include "liverecordsession.h"
#include "livesessionpresence.h"

#include <algorithm>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>

#include "framework/global/log.h"
#include "framework/global/translation.h"
#include "framework/ui/view/iconcodes.h"

#include "au3wrap/au3types.h"
#include "au3wrap/internal/domaccessor.h"
#include "au3wrap/internal/domconverter.h"
#include "au3-track/Track.h"
#include "au3-project-file-io/ProjectFileIO.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "au3-realtime-effects/SavedMasterEffectList.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "framework/global/defer.h"

using namespace au::project;

static constexpr int FOLLOW_INTERVAL_MS = 2000;

LiveRecordFollower::LiveRecordFollower(const muse::modularity::ContextPtr& ctx)
    : muse::Contextable(ctx)
{
    m_timer.setInterval(FOLLOW_INTERVAL_MS);
    QObject::connect(&m_timer, &QTimer::timeout, [this]() {
        follow();
    });
    m_syncTimer.setInterval(1000);
    QObject::connect(&m_syncTimer, &QTimer::timeout, [this]() {
        synchronizeMontage();
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

bool LiveRecordFollower::sharedMontageEnabled()
{
    return QCoreApplication::arguments().contains("--shared-montage")
           || qEnvironmentVariable("AU_SHARED_MONTAGE") == "1";
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

    m_shared = sharedMontageEnabled();
    if (m_shared) {
        m_editorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString error;
        if (!LiveRecordSession::joinMontage(m_wavPath, m_editorId, LiveSessionPresence::stationName(), error)) {
            reportError(error);
            return false;
        }
    }

    // Keep an untouched live source and give the editor an independent working copy.
    auto copy = (*tracks.begin())->Duplicate();
    au::au3::Au3TrackList::Get(*au3Project).Add(copy);
    (*tracks.begin())->SetMute(true);
    project->trackeditProject()->reload();
    projectHistory()->modifyState();

    if (m_shared) {
        m_rawTrack = std::static_pointer_cast<WaveTrack>(au::au3::Au3TrackList::Get(*au3Project).Remove(**tracks.begin()));
        // Establish an initial editable state containing only the montage.
        project->trackeditProject()->reload();
        projectHistory()->init();
        auto* working = static_cast<au::au3::Au3WaveTrack*>(copy.get());
        const auto tail = working->GetRightmostClip();
        m_tailTrackId = working->GetId();
        m_tailClipId = tail->GetId();
        m_tailSamples = tail->GetSequenceSamplesCount().as_long_long();
        if (!synchronizeMontage() && !m_project) {
            return false;
        }
        projectHistory()->historyChanged().onReceive(this, [this](const trackedit::HistoryEvent& event) {
            if (!m_project || m_applying) {
                return;
            }
            if (event == trackedit::HistoryEvent::RestoredState) {
                m_applying = true;
                const auto saved = m_stateFrames.find(projectHistory()->currentStateIndex());
                if (saved != m_stateFrames.end() && saved->second < m_framesRead) {
                    m_tailClipId = -1;
                    if (!catchUpMontage(saved->second)) {
                        m_applying = false;
                        return;
                    }
                    projectHistory()->modifyState();
                }
                m_applying = false;
            }
            m_stateFrames[projectHistory()->currentStateIndex()] = m_framesRead;
            m_dirty = true;
        });
        projectHistory()->stateModified().onNotify(this, [this]() {
            if (m_project && !m_applying) {
                m_stateFrames[projectHistory()->currentStateIndex()] = m_framesRead;
                m_dirty = true;
            }
        });
        m_stateFrames[projectHistory()->currentStateIndex()] = m_framesRead;
        m_syncTimer.start();
    }

    if (m_shared) {
        toastService()->show(muse::trc("project", "Shared live montage"),
                             muse::trc("project", "New audio is added automatically to the montage, and changes are shared "
                                                  "with the other workstations. Close this montage window to leave the session. "
                                                  "Zetta is updated after recording stops and every editor has left."),
                             muse::ui::IconCode::Code::WARNING, true, {});
    } else {
        toastService()->show(muse::trc("project", "Live recording"),
                         muse::mtrc("project", "Following the live recording \"%1\": its end is added as it is recorded. "
                                               "The muted source grows; edit the working copy. Save writes \"%2\". "
                                               "Use File > Montage finished to send the frozen edit to Zetta.")
                         .arg(muse::String::fromQString(QFileInfo(m_wavPath).fileName()))
                         .arg(draftPath.toString()).toStdString(),
                         muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});
    }

    m_timer.start();
    return true;
}

void LiveRecordFollower::stop()
{
    m_timer.stop();
    m_syncTimer.stop();
    m_project = nullptr;
}

void LiveRecordFollower::follow()
{
    if (!m_project) {
        return;
    }
    // Do not mutate the project underneath a drag, cut, or other undoable interaction.
    if (projectHistory()->interactionOngoing()) {
        return;
    }
    m_applying = true;
    DEFER { m_applying = false; };

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
        auto* track = m_shared ? m_rawTrack.get()
                     : au::au3::DomAccessor::findWaveTrack(*au3Project, au::au3::Au3TrackId(m_sourceTrackId));
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
            if (m_shared) {
                appendToMontage(channels, size_t(frames));
            }
            m_framesRead += frames;

            if (const auto trackeditProject = m_project->trackeditProject(); trackeditProject && !m_shared) {
                trackeditProject->notifyAboutClipChanged(au::au3::DomConverter::clip(track, clip.get()));
            }
            //! NOTE: keep the new audio in the current undo state
            projectHistory()->modifyState();
            if (m_shared) {
                m_stateFrames[projectHistory()->currentStateIndex()] = m_framesRead;
            }
        }
    }

    if (done && m_framesRead == availableFrames) {
        // Editing and sharing continue after capture ends, until final submission.
        m_timer.stop();
        m_audioFinished = true;
        if (!m_shared) {
            stop();
        }
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

void LiveRecordFollower::appendToMontage(const std::vector<std::vector<float>>& channels, size_t frames)
{
    auto* project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
    auto& tracks = au::au3::Au3TrackList::Get(*project);
    auto* source = m_rawTrack.get();
    auto* working = au::au3::DomAccessor::findWaveTrack(*project, au::au3::Au3TrackId(m_tailTrackId));
    double continuationTime = -1.0;
    if (!working || int(working->NChannels()) != m_format.channels || working->GetRate() != m_format.sampleRate) {
        continuationTime = 0.0;
        for (const auto* wave : tracks.Any<WaveTrack>()) {
            continuationTime = std::max(continuationTime, wave->GetEndTime());
        }
        auto copy = source->EmptyCopy();
        copy->SetMute(false);
        copy->SetSolo(false);
        working = copy.get();
        tracks.Add(copy);
        m_tailTrackId = working->GetId();
    }
    auto tail = au::au3::DomAccessor::findWaveClip(working, m_tailClipId);
    // Once the incoming tail is cut, trimmed, stretched, or removed, preserve
    // the edit and continue in a new clip after the current montage.
    if (!tail || tail->GetSequenceSamplesCount().as_long_long() != m_tailSamples
        || tail->GetTrimRight() != 0 || tail->GetStretchRatio() != 1.0
        || tail->GetCentShift() != 0 || tail->GetRate() != m_format.sampleRate
        || working->GetRightmostClip()->GetId() != tail->GetId()) {
        tail = working->CreateClip(continuationTime >= 0 ? continuationTime : std::max(0.0, working->GetEndTime()),
                                  source->GetName());
        working->InsertInterval(tail, true, true);
        m_tailClipId = tail->GetId();
        m_tailSamples = 0;
    }
    std::vector<constSamplePtr> buffers;
    for (const auto& channel : channels) {
        buffers.push_back(reinterpret_cast<constSamplePtr>(channel.data()));
    }
    tail->Append(buffers.data(), floatSample, frames, 1, floatSample);
    tail->Flush();
    tail->MarkChanged();
    m_tailSamples += int64_t(frames);
    m_project->trackeditProject()->reload();
}

bool LiveRecordFollower::publishMontage()
{
    auto* project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
    int tailIndex = -1;
    int index = 0;
    for (const auto& track : au::au3::Au3TrackList::Get(*project)) {
        if (track->GetId() == au::au3::Au3TrackId(m_tailTrackId)) {
            tailIndex = index;
        }
        ++index;
    }
    auto* working = au::au3::DomAccessor::findWaveTrack(*project, au::au3::Au3TrackId(m_tailTrackId));
    const auto tail = working ? au::au3::DomAccessor::findWaveClip(working, m_tailClipId) : nullptr;
    QTemporaryDir staging;
    const QString localPath = staging.filePath("montage.aup3");
    SavedMasterEffectList::Get(*project).UpdateCopy();
    if (!staging.isValid()
        || !ProjectFileIO::Get(*project).SaveCopy(au::au3::wxFromString(muse::String::fromQString(localPath)))) {
        reportError("Cannot save the shared montage snapshot.");
        return false;
    }
    QJsonObject metadata { { "tailIndex", tailIndex },
                           { "frames", QString::number(m_framesRead) },
                           { "tailSamples", QString::number(m_tailSamples) },
                           { "tailStart", tail ? tail->GetPlayStartTime() : -1.0 },
                           { "rate", m_format.sampleRate }, { "channels", m_format.channels } };
    QString error;
    LiveRecordSession::MontageSnapshot snapshot;
    const auto result = LiveRecordSession::publishMontage(m_wavPath, m_revision, localPath, metadata, snapshot, error);
    if (result == LiveRecordSession::MontageUpdate::Published) {
        m_revision = snapshot.revision;
        m_dirty = false;
        return true;
    }
    if (result == LiveRecordSession::MontageUpdate::Conflict) {
        // Always retain the losing edit as a full native project before asking
        // to load another editor's revision; a network race must not lose work.
        const QString backup = QFileInfo(m_wavPath).dir().filePath(
            QFileInfo(m_wavPath).completeBaseName() + ".conflict-"
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".aup3");
        if (!QFile::copy(localPath, backup)) {
            m_syncPaused = true;
            reportError("Another workstation edited this montage. The local edit could not be backed up; "
                        "save your project before reopening the shared montage.");
            return false;
        }
        const auto answer = interactive()->questionSync(
            muse::trc("project", "Shared montage conflict"),
            muse::mtrc("project", "Another workstation changed the montage at the same time. "
                                  "Your local edit is preserved in %1. Load the latest shared montage? "
                                  "Cancel keeps your local edit and pauses sharing; reopen from Zetta to rejoin.")
            .arg(muse::String::fromQString(backup)).toStdString(),
            { muse::IInteractive::Button::Cancel, muse::IInteractive::Button::Yes }, muse::IInteractive::Button::Cancel);
        if (answer.standardButton() == muse::IInteractive::Button::Yes) {
            // The snapshot may be ahead of the local incoming audio. Keep
            // catching up without repeatedly prompting for the same conflict.
            m_dirty = false;
            return applyMontage(snapshot);
        }
        m_syncPaused = true;
        return false;
    }
    if (result == LiveRecordSession::MontageUpdate::Frozen) {
        m_finalFrozen = true;
        m_syncPaused = true;
        m_timer.stop();
        m_syncTimer.stop();
        toastService()->show(muse::trc("project", "Montage finished"), error.toStdString(),
                             muse::ui::IconCode::Code::WARNING, true, {});
        return false;
    }
    // Transient share failures are retried without changing the revision or local edit.
    LOGW() << "shared montage: " << error;
    return false;
}

bool LiveRecordFollower::applyMontage(const LiveRecordSession::MontageSnapshot& snapshot)
{
    bool validFrames = false;
    bool validTail = false;
    const int64_t frames = snapshot.metadata.value("frames").toString().toLongLong(&validFrames);
    const int64_t tailSamples = snapshot.metadata.value("tailSamples").toString().toLongLong(&validTail);
    const int tailIndex = snapshot.metadata.value("tailIndex").toInt(-1);
    if (!validFrames || !validTail || frames < 0 || tailSamples < 0
        || snapshot.metadata.value("rate").toInt() != m_format.sampleRate
        || snapshot.metadata.value("channels").toInt() != m_format.channels) {
        reportError("Invalid shared montage metadata.");
        return false;
    }
    if (frames > m_framesRead) {
        return false; // Catch up with committed recording audio before applying this revision.
    }
    QTemporaryDir staging;
    const QString localPath = staging.filePath("incoming.aup3");
    if (!staging.isValid() || !QFile::copy(snapshot.projectPath, localPath)) {
        return false;
    }
    const auto incoming = au3ProjectCreator()->create(iocContext());
    if (!incoming || !incoming->load(muse::io::path_t(localPath), true)) {
        reportError("Cannot open the shared montage snapshot.");
        return false;
    }
    DEFER { incoming->close(); };
    auto* project = reinterpret_cast<au::au3::Au3Project*>(m_project->au3ProjectPtr());
    auto* loaded = reinterpret_cast<au::au3::Au3Project*>(incoming->au3ProjectPtr());
    auto* source = m_rawTrack.get();
    if (!source) {
        reportError("The live source track was removed.");
        return false;
    }
    std::vector<Track::Holder> replacements;
    std::vector<std::shared_ptr<RealtimeEffectState>> masterStates;
    const auto& incomingMaster = RealtimeEffectList::Get(*loaded);
    for (size_t effect = 0; effect < incomingMaster.GetStatesCount(); ++effect) {
        auto state = incomingMaster.GetStateAt(effect)->Clone();
        if (!state->GetEffect()) {
            reportError("A shared montage master effect is unavailable on this workstation.");
            return false;
        }
        masterStates.push_back(std::move(state));
    }
    au::au3::Au3WaveTrack* newTailTrack = nullptr;
    std::shared_ptr<au::au3::Au3WaveClip> newTail;
    int index = 0;
    for (const auto& track : au::au3::Au3TrackList::Get(*loaded)) {
        if (const auto* wave = dynamic_cast<const au::au3::Au3WaveTrack*>(track)) {
            // Copy sample blocks into the local project's database. No editor
            // holds an open SQLite project over SMB or shares writable blocks.
            auto copy = wave->EmptyCopy(source->GetSampleBlockFactory());
            for (const auto& clip : wave->Intervals()) {
                auto copied = copy->CopyClip(*clip, true);
                copy->InsertInterval(copied, false, true);
                if (index == tailIndex && copied->GetPlayStartTime() == snapshot.metadata.value("tailStart").toDouble(-1)
                    && copied->GetSequenceSamplesCount().as_long_long() == tailSamples) {
                    newTail = copied;
                }
            }
            if (index == tailIndex) {
                newTailTrack = copy.get();
            }
            replacements.push_back(copy);
        } else {
            replacements.push_back(track->Duplicate());
        }
        ++index;
    }
    auto& tracks = au::au3::Au3TrackList::Get(*project);
    std::vector<Track::Holder> old;
    for (const auto& track : tracks) {
        if (track != source) {
            old.push_back(track->shared_from_this());
        }
    }
    for (const auto& track : old) {
        tracks.Remove(*track);
    }
    auto& signature = ProjectTimeSignature::Get(*project);
    const auto& incomingSignature = ProjectTimeSignature::Get(*loaded);
    signature.SetTempo(incomingSignature.GetTempo());
    signature.SetUpperTimeSignature(incomingSignature.GetUpperTimeSignature());
    signature.SetLowerTimeSignature(incomingSignature.GetLowerTimeSignature());
    for (const auto& track : replacements) {
        tracks.Add(track);
    }
    auto& master = RealtimeEffectList::Get(*project);
    master.Clear();
    for (auto& state : masterStates) {
        master.AddState(state);
    }
    master.SetActive(incomingMaster.IsActive());
    selectionController()->resetSelectedClips();
    selectionController()->resetSelectedLabels();
    selectionController()->resetSelectedTracks();
    m_tailTrackId = newTailTrack ? int64_t(newTailTrack->GetId()) : -1;
    m_tailClipId = newTail ? newTail->GetId() : -1;
    m_tailSamples = tailSamples;

    if (!catchUpMontage(frames)) {
        return false;
    }
    m_project->trackeditProject()->reload();
    projectHistory()->pushHistoryState(muse::trc("project", "Received shared montage"), muse::trc("project", "Shared montage"));
    m_stateFrames[projectHistory()->currentStateIndex()] = m_framesRead;
    m_revision = snapshot.revision;
    return true;
}

bool LiveRecordFollower::catchUpMontage(int64_t fromFrame)
{
    const auto raw = au::au3::DomAccessor::findWaveClip(m_rawTrack.get(), m_sourceClipId);
    if (!raw) {
        reportError("The live source clip is unavailable.");
        return false;
    }
    for (int64_t position = fromFrame; position < m_framesRead;) {
        const size_t count = size_t(std::min<int64_t>(m_framesRead - position, m_format.sampleRate * 4));
        std::vector<std::vector<float>> channels(m_format.channels, std::vector<float>(count));
        for (int c = 0; c < m_format.channels; ++c) {
            if (!raw->GetSamples(size_t(c), reinterpret_cast<samplePtr>(channels[c].data()), floatSample,
                                 sampleCount(position), count, false)) {
                reportError("Cannot catch up the shared montage with recorded audio.");
                return false;
            }
        }
        appendToMontage(channels, count);
        position += int64_t(count);
    }
    return true;
}

bool LiveRecordFollower::synchronizeMontage()
{
    if (!m_shared || !m_project || m_syncPaused || projectHistory()->interactionOngoing()
        || playbackController()->isPlaying()) {
        return !m_shared;
    }
    m_applying = true;
    DEFER { m_applying = false; };
    if (QFile::exists(LiveRecordSession::submissionPath(m_wavPath))) {
        m_finalFrozen = true;
        m_syncPaused = true;
        m_syncTimer.stop();
        m_timer.stop();
        toastService()->show(muse::trc("project", "Montage finished"),
                             muse::trc("project", "The shared montage was frozen by an editor. Further local changes are not sent to Zetta."),
                             muse::ui::IconCode::Code::WARNING, true, {});
        return false;
    }
    if (m_dirty) {
        return publishMontage();
    }
    LiveRecordSession::MontageSnapshot snapshot;
    QString error;
    if (!LiveRecordSession::readMontage(m_wavPath, snapshot, error, m_revision)) {
        LOGW() << "shared montage: " << error;
        return false;
    }
    if (snapshot.revision.isEmpty()) {
        return publishMontage();
    }
    return snapshot.revision == m_revision || applyMontage(snapshot);
}

bool LiveRecordFollower::leaveSession()
{
    if (!m_shared || m_leftSession) { return true; }
    // Catch up committed audio once before saving. Any later capture is included
    // by the recorder at finalization, even if all editors leave during REC.
    follow();
    if (!m_project || !prepareSubmission()) { return false; }
    QString error;
    if (!LiveRecordSession::leaveMontage(m_wavPath, m_editorId, m_revision, error)) {
        interactive()->error(muse::trc("project", "Shared montage"), error.toStdString());
        return false;
    }
    m_leftSession = true;
    m_timer.stop();
    m_syncTimer.stop();
    return true;
}

bool LiveRecordFollower::prepareSubmission()
{
    if (!m_shared) {
        return true;
    }
    if (!synchronizeMontage() || m_dirty || m_syncPaused || m_revision.isEmpty()) {
        interactive()->error(muse::trc("project", "Shared montage"),
                             muse::trc("project", "The montage is not synchronized. Review the shared version and resolve any conflict before finishing."));
        return false;
    }
    // Include the latest automatically received audio before this editor leaves.
    m_applying = true;
    DEFER { m_applying = false; };
    return publishMontage();
}

bool LiveRecordFollower::submit(const QString& renderedPath, QString& error)
{
    return m_shared ? LiveRecordSession::submitMontage(m_wavPath, m_revision, renderedPath, error)
           : LiveRecordSession::submit(m_wavPath, renderedPath, error);
}
