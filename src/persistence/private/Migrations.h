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
    // Optional bounded row transform, within the migration's existing transaction/backup.
    std::function<std::optional<QString>(QSqlDatabase&)> transform = {};
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

Migration reviewWorkspaceMigration();
Migration refinementMigration();
Migration classificationsMigration();
Migration reviewsMigration();
Migration analysisMigration();
Migration enginesMigration();
Migration initialMigration();
Migration gamesMigration();
Migration gameTextMigration();
const std::vector<Migration>& migrations();

// Internal seam for migration fixtures; never exposed through Database.
std::optional<DatabaseError> migrate(QSqlDatabase& database, const QString& path,
                                     std::span<const Migration> registry, const Progress& progress,
                                     const Observe& observe = {});

} // namespace chess_review::persistence::detail
