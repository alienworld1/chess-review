#include "persistence/private/GameCodec.h"
#include "persistence/private/GameRecords.h"
#include "persistence/private/Migrations.h"
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {
std::optional<QString> upgrade(QSqlDatabase& database) {
    QSqlQuery rows(database);
    rows.setForwardOnly(true);
    if (!rows.exec(QStringLiteral("SELECT id FROM games ORDER BY id"))) {
        return QStringLiteral("Cannot enumerate existing game documents.");
    }
    QSqlQuery update(database);
    if (!update.prepare(QStringLiteral(
            "UPDATE games_next SET "
            "identity_version=2,format_version=2,identity=?,content_hash=?,snapshot=?,white_name="
            "CAST(? AS TEXT),black_name=CAST(? AS TEXT),result=CAST(? AS TEXT),played_at=CAST(? AS "
            "TEXT),time_control=CAST(? AS TEXT) WHERE id=?"))) {
        return QStringLiteral("Cannot prepare game document upgrade.");
    }
    while (rows.next()) {
        auto decoded = readGame(database, rows.value(0).toLongLong());
        const auto* game = std::get_if<StoredGame>(&decoded);
        if (!game) {
            const auto* error = std::get_if<GameError>(&decoded);
            if (error && (error->code == GameErrorCode::InvalidSnapshot ||
                          error->code == GameErrorCode::UnsupportedVersion)) {
                continue; // Preserve damaged records exactly; do not silently repair them.
            }
            return QStringLiteral("Cannot validate an existing game document.");
        }
        auto encoded = encodeGame(*game->document);
        const auto* corrected = std::get_if<EncodedGame>(&encoded);
        if (!corrected) {
            return QStringLiteral("Cannot encode an existing game document.");
        }
        const auto metadata = gameMetadata(*game->document);
        update.bindValue(0, corrected->identity);
        update.bindValue(1, corrected->hash);
        update.bindValue(2, corrected->snapshot);
        update.bindValue(3, utf8TextBytes(metadata.white));
        update.bindValue(4, utf8TextBytes(metadata.black));
        update.bindValue(5, utf8TextBytes(metadata.result));
        update.bindValue(6, utf8TextBytes(metadata.playedAt));
        update.bindValue(7, utf8TextBytes(metadata.timeControl));
        update.bindValue(8, rows.value(0));
        if (!update.exec() || update.numRowsAffected() != 1) {
            return QStringLiteral("Cannot upgrade an existing game document.");
        }
        update.finish();
    }
    if (rows.lastError().isValid()) {
        return QStringLiteral("Cannot finish reading existing games.");
    }
    rows.finish();
    QSqlQuery finish(database);
    for (const auto& statement :
         {QStringLiteral("DROP TABLE games"),
          QStringLiteral("ALTER TABLE games_next RENAME TO games"),
          QStringLiteral("CREATE INDEX games_imported_at ON games(imported_at DESC,id DESC)")}) {
        if (!finish.exec(statement)) {
            return QStringLiteral("Cannot finalize game document upgrade.");
        }
        finish.finish();
    }
    return std::nullopt;
}
} // namespace
Migration gameTextMigration() {
    return {3,
            QStringLiteral("game_text"),
            {QStringLiteral(
                 "CREATE TABLE games_next (id INTEGER PRIMARY KEY, identity_version INTEGER NOT "
                 "NULL CHECK(identity_version IN (1,2)), content_hash BLOB NOT NULL UNIQUE "
                 "CHECK(length(content_hash)=32), identity BLOB NOT NULL, original_pgn BLOB NOT "
                 "NULL, snapshot BLOB NOT NULL, format_version INTEGER NOT NULL, white_name TEXT "
                 "NOT NULL, black_name TEXT NOT NULL, result TEXT NOT NULL, played_at TEXT NOT "
                 "NULL, time_control TEXT NOT NULL, source_type TEXT NOT NULL, source_uri TEXT NOT "
                 "NULL, imported_at TEXT NOT NULL)"),
             QStringLiteral("INSERT INTO games_next SELECT * FROM games")},
            upgrade};
}
} // namespace chess_review::persistence::detail
