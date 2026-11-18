#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration puzzlesMigration() {
    return {
        11,
        QStringLiteral("verified_puzzles"),
        {QStringLiteral("CREATE TABLE puzzles (id INTEGER PRIMARY KEY, position_key BLOB NOT NULL, "
                        "position_identity BLOB NOT NULL UNIQUE, source_game_id INTEGER REFERENCES "
                        "games(id) ON DELETE SET NULL, "
                        "source_review_id INTEGER REFERENCES reviews(id) ON DELETE SET NULL, "
                        "origin INTEGER NOT NULL CHECK(origin IN (0,1)), "
                        "document BLOB NOT NULL, created_at TEXT NOT NULL)"),
         QStringLiteral("CREATE INDEX puzzles_position_key ON puzzles(position_key)"),
         QStringLiteral("CREATE INDEX puzzles_review ON puzzles(source_review_id,id)"),
         QStringLiteral("CREATE TABLE puzzle_jobs (id INTEGER PRIMARY KEY, source_review_id "
                        "INTEGER UNIQUE REFERENCES reviews(id) ON DELETE SET NULL, "
                        "revision INTEGER NOT NULL CHECK(revision>=0), status INTEGER NOT NULL "
                        "CHECK(status BETWEEN 0 AND 4), "
                        "current_position INTEGER NOT NULL, candidate_count INTEGER NOT NULL, "
                        "saved_count INTEGER NOT NULL, document BLOB NOT NULL)"),
         QStringLiteral("CREATE TABLE puzzle_suppressions (position_key BLOB NOT NULL, "
                        "position_identity BLOB NOT NULL PRIMARY KEY)"),
         QStringLiteral(
             "CREATE INDEX puzzle_suppressions_key ON puzzle_suppressions(position_key)")}};
}
} // namespace chess_review::persistence::detail
