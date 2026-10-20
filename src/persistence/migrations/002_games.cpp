#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration gamesMigration() {
    return {
        2,
        QStringLiteral("games"),
        {QStringLiteral("CREATE TABLE games (id INTEGER PRIMARY KEY, identity_version INTEGER NOT "
                        "NULL CHECK(identity_version=1), content_hash BLOB NOT NULL UNIQUE "
                        "CHECK(length(content_hash)=32), identity BLOB NOT NULL, original_pgn BLOB "
                        "NOT NULL, snapshot BLOB NOT NULL, format_version INTEGER NOT NULL, "
                        "white_name TEXT NOT NULL, black_name TEXT NOT NULL, result TEXT NOT NULL, "
                        "played_at TEXT NOT NULL, time_control TEXT NOT NULL, source_type TEXT NOT "
                        "NULL, source_uri TEXT NOT NULL, imported_at TEXT NOT NULL)"),
         QStringLiteral("CREATE INDEX games_imported_at ON games(imported_at DESC, id DESC)")}};
}
} // namespace chess_review::persistence::detail
