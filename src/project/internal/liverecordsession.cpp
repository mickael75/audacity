/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity-CLA-applies
 */
#include "liverecordsession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

#include "framework/global/log.h"

using namespace au::project;

namespace {
QString basePath(const QString& wavPath)
{
    const QFileInfo info(wavPath);
    return info.dir().filePath(info.completeBaseName());
}

bool readJson(const QString& path, QJsonObject& object, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = path + ": " + file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value("version").toInt() != 1) {
        error = "Invalid live session file: " + path;
        return false;
    }
    object = document.object();
    return true;
}

bool writeJson(const QString& path, const QJsonObject& object, QString& error)
{
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = path + ": " + file.errorString();
        return false;
    }
    return true;
}

bool audioHash(const QString& path, QString& hash, QString& error)
{
    QFile file(path);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0 || !digest.addData(&file)) {
        error = "Cannot read final montage: " + path + ": " + file.errorString();
        return false;
    }
    hash = QString::fromLatin1(digest.result().toHex());
    return true;
}
}

QString LiveRecordSession::targetKey(const QString& target)
{
    QString path = target;
    path.replace(u'\\', u'/');
    const bool windowsPath = path.startsWith("//") || (path.size() > 1 && path.at(1) == u':');
    path = QDir::cleanPath(windowsPath ? path : QFileInfo(path).absoluteFilePath());
    path = path.normalized(QString::NormalizationForm_C);
    return windowsPath ? path.toCaseFolded() : path;
}

QString LiveRecordSession::targetIndex(const QString& directory, const QString& target)
{
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(targetKey(target).toUtf8(),
                                                                     QCryptographicHash::Sha256).toHex());
    return QDir(directory).filePath("zetta-target-" + hash + ".json");
}

LiveRecordSession::LiveRecordSession(const QString& directory, const QString& target)
    : m_directory(directory), m_target(target), m_index(targetIndex(directory, target)), m_targetLock(m_index + ".lock")
{
    m_targetLock.setStaleLockTime(0);
}

LiveRecordSession::~LiveRecordSession()
{
    if (m_indexPublished && !QFile::remove(m_index)) {
        LOGW() << "could not remove live Zetta target registration: " << m_index;
    }
    // The lock outlives removal, so no new recording can inherit this mapping.
}

bool LiveRecordSession::claimTarget(QString& error)
{
    error.clear();
    if (m_claimed) {
        return true;
    }
    if (!QDir().mkpath(m_directory)) {
        error = "Cannot create the live session directory: " + m_directory;
        return false;
    }
    if (!m_targetLock.tryLock(0)) {
        error = "This Zetta target is already owned by another recording, or the shared lock is unavailable: " + m_target;
        return false;
    }
    // An orphaned index is never silently reused after a recorder crash.
    if (QFile::exists(m_index)) {
        m_targetLock.unlock();
        error = "An unfinished live session exists for this Zetta target. Recover it before starting another recording: " + m_index;
        return false;
    }
    m_claimed = true;
    return true;
}

bool LiveRecordSession::publishTarget(const QString& wavPath, QString& error)
{
    error.clear();
    if (!m_claimed || QFileInfo(wavPath).absolutePath() != QFileInfo(m_index).absolutePath()) {
        error = "The recording does not own this shared Zetta target.";
        return false;
    }
    if (!writeJson(m_index, QJsonObject { { "version", 1 }, { "target", targetKey(m_target) },
                                        { "audio", QFileInfo(wavPath).fileName() } }, error)) {
        return false;
    }
    m_indexPublished = true;
    return true;
}

LiveRecordSession::Lookup LiveRecordSession::findTarget(const QString& directory, const QString& target,
                                                       QString& wavPath, QString& error)
{
    error.clear();
    wavPath.clear();
    if (!QDir(directory).exists()) {
        error = "The live session directory is unavailable: " + directory;
        return Lookup::Error;
    }
    const QString index = targetIndex(directory, target);
    QLockFile lock(index + ".lock");
    lock.setStaleLockTime(0);
    const bool owned = !lock.tryLock(0);
    if (owned && lock.error() != QLockFile::LockFailedError) {
        error = "Cannot inspect the shared Zetta recording lock: " + index;
        return Lookup::Error;
    }
    if (!QFile::exists(index)) {
        return owned ? Lookup::Pending : Lookup::Absent;
    }
    if (!owned) {
        error = "The recording owner is no longer available. Recover this unfinished live session: " + index;
        return Lookup::Error;
    }
    QJsonObject object;
    if (!readJson(index, object, error)) {
        return Lookup::Error;
    }
    const QString audio = object.value("audio").toString();
    if (object.value("target").toString() != targetKey(target) || audio.isEmpty()
        || QFileInfo(audio).fileName() != audio || audio.contains(u'\\') || !audio.endsWith(".wav")) {
        error = "Invalid Zetta target registration: " + index;
        return Lookup::Error;
    }
    wavPath = QDir(directory).filePath(audio);
    State state;
    if (!readState(wavPath, state, error)) {
        return Lookup::Error;
    }
    if (state == State::Failed) {
        error = "This Zetta recording failed or was cancelled: " + target;
        return Lookup::Error;
    }
    if (QFile::exists(resultPath(wavPath))) {
        if (!isAcknowledged(wavPath, error)) {
            return Lookup::Error;
        }
        wavPath.clear();
        return Lookup::Absent;
    }
    return QFileInfo(wavPath).size() > 44 ? Lookup::Found : Lookup::Pending;
}

QString LiveRecordSession::statusPath(const QString& wavPath)
{
    return basePath(wavPath) + ".json";
}

QString LiveRecordSession::submissionPath(const QString& wavPath)
{
    return basePath(wavPath) + ".montage.json";
}

QString LiveRecordSession::resultPath(const QString& wavPath)
{
    return basePath(wavPath) + ".result.json";
}

QString LiveRecordSession::finalAudioPath(const QString& wavPath)
{
    return basePath(wavPath) + ".final.wav";
}

bool LiveRecordSession::readState(const QString& wavPath, State& state, QString& error)
{
    error.clear();
    QJsonObject object;
    if (!readJson(statusPath(wavPath), object, error)) {
        return false;
    }
    const QString status = object.value("status").toString();
    if (status == "recording") {
        state = State::Recording;
    } else if (status == "done") {
        state = State::Done;
    } else if (status == "error") {
        state = State::Failed;
    } else {
        error = "Invalid recording state: " + status;
        return false;
    }
    return true;
}

bool LiveRecordSession::submit(const QString& wavPath, const QString& renderedPath, QString& error)
{
    error.clear();
    QLockFile lock(basePath(wavPath) + ".montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "Another editor is submitting the final montage.";
        return false;
    }
    if (QFile::exists(submissionPath(wavPath))) {
        error = "A final montage has already been submitted for this recording.";
        return false;
    }
    State state;
    if (!readState(wavPath, state, error) || state == State::Failed) {
        if (error.isEmpty()) {
            error = "The live recording failed. The montage was not submitted.";
        }
        return false;
    }

    QFile source(renderedPath);
    QSaveFile target(finalAudioPath(wavPath));
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
        error = "Cannot stage final montage: " + source.errorString() + "; " + target.errorString();
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(65536);
        if (bytes.isEmpty() || target.write(bytes) != bytes.size()) {
            error = "Cannot copy final montage: " + source.errorString() + "; " + target.errorString();
            return false;
        }
    }
    if (!target.commit()) {
        error = target.errorString();
        return false;
    }
    QString hash;
    if (!audioHash(finalAudioPath(wavPath), hash, error)) {
        return false;
    }
    return writeJson(submissionPath(wavPath), QJsonObject { { "version", 1 }, { "sha256", hash } }, error);
}

bool LiveRecordSession::readSubmission(const QString& wavPath, QString& renderedPath, QString& error)
{
    error.clear();
    QJsonObject object;
    if (!readJson(submissionPath(wavPath), object, error)) {
        return false;
    }
    QString hash;
    renderedPath = finalAudioPath(wavPath);
    if (!audioHash(renderedPath, hash, error)) {
        return false;
    }
    if (object.value("sha256").toString() != hash) {
        error = "The submitted montage has changed or is incomplete: " + renderedPath;
        return false;
    }
    return true;
}

bool LiveRecordSession::acknowledge(const QString& wavPath, QString& error)
{
    error.clear();
    return writeJson(resultPath(wavPath), QJsonObject { { "version", 1 }, { "status", "published" } }, error);
}

bool LiveRecordSession::publicationReady(const QString& wavPath, QString& renderedPath, QString& error)
{
    error.clear();
    State state;
    if (!readState(wavPath, state, error)) {
        return false;
    }
    if (state == State::Failed) {
        error = "The recording failed or the live session was cancelled.";
        return false;
    }
    if (state != State::Done || !QFile::exists(submissionPath(wavPath))) {
        return false;
    }
    return readSubmission(wavPath, renderedPath, error);
}

bool LiveRecordSession::isAcknowledged(const QString& wavPath, QString& error)
{
    error.clear();
    QJsonObject object;
    if (!readJson(resultPath(wavPath), object, error)) {
        return false;
    }
    if (object.value("status").toString() != "published") {
        error = "Invalid publication acknowledgement: " + resultPath(wavPath);
        return false;
    }
    return true;
}
