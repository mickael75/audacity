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
#include <QUuid>

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


namespace {
QString participationPath(const QString& wavPath)
{
    return basePath(wavPath) + ".editors.json";
}

bool readParticipation(const QString& wavPath, QJsonObject& object, QString& error)
{
    if (!QFile::exists(participationPath(wavPath))) {
        object = { { "version", 1 }, { "editors", QJsonObject {} }, { "sealed", false } };
        return true;
    }
    if (!readJson(participationPath(wavPath), object, error)) {
        return false;
    }
    if (!object.value("editors").isObject() || !object.value("sealed").isBool()) {
        error = "Invalid montage participation registry.";
        return false;
    }
    return true;
}
}

bool LiveRecordSession::joinMontage(const QString& wavPath, const QString& editorId,
                                   const QString& station, QString& error)
{
    error.clear();
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "The shared montage is busy. Try opening it again.";
        return false;
    }
    State state;
    QJsonObject registry;
    if (QUuid(editorId).isNull() || !readState(wavPath, state, error) || state == State::Failed
        || !readParticipation(wavPath, registry, error)) {
        if (error.isEmpty()) { error = "Cannot join this live montage."; }
        return false;
    }
    if (registry.value("sealed").toBool() || QFile::exists(submissionPath(wavPath))) {
        error = "All editors have left and the recording is finished. The montage is being published to Zetta.";
        return false;
    }
    auto editors = registry.value("editors").toObject();
    editors.insert(editorId, station);
    registry.insert("editors", editors);
    return writeJson(participationPath(wavPath), registry, error);
}

bool LiveRecordSession::leaveMontage(const QString& wavPath, const QString& editorId,
                                    const QString& expectedRevision, QString& error)
{
    error.clear();
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "Another workstation is updating the montage. Try leaving again.";
        return false;
    }
    QJsonObject registry;
    MontageSnapshot snapshot;
    if (!readParticipation(wavPath, registry, error) || !readMontage(wavPath, snapshot, error)) {
        return false;
    }
    if (registry.value("sealed").toBool() || snapshot.revision.isEmpty() || snapshot.revision != expectedRevision) {
        error = "The shared montage changed. Synchronize it before leaving the session.";
        return false;
    }
    auto editors = registry.value("editors").toObject();
    if (!editors.contains(editorId)) {
        error = "This window is not registered in the montage session.";
        return false;
    }
    editors.remove(editorId);
    registry.insert("editors", editors);
    return writeJson(participationPath(wavPath), registry, error);
}

bool LiveRecordSession::sealMontage(const QString& wavPath, MontageSnapshot& snapshot, QString& error)
{
    error.clear();
    snapshot = {};
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { return false; }
    State state;
    QJsonObject registry;
    if (!readState(wavPath, state, error) || !readParticipation(wavPath, registry, error)) {
        return false;
    }
    if (state == State::Failed) { error = "The recording failed."; return false; }
    if (state != State::Done || !registry.value("editors").toObject().isEmpty()) { return false; }
    // No native montage means no editor has successfully published yet.
    if (!readMontage(wavPath, snapshot, error) || snapshot.revision.isEmpty()) { return false; }
    if (registry.value("sealed").toBool()) {
        if (registry.value("revision").toString() != snapshot.revision) {
            error = "The sealed montage revision changed.";
            return false;
        }
        return true; // Retrying a failed render/export uses the same immutable revision.
    }
    registry.insert("sealed", true);
    registry.insert("revision", snapshot.revision);
    return writeJson(participationPath(wavPath), registry, error);
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

QString LiveRecordSession::sharedMontagePath(const QString& wavPath)
{
    return basePath(wavPath) + ".shared-montage.json";
}

bool LiveRecordSession::readMontage(const QString& wavPath, MontageSnapshot& snapshot, QString& error,
                                  const QString& verifiedRevision)
{
    error.clear();
    snapshot = {};
    const QString manifest = sharedMontagePath(wavPath);
    if (!QFile::exists(manifest)) {
        return true;
    }
    QJsonObject object;
    if (!readJson(manifest, object, error)) {
        return false;
    }
    const QString revision = object.value("revision").toString();
    const QUuid uuid(revision);
    if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != revision
        || !object.value("metadata").isObject()) {
        error = "Invalid shared montage manifest: " + manifest;
        return false;
    }
    const QString projectPath = basePath(wavPath) + ".montage-" + revision + ".aup3";
    QString hash;
    if (revision != verifiedRevision
        && (!audioHash(projectPath, hash, error) || hash != object.value("sha256").toString())) {
        error = "The shared montage snapshot is missing or damaged: " + projectPath;
        return false;
    }
    snapshot = { revision, projectPath, object.value("metadata").toObject() };
    return true;
}

LiveRecordSession::MontageUpdate LiveRecordSession::publishMontage(
    const QString& wavPath, const QString& expectedRevision, const QString& localProject,
    const QJsonObject& metadata, MontageSnapshot& snapshot, QString& error)
{
    error.clear();
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "Another workstation is updating the shared montage. Try again.";
        return MontageUpdate::Error;
    }
    if (QFile::exists(submissionPath(wavPath))) {
        error = "The final montage is already frozen.";
        return MontageUpdate::Frozen;
    }
    QJsonObject registry;
    if (!readParticipation(wavPath, registry, error)) { return MontageUpdate::Error; }
    if (registry.value("sealed").toBool()) {
        error = "The shared montage session has finished.";
        return MontageUpdate::Frozen;
    }
    State state;
    if (!readState(wavPath, state, error) || state == State::Failed) {
        if (error.isEmpty()) {
            error = "The recording failed; the shared montage was not updated.";
        }
        return MontageUpdate::Error;
    }
    if (!readMontage(wavPath, snapshot, error)) {
        return MontageUpdate::Error;
    }
    if (snapshot.revision != expectedRevision) {
        error = "Another workstation has changed the montage.";
        return MontageUpdate::Conflict;
    }
    const QString revision = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString targetPath = basePath(wavPath) + ".montage-" + revision + ".aup3";
    QFile source(localProject);
    QSaveFile target(targetPath);
    if (!source.open(QIODevice::ReadOnly) || source.size() == 0 || !target.open(QIODevice::WriteOnly)) {
        error = "Cannot stage shared montage: " + source.errorString() + "; " + target.errorString();
        return MontageUpdate::Error;
    }
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || target.write(bytes) != bytes.size()) {
            error = "Cannot copy shared montage: " + source.errorString() + "; " + target.errorString();
            return MontageUpdate::Error;
        }
    }
    if (!target.commit()) {
        error = target.errorString();
        return MontageUpdate::Error;
    }
    QString hash;
    if (!audioHash(targetPath, hash, error)
        || !writeJson(sharedMontagePath(wavPath),
                      { { "version", 1 }, { "revision", revision }, { "sha256", hash }, { "metadata", metadata } }, error)) {
        QFile::remove(targetPath);
        return MontageUpdate::Error;
    }
    snapshot = { revision, targetPath, metadata };
    return MontageUpdate::Published;
}

bool LiveRecordSession::submitMontage(const QString& wavPath, const QString& expectedRevision,
                                    const QString& renderedPath, QString& error)
{
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "Another workstation is updating the montage. Try again.";
        return false;
    }
    MontageSnapshot snapshot;
    if (!readMontage(wavPath, snapshot, error)) {
        return false;
    }
    if (snapshot.revision.isEmpty() || snapshot.revision != expectedRevision) {
        error = "The shared montage changed. Review the latest version before finishing.";
        return false;
    }
    QJsonObject registry;
    State state;
    if (!readParticipation(wavPath, registry, error) || !readState(wavPath, state, error)) { return false; }
    if (state != State::Done || !registry.value("sealed").toBool()
        || !registry.value("editors").toObject().isEmpty()
        || registry.value("revision").toString() != expectedRevision) {
        error = "Stop recording and wait for every editor to leave before publishing the montage.";
        return false;
    }
    return submitUnlocked(wavPath, renderedPath, error);
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
    QLockFile lock(basePath(wavPath) + ".shared-montage.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        error = "Another workstation is updating the montage. Try again.";
        return false;
    }
    if (QFile::exists(sharedMontagePath(wavPath)) || QFile::exists(participationPath(wavPath))) {
        error = "This recording uses a shared montage. Reopen with shared montage enabled before finishing.";
        return false;
    }
    return submitUnlocked(wavPath, renderedPath, error);
}

bool LiveRecordSession::submitUnlocked(const QString& wavPath, const QString& renderedPath, QString& error)
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
