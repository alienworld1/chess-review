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

enum class StorageEvent {
    SchemaChecked,
    BackupStarted,
    BackupVerified,
    MigrationStarted,
    MigrationCompleted
};
using Observe = std::function<void(StorageEvent, int)>;

using Progress = std::function<void(const QString&)>;

Migration initialMigration();
Migration gamesMigration();
const std::vector<Migration>& migrations();

// Internal seam for migration fixtures; never exposed through Database.
std::optional<DatabaseError> migrate(QSqlDatabase& database, const QString& path,
                                     std::span<const Migration> registry, const Progress& progress,
                                     const Observe& observe = {});

} // namespace chess_review::persistence::detail
