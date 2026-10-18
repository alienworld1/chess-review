#pragma once

#include "persistence/private/Migrations.h"

#include <variant>

namespace chess_review::persistence::detail {

// Reads bounded metadata without changing the database. Zero denotes an empty database.
std::variant<int, DatabaseError> readSchemaVersion(QSqlDatabase& database, const QString& path,
                                                   std::span<const Migration> registry);

} // namespace chess_review::persistence::detail
