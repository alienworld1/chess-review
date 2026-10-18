#pragma once

#include "persistence/Database.h"
#include "persistence/private/Migrations.h"

#include <QSqlDatabase>

#include <memory>
#include <optional>

class QLockFile;

namespace chess_review::persistence::detail {

// Construct, initialize, and destroy on one storage thread. Owns the write connection.
class StorageSession final {
  public:
    StorageSession();
    ~StorageSession();
    StorageSession(const StorageSession&) = delete;
    StorageSession& operator=(const StorageSession&) = delete;

    std::optional<DatabaseError> initialize(const QString& path,
                                            std::span<const Migration> registry,
                                            const Progress& progress, const Observe& observe = {});

  private:
    QString connectionName_;
    QSqlDatabase database_;
    std::unique_ptr<QLockFile> lock_;
};

} // namespace chess_review::persistence::detail
