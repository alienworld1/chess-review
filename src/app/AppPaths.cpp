#include "app/AppPaths.h"

#include <QDir>
#include <QStandardPaths>

namespace chess_review::app {

AppPaths AppPaths::resolve() {
    return {QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation)};
}

QString AppPaths::databasePath() const {
    return dataDirectory.isEmpty()
               ? QString{}
               : QDir(dataDirectory).filePath(QStringLiteral("chess-review.sqlite3"));
}

QString AppPaths::configFilePath() const {
    return configDirectory.isEmpty()
               ? QString{}
               : QDir(configDirectory).filePath(QStringLiteral("config.toml"));
}

} // namespace chess_review::app
