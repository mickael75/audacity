/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "livesessionpresence.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QUuid>

using namespace au::project;

namespace {
bool validRole(const QString& role)
{
    return role == "ready" || role == "recording" || role == "recorded"
           || role == "editing" || role == "paused" || role == "submitted" || role == "frozen" || role == "failed";
}
QString cleanStationName(const QString& name)
{
    QString result;
    for (const QChar c : name.left(128)) {
        if (c.isPrint()) {
            result += c;
        }
    }
    result.replace(u'<', QChar(0x2039));
    result.replace(u'>', QChar(0x203A));
    return result.trimmed();
}
}

LiveSessionPresence::LiveSessionPresence(const QString& station)
    : m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_station(station.isEmpty() ? stationName() : cleanStationName(station)),
      m_machine(cleanStationName(QSysInfo::machineHostName()).toCaseFolded())
{
}

LiveSessionPresence::~LiveSessionPresence()
{
    clear();
}

QString LiveSessionPresence::stationName()
{
    QString name = cleanStationName(qEnvironmentVariable("AU_STATION_NAME"));
    if (name.isEmpty()) {
        name = cleanStationName(QSysInfo::machineHostName());
    }
    return name.isEmpty() ? QStringLiteral("Audacity") : name;
}

void LiveSessionPresence::setSession(const QString& wavPath)
{
    const QFileInfo info(wavPath);
    const QString directory = wavPath.isEmpty() ? QString()
                              : info.dir().filePath(info.completeBaseName() + ".presence");
    if (m_directory != directory) {
        clear();
        m_directory = directory;
    }
}

void LiveSessionPresence::clear()
{
    if (m_published) {
        QFile::remove(QDir(m_directory).filePath(m_id + ".json"));
    }
    m_published = false;
    m_directory.clear();
    m_observed.clear();
}

bool LiveSessionPresence::publish(const QString& role, bool sharedMontage, QString& error)
{
    error.clear();
    if (m_directory.isEmpty() || !validRole(role) || !QDir().mkpath(m_directory)) {
        error = "The live presence directory or role is unavailable.";
        return false;
    }
    const QJsonObject object { { "version", 1 }, { "id", m_id }, { "station", m_station }, { "machine", m_machine },
                               { "role", role }, { "shared", sharedMontage },
                               { "pulse", QUuid::createUuid().toString(QUuid::WithoutBraces) } };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(m_directory).filePath(m_id + ".json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    m_published = true;
    return true;
}

bool LiveSessionPresence::read(qint64 elapsedMs, QVector<Participant>& participants, QString& error)
{
    participants.clear();
    error.clear();
    QDir directory(m_directory);
    if (m_directory.isEmpty() || !directory.exists() || !QFileInfo(m_directory).isReadable()) {
        error = "The live presence directory is unavailable.";
        return false;
    }
    QSet<QString> seen;
    const auto files = directory.entryList({ "*.json" }, QDir::Files, QDir::Name);
    for (const auto& name : files) {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
            continue;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        const auto object = document.object();
        const QString id = object.value("id").toString();
        const QString pulse = object.value("pulse").toString();
        const QString role = object.value("role").toString();
        const QString station = cleanStationName(object.value("station").toString());
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || object.value("version").toInt() != 1 || id + ".json" != name
            || QUuid(id).isNull() || QUuid(pulse).isNull() || !validRole(role) || station.isEmpty()) {
            continue;
        }
        seen.insert(id);
        auto found = m_observed.find(id);
        if (found == m_observed.end() || found->pulse != pulse) {
            m_observed.insert(id, { pulse, elapsedMs });
            found = m_observed.find(id);
        }
        if (elapsedMs - found->lastChange < TIMEOUT_MS) {
            const QString machine = cleanStationName(object.value("machine").toString()).toCaseFolded();
            participants.push_back({ id, station, role, object.value("shared").toBool(), machine.isEmpty() ? station : machine });
        }
    }
    for (auto it = m_observed.begin(); it != m_observed.end();) {
        if (seen.contains(it.key())) {
            ++it;
        } else {
            it = m_observed.erase(it);
        }
    }
    return true;
}
