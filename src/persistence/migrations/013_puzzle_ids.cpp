#include "persistence/private/Migrations.h"

namespace chess_review::persistence::detail {
Migration puzzleIdsMigration() {
    return {13,
            QStringLiteral("stable_puzzle_ids"),
            {QStringLiteral("ALTER TABLE puzzles RENAME TO puzzles_previous"),
             QStringLiteral("CREATE TABLE puzzles (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                            "position_key BLOB NOT NULL, position_identity BLOB NOT NULL UNIQUE, "
                            "source_game_id INTEGER REFERENCES games(id) ON DELETE SET NULL, "
                            "source_review_id INTEGER REFERENCES reviews(id) ON DELETE SET NULL, "
                            "origin INTEGER NOT NULL CHECK(origin IN (0,1)), "
                            "document BLOB NOT NULL, created_at TEXT NOT NULL)"),
             QStringLiteral("INSERT INTO puzzles SELECT id,position_key,position_identity,"
                            "source_game_id,source_review_id,origin,document,created_at "
                            "FROM puzzles_previous"),
             QStringLiteral("DROP TABLE puzzles_previous"),
             QStringLiteral("CREATE INDEX puzzles_position_key ON puzzles(position_key)"),
             QStringLiteral("CREATE INDEX puzzles_review ON puzzles(source_review_id,id)")}};
}
} // namespace chess_review::persistence::detail
