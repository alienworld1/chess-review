#pragma once

#include <QString>

#include <optional>

namespace chess_review::persistence::detail {

// Exclusively reserve a new, empty file before SQLite writes any private data into it.
std::optional<QString> createPrivateFile(const QString& path);

// Tighten a recognized database or its existing SQLite sidecars before further writes.
std::optional<QString> restrictFilePermissions(const QString& path);

} // namespace chess_review::persistence::detail
