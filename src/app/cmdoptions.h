/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <optional>
#include <string>

#include <QString>
#include <QUrl>

#include "global/io/path.h"
#include "global/logger.h"
#include "global/internal/cmdoptions.h"

namespace au::app {
struct AudacityCmdOptions : public muse::CmdOptions {
    struct {
        std::optional<bool> revertToFactorySettings;
        bool memoryLeakReport = false;
        bool version = false;
        bool longVersion = false;
    } app;

    struct {
        std::optional<std::string> type;
        std::optional<QUrl> projectUrl;
        std::optional<QString> projectDisplayNameOverride;
        std::optional<QString> startupUrl;
        muse::io::paths_t mediaFiles;
        bool removeMediaFilesAfterImport = false;
        //! NOTE: like Adobe Audition's external-editor "%F" workflow: import the file for editing
        //! and export changes back to the same path/format instead of prompting to save a project
        bool quickEdit = false;
    } startup;

    struct Testflow {
        QString testCaseNameOrFile;
        QString testCaseContextNameOrFile;
        QString testCaseContextValue;
        QString testCaseFunc;
        QString testCaseFuncArgs;
    } testflow;

    struct AudioPluginRegistration {
        muse::io::path_t pluginPath;
        muse::io::path_t outputPath;
        bool selfTest = false;
    } audioPluginRegistration;
};
}
