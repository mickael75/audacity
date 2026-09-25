#include "projectactionscontroller.h"

#include <QFile>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWindow>

#include <sndfile.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <variant>

#include "framework/global/async/async.h"
#include "framework/global/defer.h"
#include "framework/global/translation.h"
#include "framework/global/io/path.h"
#include "framework/global/progress.h"
#include "framework/global/log.h"
#include "framework/global/types/ret.h"
#include "framework/ui/view/iconcodes.h"
#include "framework/interactive/iinteractive.h"

#include "au3cloud/au3clouderrors.h"
#include "importexport/export/types/exporttypes.h"
#include "trackedit/dom/track.h"
#include "au3wrap/internal/wxtypes_convert.h"
#include "au3-project/Project.h"
#include "au3-project-file-io/ProjectFileIO.h"

#include "audacityproject.h"
#include "projecterrors.h"
#include "project/types/projecttypes.h"

using namespace muse;
using namespace au::project;

static const muse::Uri PROJECT_PAGE_URI("audacity://project");
static const muse::Uri HOME_PAGE_URI("audacity://home");
static const muse::Uri NEW_PROJECT_URI("audacity://project/new");

static const muse::Uri SAVE_TO_CLOUD_URI("audacity://project/savetocloud");
static const muse::Uri EXPORT_URI("audacity://project/export");
static const muse::Uri ASK_LOCATION_TYPE_URI("audacity://project/asklocationtype");
static const muse::Uri CUSTOM_FFMPEG_OPTIONS("audacity://project/export/ffmpeg");
static const muse::Uri METADATA_DIALOG_URI("audacity://project/export/metadata");
static const muse::Uri EXPORT_LABELS_URI("audacity://project/export/labels");
static const muse::Uri CUSTOM_MAPPING("audacity://project/export/mapping");

static constexpr int BTN_QUICK_EDIT_SAVE_SELECTION = int(muse::IInteractive::Button::CustomButton) + 1;
static constexpr int BTN_QUICK_EDIT_SAVE_WHOLE_FILE = int(muse::IInteractive::Button::CustomButton) + 2;

static const QString AUDACITY_URL_SCHEME("audacity");
static const QString OPEN_PROJECT_URL_HOSTNAME("open-project");

static muse::Val exportParameter(int id, const muse::Val& value)
{
    muse::ValMap entry;
    entry["id"] = muse::Val(id);
    entry["value"] = value;
    return muse::Val(entry);
}

struct MpegAudioInfo {
    int layer = 2; // 2: MP2, 3: MP3
    bool mpeg1 = true;
    int bitrateKbps = 0;
    bool variableBitrate = false; // a "Xing" header in the first frame
};

//! NOTE: some broadcast systems (e.g. RCS Zetta) store MPEG audio with another extension (.mpg) or none at all:
//! detect MPEG-1/2 Layer II / III audio from its frame headers (two consecutive valid frames)
static std::optional<MpegAudioInfo> mpegAudioInfo(const muse::io::path_t& path)
{
    QFile file(path.toQString());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    //! NOTE: skip an ID3v2 tag
    const QByteArray id3 = file.read(10);
    qint64 offset = 0;
    if (id3.size() == 10 && id3.startsWith("ID3")) {
        const auto b = [&id3](int i) { return static_cast<qint64>(static_cast<unsigned char>(id3.at(i)) & 0x7F); };
        offset = 10 + ((b(6) << 21) | (b(7) << 14) | (b(8) << 7) | b(9));
        if (static_cast<unsigned char>(id3.at(5)) & 0x10) {
            offset += 10;
        }
    }

    if (!file.seek(offset)) {
        return std::nullopt;
    }

    const QByteArray data = file.read(64 * 1024);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.constData());
    const int size = static_cast<int>(data.size());

    //! NOTE: a real MPEG video (program / video stream) must not be overwritten with audio only
    if (size >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x01 && (bytes[3] == 0xBA || bytes[3] == 0xB3)) {
        return std::nullopt;
    }

    static const int MPEG1_L2_BITRATES[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
    static const int MPEG1_L3_BITRATES[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int MPEG2_BITRATES[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int MPEG1_RATES[4] = { 44100, 48000, 32000, 0 };
    static const int MPEG2_RATES[4] = { 22050, 24000, 16000, 0 };

    //! returns the frame length, or 0 when there is no Layer II / III frame header at pos
    auto frameAt = [bytes, size](int pos, MpegAudioInfo& info) -> int {
        if (pos < 0 || pos + 4 > size || bytes[pos] != 0xFF || (bytes[pos + 1] & 0xE0) != 0xE0) {
            return 0;
        }

        const int version = (bytes[pos + 1] >> 3) & 0x3; // 3: MPEG-1, 2: MPEG-2 (MPEG-2.5 isn't supported by the encoder)
        const int layerBits = (bytes[pos + 1] >> 1) & 0x3; // 2: Layer II, 1: Layer III
        if ((layerBits != 2 && layerBits != 1) || (version != 3 && version != 2)) {
            return 0;
        }

        const int layer = layerBits == 2 ? 2 : 3;
        const bool mpeg1 = version == 3;
        const int kbps = (mpeg1 ? (layer == 2 ? MPEG1_L2_BITRATES : MPEG1_L3_BITRATES) : MPEG2_BITRATES)[bytes[pos + 2] >> 4];
        const int rate = (mpeg1 ? MPEG1_RATES : MPEG2_RATES)[(bytes[pos + 2] >> 2) & 0x3];
        if (kbps == 0 || rate == 0) {
            return 0;
        }

        info = { layer, mpeg1, kbps, false };
        //! NOTE: MPEG-2 Layer III frames hold half as many samples
        const int samplesFactor = (layer == 3 && !mpeg1) ? 72000 : 144000;
        return samplesFactor * kbps / rate + ((bytes[pos + 2] >> 1) & 0x1);
    };

    for (int pos = 0; pos + 4 <= size; ++pos) {
        MpegAudioInfo info;
        const int length = frameAt(pos, info);
        if (length == 0) {
            continue;
        }

        MpegAudioInfo next;
        if (frameAt(pos + length, next) > 0 && next.mpeg1 == info.mpeg1 && next.layer == info.layer) {
            info.variableBitrate = data.mid(pos, length).contains("Xing");
            return info;
        }
    }

    return std::nullopt;
}

//! NOTE: rewrite the file in place (same name, same file on disk) instead of replacing it with another file,
//! so that the application which launched the quick edit finds its own file, only with new contents
static bool overwriteFileContents(const QString& fromPath, const QString& toPath)
{
    QFile from(fromPath);
    QFile to(toPath);
    if (!from.open(QIODevice::ReadOnly) || !to.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    while (!from.atEnd()) {
        const QByteArray chunk = from.read(1024 * 1024);
        if (chunk.isEmpty() || to.write(chunk) != chunk.size()) {
            return false;
        }
    }

    return to.flush();
}

//! NOTE: an option of the quick edits: "--<name> <value>" on the command line, or an environment variable
//! (also set by GuiApp for a quick edit handed off by another process)
static QString quickEditOption(const QString& argName, const char* envName)
{
    const QStringList args = QCoreApplication::arguments();
    const int index = args.indexOf(argName);
    if (index >= 0 && index + 1 < args.size()) {
        return args.at(index + 1);
    }
    return qEnvironmentVariable(envName);
}

//! NOTE: quick edit keeps a backup (original file + project) of each save, removed after this many days
static constexpr int QUICK_EDIT_BACKUP_DAYS = 5;

static QString quickEditBackupRoot()
{
    //! NOTE: a chosen directory (--backup-dir / AU_QUICK_EDIT_BACKUP_DIR), e.g. not cleaned up by Windows like the temp one
    const QString configured = quickEditOption("--backup-dir", "AU_QUICK_EDIT_BACKUP_DIR");
    if (!configured.isEmpty()) {
        return configured;
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("Audacity Quick Edit Backups");
}

//! NOTE: an accented character may be written as one character ("ê") or as a letter + a combining accent
//! ("e" + "^"), which are different names for Windows; the calling application may also pass it in another
//! encoding ("ForÃªt", "For?t"). These helpers find the existing file or directory which was meant.

//! the name without accents, to compare names whatever the form of their accents
static QString nameKey(const QString& name)
{
    QString key;
    for (const QChar c : name.normalized(QString::NormalizationForm_D)) {
        if (c.category() != QChar::Mark_NonSpacing) {
            key += c;
        }
    }
    return key;
}

static bool hasNonAsciiCharacters(const QString& name)
{
    for (const QChar c : name) {
        if (c.unicode() > 127 || c == u'?') {
            return true;
        }
    }
    return false;
}

//! the entry of dir which the given name means, or an empty string
static QString matchingEntryName(const QDir& dir, const QString& name)
{
    const QStringList variants = {
        name.normalized(QString::NormalizationForm_C),
        name.normalized(QString::NormalizationForm_D),
        QString::fromUtf8(name.toLatin1()),     // UTF-8 bytes read as Latin-1
        QString::fromLocal8Bit(name.toLatin1()) // ANSI code page bytes read as Latin-1
    };
    for (const QString& variant : variants) {
        if (!variant.isEmpty() && !variant.contains(QChar::ReplacementCharacter) && dir.exists(variant)) {
            return variant;
        }
    }

    //! NOTE: an ASCII name is taken as is: it must not match another file (e.g. "resume" with "résumé")
    if (!hasNonAsciiCharacters(name)) {
        return QString();
    }

    const QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    //! NOTE: same name whatever the form of its accents
    const QString key = nameKey(name);
    QStringList matches;
    for (const QString& entry : entries) {
        if (nameKey(entry) == key) {
            matches << entry;
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }

    //! NOTE: last resort, the only entry matching the name with any non-ASCII character as a wildcard
    QString pattern = name.normalized(QString::NormalizationForm_C);
    for (QChar& c : pattern) {
        if (c.unicode() > 127 || c == u'?') {
            c = u'*';
        }
    }
    matches = dir.entryList({ pattern }, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    if (matches.size() == 1) {
        return matches.front();
    }

    return QString();
}

//! the existing path which the given absolute path means (each directory and the file name are looked up),
//! or the given path when there is none
static QString existingFilePath(const QString& path)
{
    if (QFileInfo::exists(path)) {
        return path;
    }

    const QStringList parts = QDir::fromNativeSeparators(path).split(u'/');

    //! NOTE: the root is kept as is: "C:", "" (Unix) or "//server/share" (network)
    int first = 1;
    QString current = parts.front();
    if (parts.size() > 3 && parts.at(0).isEmpty() && parts.at(1).isEmpty()) {
        current = "//" + parts.at(2) + "/" + parts.at(3);
        first = 4;
    }

    for (int i = first; i < parts.size(); ++i) {
        const QString& part = parts.at(i);
        if (part.isEmpty()) {
            continue;
        }

        QString next = current + "/" + part;
        if (!QFileInfo::exists(next)) {
            const QString entry = matchingEntryName(QDir(current + "/"), part);
            if (!entry.isEmpty()) {
                next = current + "/" + entry;
            } else if (i != parts.size() - 1) {
                return path;
            }
            //! NOTE: else a file to create (e.g. to record into), in the found directory
        }
        current = next;
    }

    return current;
}

//! NOTE: one lock file per quick edited file, shared by all the Audacity processes
static std::shared_ptr<QLockFile> quickEditLock(const QString& path)
{
    const QByteArray key = QFileInfo(path).absoluteFilePath().normalized(QString::NormalizationForm_C).toLower().toUtf8();
    const QString name = "audacity-quick-edit-" + QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex() + ".lock";

    auto lock = std::make_shared<QLockFile>(QDir(QDir::tempPath()).filePath(name));
    //! NOTE: never stale while its process runs (a recording may last long); the lock of a dead process is removed
    lock->setStaleLockTime(0);
    return lock;
}

static bool isMpegAudioExtension(std::string extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "mpg" || extension == "mpeg" || extension == "mpa" || extension == "m2a";
}

static void purgeQuickEditBackups()
{
    const QDateTime limit = QDateTime::currentDateTime().addDays(-QUICK_EDIT_BACKUP_DAYS);
    const QFileInfoList backups = QDir(quickEditBackupRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& backup : backups) {
        if (backup.lastModified() < limit) {
            QDir(backup.absoluteFilePath()).removeRecursively();
        }
    }
}

//! NOTE: mod-mp2: option 0 is the MPEG version (1: MPEG-1), options 1 / 2 the MPEG-1 / MPEG-2 bitrate;
//! mod-mp3: option 0 is the bitrate mode, option 4 the constant bitrate (a variable bitrate uses the preferences)
static muse::ValList mpegEncodingParameters(const MpegAudioInfo& info)
{
    if (info.layer == 2) {
        return { exportParameter(0, muse::Val(info.mpeg1 ? 1 : 0)),
                 exportParameter(info.mpeg1 ? 1 : 2, muse::Val(info.bitrateKbps)) };
    }

    if (info.variableBitrate) {
        return {};
    }

    return { exportParameter(0, muse::Val(std::string("CBR"))), exportParameter(4, muse::Val(info.bitrateKbps)) };
}

struct RecordFormat {
    std::string extension; // of the exporter
    muse::ValList parameters;
};

//! NOTE: the format of new files (e.g. RCS Zetta recordings), from --record-format / AU_RECORD_FORMAT:
//! "wav16", "wav24", "wav32f", "mp2-<kbps>" (MPEG-1 Layer II) or "mp3-<kbps>" (constant bitrate)
static std::optional<RecordFormat> configuredRecordFormat()
{
    const QString spec = quickEditOption("--record-format", "AU_RECORD_FORMAT").trimmed().toLower();
    if (spec.isEmpty()) {
        return std::nullopt;
    }

    if (spec == "wav16" || spec == "wav24" || spec == "wav32f") {
        const int subtype = spec == "wav24" ? SF_FORMAT_PCM_24 : (spec == "wav32f" ? SF_FORMAT_FLOAT : SF_FORMAT_PCM_16);
        return RecordFormat { "wav", { exportParameter(0, muse::Val(SF_FORMAT_WAV)), exportParameter(SF_FORMAT_WAV, muse::Val(subtype)) } };
    }

    const int kbps = spec.section(u'-', 1).toInt();
    if (spec.startsWith("mp2-") && kbps > 0) {
        return RecordFormat { "mp2", mpegEncodingParameters(MpegAudioInfo { 2, true, kbps, false }) };
    }
    if (spec.startsWith("mp3-") && kbps > 0) {
        return RecordFormat { "mp3", mpegEncodingParameters(MpegAudioInfo { 3, true, kbps, false }) };
    }

    LOGW() << "unknown record format: " << spec;
    return std::nullopt;
}

//! NOTE: export parameters (see mod-pcm / mod-flac option ids) reproducing the encoding of the given audio file,
//! or an empty list when it can't be read or the format isn't handled
static muse::ValList sourceEncodingParameters(const muse::io::path_t& path)
{
    //! NOTE: open through QFile so that non-ASCII paths also work on Windows
    QFile file(path.toQString());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    SF_INFO info {};
    SNDFILE* sndFile = sf_open_fd(static_cast<int>(file.handle()), SFM_READ, &info, SF_FALSE);
    if (!sndFile) {
        return {};
    }
    sf_close(sndFile);

    const int container = info.format & SF_FORMAT_TYPEMASK;
    const int subtype = info.format & SF_FORMAT_SUBMASK;

    switch (container) {
    case SF_FORMAT_WAV:
    case SF_FORMAT_WAVEX:
    case SF_FORMAT_AIFF: {
        switch (subtype) {
        case SF_FORMAT_PCM_S8:
        case SF_FORMAT_PCM_U8:
        case SF_FORMAT_PCM_16:
        case SF_FORMAT_PCM_24:
        case SF_FORMAT_PCM_32:
        case SF_FORMAT_FLOAT:
        case SF_FORMAT_DOUBLE:
            break;
        default:
            return {};
        }

        //! NOTE: mod-pcm: option 0 is the header type, option <header type> is its encoding
        const int type = container == SF_FORMAT_AIFF ? SF_FORMAT_AIFF : SF_FORMAT_WAV;
        return { exportParameter(0, muse::Val(type)), exportParameter(type, muse::Val(subtype)) };
    }
    case SF_FORMAT_FLAC: {
        //! NOTE: mod-flac: option 0 is the bit depth, option 1 the compression level
        const std::string bitDepth = subtype == SF_FORMAT_PCM_24 ? "24" : "16";
        return { exportParameter(0, muse::Val(bitDepth)), exportParameter(1, muse::Val(std::string("5"))) };
    }
    default:
        return {};
    }
}

static const muse::actions::ActionCode OPEN_CUSTOM_FFMPEG_OPTIONS("open-custom-ffmpeg-options");
static const muse::actions::ActionCode OPEN_METADATA_DIALOG("open-metadata-dialog");
static const muse::actions::ActionCode OPEN_CUSTOM_MAPPING("open-custom-mapping");
static const muse::actions::ActionQuery OPEN_CLOUD_AUDIO_FILE_URI("audacity://cloud/open-audio-file");
static const muse::actions::ActionQuery UPDATE_AUDIO_PREVIEW_ACTION("audacity://cloud/update-audio-preview");
static const muse::actions::ActionQuery UPDATE_AUDIO_PREVIEW_FOR_PROJECT_ACTION("audacity://cloud/update-audio-preview-for-project");

namespace {
QString cloudProjectOpenUrl(const muse::String& projectId, const muse::String& snapshotId)
{
    muse::UriQuery cloudUri(AUDACITY_URL_SCHEME.toStdString() + "://open");
    cloudUri.addParam("projectId", muse::Val(projectId.toStdString()));
    if (!snapshotId.isEmpty()) {
        cloudUri.addParam("snapshotId", muse::Val(snapshotId.toStdString()));
    }

    return QString::fromStdString(cloudUri.toString());
}

au::au3cloud::UploadMode toUploadMode(CloudSaveMode mode)
{
    switch (mode) {
    case CloudSaveMode::CreateNew:
        return au::au3cloud::UploadMode::CreateNew;
    case CloudSaveMode::ForceOverwrite:
        return au::au3cloud::UploadMode::ForceOverwrite;
    case CloudSaveMode::NormalUpdate:
        return au::au3cloud::UploadMode::NormalUpdate;
    }

    return au::au3cloud::UploadMode::NormalUpdate;
}

const muse::actions::ActionCodeList& prohibitedWhileRecording()
{
    static const muse::actions::ActionCodeList codes {
        "file-close",
        "project-import",
        "file-save",
        "file-save-to-cloud",
        "file-save-as",
        "export-audio",
        "export-labels",
        "export-midi",
        "file-share-audio",
        "audacity://cloud/update-audio-preview",
        "audacity://cloud/update-audio-preview-for-project",
    };

    return codes;
}

const std::unordered_set<muse::actions::ActionCode>& dontRequireOpenProject()
{
    static const std::unordered_set<muse::actions::ActionCode> codes {
        "file-new",
        "file-open",
        "file-open-recent",
        "project-import-startup-media",
        "cloud-file-open",
        "continue-last-session",
        "clear-recent",
        "audacity://cloud/open-audio-file",
        "audacity://cloud/update-audio-preview-for-project",
        "plugin-manager",
        "project-show-in-folder",
    };

    return codes;
}

const std::unordered_set<muse::actions::ActionCode>& prohibitedOnNonCloudProject()
{
    static const std::unordered_set<muse::actions::ActionCode> codes {
        "audacity://cloud/update-audio-preview",
    };

    return codes;
}

const std::unordered_set<muse::actions::ActionCode>& prohibitedWithoutAudio()
{
    static const std::unordered_set<muse::actions::ActionCode> codes {
        "file-share-audio",
        "audacity://cloud/update-audio-preview",
        "export-audio",
    };

    return codes;
}
}

ProjectActionsController::ProjectActionsController(muse::modularity::ContextPtr ctx)
    : muse::Contextable(ctx)
{
}

void ProjectActionsController::init()
{
    dispatcher()->reg(this, "file-new", this, &ProjectActionsController::newProject);
    dispatcher()->reg(this, "file-open", this, &ProjectActionsController::open);
    dispatcher()->reg(this, "file-open-recent", this, &ProjectActionsController::open);
    dispatcher()->reg(this, "cloud-file-open", this, &ProjectActionsController::openCloudProject);
    dispatcher()->reg(this, "clear-recent", this, &ProjectActionsController::clearRecentProjects);
    dispatcher()->reg(this, "project-import", this, &ProjectActionsController::importFiles);
    dispatcher()->reg(this, "project-import-startup-media", this, &ProjectActionsController::importStartupMedia);

    dispatcher()->reg(this, "file-save", [this]() { saveProject(SaveMode::Save); });
    dispatcher()->reg(this, "file-save-to-cloud", [this]() { saveProject(SaveMode::Save, SaveLocationType::Cloud); });
    //! TODO AU4: decide whether to implement these functions from scratch in AU4 or
    //! to install our own implementation of the UI (BasicUI API)
    //! right now there's only BasicUI stub which means there's no progress dialog shown on saving
    dispatcher()->reg(this, "file-save-as", [this]() { saveProject(SaveMode::SaveAs); });

    dispatcher()->reg(this, "file-share-audio", this, &ProjectActionsController::shareAudio);
    dispatcher()->reg(this, OPEN_CLOUD_AUDIO_FILE_URI, this, &ProjectActionsController::openCloudAudioFile);
    dispatcher()->reg(this, UPDATE_AUDIO_PREVIEW_ACTION, this, &ProjectActionsController::updateCloudAudioPreview);
    dispatcher()->reg(this, UPDATE_AUDIO_PREVIEW_FOR_PROJECT_ACTION, this, &ProjectActionsController::updateCloudAudioPreview);

    dispatcher()->reg(this, "export-audio", this, &ProjectActionsController::exportAudio);
    dispatcher()->reg(this, "export-labels", this, &ProjectActionsController::exportLabels);
    dispatcher()->reg(this, "export-midi", this, &ProjectActionsController::exportMIDI);

    dispatcher()->reg(this, "open-metadata-editor", this, &ProjectActionsController::openMetadataDialog);

    dispatcher()->reg(this, "file-close", [this]() {
        // reset preferred export sample rate
        exportConfiguration()->setExportSampleRate(-1);

        if (multiwindowsProvider()->windowCount() > 1) {
            mainWindow()->qWindow()->close();
            return;
        }

        closeOpenedProject(false);
    });

    dispatcher()->reg(this, OPEN_CUSTOM_FFMPEG_OPTIONS, this, &ProjectActionsController::openCustomFFmpegOptions);
    dispatcher()->reg(this, OPEN_METADATA_DIALOG, this, &ProjectActionsController::openMetadataDialog);
    dispatcher()->reg(this, OPEN_CUSTOM_MAPPING, this, &ProjectActionsController::openCustomMapping);

    globalContext()->currentTrackeditProjectChanged().onNotify(this, [this]() {
        listenTrackeditProjectChanges();
        listenCloudProjectChanges();
    });

    listenTrackeditProjectChanges();
    listenCloudProjectChanges();

    recordController()->isRecordingChanged().onNotify(this, [this]() {
        m_actionEnabledChanged.send(prohibitedWhileRecording());
    });
}

void ProjectActionsController::listenTrackeditProjectChanges()
{
    trackedit::ITrackeditProjectPtr prj = globalContext()->currentTrackeditProject();
    if (!prj) {
        return;
    }

    prj->hasAudioContent().ch.onReceive(this, [this](bool) {
        m_actionEnabledChanged.send({ "file-share-audio" });
    }, muse::async::Asyncable::Mode::SetReplace);

    m_actionEnabledChanged.send({ "file-share-audio" });
}

void ProjectActionsController::listenCloudProjectChanges()
{
    IAudacityProjectPtr prj = currentProject();
    if (!prj) {
        return;
    }

    prj->isCloudProjectChanged().onNotify(this, [this]() {
        m_actionEnabledChanged.send({ UPDATE_AUDIO_PREVIEW_ACTION.toString() });
    }, muse::async::Asyncable::Mode::SetReplace);

    m_actionEnabledChanged.send({ UPDATE_AUDIO_PREVIEW_ACTION.toString() });
}

muse::async::Channel<muse::actions::ActionCodeList> ProjectActionsController::actionEnabledChanged() const
{
    return m_actionEnabledChanged;
}

bool ProjectActionsController::canReceiveAction(const muse::actions::ActionCode& code) const
{
    const IAudacityProjectPtr project = currentProject();
    if (!project) {
        return muse::contains(dontRequireOpenProject(), code);
    }

    if (muse::contains(prohibitedOnNonCloudProject(), code) && !project->isCloudProject()) {
        return false;
    }

    if (muse::contains(prohibitedWithoutAudio(), code)) {
        const trackedit::ITrackeditProjectPtr trackeditProject = globalContext()->currentTrackeditProject();
        if (!trackeditProject || !trackeditProject->hasAudioContent().val) {
            return false;
        }
    }

    //! NOTE: saving a quick edit while recording stops the recording first (see saveProject)
    if (muse::contains(prohibitedWhileRecording(), code) && recordController()->isRecording()
        && !(code == "file-save" && isQuickEditProject(project))) {
        return false;
    }

    return true;
}

IAudacityProjectPtr ProjectActionsController::currentProject() const
{
    return globalContext()->currentProject();
}

Ret ProjectActionsController::openProject(const ProjectFile& file)
{
    LOGI() << "Try open project: url = " << file.url.toString() << ", displayNameOverride = " << file.displayNameOverride;

    if (file.isNull() || file.url.isLocalFile()) {
        muse::io::paths_t filenames = file.isNull() ? selectOpeningFiles() : muse::io::paths_t { file.path() };
        muse::io::path_t filename = filenames.empty() ? muse::io::path_t() : filenames.front();

        if (filename.empty()) {
            return make_ret(Ret::Code::Cancel);
        }

        if (au::project::isAudacity3File(filename)) {
            auto resolved = openSaveProjectScenario()->resolveLegacyProjectFormat(filename);
            if (!resolved.ret) {
                return resolved.ret;
            }
            filename = resolved.val;
        }

        return openProject(filename, file.displayNameOverride);
    }

    if (file.url.scheme() == AUDACITY_URL_SCHEME) {
        muse::UriQuery query(file.url.toString().toStdString());
        const std::string projectId = query.param("projectId").toString();
        if (!projectId.empty()) {
            const std::string snapshotId = query.param("snapshotId").toString();
            dispatcher()->dispatch("cloud-file-open",
                                   muse::actions::ActionData::make_arg2<QString, QString>(
                                       QString::fromStdString(projectId),
                                       QString::fromStdString(snapshotId)));
            return muse::make_ok();
        }
    }

    return make_ret(Err::UnsupportedUrl);
}

void ProjectActionsController::newProject()
{
    //! NOTE This method is synchronous,
    //! but inside `multiwindowsProvider` there can be an event loop
    //! to wait for the responses from other instances, accordingly,
    //! the events (like user click) can be executed and this method can be called several times,
    //! before the end of the current call.
    //! So we ignore all subsequent calls until the current one completes.
    if (m_isProjectProcessing) {
        return;
    }
    m_isProjectProcessing = true;

    DEFER {
        m_isProjectProcessing = false;
    };

    if (globalContext()->currentProject()) {
        //! Check, if any project is already open in the current window
        //! and there is already a created instance without a project, then activate it
        if (multiwindowsProvider()->isHasWindowWithoutProject()) {
            multiwindowsProvider()->activateWindowWithoutProject();
            return;
        }

        //! Otherwise, we will create a new instance
        QStringList args;
        args << "--session-type" << "start-with-new";
        multiwindowsProvider()->openNewWindow(args);
        return;
    }

    auto project = createProjectInCurrentWindow();
    if (!project) {
        LOGE() << "Failed to create new project in current window";
    }

    muse::async::Async::call(this, [this, ok = !!project]() {
        openPageIfNeed(ok ? PROJECT_PAGE_URI : HOME_PAGE_URI);
    });
}

void ProjectActionsController::open(const muse::actions::ActionData& args)
{
    const QUrl url = !args.empty() ? args.arg<QUrl>(0) : QUrl();
    const QString displayNameOverride = args.count() >= 2 ? args.arg<QString>(1) : QString();

    Ret ret = make_ret(Ret::Code::Cancel);

    if (url.isValid() && !url.isEmpty() && !url.isLocalFile()) {
        ret = openProject(ProjectFile(url, displayNameOverride));
        if (!ret) {
            openPageIfNeed(HOME_PAGE_URI);
        }
        return;
    }

    const muse::io::paths_t filePaths = url.isLocalFile() ? muse::io::paths_t { muse::io::path_t(url) } : selectOpeningFiles();

    if (filePaths.empty()) {
        ret = make_ret(Ret::Code::Cancel);
    } else if (filePaths.size() > 1) {
        auto projectIt = std::find_if(filePaths.cbegin(), filePaths.cend(), [](const auto& filePath) {
            return au::project::isAudacityFile(filePath);
        });

        if (projectIt != filePaths.cend()) {
            ret = openProject(ProjectFile(QUrl::fromLocalFile(projectIt->toQString()), displayNameOverride));
        } else {
            muse::io::paths_t supportedFilePaths;
            supportedFilePaths.reserve(filePaths.size());
            for (const auto& filePath : filePaths) {
                if (isFileSupported(filePath)) {
                    supportedFilePaths.emplace_back(filePath);
                }
            }

            if (supportedFilePaths.empty()) {
                ret = make_ret(Err::UnsupportedUrl);
            } else {
                ret = processMediaFiles(supportedFilePaths);
            }
        }
    } else if (!isFileSupported(filePaths.front())) {
        interactive()->error(muse::trc("project", "Error opening file"),
                             muse::mtrc("project", "Could not open file: %1").arg(filePaths.front().toString()).toStdString());
        ret = make_ret(Err::UnsupportedUrl);
    } else if (au::project::isAudacityFile(filePaths.front())) {
        ret = openProject(ProjectFile(QUrl::fromLocalFile(filePaths.front().toQString()), displayNameOverride));
    } else {
        ret = processMediaFiles({ filePaths.front() });
    }

    if (!ret) {
        openPageIfNeed(HOME_PAGE_URI);
    }
}

void ProjectActionsController::openCloudProject(const muse::actions::ActionData& args)
{
    if (args.count() > 2 || args.count() < 1) {
        return;
    }

    const QString cloudProjectId = args.arg<QString>(0);
    const QString snapshotId = args.count() >= 2 ? args.arg<QString>(1) : QString();

    if (m_isProjectProcessing) {
        return;
    }
    m_isProjectProcessing = true;

    DEFER {
        m_isProjectProcessing = false;
    };

    std::optional<io::path_t> localPath;
    if (const auto record = cloudProjectsProvider()->projectRecordForId(cloudProjectId.toStdString());
        record&& !record->localPath.empty()) {
        localPath = record->localPath;
    }

    if (localPath && snapshotId.isEmpty()) {
        if (isProjectOpened(localPath.value())) {
            openPageIfNeed(PROJECT_PAGE_URI);
            return;
        }

        if (multiwindowsProvider()->isProjectAlreadyOpened(localPath.value())) {
            multiwindowsProvider()->activateWindowWithProject(localPath.value());
            return;
        }
    }

    if (globalContext()->currentProject()) {
        QStringList newWindowArgs;
        newWindowArgs << cloudProjectOpenUrl(cloudProjectId, snapshotId);
        multiwindowsProvider()->openNewWindow(newWindowArgs);
        return;
    }

    Ret ret = openCloudProject(localPath.value_or(io::path_t {}), cloudProjectId, snapshotId);
    if (!ret) {
        openPageIfNeed(HOME_PAGE_URI);
    }
}

void ProjectActionsController::importFiles(const muse::actions::ActionData& args)
{
    const IAudacityProjectPtr project = globalContext()->currentProject();
    if (!project) {
        return;
    }

    muse::io::paths_t filePaths;
    if (!args.empty()) {
        const QStringList files = args.arg<QStringList>(0);
        filePaths.reserve(files.size());
        for (const QString& file : files) {
            const io::path_t path(file);
            const io::path_t actualPath = fileSystem()->absoluteFilePath(path);
            filePaths.emplace_back(actualPath.empty() ? path : actualPath);
        }
    } else {
        filePaths = selectImportFiles();
    }

    if (filePaths.empty()) {
        return;
    }

    project->import(filePaths);
}

void ProjectActionsController::importStartupMedia(const muse::actions::ActionData& args)
{
    const QStringList files = !args.empty() ? args.arg<QStringList>(0) : QStringList();
    const bool removeAfterImport = args.count() >= 2 ? args.arg<bool>(1) : false;
    const bool quickEdit = args.count() >= 3 ? args.arg<bool>(2) : false;
    const QString quickEditToken = args.count() >= 4 ? args.arg<QString>(3) : QString();

    muse::io::paths_t filePaths;
    filePaths.reserve(files.size());
    for (const QString& file : files) {
        filePaths.emplace_back(file);
    }

    Ret ret = processMediaFiles(filePaths, quickEdit, quickEditToken);
    if (removeAfterImport) {
        for (const auto& filePath : filePaths) {
            fileSystem()->remove(filePath);
        }
    }

    if (!ret) {
        openPageIfNeed(HOME_PAGE_URI);
    }
}

muse::Ret ProjectActionsController::processMediaFiles(const muse::io::paths_t& paths, bool quickEdit, const QString& quickEditToken)
{
    if (paths.empty()) {
        return make_ret(Ret::Code::Cancel);
    }

    muse::io::paths_t actualPaths;
    actualPaths.reserve(paths.size());
    for (const auto& givenPath : paths) {
        //! NOTE: find the file even when the accents of its path were passed in another form or encoding
        const QString givenAbsolutePath = QFileInfo(givenPath.toQString()).absoluteFilePath();
        const QString existingPath = existingFilePath(givenAbsolutePath);
        if (existingPath != givenAbsolutePath) {
            LOGI() << "\"" << givenAbsolutePath << "\" found as \"" << existingPath << "\"";
        }

        io::path_t actualPath = fileSystem()->absoluteFilePath(io::path_t(existingPath));
        if (actualPath.empty() && quickEdit) {
            //! NOTE: the file to quick edit may not exist yet (see below)
            actualPath = io::path_t(existingPath);
        }
        if (actualPath.empty()) {
            return make_ret(Ret::Code::UnknownError);
        }

        actualPaths.emplace_back(actualPath);
    }

    //! NOTE: quick edit works on one source file per project: when several files are given (e.g. %F),
    //! open each extra file in its own quick edit window and keep the first one for this window
    if (quickEdit && actualPaths.size() > 1) {
        for (size_t i = 1; i < actualPaths.size(); ++i) {
            QStringList args;
            args << "--session-type" << "start-with-new"
                 << "--import-media-file" << actualPaths.at(i).toQString()
                 << "--quick-edit";
            multiwindowsProvider()->openNewWindow(args);
        }
        actualPaths.resize(1);
    }

    const bool isQuickEdit = quickEdit;

    if (globalContext()->currentProject()) {
        QStringList args;
        args << "--session-type" << "start-with-new";
        for (const auto& actualPath : actualPaths) {
            args << "--import-media-file" << actualPath.toQString();
        }
        if (isQuickEdit) {
            args << "--quick-edit";
        }

        multiwindowsProvider()->openNewWindow(args);
        return make_ret(Ret::Code::Ok);
    }

    IAudacityProjectPtr project = createProjectInCurrentWindow();
    if (!project) {
        return make_ret(Ret::Code::InternalError);
    }

    if (!quickEditToken.isEmpty()) {
        m_quickEditHandoffs[project.get()] = std::make_unique<QuickEditHandoff>(quickEditToken);
    }

    Ret ret = openPageIfNeed(PROJECT_PAGE_URI);
    if (!ret) {
        return ret;
    }

    if (isQuickEdit) {
        LOGI() << "quick edit of " << actualPaths.front().toQString();
    }

    //! NOTE: the same file may already be quick edited (or recorded into) by another Audacity process
    std::shared_ptr<QLockFile> lock;
    bool openedElsewhere = false;
    if (isQuickEdit) {
        lock = quickEditLock(actualPaths.front().toQString());
        openedElsewhere = !lock->tryLock(0);
        if (openedElsewhere) {
            lock.reset();
        }
    }

    //! NOTE: the calling application may give a file to create (missing or empty, e.g. to record into it):
    //! quick edit then starts with an empty project, which is saved to that file
    const bool isNewQuickEditFile = isQuickEdit && QFileInfo(actualPaths.front().toQString()).size() == 0;
    if (openedElsewhere) {
        toastService()->show(muse::trc("project", "Already open"),
                             muse::mtrc("project", "\"%1\" is also open in another Audacity window: what is saved last "
                                                   "replaces the file.").arg(actualPaths.front().toString()).toStdString(),
                             muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});
    }

    if (isNewQuickEditFile && !openedElsewhere && canQuickEdit(actualPaths.front())) {
        //! NOTE: tell it, a wrong path would otherwise silently open an empty project
        toastService()->show(muse::trc("project", "Recording"),
                             muse::mtrc("project", "Recording into \"%1\". Save (Ctrl+S) to stop, write the file and close.")
                             .arg(actualPaths.front().toString()).toStdString(),
                             muse::ui::IconCode::Code::WARNING, true /*dismissable*/, {});
    } else if (!isNewQuickEditFile) {
        ret = project->import(actualPaths);
    }

    //! NOTE: a live recording still running is followed (its end is added as it is recorded), and its edit is saved
    //! next to it: never over the live file, which the recording Audacity writes
    if (ret && !isNewQuickEditFile && actualPaths.size() == 1 && LiveRecordFollower::isRunningLiveRecording(actualPaths.front())) {
        m_liveRecordFollower = std::make_unique<LiveRecordFollower>(iocContext());
        m_liveRecordFollower->start(project, actualPaths.front());
        startQuickEdit(project, LiveRecordFollower::montagePath(actualPaths.front()), nullptr);
        return ret;
    }

    //! NOTE: only files which can be written back are quick edited, others open as a regular project
    if (ret && isQuickEdit && canQuickEdit(actualPaths.front())) {
        startQuickEdit(project, actualPaths.front(), lock);

        //! NOTE: a new (empty) file is given to record into (e.g. RCS Zetta "record"): start recording right away,
        //! once the project page is ready (not when another Audacity is already recording into it)
        if (isNewQuickEditFile && !openedElsewhere) {
            //! NOTE: with a live recording directory, the recording is also written there while it runs
            if (!LiveRecordMirror::liveRecordDir().isEmpty()) {
                m_liveRecordMirror = std::make_unique<LiveRecordMirror>(iocContext());
            }

            const muse::io::path_t targetPath = actualPaths.front();
            muse::async::Async::call(this, [this, project, targetPath]() {
                dispatcher()->dispatch("record-on-new-track");
                if (m_liveRecordMirror) {
                    m_liveRecordMirror->start(project, targetPath);
                }
            });
        }
    }

    return ret;
}

ProjectActionsController::QuickEditHandoff::QuickEditHandoff(const QString& token)
    : m_basePath(QDir(QDir::tempPath()).filePath("audacity-quick-edit-" + token)), m_lock(m_basePath + ".lock")
{
    m_lock.setStaleLockTime(0);
    //! NOTE: the waiting process tries the lock now and then: retry for a while
    if (!m_lock.tryLock(5000)) {
        LOGW() << "quick edit " << token << ": can't take " << m_basePath << ".lock";
    }
}

ProjectActionsController::QuickEditHandoff::~QuickEditHandoff()
{
    //! NOTE: "done" first, so that the waiting process never sees the lock free without it
    QFile done(m_basePath + ".done");
    if (done.open(QIODevice::WriteOnly)) {
        done.close();
    }
    m_lock.unlock();
}

bool ProjectActionsController::canQuickEdit(const muse::io::path_t& sourcePath) const
{
    //! NOTE: never write audio over a program or a script
    static const QStringList NEVER_WRITTEN = { "exe", "dll", "com", "bat", "cmd", "msi", "lnk", "ps1", "vbs", "js", "sys" };
    if (NEVER_WRITTEN.contains(QFileInfo(sourcePath.toQString()).suffix(), Qt::CaseInsensitive)) {
        return false;
    }

    return !quickEditFormat(sourcePath).empty();
}

std::string ProjectActionsController::formatNameForContents(const muse::io::path_t& path) const
{
    QFile file(path.toQString());
    if (file.open(QIODevice::ReadOnly)) {
        SF_INFO info {};
        SNDFILE* sndFile = sf_open_fd(static_cast<int>(file.handle()), SFM_READ, &info, SF_FALSE);
        if (sndFile) {
            sf_close(sndFile);
            switch (info.format & SF_FORMAT_TYPEMASK) {
            case SF_FORMAT_WAV:
            case SF_FORMAT_WAVEX:
            case SF_FORMAT_RF64:
            case SF_FORMAT_W64:
                return formatNameForExtension("wav");
            case SF_FORMAT_AIFF:
                return formatNameForExtension("aiff");
            case SF_FORMAT_FLAC:
                return formatNameForExtension("flac");
            case SF_FORMAT_OGG:
                return formatNameForExtension((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_OPUS ? "opus" : "ogg");
            default:
                break;
            }
        }
    }

    if (const std::optional<MpegAudioInfo> mpeg = mpegAudioInfo(path)) {
        return formatNameForExtension(mpeg->layer == 2 ? "mp2" : "mp3");
    }

    return std::string();
}

std::string ProjectActionsController::quickEditFormat(const muse::io::path_t& sourcePath) const
{
    //! NOTE: RCS Zetta passes files without an audio extension (e.g. "name.-12283"): rely on the contents first
    std::string format = formatNameForContents(sourcePath);
    if (!format.empty()) {
        return format;
    }

    const bool isNewFile = QFileInfo(sourcePath.toQString()).size() == 0;
    if (isNewFile) {
        if (const std::optional<RecordFormat> recordFormat = configuredRecordFormat()) {
            return formatNameForExtension(recordFormat->extension);
        }
    }

    const std::string extension = io::suffix(sourcePath);
    format = formatNameForExtension(extension);
    if (!format.empty()) {
        return format;
    }

    //! NOTE: a new (empty) file to record into: MP2 for MPEG extensions, WAV otherwise
    if (isNewFile) {
        return formatNameForExtension(isMpegAudioExtension(extension) ? "mp2" : "wav");
    }

    return std::string();
}

void ProjectActionsController::startQuickEdit(const IAudacityProjectPtr& project, const muse::io::path_t& sourcePath,
                                              std::shared_ptr<QLockFile> lock)
{
    m_quickEditSourceFiles[project.get()] = QuickEditSource { sourcePath, true, std::move(lock) };

    //! NOTE: any edit after opening / exporting makes the source file outdated again
    IAudacityProject* rawProject = project.get();
    project->needSave().notification.onNotify(this, [this, rawProject]() {
        const auto it = m_quickEditSourceFiles.find(rawProject);
        if (it != m_quickEditSourceFiles.end()) {
            it->second.upToDate = false;
        }
    }, muse::async::Asyncable::Mode::SetReplace);
}

bool ProjectActionsController::isQuickEditProjectUpToDate(const IAudacityProjectPtr& project) const
{
    if (!project) {
        return false;
    }

    const auto it = m_quickEditSourceFiles.find(project.get());
    return it != m_quickEditSourceFiles.end() && it->second.upToDate;
}


bool ProjectActionsController::isUrlSupported(const QUrl& url) const
{
    if (url.isLocalFile()) {
        return isFileSupported(muse::io::path_t(url));
    }

    if (url.scheme() == AUDACITY_URL_SCHEME) {
        if (url.host() == OPEN_PROJECT_URL_HOSTNAME) {
            return true;
        }
    }

    return false;
}

bool ProjectActionsController::isFileSupported(const muse::io::path_t& path) const
{
    if (au::project::isAudacityFile(path)) {
        return true;
    }

    const std::string ext = io::suffix(path);
    if (ext.empty()) {
        return false;
    }

    const auto supportedExtensions = importer()->supportedExtensions();
    return std::find(supportedExtensions.cbegin(), supportedExtensions.cend(), ext) != supportedExtensions.cend();
}

bool ProjectActionsController::closeOpenedProject(const bool quitApp)
{
    if (m_isProjectClosing) {
        return false;
    }

    m_isProjectClosing = true;
    DEFER {
        m_isProjectClosing = false;
    };

    const IAudacityProjectPtr project = globalContext()->currentProject();
    if (!project) {
        return true;
    }

    bool result = true;

    //! NOTE: a quick edit project whose changes were already exported back to the source has nothing to save
    if (project->hasUnsavedChanges() && !isQuickEditProjectUpToDate(project)) {
        IInteractive::Button btn = askAboutSavingProject(project);

        if (btn == IInteractive::Button::Cancel) {
            result = false;
        } else if (btn == IInteractive::Button::Save) {
            result = saveProject();
        } else if (btn == IInteractive::Button::DontSave) {
            result = true;
        }
    }

    if (result && project->isCloudProject()) {
        result = askAboutStoppingCloudSync();
    }

    if (result) {
        interactive()->closeAllDialogsSync();

        //! NOTE: finish the live recording file while the project still exists
        m_liveRecordMirror.reset();
        m_liveRecordFollower.reset();
        m_quickEditHandoffs.erase(project.get());

        project->close();

        m_quickEditSourceFiles.erase(project.get());
        globalContext()->setCurrentProject(nullptr);

        if (quitApp) {
            //! NOTE: we need to call `quit` in the next event loop due to controlling the lifecycle of this method
            muse::async::Async::call(this, [this](){
                dispatcher()->dispatch("quit", actions::ActionData::make_arg1<bool>(false));
            });
        } else {
            Ret ret = openPageIfNeed(HOME_PAGE_URI);
            if (!ret) {
                LOGE() << ret.toString();
            }
        }
    }

    return result;
}

bool ProjectActionsController::askAboutStoppingCloudSync()
{
    if (!audioComService()->syncingInProgressChanged().val) {
        return true;
    }

    static const Uri CLOUD_PROJECT_SYNC_URI("audacity://project/cloudprojectsyncing");

    RetVal<Val> rv = interactive()->openSync(CLOUD_PROJECT_SYNC_URI);
    std::string status = rv.val.toString();
    return status == "stopped" || status == "synced";
}

bool ProjectActionsController::saveProject(const muse::io::path_t& path)
{
    if (!path.empty()) {
        return saveProjectLocally(path);
    }

    return saveProject(SaveMode::Save);
}

muse::Ret ProjectActionsController::saveProjectToCloud(const CloudProjectInfo& cloudInfo, CloudSaveMode cloudSaveMode,
                                                       std::function<void()> onSuccess)
{
    if (!audioComService()->enabled()) {
        return make_ret(Ret::Code::NotSupported, std::string { "Cloud support is not available" });
    }

    if (const Ret ret = ensureAuthorization(); !ret) {
        return ret;
    }

    io::path_t cloudProjectsPath = configuration()->cloudProjectsPath();
    if (cloudProjectsPath.empty()) {
        return make_ret(Ret::Code::UnknownError, std::string { "Cloud projects path is not set" });
    }

    IAudacityProjectPtr project = currentProject();
    if (!project) {
        return make_ret(Ret::Code::UnknownError, std::string { "No project opened" });
    }

    io::path_t projectFilePath;
    if (cloudSaveMode != CloudSaveMode::CreateNew && project->isCloudProject()) {
        projectFilePath = project->path();
    } else {
        projectFilePath = cloudProjectsProvider()->makeSafeFilePath(cloudProjectsPath, cloudInfo.name.toStdString(),
                                                                    au::project::AUP4);
    }

    auto [uploadRet, progress] = audioComService()->uploadProject(project, cloudInfo.name.toStdString(), [this, projectFilePath]() {
        return saveProjectLocally(projectFilePath, SaveMode::Save);
    }, toUploadMode(cloudSaveMode));

    if (!uploadRet) {
        handleCloudSaveError(uploadRet);
        return uploadRet;
    }

    if (!progress) {
        if (onSuccess) {
            onSuccess();
        }
        return make_ok();
    }

    progress->finished().onReceive(this, [this, projectFilePath, onSuccess](const ProgressResult& result) {
        if (!result.ret.success()) {
            handleCloudSaveError(result.ret);
            return;
        }

        const bool dismissable = false;
        toastService()->show(trc("global", "Success"),
                             trc("project",
                                 "All saved changes will now update to the cloud.\nYou can manage this file from your updated projects page on audio.com"),
                             muse::ui::IconCode::Code::TICK,
                             dismissable,
        {
            //: Label of the button that dismisses a notification
            { trc("project", "Dismiss"), muse::toast::ToastActionCode::None },
            { trc("cloud", "View on audio.com"), muse::toast::ToastActionCode::Custom }
        }
                             ).onResolve(this, [this, url = result.val.toQString()](muse::toast::ToastActionCode actionCode) {
            if (actionCode == muse::toast::ToastActionCode::Custom) {
                platformInteractive()->openUrl(url);
            }
        });

        if (onSuccess) {
            onSuccess();
        }
    });

    const bool dismissible = false;
    const bool showProgressInfo = true;
    toastService()->showWithProgress(
        trc("project", "Upload project to audio.com…"),
        {},
        progress,
        muse::ui::IconCode::Code::CLOUD,
        dismissible,
    {
        { trc("project", "Dismiss"), muse::toast::ToastActionCode::None },
        { trc("global", "Stop"), muse::toast::ToastActionCode::Custom }
    },
        showProgressInfo
        ).onResolve(this, [this, progress = progress](const muse::toast::ToastActionCode& actionCode) {
        if (actionCode == muse::toast::ToastActionCode::Custom) {
            audioComService()->stopProjectSync();
            progress->cancel();
        }
    });

    return make_ok();
}

bool ProjectActionsController::saveProjectLocally(const muse::io::path_t& filePath, SaveMode saveMode)
{
    IAudacityProjectPtr project = currentProject();
    if (!project) {
        return false;
    }

    Ret ret = project->save(filePath, saveMode);
    if (!ret) {
        LOGE() << ret.toString();
        return false;
    }

    //! NOTE: the user explicitly chose a project file location, so this is no longer a "quick edit" session
    m_quickEditSourceFiles.erase(project.get());

    recentFilesController()->prependRecentFile(makeRecentFile(project));
    return true;
}

const ProjectBeingDownloaded& ProjectActionsController::projectBeingDownloaded() const
{
    return m_projectBeingDownloaded;
}

async::Notification ProjectActionsController::projectBeingDownloadedChanged() const
{
    return m_projectBeingDownloadedChanged;
}

muse::io::paths_t ProjectActionsController::selectOpeningFiles()
{
    std::vector<std::string> supportedExtensions = importer()->supportedExtensions();

    std::string mediaExt;
    for (const std::string& ext : supportedExtensions) {
        if (ext.empty()) {
            continue;
        }

        if (!mediaExt.empty()) {
            mediaExt += " ";
        }

        mediaExt += "*." + ext;
    }

    const std::string projectExt = "*.aup3 *.aup4";
    const std::string allExt = mediaExt.empty() ? projectExt : projectExt + " " + mediaExt;

    std::vector<std::string> filter {
        trc("project", "All supported files") + " (*.aup4,*.mp3, ...) (" + allExt + ")",
        trc("project", "Audacity project files") + " (*.aup3,*.aup4, ...) (" + projectExt + ")",
        trc("project", "Audacity 3 files") + " (*.aup3, ...) (*.aup3)",
        trc("project", "Audacity 4 files") + " (*.aup4, ...) (*.aup4)",
        trc("project", "Importable audio and media files") + " (*.mp3,*.aac, ...) (" + mediaExt + ")",
    };

    io::path_t defaultDir = configuration()->lastOpenedProjectsPath();

    if (defaultDir.empty()) {
        defaultDir = configuration()->userProjectsPath();
    }

    if (defaultDir.empty()) {
        defaultDir = configuration()->defaultUserProjectsPath();
    }

    //: Title of a file picker dialog
    io::paths_t filePaths = interactive()->selectOpeningFilesSync(muse::trc("project",
                                                                            "Open"), defaultDir, filter,
                                                                  QFileDialog::HideNameFilterDetails);

    if (!filePaths.empty()) {
        configuration()->setLastOpenedProjectsPath(io::dirpath(filePaths.front()));
    }

    return filePaths;
}

muse::io::paths_t ProjectActionsController::selectImportFiles()
{
    std::string audioFileExt
        = "*.aac *.ac3 *.mp2 *.mp3 *.wma *.wav *.flac *.ogg *.opus *.aif *.aiff *.amr *.ape *.au *.dts *.mpc *.tta *.wv *.shn *.voc *.mmf";
    std::string videoFileExt
        = "*.avi *.mp4 *.mkv *.mov *.flv *.wmv *.asf *.webm *.mpg *.mpeg *.m4v *.ts *.gxf *.mxf *.nut *.dv *.3gp *.3g2 *.mj2";
    std::string gameMediaFileExt
        =
            "*.roq *.bethsoftvid *.c93 *.dsicin *.dxa *.ea *.cdata *.film_cpk *.idcin *.ipmovie *.psxstr *.rl2 *.siff *.smk *.thp *.tiertexseq *.vmd *.wc3movie *.wsaud *.wsvqa *.txd";
    std::string streamingFileExt = "*.rtsp *.sdp *.nsv *.pva *.msnwctcp *.lmlm4 *.redir";
    std::string animationAndImageFileExt = "*.gif *.flic *.swf *.image2 *.image2pipe";
    std::string rawFileExt
        =
            "*.al *.ul *.s16be *.u16be *.s8 *.u8 *.ub *.uw *.4xm *.MTV *.afc *.aifc *.apc *.apl *.mac *.avs *.302 *.daud *.ffm *.cgi *.mm *.mpegtsraw *.mpegvideo *.nuv *.sw *.sb *.son *.sol *.vfwcap";
    std::string textFileExt = "*.txt *.srt *.vtt";

    std::string allExt = audioFileExt + " " + videoFileExt + " " + gameMediaFileExt + " " + streamingFileExt + " "
                         + animationAndImageFileExt + " " + rawFileExt + " " + textFileExt;

    std::vector<std::string> filter {
        trc("project", "All supported files") + " (*.mp3,*.aac, ...) (" + allExt + ")",
        trc("project", "Audio files") + " (*.mp3,*.aac, ...) (" + audioFileExt + ")",
        trc("project", "Video files") + " (*.mp4,*.avi, ...) (" + videoFileExt + ")",
        trc("project", "Game media files") + " (*.roq,*.ea, ...) (" + gameMediaFileExt + ")",
        trc("project", "Streaming files") + " (*.rtsp,*.sdp, ...) (" + streamingFileExt + ")",
        trc("project", "Animation and image files") + " (*.gif, ...) (" + animationAndImageFileExt + ")",
        trc("project", "Raw files") + " (*.al,*.ul, ...) (" + rawFileExt + ")"
    };

    io::path_t defaultDir = configuration()->lastOpenedProjectsPath();

    if (defaultDir.empty()) {
        defaultDir = configuration()->userProjectsPath();
    }

    if (defaultDir.empty()) {
        defaultDir = configuration()->defaultUserProjectsPath();
    }

    io::paths_t filePaths = interactive()->selectOpeningFilesSync(muse::trc("project",
                                                                            "Open"), defaultDir, filter,
                                                                  QFileDialog::HideNameFilterDetails);

    if (!filePaths.empty()) {
        configuration()->setLastOpenedProjectsPath(io::dirpath(filePaths.front()));
    }

    return filePaths;
}

IInteractive::Button ProjectActionsController::askAboutSavingProject(IAudacityProjectPtr project)
{
    std::string title;

    if (project->isNewlyCreated()) {
        title = muse::qtrc("project", "Do you want to save changes to the project before closing?").toStdString();
    } else {
        title = muse::qtrc("project", "Do you want to save changes to the project “%1” before closing?")
                .arg(project->displayName()).toStdString();
    }

    std::string body = muse::trc("project", "Your changes will be lost if you don’t save them.");

    IInteractive::Result result = interactive()->warningSync(title, body, {
        IInteractive::Button::DontSave,
        IInteractive::Button::Cancel,
        IInteractive::Button::Save
    }, IInteractive::Button::Save, { IInteractive::Option::WithIcon },
                                                             muse::trc("project", "Unsaved changes"));

    return result.standardButton();
}

Ret ProjectActionsController::canSaveProject() const
{
    auto project = currentProject();
    if (!project) {
        LOGW() << "no current project";
        return make_ret(Err::NoProjectError);
    }

    return project->canSave();
}

bool ProjectActionsController::isQuickEditProject(const IAudacityProjectPtr& project) const
{
    return project && m_quickEditSourceFiles.find(project.get()) != m_quickEditSourceFiles.end();
}

std::string ProjectActionsController::formatNameForExtension(const std::string& extension) const
{
    std::string lowerExtension = extension;
    std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const std::string& format : exporter()->formatsList()) {
        const auto extensions = exporter()->formatExtensions(format);
        if (std::find(extensions.cbegin(), extensions.cend(), lowerExtension) != extensions.cend()) {
            return format;
        }
    }

    return std::string();
}

bool ProjectActionsController::exportQuickEditToSource(const IAudacityProjectPtr& project)
{
    const auto it = m_quickEditSourceFiles.find(project.get());
    if (it == m_quickEditSourceFiles.end()) {
        return false;
    }

    const muse::io::path_t sourcePath = it->second.path;
    const bool isNewFile = QFileInfo(sourcePath.toQString()).size() == 0;
    const std::string format = quickEditFormat(sourcePath);

    //! NOTE: MP2 / MP3 keep the source MPEG version and bitrate
    std::optional<MpegAudioInfo> mpegAudio;
    if (!format.empty()) {
        mpegAudio = mpegAudioInfo(sourcePath);
        if (mpegAudio && format != formatNameForExtension(mpegAudio->layer == 2 ? "mp2" : "mp3")) {
            mpegAudio.reset();
        }
    }

    if (format.empty()) {
        interactive()->error(muse::trc("project", "Export error"),
                             muse::mtrc("project", "Could not determine an export format for \"%1\".")
                             .arg(sourcePath.toString()).toStdString());
        return false;
    }

    //! NOTE: with a time selection, ask whether only the selection or the whole project replaces the source file
    importexport::ExportProcessType processType = importexport::ExportProcessType::FULL_PROJECT_AUDIO;
    if (!selectionController()->timeSelectionIsEmpty()) {
        const IInteractive::ButtonDatas buttons = {
            IInteractive::ButtonData(IInteractive::Button::Cancel, muse::trc("project", "Cancel")),
            IInteractive::ButtonData(BTN_QUICK_EDIT_SAVE_WHOLE_FILE, muse::trc("project", "Whole file")),
            IInteractive::ButtonData(BTN_QUICK_EDIT_SAVE_SELECTION, muse::trc("project", "Selection only"), true /*accent*/),
        };

        const IInteractive::Result result = interactive()->questionSync(
            muse::trc("project", "Save selection?"),
            muse::mtrc("project", "Part of the audio is selected. Replace \"%1\" with the selection only, or with the whole file?")
            .arg(io::filename(sourcePath).toString()).toStdString(),
            buttons,
            BTN_QUICK_EDIT_SAVE_SELECTION);

        if (result.button() == BTN_QUICK_EDIT_SAVE_SELECTION) {
            processType = importexport::ExportProcessType::SELECTED_AUDIO;
        } else if (result.button() != BTN_QUICK_EDIT_SAVE_WHOLE_FILE) {
            return false;
        }
    }

    //! NOTE: don't rely on the user's last export settings (mono, custom rate...):
    //! the source file must be rewritten with the same channel layout / rate
    bool stereo = false;
    uint64_t rate = 0;
    if (const auto trackeditProject = project->trackeditProject()) {
        for (const trackedit::Track& track : trackeditProject->trackList()) {
            if (track.type == trackedit::TrackType::Label) {
                continue;
            }
            stereo = stereo || track.type == trackedit::TrackType::Stereo;
            rate = std::max(rate, track.rate);
        }
    }

    importexport::IExporter::Options options;
    options[importexport::IExporter::OptionKey::Format] = muse::Val(format);
    options[importexport::IExporter::OptionKey::ProcessType] = muse::Val(processType);
    options[importexport::IExporter::OptionKey::ExportChannelsType]
        = muse::Val(static_cast<int>(stereo ? importexport::ExportChannelsPref::ExportChannels::STEREO
                                     : importexport::ExportChannelsPref::ExportChannels::MONO));
    options[importexport::IExporter::OptionKey::ExportChannels] = muse::Val(stereo ? 2 : 1);
    if (rate > 0) {
        options[importexport::IExporter::OptionKey::ExportSampleRate] = muse::Val(static_cast<int>(rate));
    }

    //! NOTE: keep the source bit depth / encoding when it can be read (WAV, AIFF, FLAC, MP2, MP3);
    //! other formats (MP3, OGG...) use the user's export preferences
    muse::ValList encodingParameters = mpegAudio ? mpegEncodingParameters(*mpegAudio) : sourceEncodingParameters(sourcePath);
    if (isNewFile) {
        if (const std::optional<RecordFormat> recordFormat = configuredRecordFormat()) {
            encodingParameters = recordFormat->parameters;
        }
    }
    if (!encodingParameters.empty()) {
        options[importexport::IExporter::OptionKey::Parameters] = muse::Val(encodingParameters);
    }

    //! NOTE: export to a temporary directory first (with the same file name), so that a failed export never
    //! corrupts the original and no extra file ever appears next to it; then rewrite the original in place
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        interactive()->error(muse::trc("project", "Export error"), tempDir.errorString().toStdString());
        return false;
    }

    const muse::io::path_t tempPath = muse::io::path_t(tempDir.filePath(io::filename(sourcePath).toQString()));

    const Ret ret = exporter()->exportData(tempPath, options, nullptr, project);
    if (!ret) {
        interactive()->error(muse::trc("project", "Export error"), ret.text());
        return false;
    }

    //! NOTE: back up the original file (before it is overwritten) and the project, in a dated directory
    purgeQuickEditBackups();

    const QString sourceFileName = io::filename(sourcePath).toQString();
    const QString sourceBaseName = QFileInfo(sourceFileName).completeBaseName();
    const QDir backupDir(QDir(quickEditBackupRoot()).filePath(
                             QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + "_" + sourceBaseName));

    //! NOTE: a new (empty) file has no original contents to back up
    const bool backupOriginal = !isNewFile;
    if (!QDir().mkpath(backupDir.path())
        || (backupOriginal && !QFile::copy(sourcePath.toQString(), backupDir.filePath(sourceFileName)))) {
        interactive()->error(muse::trc("project", "Export error"),
                             muse::mtrc("project", "Could not back up \"%1\" into \"%2\", the file was not saved.")
                             .arg(sourcePath.toString()).arg(muse::String::fromQString(backupDir.path())).toStdString());
        return false;
    }

    auto* au3Project = reinterpret_cast<AudacityProject*>(project->au3ProjectPtr());
    const QString projectBackupPath = backupDir.filePath(sourceBaseName + ".aup3");
    if (!au3Project || !ProjectFileIO::Get(*au3Project).SaveCopy(au::au3::wxFromString(muse::String::fromQString(projectBackupPath)))) {
        LOGW() << "could not back up the quick edit project to " << projectBackupPath;
    }

    //! NOTE: the application which launched the quick edit may keep the file open, allowing to replace it but not
    //! to write into it: then replace it with a copy of the export under the same name (via a file next to it)
    bool written = overwriteFileContents(tempPath.toQString(), sourcePath.toQString());
    if (!written) {
        LOGW() << "could not rewrite " << sourcePath.toQString() << " in place, replacing it";

        const QFileInfo sourceInfo(sourcePath.toQString());
        const muse::io::path_t siblingPath(sourceInfo.dir().filePath("." + sourceInfo.fileName() + ".quickedit"));
        QFile::remove(siblingPath.toQString());
        written = QFile::copy(tempPath.toQString(), siblingPath.toQString())
                  && fileSystem()->move(siblingPath, sourcePath, true /*replace*/);
        if (!written) {
            QFile::remove(siblingPath.toQString());
        }
    }

    if (!written) {
        //! NOTE: keep the exported file, the original may be partially written
        tempDir.setAutoRemove(false);
        interactive()->error(muse::trc("project", "Export error"),
                             muse::mtrc("project", "Could not write \"%1\". Check that it isn't read-only or locked by another "
                                                   "application. The exported audio was kept in \"%2\".")
                             .arg(sourcePath.toString()).arg(tempPath.toString()).toStdString());
        return false;
    }

    it->second.upToDate = true;

    const bool dismissable = true;
    toastService()->show(muse::trc("project", "Saved"),
                         muse::mtrc("project", "Changes exported back to \"%1\"").arg(sourcePath.toString()).toStdString(),
                         muse::ui::IconCode::Code::TICK,
                         dismissable, {});

    return true;
}

bool ProjectActionsController::saveProject(SaveMode saveMode, SaveLocationType saveLocationType, bool force)
{
    if (m_isProjectSaving) {
        return false;
    }

    m_isProjectSaving = true;
    DEFER {
        m_isProjectSaving = false;
    };

    IAudacityProjectPtr project = currentProject();

    if (saveMode == SaveMode::Save && isQuickEditProject(project)) {
        if (recordController()->isRecording()) {
            dispatcher()->dispatch(muse::actions::ActionQuery("action://record/stop"));
        }

        const bool exported = exportQuickEditToSource(project);

        //! NOTE: the application which launched the quick edit takes the file back once Audacity exits,
        //! so saving also closes Audacity (not when the save was asked by closing, which closes anyway)
        //! Closing the window like the user would: only this window when there are others (e.g. a quick edit
        //! handed to a running Audacity), else Audacity quits
        if (exported && !m_isProjectClosing) {
            muse::async::Async::call(this, [this]() {
                if (QWindow* window = mainWindow()->qWindow()) {
                    window->close();
                }
            });
        }

        return exported;
    }

    if (saveMode == SaveMode::Save && !project->isNewlyCreated() && saveLocationType == SaveLocationType::Undefined) {
        if (project->isCloudProject()) {
            return saveProjectAt(SaveLocation(SaveLocationType::Cloud, CloudProjectInfo { project->displayName() }));
        }

        return saveProjectAt(SaveLocation(SaveLocationType::Local));
    }

    //! TODO AU4
    RetVal<SaveLocation> response = openSaveProjectScenario()->askSaveLocation(project, saveMode, saveLocationType);
    if (!response.ret) {
        LOGE() << response.ret.toString();
        return false;
    }

    return saveProjectAt(response.val, saveMode, force);
}

bool ProjectActionsController::saveProjectAt(const SaveLocation& location, SaveMode saveMode, bool force)
{
    //! TODO AU4
    // if (!force) {
    //     Ret ret = canSaveProject();
    //     if (!ret) {
    //         ret = askIfUserAgreesToSaveProjectWithErrors(ret, location);
    //         if (!ret) {
    //             return ret;
    //         }
    //     }
    // }

    if (location.isLocal()) {
        return saveProjectLocally(location.localPath(), saveMode);
    }

    if (location.isCloud()) {
        const Ret ret = saveProjectToCloud(location.cloudInfo());
        if (!ret) {
            LOGE() << ret.toString();
        }
        return ret;
    }

    return false;
}

muse::Ret ProjectActionsController::openProject(const muse::io::path_t& path, const String& displayNameOverride,
                                                const String& projectId)
{
    //! NOTE This method is synchronous,
    //! but inside `multiwindowsProvider` there can be an event loop
    //! to wait for the responses from other instances, accordingly,
    //! the events (like user click) can be executed and this method can be called several times,
    //! before the end of the current call.
    //! So we ignore all subsequent calls until the current one completes.
    if (m_isProjectProcessing) {
        return make_ret(Ret::Code::InternalError);
    }
    m_isProjectProcessing = true;

    DEFER {
        m_isProjectProcessing = false;
    };

    //! Step 1. Take absolute path
    io::path_t actualPath = fileSystem()->absoluteFilePath(path);
    if (actualPath.empty()) {
        // We assume that a valid path has been specified to this method
        return make_ret(Ret::Code::UnknownError);
    }

    //! Step 2. If the project is already open in the current window, then just switch to showing the project
    if (isProjectOpened(actualPath)) {
        return openPageIfNeed(PROJECT_PAGE_URI);
    }

    //! Step 3. Check, if the project already opened in another window, then activate the window with the project
    if (multiwindowsProvider()->isProjectAlreadyOpened(actualPath)) {
        multiwindowsProvider()->activateWindowWithProject(actualPath);
        return make_ret(Ret::Code::Ok);
    }

    //! Step 4. Check, if a any project is already open in the current window,
    //! then create a new instance
    if (globalContext()->currentProject()) {
        QStringList args;
        args << actualPath.toQString();

        if (!displayNameOverride.isEmpty()) {
            args << "--project-display-name-override" << displayNameOverride;
        }
        multiwindowsProvider()->openNewWindow(args);
        return make_ret(Ret::Code::Ok);
    }

    //! Step 5. Check, if the project is a known cloud project, then open it through the cloud flow
    if (const auto record = cloudProjectsProvider()->projectRecordForPath(actualPath)) {
        return openCloudProject(actualPath, muse::String::fromStdString(record->projectId));
    }

    //! Step 6. Open project in the current window
    return doOpenProject(actualPath);
}

IAudacityProjectPtr ProjectActionsController::createProjectInCurrentWindow()
{
    IAudacityProjectPtr project = std::make_shared<Audacity4Project>(iocContext());
    Ret ret = project->createNew();
    if (!ret) {
        LOGE() << ret.toString();
        return nullptr;
    }

    globalContext()->setCurrentProject(project);
    projectHistory()->init();

    return project;
}

Ret ProjectActionsController::openCloudProject(const io::path_t& localPath, const String& projectId,
                                               const String& snapshotId, bool forceOverwrite)
{
    if (!audioComService()->enabled()) {
        LOGE() << "Cloud support is not available";
        return make_ret(Ret::Code::NotSupported);
    }

    if (std::holds_alternative<au::au3cloud::Authorizing>(authorization()->authState().val)) {
        dispatcher()->dispatch("open-url",
                               muse::actions::ActionData::make_arg1<QString>(cloudProjectOpenUrl(projectId, snapshotId)));
        return muse::make_ok();
    }

    if (!ensureAuthorization()) {
        return make_ret(Ret::Code::Cancel);
    }

    const std::string cloudProjectIdStr = projectId.toStdString();
    const std::string snapshotIdStr = snapshotId.toStdString();
    auto [openRet, progress] = audioComService()->openCloudProject(localPath, cloudProjectIdStr, snapshotIdStr, forceOverwrite);
    if (!openRet) {
        handleCloudOpenError(openRet, localPath, cloudProjectIdStr);
        return openRet;
    }

    if (!progress) {
        return make_ret(Ret::Code::UnknownError);
    }

    progress->finished().onReceive(this, [this, localPath, cloudProjectIdStr](const ProgressResult& result) {
        if (!result.ret) {
            handleCloudOpenError(result.ret, localPath, cloudProjectIdStr);
            return;
        }

        const io::path_t projectPath = !result.val.isNull() ? result.val.toPath() : localPath;
        doOpenProject(projectPath);

        auto project = globalContext()->currentProject();
        if (!project) {
            return;
        }

        if (!ensureAuthorization()) {
            return;
        }

        auto [syncRet, syncProgress] = audioComService()->resumeProjectSync(project);
        if (!syncRet || !syncProgress || syncProgress->isCanceled()) {
            return;
        }

        syncProgress->finished().onReceive(this, [this](const ProgressResult& result) {
            if (!result.ret.success()) {
                handleCloudSaveError(result.ret);
                return;
            }

            const bool dismissable = false;
            toastService()->show(trc("global", "Success"),
                                 trc("project",
                                     "All saved changes will now update to the cloud.\nYou can manage this file from your updated projects page on audio.com"),
                                 muse::ui::IconCode::Code::TICK,
                                 dismissable,
            {
                { trc("project", "Dismiss"), muse::toast::ToastActionCode::None },
                { trc("cloud", "View on audio.com"), muse::toast::ToastActionCode::Custom }
            }
                                 ).onResolve(this, [this, url = result.val.toQString()](muse::toast::ToastActionCode actionCode) {
                if (actionCode == muse::toast::ToastActionCode::Custom) {
                    platformInteractive()->openUrl(url);
                }
            });
        });

        const bool dismissible = false;
        const bool showProgressInfo = true;
        toastService()->showWithProgress(
            trc("project", "Resuming sync to audio.com…"),
            {},
            syncProgress,
            muse::ui::IconCode::Code::CLOUD,
            dismissible,
        {
            { trc("project", "Dismiss"), muse::toast::ToastActionCode::None },
            { trc("global", "Stop"), muse::toast::ToastActionCode::Custom }
        },
            showProgressInfo
            ).onResolve(this, [this, progress = syncProgress](const muse::toast::ToastActionCode& actionCode) {
            if (actionCode == muse::toast::ToastActionCode::Custom) {
                audioComService()->stopProjectSync();
                progress->cancel();
            }
        });
    });

    interactive()->showProgress(trc("project", "Syncing project from cloud…"), *progress);

    return make_ret(Ret::Code::Ok);
}

Ret ProjectActionsController::doOpenProject(const io::path_t& filePath)
{
    TRACEFUNC;

    RetVal<IAudacityProjectPtr> rv = loadProject(filePath);
    if (!rv.ret) {
        return rv.ret;
    }

    IAudacityProjectPtr project = rv.val;

    // Check if this is an autosave of a newly created project
    if (!project->isNewlyCreated() && !au::project::isAudacityUnsavedFile(project->path())) {
        recentFilesController()->prependRecentFile(makeRecentFile(project));
    }

    globalContext()->setCurrentProject(project);

#ifndef AU_LOAD_TIMETRACK
    const auto trackeditProject = project->trackeditProject();
    if (trackeditProject && trackeditProject->timeTrackFound()) {
        interactive()->infoSync(muse::trc("project/open", "Time Track not supported"),
                                muse::trc("project/open",
                                          "The project contains a time track, which is not yet supported in Audacity 4, and will need to be removed. This does not affect your original Audacity 3 project."),
        {
            muse::IInteractive::ButtonData(
                muse::IInteractive::Button::Ok, muse::trc("project/open", "OK"), false)
        });

        // When saving we do a full project rewrite
        // We need to save the project immediately to remove the time track from the project file and avoid showing this message repeatedly
        saveProject(SaveMode::Save);
    }
#endif

    projectHistory()->init();

    const Ret ret = openPageIfNeed(PROJECT_PAGE_URI);
    if (ret) {
        missingEffectChecker()->warnIfEffectsMissing();
    }

    return ret;
}

//! TODO AU4
// Ret ProjectActionsController::openAudacityUrl(const QUrl& url)
// {
//
//     if (url.host() == OPEN_PROJECT_URL_HOSTNAME) {
//         return openScoreFromMuseScoreCom(url);
//     }

//     return make_ret(Err::UnsupportedUrl);
// }

RetVal<IAudacityProjectPtr> ProjectActionsController::loadProject(const io::path_t& filePath)
{
    TRACEFUNC;

    //! TODO AU4
    // auto project = projectCreator()->newProject();
    // IF_ASSERT_FAILED(project) {
    //     return make_ret(Ret::Code::InternalError);
    // }
    IAudacityProjectPtr project = std::make_shared<Audacity4Project>(iocContext());

    //! TODO AU4
    // bool hasUnsavedChanges = project->hasUnsavedChanges();
    // io::path_t loadPath = hasUnsavedChanges ? project->autoSavePath(filePath) : filePath;

    const io::path_t loadPath = filePath;
    const std::string format = io::suffix(filePath);

    if (Ret result = loadWithFallback(project, loadPath, format); !result) {
        return result;
    }

    //! TODO AU4
    // if (hasUnsavedChanges) {
    //     //! NOTE: redirect the project to the original file path
    //     project->setPath(filePath);

    //     project->markAsUnsaved();
    // }

    // Mark project as newly created if it's an autosave of a new project
    if (project->isNewlyCreated()) {
        // Mark as newly created (this will be implemented if needed)
        // project->markAsNewlyCreated();
    }

    return RetVal<IAudacityProjectPtr>::make_ok(project);
}

Ret ProjectActionsController::loadWithFallback(const IAudacityProjectPtr& project,
                                               const muse::io::path_t& loadPath,
                                               const std::string& format)
{
    bool forceLoad = false;
    Ret result = project->load(loadPath, forceLoad, format);

    if (result || result.code() == static_cast<int>(Ret::Code::Cancel)) {
        return result;
    }

    forceLoad = shouldRetryLoadAfterError(result, loadPath);
    if (forceLoad) {
        result = project->load(loadPath, forceLoad, format);
    }

    return result;
}

bool ProjectActionsController::isProjectOpened(const muse::io::path_t& projectPath) const
{
    auto project = globalContext()->currentProject();
    if (!project) {
        return false;
    }

    LOGD() << "project->path: " << project->path() << ", check path: " << projectPath;
    if (project->path() == projectPath) {
        return true;
    }

    return false;
}

RecentFile ProjectActionsController::makeRecentFile(IAudacityProjectPtr project)
{
    RecentFile file;
    file.path = project->path();
    file.cloudRecord = project->cloudRecord();

    return file;
}

void ProjectActionsController::clearRecentProjects()
{
    recentFilesController()->clearRecentFiles();
}

bool ProjectActionsController::shouldRetryLoadAfterError(const Ret& ret, const muse::io::path_t& filepath)
{
    if (ret) {
        return true;
    }
    warnProjectCannotBeOpened(ret, filepath);
    return false;
}

void ProjectActionsController::warnProjectCannotBeOpened(const Ret& ret, const muse::io::path_t& filepath) const
{
    const std::string title
        = ret.data<std::string>("title",
                                muse::mtrc("project", "Cannot read file %1")
                                .arg(io::toNativeSeparators(filepath).toString())
                                .toStdString());

    const std::string body
        = ret.data<std::string>("body", !ret.text().empty() ? ret.text() : muse::trc("project",
                                                                                     "An error occurred while reading this file."));
    interactive()->error(title, body);
}

void ProjectActionsController::shareAudio()
{
    if (!audioComService()->enabled()) {
        LOGE() << "Cloud support is not available";
        return;
    }

    muse::UriQuery query(SAVE_TO_CLOUD_URI);
    query.addParam("formTitle", Val(trc("cloud", "Track title")));
    query.addParam("title", Val(trc("cloud", "Share audio")));
    query.addParam("actionText", Val(trc("cloud", "Share")));

    RetVal<Val> rv = interactive()->openSync(query);
    if (!rv.ret) {
        return;
    }

    std::string title = rv.val.toQString().toStdString();
    if (title.empty()) {
        return;
    }

    auto [shareRet, progress] = audioComService()->shareAudio(title);
    if (!shareRet || !progress) {
        return;
    }

    progress->finished().onReceive(this, [this](const ProgressResult& result) {
        if (result.ret.success()) {
            const bool dismissable = false;
            toastService()->show(trc("global", "Success"),
                                 trc("cloud", "Audio shared to audio.com"),
                                 muse::ui::IconCode::Code::TICK,
                                 dismissable,
            {
                { trc("global", "Dismiss"), muse::toast::ToastActionCode::None },
                { trc("cloud", "View on audio.com"), muse::toast::ToastActionCode::Custom }
            }
                                 ).onResolve(this, [this, url = result.val.toQString()](muse::toast::ToastActionCode actionCode) {
                if (actionCode == muse::toast::ToastActionCode::Custom) {
                    platformInteractive()->openUrl(url);
                }
            });
        } else {
            handleCloudSaveError(result.ret);
        }
    });

    const bool dismissable = false;
    const bool showProgressInfo = true;
    toastService()->showWithProgress(
        trc("cloud", "Sharing audio to audio.com…"),
        {},
        progress,
        muse::ui::IconCode::Code::SHARE_AUDIO,
        dismissable,
        {},
        showProgressInfo
        );
}

void ProjectActionsController::openCloudAudioFile(const muse::actions::ActionQuery& query)
{
    const auto audioId = query.param("audioId").toString();
    if (audioId.empty()) {
        return;
    }

    auto [downloadRet, progress] = audioComService()->downloadAudioFile(audioId);
    if (!downloadRet) {
        handleCloudAudioOpenError(downloadRet);
        return;
    }

    if (!progress) {
        return;
    }

    progress->finished().onReceive(this, [this](const ProgressResult& result) {
        if (!result.ret) {
            handleCloudAudioOpenError(result.ret);
            return;
        }

        const auto localPath = result.val.toQString();
        const auto project = globalContext()->currentProject();
        if (project) {
            QStringList args;
            args << "--session-type" << "start-with-new";
            args << "--import-media-file" << localPath;
            args << "--remove-media-after-import";
            multiwindowsProvider()->openNewWindow(args);
            return;
        }

        auto newproject = createProjectInCurrentWindow();
        if (!newproject) {
            return;
        }

        const auto importRet = newproject->import(muse::io::paths_t { localPath });
        fileSystem()->remove(localPath);
        if (!importRet) {
            LOGE() << importRet.toString();
            return;
        }

        openPageIfNeed(PROJECT_PAGE_URI);
    });

    interactive()->showProgress(muse::trc("cloud", "Downloading audio from cloud…"), *progress);
}

void ProjectActionsController::updateCloudAudioPreview(const muse::actions::ActionQuery& query)
{
    const std::string projectId = query.param("id").toString();

    auto project = currentProject();

    bool isCurrentProject = false;
    if (project) {
        isCurrentProject = projectId.empty()
                           ? project->isCloudProject()
                           : project->cloudRecord() && project->cloudRecord()->projectId == projectId;
    }

    if (!isCurrentProject) {
        if (projectId.empty()) {
            return;
        }

        std::optional<muse::io::path_t> localPath;
        if (const auto record = cloudProjectsProvider()->projectRecordForId(projectId); record&& !record->localPath.empty()) {
            localPath = record->localPath;
        }

        if (localPath && dispatchAudioPreviewToWindowWithProject(*localPath, projectId)) {
            return;
        }

        downloadCloudProject(projectId, localPath.value_or(muse::io::path_t {}), [this](IAudacityProjectPtr downloaded) {
            doUpdateCloudAudioPreview(downloaded, [downloaded]() { downloaded->close(); });
        });
        return;
    }

    if (project->hasUnsavedChanges()) {
        const IInteractive::Result result = interactive()->warningSync(
            trc("cloud", "The project must be saved before updating the audio preview"),
            trc("cloud", "Save your changes to continue, or cancel the update."),
            { IInteractive::Button::Cancel, IInteractive::Button::Save },
            IInteractive::Button::Save,
            { IInteractive::Option::WithIcon },
            trc("cloud", "Unsaved changes"));

        if (result.standardButton() != IInteractive::Button::Save) {
            return;
        }
    }

    saveProjectToCloud(CloudProjectInfo { project->displayName() }, CloudSaveMode::NormalUpdate, [this, project]() {
        doUpdateCloudAudioPreview(project);
    });
}

void ProjectActionsController::doUpdateCloudAudioPreview(const IAudacityProjectPtr& project, const std::function<void()>& onFinished)
{
    auto [ret, progress] = audioComService()->updateAudioPreview(project);
    if (!ret) {
        if (onFinished) {
            onFinished();
        }

        //: Title of an error dialog shown when generating the audio preview fails
        interactive()->error(trc("cloud", "Generate audio preview"), ret.text());
        return;
    }

    if (!progress) {
        if (onFinished) {
            onFinished();
        }

        return;
    }

    progress->finished().onReceive(this, [this, onFinished](const ProgressResult& result) {
        async::Async::call(this, [this, onFinished, result]() {
            if (onFinished) {
                onFinished();
            }

            if (result.ret.success()) {
                interactive()->info(trc("cloud", "Cloud audio preview updated"),
                                    trc("cloud", "The audio preview has been uploaded to audio.com"));
                return;
            }

            if (result.ret.code() == static_cast<int>(au::au3cloud::Err::AudioPreviewUpToDate)) {
                interactive()->info(trc("cloud", "Audio preview is up to date"),
                                    trc("cloud", "The audio preview already matches the latest saved version of this project."));
                return;
            }

            if (result.ret.code() != static_cast<int>(Ret::Code::Cancel)) {
                interactive()->error(trc("cloud", "Generate audio preview"), result.ret.text());
            }
        });
    });

    interactive()->showProgress(trc("cloud", "Updating cloud audio preview…"), *progress);
}

void ProjectActionsController::downloadCloudProject(const std::string& projectId, const muse::io::path_t& localPath,
                                                    std::function<void(IAudacityProjectPtr)> onSuccess)
{
    auto [ret, progress] = audioComService()->openCloudProject(localPath, projectId);
    if (!ret) {
        interactive()->error(trc("cloud", "Generate audio preview"), ret.text());
        return;
    }

    if (!progress) {
        return;
    }

    progress->finished().onReceive(this, [this, localPath, onSuccess](const ProgressResult& result) {
        async::Async::call(this, [this, localPath, onSuccess, result]() {
            if (!result.ret) {
                if (result.ret.code() != static_cast<int>(Ret::Code::Cancel)
                    && result.ret.code() != static_cast<int>(au::au3cloud::Err::OpenProjectCancelled)) {
                    interactive()->error(trc("cloud", "Generate audio preview"), result.ret.text());
                }
                return;
            }

            const muse::io::path_t projectPath = !result.val.isNull() ? result.val.toPath() : localPath;
            if (projectPath.empty()) {
                interactive()->error(trc("cloud", "Generate audio preview"),
                                     trc("cloud", "Could not determine the local path of the downloaded project"));
                return;
            }

            const RetVal<IAudacityProjectPtr> loaded = loadProject(projectPath);
            if (!loaded.ret) {
                interactive()->error(trc("cloud", "Generate audio preview"), loaded.ret.text());
                return;
            }

            if (onSuccess) {
                onSuccess(loaded.val);
            }
        });
    });

    interactive()->showProgress(trc("project", "Syncing project from cloud…"), *progress);
}

bool ProjectActionsController::dispatchAudioPreviewToWindowWithProject(const muse::io::path_t& projectPath, const std::string& projectId)
{
    for (const auto& ctx : application()->contexts()) {
        if (ctx == iocContext()) {
            continue;
        }

        auto ctxGlobalContext = muse::modularity::ioc(ctx)->resolve<au::context::IGlobalContext>("project");
        if (!ctxGlobalContext) {
            continue;
        }

        auto project = ctxGlobalContext->currentProject();
        if (!project || project->path() != projectPath) {
            continue;
        }

        auto ctxDispatcher = muse::modularity::ioc(ctx)->resolve<muse::actions::IActionsDispatcher>("project");
        IF_ASSERT_FAILED(ctxDispatcher) {
            return false;
        }

        if (auto window = muse::modularity::ioc(ctx)->resolve<muse::ui::IMainWindow>("project")) {
            window->requestShowOnFront();
        }

        muse::actions::ActionQuery action(UPDATE_AUDIO_PREVIEW_FOR_PROJECT_ACTION);
        action.addParam("id", Val(projectId));
        ctxDispatcher->dispatch(action);

        return true;
    }

    return false;
}

void ProjectActionsController::exportAudio()
{
    if (audioComService()->enabled() && exportConfiguration()->askExportLocationType()) {
        muse::UriQuery query(ASK_LOCATION_TYPE_URI);
        query.addParam("purpose", Val(std::string("export")));
        query.addParam("askAgain", Val(true));

        RetVal<Val> rv = interactive()->openSync(query);
        if (!rv.ret) {
            return;
        }

        QVariantMap vals = rv.val.toQVariant().toMap();
        exportConfiguration()->setAskExportLocationType(vals["askAgain"].toBool());

        if (static_cast<SaveLocationType>(vals["locationType"].toInt()) == SaveLocationType::Cloud) {
            shareAudio();
            return;
        }
    }

    interactive()->open(EXPORT_URI);
}

void ProjectActionsController::exportLabels(const actions::ActionData& args)
{
    muse::UriQuery query(EXPORT_LABELS_URI);

    trackedit::TrackId trackId = args.count() == 1 ? args.arg<trackedit::LabelKey>(0).trackId : -1;
    query.addParam("trackId", Val(trackId));

    interactive()->open(query);
}

void ProjectActionsController::exportMIDI()
{
    NOT_IMPLEMENTED;
}

void ProjectActionsController::undo()
{
    NOT_IMPLEMENTED;
}

void ProjectActionsController::redo()
{
    NOT_IMPLEMENTED;
}

muse::Ret ProjectActionsController::openPageIfNeed(muse::Uri pageUri)
{
    if (interactive()->isOpened(pageUri).val) {
        return muse::make_ret(muse::Ret::Code::Ok);
    }

    interactive()->open(pageUri);
    return muse::make_ok();
}

void ProjectActionsController::openCustomFFmpegOptions()
{
    interactive()->open(CUSTOM_FFMPEG_OPTIONS);
}

void ProjectActionsController::openMetadataDialog()
{
    interactive()->open(METADATA_DIALOG_URI);
}

void ProjectActionsController::openCustomMapping()
{
    interactive()->open(CUSTOM_MAPPING);
}

muse::Ret ProjectActionsController::ensureAuthorization()
{
    if (authorization()->isAuthorized()) {
        return make_ret(Ret::Code::Ok);
    }

    muse::actions::ActionQuery query("audacity://cloud/open-signin-dialog");
    query.addParam("sync", muse::Val(true));

    dispatcher()->dispatch(query);

    return authorization()->isAuthorized() ? make_ret(Ret::Code::Ok) : make_ret(Ret::Code::Cancel);
}

void ProjectActionsController::handleCloudOpenError(const muse::Ret& error, const io::path_t& localPath,
                                                    const std::string& cloudProjectId)
{
    const auto ret = openSaveProjectScenario()->showCloudOpenError(error, localPath);

    switch (ret.code()) {
    case IOpenSaveProjectScenario::RET_CODE_OPEN_LOCAL:
        doOpenProject(localPath);
        break;
    case IOpenSaveProjectScenario::RET_CODE_SAVE_LOCALLY_AND_REMOVE_CACHE: {
        const auto openRet = doOpenProject(localPath);
        if (!openRet) {
            LOGE() << openRet.toString();
            break;
        }

        IAudacityProjectPtr project = currentProject();
        if (!project) {
            break;
        }

        const auto askRet = openSaveProjectScenario()->askLocalPath(project, SaveMode::Save);
        if (!askRet.ret || askRet.val.empty()) {
            break;
        }

        const auto deleteRet = audioComService()->deleteCloudProject(localPath);
        if (!deleteRet) {
            LOGW() << deleteRet.toString();
        }

        const auto newPath = askRet.val;
        if (!saveProjectLocally(newPath, SaveMode::Save)) {
            break;
        }

        if (newPath != localPath) {
            const auto removeRet = fileSystem()->remove(localPath);
            if (!removeRet) {
                LOGW() << removeRet.toString();
            }
        }
        break;
    }
    case IOpenSaveProjectScenario::RET_CODE_SAVE_TO_CLOUD: {
        const auto openRet = doOpenProject(localPath);
        if (!openRet) {
            LOGE() << openRet.toString();
            break;
        }

        IAudacityProjectPtr project = currentProject();
        if (!project) {
            break;
        }

        const auto deleteRet = audioComService()->deleteCloudProject(localPath);
        if (!deleteRet) {
            LOGW() << deleteRet.toString();
        }
        const auto saveRet = saveProjectToCloud(CloudProjectInfo { project->displayName() }, CloudSaveMode::CreateNew);
        if (!saveRet) {
            LOGE() << saveRet.toString();
        }
        break;
    }
    case IOpenSaveProjectScenario::RET_CODE_OPEN_CLOUD_FORCE:
        openCloudProject(localPath, {}, {}, true);
        break;
    case IOpenSaveProjectScenario::RET_CODE_LOAD_LATEST_SYNCED:
        openCloudProject(localPath, muse::String::fromStdString(cloudProjectId), {}, true);
        break;
    case IOpenSaveProjectScenario::RET_CODE_OPEN_ON_AUDIOCOM:
        if (!cloudProjectId.empty()) {
            platformInteractive()->openUrl(audioComService()->getCloudProjectPage(cloudProjectId));
        }
        break;
    default:
        break;
    }
}

void ProjectActionsController::handleCloudSaveError(const muse::Ret& error)
{
    IAudacityProjectPtr project = currentProject();
    if (!project) {
        return;
    }

    const auto ret = openSaveProjectScenario()->showCloudSaveError(error);

    switch (ret.code()) {
    case IOpenSaveProjectScenario::RET_CODE_SAVE_LOCALLY: {
        const auto askRet = openSaveProjectScenario()->askLocalPath(project, SaveMode::Save);
        if (!askRet.ret || askRet.val.empty()) {
            break;
        }
        saveProjectLocally(askRet.val, SaveMode::Save);
        break;
    }
    case IOpenSaveProjectScenario::RET_CODE_SAVE_LOCALLY_AND_REMOVE_CACHE: {
        const auto oldPath = project->path();
        const auto askRet = openSaveProjectScenario()->askLocalPath(project, SaveMode::Save);
        if (!askRet.ret || askRet.val.empty()) {
            break;
        }

        const auto deleteRet = audioComService()->deleteCloudProject(oldPath);
        if (!deleteRet) {
            LOGW() << deleteRet.toString();
        }

        const auto newPath = askRet.val;
        if (!saveProjectLocally(newPath, SaveMode::Save)) {
            break;
        }

        if (newPath != oldPath) {
            const auto removeRet = fileSystem()->remove(oldPath);
            if (!removeRet) {
                LOGW() << removeRet.toString();
            }
        }
        break;
    }
    case IOpenSaveProjectScenario::RET_CODE_SAVE_TO_CLOUD: {
        const auto deleteRet = audioComService()->deleteCloudProject(project->path());
        if (!deleteRet) {
            LOGW() << deleteRet.toString();
        }
        const auto saveRet = saveProjectToCloud(CloudProjectInfo { project->displayName() }, CloudSaveMode::CreateNew);
        if (!saveRet) {
            LOGE() << saveRet.toString();
        }
    }
    break;
    case IOpenSaveProjectScenario::RET_CODE_SAVE_TO_CLOUD_FORCE: {
        const auto saveRet = saveProjectToCloud(CloudProjectInfo { project->displayName() }, CloudSaveMode::ForceOverwrite);
        if (!saveRet) {
            LOGE() << saveRet.toString();
        }
        break;
    }
    case IOpenSaveProjectScenario::RET_CODE_CLOSE_AND_OPEN_CLOUD_FORCE: {
        const io::path_t localPath = project->path();
        closeOpenedProject(false);
        openCloudProject(localPath, {}, {}, true);
        break;
    }
    default:
        break;
    }
}

void ProjectActionsController::handleCloudAudioOpenError(const muse::Ret& error)
{
    openSaveProjectScenario()->showCloudAudioOpenError(error);
}
