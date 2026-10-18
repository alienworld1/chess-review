#pragma once

#include <QString>

namespace chess_review::app {

struct AppPaths {
    QString dataDirectory;
    QString configDirectory;
    QString cacheDirectory;

    // Set the application identity before resolving. Lookup has no filesystem side effects.
    static AppPaths resolve();
    QString databasePath() const;
    QString configFilePath() const;
};

} // namespace chess_review::app
