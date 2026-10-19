#pragma once

#include <QString>

namespace chess_review::app {

struct AppPaths {
    QString dataDirectory;
    QString configDirectory;
    QString cacheDirectory;

    // Set application identity first and keep it stable for the process lifetime.
    // This snapshot only selects locations; it does not establish existence, access,
    // filesystem suitability, or separation between user-selected XDG roots.
    static AppPaths resolve();
    // Empty means the directory is unavailable/invalid, never the working directory.
    // Writers must handle that case and report filesystem failures at the write boundary.
    QString databasePath() const;
    QString configFilePath() const;
    QString logDirectory() const;
    QString uiStateFilePath() const;
};

} // namespace chess_review::app
