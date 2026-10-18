#include "app/AppPaths.h"

#include <QDir>
#include <QStandardPaths>

namespace chess_review::app {
namespace {

QString filePath(const QString& directory, const QString& name) {
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null)) {
        return {};
    }
    return QDir(directory).filePath(name);
}

} // namespace

AppPaths AppPaths::resolve() {
    return {QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation)};
}

QString AppPaths::databasePath() const {
    return filePath(dataDirectory, QStringLiteral("chess-review.sqlite3"));
}

QString AppPaths::configFilePath() const {
    return filePath(configDirectory, QStringLiteral("config.toml"));
}

QString AppPaths::logDirectory() const { return filePath(cacheDirectory, QStringLiteral("logs")); }

} // namespace chess_review::app
