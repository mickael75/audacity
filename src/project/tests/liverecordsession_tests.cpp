/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include <gtest/gtest.h>

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>

#include <thread>

#include "project/internal/liverecordsession.h"

using namespace au::project;

class LiveRecordSessionTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        live = directory.filePath("recording.wav");
        render = directory.filePath("render.wav");
        audio = QByteArray::fromHex("524946462600000057415645666d7420100000000100010080bb00000077010002001000"
                                   "64617461020000000100");
        writeFile(render, audio);
        setState("recording");
    }

    void writeFile(const QString& path, const QByteArray& data)
    {
        QSaveFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(data), data.size());
        ASSERT_TRUE(file.commit());
    }

    void setState(const QString& state)
    {
        writeFile(LiveRecordSession::statusPath(live),
                  QJsonDocument(QJsonObject { { "version", 1 }, { "status", state } }).toJson());
    }

    QByteArray readFile(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    QTemporaryDir directory;
    QString live;
    QString render;
    QByteArray audio;
};

TEST_F(LiveRecordSessionTests, MontageFirstWaitsForRecordingStop)
{
    QString error;
    QString path;
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error)) << error.toStdString();
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(LiveRecordSession::resultPath(live)));
    setState("done");
    ASSERT_TRUE(LiveRecordSession::publicationReady(live, path, error)) << error.toStdString();
    EXPECT_EQ(readFile(path), audio);
}

TEST_F(LiveRecordSessionTests, RecordingFirstWaitsForFinalMontage)
{
    setState("done");
    QString error;
    QString path;
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_TRUE(error.isEmpty());
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error)) << error.toStdString();
    EXPECT_TRUE(LiveRecordSession::publicationReady(live, path, error));
}

TEST_F(LiveRecordSessionTests, SubmittedSnapshotDoesNotChangeWithLaterEdits)
{
    QString error;
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
    writeFile(render, audio + "later edits");
    writeFile(live, audio + "later recording");
    setState("done");
    QString path;
    ASSERT_TRUE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_EQ(readFile(path), audio);
}

TEST_F(LiveRecordSessionTests, SecondEditorCannotReplaceFinalMontage)
{
    QString error;
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
    writeFile(render, audio + "second montage");
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(readFile(LiveRecordSession::finalAudioPath(live)), audio);
}

TEST_F(LiveRecordSessionTests, ConcurrentEditorsHaveExactlyOneWinner)
{
    QString firstError;
    QString secondError;
    bool first = false;
    bool second = false;
    std::thread a([&]() { first = LiveRecordSession::submit(live, render, firstError); });
    std::thread b([&]() { second = LiveRecordSession::submit(live, render, secondError); });
    a.join();
    b.join();
    EXPECT_NE(first, second);
    EXPECT_EQ(readFile(LiveRecordSession::finalAudioPath(live)), audio);
}

TEST_F(LiveRecordSessionTests, ActiveSubmissionLockIsRespected)
{
    QLockFile lock(directory.filePath("recording.montage.lock"));
    lock.setStaleLockTime(0);
    ASSERT_TRUE(lock.tryLock(0));
    QString error;
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
    lock.unlock();
    error.clear();
    EXPECT_TRUE(LiveRecordSession::submit(live, render, error));
}

TEST_F(LiveRecordSessionTests, DamagedSnapshotCannotBePublished)
{
    QString error;
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
    writeFile(LiveRecordSession::finalAudioPath(live), audio + "modified");
    setState("done");
    QString path;
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(LiveRecordSessionTests, FailureOrCancellationNeverPublishes)
{
    QString error;
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
    setState("error");
    QString path;
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_FALSE(error.isEmpty());
    error.clear();
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error));
}

TEST_F(LiveRecordSessionTests, MalformedStatusIsNotTreatedAsRecordingFinished)
{
    writeFile(LiveRecordSession::statusPath(live), "{\"version\":1,\"status\":");
    QString error;
    QString path;
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, path, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error));
}

TEST_F(LiveRecordSessionTests, MissingRenderDoesNotPublishReadiness)
{
    QString error;
    EXPECT_FALSE(LiveRecordSession::submit(live, directory.filePath("missing.wav"), error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
}

TEST_F(LiveRecordSessionTests, AcknowledgementIsExplicit)
{
    QString error;
    EXPECT_FALSE(LiveRecordSession::isAcknowledged(live, error));
    error.clear();
    ASSERT_TRUE(LiveRecordSession::acknowledge(live, error));
    EXPECT_TRUE(LiveRecordSession::isAcknowledged(live, error));
}

TEST_F(LiveRecordSessionTests, ZettaTargetFindsRecordingBeforeTargetAudioIsSaved)
{
    const QString target = directory.filePath("zetta/emission.-123");
    LiveRecordSession recorder(directory.path(), target);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error)) << error.toStdString();
    ASSERT_TRUE(recorder.publishTarget(live, error)) << error.toStdString();
    writeFile(live, audio);
    EXPECT_FALSE(QFile::exists(target));
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, live);
}

TEST_F(LiveRecordSessionTests, ZettaTargetRemainsAvailableForMontageAfterRecordingStops)
{
    const QString target = directory.filePath("zetta/emission.-123");
    LiveRecordSession recorder(directory.path(), target);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    ASSERT_TRUE(recorder.publishTarget(live, error));
    writeFile(live, audio);
    setState("done");

    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, live);
}

TEST_F(LiveRecordSessionTests, MultipleEmissionsWithSameFileNameStayIndependent)
{
    const QString targetA = directory.filePath("studio-a/emission.-123");
    const QString targetB = directory.filePath("studio-b/emission.-123");
    const QString liveB = directory.filePath("recording-b.wav");
    writeFile(live, audio);
    writeFile(liveB, audio + "second emission");
    writeFile(LiveRecordSession::statusPath(liveB),
              QJsonDocument(QJsonObject { { "version", 1 }, { "status", "recording" } }).toJson());
    LiveRecordSession recorderA(directory.path(), targetA);
    LiveRecordSession recorderB(directory.path(), targetB);
    QString error;
    ASSERT_TRUE(recorderA.claimTarget(error));
    ASSERT_TRUE(recorderB.claimTarget(error));
    ASSERT_TRUE(recorderA.publishTarget(live, error));
    ASSERT_TRUE(recorderB.publishTarget(liveB, error));
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), targetA, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, live);
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), targetB, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, liveB);
    setState("done");
    ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
    ASSERT_TRUE(LiveRecordSession::acknowledge(live, error));
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), targetA, found, error), LiveRecordSession::Lookup::Absent);
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), targetB, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, liveB);
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(liveB)));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::resultPath(liveB)));
}

TEST_F(LiveRecordSessionTests, SeveralEditorsFindTheSameEmissionWithoutTakingItsRecordingLock)
{
    const QString target = "\\\\ZETTA\\Audio\\Emission A.-123";
    LiveRecordSession recorder(directory.path(), target);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    ASSERT_TRUE(recorder.publishTarget(live, error));
    writeFile(live, audio);
    QString found;
    for (int editor = 0; editor < 3; ++editor) {
        EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Found);
        EXPECT_EQ(found, live);
    }
    LiveRecordSession duplicate(directory.path(), target);
    EXPECT_FALSE(duplicate.claimTarget(error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(LiveRecordSessionTests, WindowsPathCaseAndSeparatorsIdentifyTheSameEmission)
{
    LiveRecordSession recorder(directory.path(), "\\\\SERVER\\Zetta\\Audio\\Emission A.-123");
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    ASSERT_TRUE(recorder.publishTarget(live, error));
    writeFile(live, audio);
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), "//server/zetta/audio/emission a.-123", found, error),
              LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, live);
}

TEST_F(LiveRecordSessionTests, UnregisteredEmissionDoesNotOpenAnUnrelatedRecording)
{
    const QString targetA = directory.filePath("emission-a.-123");
    LiveRecordSession recorder(directory.path(), targetA);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    ASSERT_TRUE(recorder.publishTarget(live, error));
    writeFile(live, audio);
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), directory.filePath("emission-b.-123"), found, error),
              LiveRecordSession::Lookup::Absent);
    EXPECT_TRUE(found.isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

TEST_F(LiveRecordSessionTests, StartingRecorderIsPendingNotAnInvitationToRecordAgain)
{
    const QString target = directory.filePath("emission.-123");
    LiveRecordSession recorder(directory.path(), target);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Pending);
    ASSERT_TRUE(recorder.publishTarget(live, error));
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Pending);
    writeFile(live, audio);
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Found);
}

TEST_F(LiveRecordSessionTests, StoppedRecordingStillRoutesToMontageUntilPublication)
{
    const QString target = directory.filePath("emission.-123");
    LiveRecordSession recorder(directory.path(), target);
    QString error;
    ASSERT_TRUE(recorder.claimTarget(error));
    ASSERT_TRUE(recorder.publishTarget(live, error));
    writeFile(live, audio);
    setState("done");
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, live);
}

TEST_F(LiveRecordSessionTests, ClosedRecorderRemovesOnlyItsOwnTargetAssociation)
{
    const QString target = directory.filePath("emission.-123");
    QString error;
    {
        LiveRecordSession recorder(directory.path(), target);
        ASSERT_TRUE(recorder.claimTarget(error));
        ASSERT_TRUE(recorder.publishTarget(live, error));
    }
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Absent);
    EXPECT_TRUE(QFile::exists(LiveRecordSession::statusPath(live)));
    LiveRecordSession nextRecorder(directory.path(), target);
    EXPECT_TRUE(nextRecorder.claimTarget(error));
}

TEST_F(LiveRecordSessionTests, OrphanedAssociationFailsClosedInsteadOfStartingAnotherRecorder)
{
    const QString target = directory.filePath("emission.-123");
    QString error;
    QByteArray index;
    QString indexPath;
    {
        LiveRecordSession recorder(directory.path(), target);
        ASSERT_TRUE(recorder.claimTarget(error));
        ASSERT_TRUE(recorder.publishTarget(live, error));
        const QStringList indexes = QDir(directory.path()).entryList({ "zetta-target-*.json" }, QDir::Files);
        ASSERT_EQ(indexes.size(), 1);
        indexPath = directory.filePath(indexes.front());
        index = readFile(indexPath);
    }
    writeFile(indexPath, index);
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error), LiveRecordSession::Lookup::Error);
    EXPECT_FALSE(error.isEmpty());
    LiveRecordSession recorder(directory.path(), target);
    EXPECT_FALSE(recorder.claimTarget(error));
}

TEST_F(LiveRecordSessionTests, UnavailableSharedDirectoryIsAnError)
{
    QString error;
    QString found;
    EXPECT_EQ(LiveRecordSession::findTarget(directory.filePath("missing-share"), "emission.-123", found, error),
              LiveRecordSession::Lookup::Error);
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(LiveRecordSessionTests, SharedMontagePublishesImmutableNativeSnapshot)
{
    QString error;
    LiveRecordSession::MontageSnapshot initial;
    ASSERT_TRUE(LiveRecordSession::readMontage(live, initial, error));
    EXPECT_TRUE(initial.revision.isEmpty());
    const QJsonObject metadata { { "frames", "123" }, { "rate", 48000 } };
    LiveRecordSession::MontageSnapshot first;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, metadata, first, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_FALSE(first.revision.isEmpty());
    EXPECT_NE(LiveRecordSession::sharedMontagePath(live), LiveRecordSession::submissionPath(live));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
    EXPECT_EQ(readFile(first.projectPath), audio);
    LiveRecordSession::MontageSnapshot read;
    ASSERT_TRUE(LiveRecordSession::readMontage(live, read, error));
    EXPECT_EQ(read.revision, first.revision);
    EXPECT_EQ(read.metadata, metadata);

    writeFile(render, audio + "new edit");
    LiveRecordSession::MontageSnapshot second;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, first.revision, render, metadata, second, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_NE(first.revision, second.revision);
    EXPECT_EQ(readFile(first.projectPath), audio); // Readers of the old revision remain safe.
    EXPECT_EQ(readFile(second.projectPath), audio + "new edit");
}

TEST_F(LiveRecordSessionTests, StaleWorkstationCannotOverwriteSharedMontage)
{
    QString error;
    LiveRecordSession::MontageSnapshot first;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, first, error),
              LiveRecordSession::MontageUpdate::Published);
    const QString current = first.revision;
    LiveRecordSession::MontageSnapshot stale;
    EXPECT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, stale, error),
              LiveRecordSession::MontageUpdate::Conflict);
    EXPECT_EQ(stale.revision, current);
    EXPECT_FALSE(error.isEmpty());
    ASSERT_TRUE(LiveRecordSession::readMontage(live, first, error));
    EXPECT_EQ(first.revision, current);
}

TEST_F(LiveRecordSessionTests, SharedMontageRejectsCorruptSnapshot)
{
    QString error;
    LiveRecordSession::MontageSnapshot snapshot;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Published);
    writeFile(snapshot.projectPath, "corrupt project");
    EXPECT_FALSE(LiveRecordSession::readMontage(live, snapshot, error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(LiveRecordSessionTests, SharedMontageCannotSubmitAnOutdatedRevision)
{
    QString error;
    LiveRecordSession::MontageSnapshot first;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, first, error),
              LiveRecordSession::MontageUpdate::Published);
    LiveRecordSession::MontageSnapshot second;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, first.revision, render, {}, second, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, first.revision, render, error));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error)); // Old editors cannot bypass revision checks.
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
}

TEST_F(LiveRecordSessionTests, SharedMontageStillWaitsForRecordingBeforeZettaPublication)
{
    QString error;
    QString rendered;
    LiveRecordSession::MontageSnapshot snapshot;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, snapshot.revision, render, error));
    EXPECT_FALSE(LiveRecordSession::publicationReady(live, rendered, error));
    LiveRecordSession::MontageSnapshot sealed;
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    setState("done");
    ASSERT_TRUE(LiveRecordSession::sealMontage(live, sealed, error));
    ASSERT_TRUE(LiveRecordSession::submitMontage(live, sealed.revision, render, error));
    const QString frozenRevision = snapshot.revision;
    EXPECT_EQ(LiveRecordSession::publishMontage(live, frozenRevision, render, {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Frozen);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, frozenRevision, render, error));
    setState("done");
    ASSERT_TRUE(LiveRecordSession::publicationReady(live, rendered, error));
    EXPECT_EQ(readFile(rendered), audio);
    ASSERT_TRUE(LiveRecordSession::acknowledge(live, error));
    EXPECT_TRUE(LiveRecordSession::isAcknowledged(live, error));
}

TEST_F(LiveRecordSessionTests, ConcurrentWorkstationsPublishOnlyOneRevision)
{
    QString error;
    LiveRecordSession::MontageSnapshot first;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, first, error),
              LiveRecordSession::MontageUpdate::Published);
    LiveRecordSession::MontageUpdate results[2];
    auto publish = [&](int index) {
        LiveRecordSession::MontageSnapshot snapshot;
        QString threadError;
        results[index] = LiveRecordSession::publishMontage(live, first.revision, render, {}, snapshot, threadError);
    };
    std::thread a(publish, 0);
    std::thread b(publish, 1);
    a.join();
    b.join();
    const int published = int(results[0] == LiveRecordSession::MontageUpdate::Published)
                          + int(results[1] == LiveRecordSession::MontageUpdate::Published);
    EXPECT_EQ(published, 1);
    LiveRecordSession::MontageSnapshot stale;
    EXPECT_EQ(LiveRecordSession::publishMontage(live, first.revision, render, {}, stale, error),
              LiveRecordSession::MontageUpdate::Conflict);
}

TEST_F(LiveRecordSessionTests, SharedMontageFailureCannotChangeCurrentRevision)
{
    QString error;
    LiveRecordSession::MontageSnapshot snapshot;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Published);
    const QString revision = snapshot.revision;
    EXPECT_EQ(LiveRecordSession::publishMontage(live, revision, directory.filePath("missing.aup3"), {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Error);
    ASSERT_TRUE(LiveRecordSession::readMontage(live, snapshot, error));
    EXPECT_EQ(snapshot.revision, revision);
    setState("error");
    EXPECT_EQ(LiveRecordSession::publishMontage(live, revision, render, {}, snapshot, error),
              LiveRecordSession::MontageUpdate::Error);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, revision, render, error));
}

TEST_F(LiveRecordSessionTests, RecordingOwnershipBelongsToASessionAndCanChangeForTheNextRecording)
{
    const QString target = "//server/Zetta/emission.-123";
    QString error;
    QString found;
    writeFile(live, audio);
    // The first workstation owns this recording and publishes its result.
    {
        LiveRecordSession firstRecorder(directory.path(), target);
        ASSERT_TRUE(firstRecorder.claimTarget(error));
        ASSERT_TRUE(firstRecorder.publishTarget(live, error));
        ASSERT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error),
                  LiveRecordSession::Lookup::Found);
        EXPECT_EQ(found, live);
        setState("done");
        ASSERT_TRUE(LiveRecordSession::submit(live, render, error));
        ASSERT_TRUE(LiveRecordSession::acknowledge(live, error));
    }

    // A new recorder may take the same target after that session closes.
    // The former recorder now resolves the new recording as an editor.
    const QString nextLive = directory.filePath("next-recording.wav");
    writeFile(nextLive, audio);
    writeFile(LiveRecordSession::statusPath(nextLive),
              QJsonDocument(QJsonObject { { "version", 1 }, { "status", "recording" } }).toJson());
    LiveRecordSession nextRecorder(directory.path(), target);
    ASSERT_TRUE(nextRecorder.claimTarget(error));
    ASSERT_TRUE(nextRecorder.publishTarget(nextLive, error));
    ASSERT_EQ(LiveRecordSession::findTarget(directory.path(), target, found, error),
              LiveRecordSession::Lookup::Found);
    EXPECT_EQ(found, nextLive);
    LiveRecordSession formerRecorder(directory.path(), target);
    EXPECT_FALSE(formerRecorder.claimTarget(error)); // Joining must not start another REC.
    EXPECT_TRUE(LiveRecordSession::isAcknowledged(live, error));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(nextLive)));
}

TEST_F(LiveRecordSessionTests, CollectiveClosureWaitsForRecordingAndEveryEditor)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    const QString b = "22222222-2222-2222-2222-222222222222";
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "Studio A", error));
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, b, "Studio B", error));
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error)); // Even before the first native snapshot.
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, current.revision, render, error));
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_TRUE(error.isEmpty());
    setState("done");
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error)); // B can keep editing after REC.
    ASSERT_EQ(LiveRecordSession::publishMontage(live, current.revision, render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, b, current.revision, error));
    ASSERT_TRUE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_EQ(sealed.revision, current.revision);
    ASSERT_TRUE(LiveRecordSession::submitMontage(live, sealed.revision, render, error));
    QString rendered;
    EXPECT_TRUE(LiveRecordSession::publicationReady(live, rendered, error));
}

TEST_F(LiveRecordSessionTests, AllEditorsMayLeaveDuringRecordingAndRejoinBeforeClosure)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_TRUE(error.isEmpty());
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    setState("done");
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    EXPECT_TRUE(LiveRecordSession::sealMontage(live, sealed, error));
}

TEST_F(LiveRecordSessionTests, StaleDepartureKeepsEditorRegisteredUntilItsWorkIsSynchronized)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    const QString b = "22222222-2222-2222-2222-222222222222";
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, b, "B", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    const QString oldRevision = current.revision;
    ASSERT_EQ(LiveRecordSession::publishMontage(live, oldRevision, render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    EXPECT_FALSE(LiveRecordSession::leaveMontage(live, a, oldRevision, error));
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, b, current.revision, error));
    setState("done");
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    EXPECT_TRUE(LiveRecordSession::sealMontage(live, sealed, error));
}

TEST_F(LiveRecordSessionTests, SealedSessionRejectsNewEditorsAndChangesButAllowsRenderRetry)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    setState("done");
    ASSERT_TRUE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_FALSE(LiveRecordSession::joinMontage(live, a, "A", error));
    EXPECT_EQ(LiveRecordSession::publishMontage(live, current.revision, render, {}, current, error),
              LiveRecordSession::MontageUpdate::Frozen);
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, sealed.revision, directory.filePath("missing.wav"), error));
    LiveRecordSession::MontageSnapshot retry;
    ASSERT_TRUE(LiveRecordSession::sealMontage(live, retry, error));
    EXPECT_EQ(retry.revision, sealed.revision);
    EXPECT_TRUE(LiveRecordSession::submitMontage(live, retry.revision, render, error));
}

TEST_F(LiveRecordSessionTests, MissingPresenceDoesNotReleaseUnfinishedEditor)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "Offline studio", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    setState("done");
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_TRUE(error.isEmpty()); // No heartbeat exists, but this editor has not left.
    EXPECT_FALSE(LiveRecordSession::submit(live, render, error));
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, current.revision, render, error));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
}

TEST_F(LiveRecordSessionTests, JoiningAndFinalizingConcurrentlyCannotFreezeAnActiveEditor)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    const QString b = "22222222-2222-2222-2222-222222222222";
    LiveRecordSession::MontageSnapshot current;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    ASSERT_TRUE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    setState("done");
    bool joined = false, sealed = false;
    std::thread joining([&] { QString e; joined = LiveRecordSession::joinMontage(live, b, "B", e); });
    std::thread sealing([&] { QString e; LiveRecordSession::MontageSnapshot result;
                             sealed = LiveRecordSession::sealMontage(live, result, e); });
    joining.join();
    sealing.join();
    EXPECT_FALSE(joined && sealed);
    if (joined) {
        LiveRecordSession::MontageSnapshot result;
        EXPECT_FALSE(LiveRecordSession::sealMontage(live, result, error));
        EXPECT_TRUE(LiveRecordSession::leaveMontage(live, b, current.revision, error));
    }
}

TEST_F(LiveRecordSessionTests, CorruptParticipationRegistryCannotCloseOrPublishTheMontage)
{
    QString error;
    const QString a = "11111111-1111-1111-1111-111111111111";
    LiveRecordSession::MontageSnapshot current, sealed;
    ASSERT_TRUE(LiveRecordSession::joinMontage(live, a, "A", error));
    ASSERT_EQ(LiveRecordSession::publishMontage(live, "", render, {}, current, error),
              LiveRecordSession::MontageUpdate::Published);
    writeFile(directory.filePath("recording.editors.json"),
              "{\"version\":1,\"editors\":null,\"sealed\":false}");
    setState("done");
    EXPECT_FALSE(LiveRecordSession::sealMontage(live, sealed, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(LiveRecordSession::leaveMontage(live, a, current.revision, error));
    EXPECT_FALSE(LiveRecordSession::submitMontage(live, current.revision, render, error));
    EXPECT_FALSE(QFile::exists(LiveRecordSession::submissionPath(live)));
}
