#pragma once

#include "persistence/Database.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <span>
#include <vector>

class QSqlDatabase;

namespace chess_review::persistence::detail {

struct Migration {
    int version;
    QString name;
    QStringList statements;
};

using Progress = std::function<void(const QString&)>;

Migration initialMigration();
const std::vector<Migration>& migrations();

// Internal seam for migration fixtures; never exposed through Database.
std::optional<DatabaseError> migrate(QSqlDatabase& database, const QString& path,
                                     std::span<const Migration> registry, const Progress& progress);

} // namespace chess_review::persistence::detail
