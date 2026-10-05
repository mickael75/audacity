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
