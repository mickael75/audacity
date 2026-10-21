/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QSysInfo>

#include "project/internal/livesessionpresence.h"

using namespace au::project;

class LiveSessionPresenceTests : public ::testing::Test
{
protected:
    QTemporaryDir directory;
    QString wav() const { return directory.filePath("emission.wav"); }
};

TEST_F(LiveSessionPresenceTests, RolesBelongToWindowsAndCanChange)
{
    ASSERT_TRUE(directory.isValid());
    QString error;
    LiveSessionPresence recorder("Studio A");
    LiveSessionPresence editor("Studio B");
    LiveSessionPresence reader;
    recorder.setSession(wav());
    editor.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(recorder.publish("recording", true, error));
    ASSERT_TRUE(editor.publish("editing", true, error));
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(0, participants, error));
    ASSERT_EQ(participants.size(), 2);
    ASSERT_TRUE(recorder.publish("editing", true, error));
    ASSERT_TRUE(editor.publish("recording", true, error));
    ASSERT_TRUE(reader.read(2000, participants, error));
    for (const auto& participant : participants) {
        EXPECT_EQ(participant.role, participant.station == "Studio A" ? "editing" : "recording");
        EXPECT_TRUE(participant.sharedMontage);
    }
}

TEST_F(LiveSessionPresenceTests, StalePresenceExpiresAndFreshPulseRestoresIt)
{
    QString error;
    LiveSessionPresence writer("Studio A");
    LiveSessionPresence reader;
    writer.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(writer.publish("editing", true, error));
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(0, participants, error));
    ASSERT_EQ(participants.size(), 1);
    ASSERT_TRUE(reader.read(LiveSessionPresence::TIMEOUT_MS - 1, participants, error));
    EXPECT_EQ(participants.size(), 1);
    ASSERT_TRUE(reader.read(LiveSessionPresence::TIMEOUT_MS, participants, error));
    EXPECT_TRUE(participants.isEmpty());
    ASSERT_TRUE(reader.read(60000, participants, error));
    EXPECT_TRUE(participants.isEmpty()); // An unchanged orphan does not reappear on each poll.
    ASSERT_TRUE(writer.publish("editing", true, error));
    ASSERT_TRUE(reader.read(60001, participants, error));
    EXPECT_EQ(participants.size(), 1);
}

TEST_F(LiveSessionPresenceTests, ClosingOneWindowDoesNotRemoveOtherWindowsOnTheSameStation)
{
    QString error;
    LiveSessionPresence editor("Studio A");
    LiveSessionPresence reader;
    editor.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(editor.publish("editing", true, error));
    {
        LiveSessionPresence recorder("Studio A");
        EXPECT_NE(recorder.id(), editor.id());
        recorder.setSession(wav());
        ASSERT_TRUE(recorder.publish("recording", true, error));
        QVector<LiveSessionPresence::Participant> participants;
        ASSERT_TRUE(reader.read(0, participants, error));
        EXPECT_EQ(participants.size(), 2);
    }
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(2000, participants, error));
    ASSERT_EQ(participants.size(), 1);
    EXPECT_EQ(participants.front().id, editor.id());
}

TEST_F(LiveSessionPresenceTests, SwitchingSessionRemovesOldPresenceAndKeepsEmissionsSeparate)
{
    QString error;
    LiveSessionPresence writer("Studio A");
    LiveSessionPresence reader;
    writer.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(writer.publish("editing", false, error));
    const QString secondWav = directory.filePath("emission-2.wav");
    writer.setSession(secondWav);
    ASSERT_TRUE(writer.publish("ready", true, error));
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(0, participants, error));
    EXPECT_TRUE(participants.isEmpty());
    reader.setSession(secondWav);
    ASSERT_TRUE(reader.read(0, participants, error));
    ASSERT_EQ(participants.size(), 1);
    EXPECT_EQ(participants.front().role, "ready");
}

TEST_F(LiveSessionPresenceTests, MalformedPresenceDoesNotCreateAnActiveWorkstation)
{
    QString error;
    LiveSessionPresence writer("Studio A");
    LiveSessionPresence reader;
    writer.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(writer.publish("editing", true, error));
    QFile corrupt(directory.filePath("emission.presence/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json"));
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("{ broken json }");
    corrupt.close();
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(0, participants, error));
    ASSERT_EQ(participants.size(), 1);
    EXPECT_EQ(participants.front().station, "Studio A");
}

TEST_F(LiveSessionPresenceTests, MissingShareAndUnknownRoleAreReported)
{
    QString error;
    LiveSessionPresence writer("Studio A");
    writer.setSession(wav());
    EXPECT_FALSE(writer.publish("unknown-role", true, error));
    EXPECT_FALSE(error.isEmpty());
    QVector<LiveSessionPresence::Participant> participants;
    EXPECT_FALSE(writer.read(0, participants, error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(LiveSessionPresenceTests, DisplayAliasesDoNotChangeTheMachineIdentity)
{
    if (QSysInfo::machineHostName().isEmpty()) {
        GTEST_SKIP() << "This platform does not provide a machine host name.";
    }
    QString error;
    LiveSessionPresence first("Studio A");
    LiveSessionPresence second("Montage 2");
    LiveSessionPresence reader;
    first.setSession(wav());
    second.setSession(wav());
    reader.setSession(wav());
    ASSERT_TRUE(first.publish("editing", true, error));
    ASSERT_TRUE(second.publish("editing", true, error));
    QVector<LiveSessionPresence::Participant> participants;
    ASSERT_TRUE(reader.read(0, participants, error));
    ASSERT_EQ(participants.size(), 2);
    EXPECT_NE(participants[0].station, participants[1].station);
    EXPECT_EQ(participants[0].machine, participants[1].machine);
    EXPECT_NE(participants[0].id, participants[1].id);
}
