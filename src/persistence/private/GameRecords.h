#pragma once
#include "persistence/GameRepository.h"
#include <QByteArray>
class QSqlDatabase;
namespace chess_review::persistence::detail {
// Bind these bytes through CAST(? AS TEXT), avoiding SQLite text16 BOM stripping.
QByteArray utf8TextBytes(const QString& value);
// One bounded, validating reader shared by runtime loads and version migrations.
LoadGameResult readGame(QSqlDatabase& database, GameId id);
} // namespace chess_review::persistence::detail
