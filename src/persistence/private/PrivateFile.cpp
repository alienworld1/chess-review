#include "persistence/private/PrivateFile.h"

#include <QFile>
#include <QIODevice>

namespace chess_review::persistence::detail {

std::optional<QString> createPrivateFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly,
                   QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return file.errorString();
    }
    return std::nullopt;
}

std::optional<QString> restrictFilePermissions(const QString& path) {
    QFile file(path);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return file.errorString();
    }
    return std::nullopt;
}

} // namespace chess_review::persistence::detail
