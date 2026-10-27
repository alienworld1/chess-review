#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration reviewsMigration() {
    return {
        6,
        QStringLiteral("durable_reviews"),
        {QStringLiteral(
             "CREATE TABLE reviews (id INTEGER PRIMARY KEY AUTOINCREMENT, game_id INTEGER NOT NULL "
             "REFERENCES games(id), engine_profile_id INTEGER NOT NULL REFERENCES engines(id), "
             "selected_side INTEGER NOT NULL CHECK(selected_side IN (0,1)), quality INTEGER NOT "
             "NULL CHECK(quality BETWEEN 0 AND 2), mainline_identity BLOB NOT NULL, settings BLOB "
             "NOT NULL, format_version INTEGER NOT NULL, created_at INTEGER NOT NULL)"),
         QStringLiteral("CREATE INDEX reviews_game ON reviews(game_id,id DESC)"),
         QStringLiteral(
             "CREATE TABLE review_jobs (id INTEGER PRIMARY KEY AUTOINCREMENT, review_id INTEGER "
             "NOT NULL UNIQUE REFERENCES reviews(id), status INTEGER NOT NULL CHECK(status BETWEEN "
             "0 AND 9), revision INTEGER NOT NULL CHECK(revision>=0), total_positions INTEGER NOT "
             "NULL CHECK(total_positions>0), completed_positions INTEGER NOT NULL "
             "CHECK(completed_positions BETWEEN 0 AND total_positions), updated_at INTEGER NOT "
             "NULL)"),
         QStringLiteral("CREATE TABLE review_evidence (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "review_id INTEGER NOT NULL REFERENCES reviews(id), position_index INTEGER "
                        "NOT NULL CHECK(position_index>=0), session BLOB, attempt BLOB, analysis "
                        "BLOB, adapted BLOB NOT NULL, format_version INTEGER NOT NULL, "
                        "UNIQUE(review_id,session,attempt), UNIQUE(review_id,id))"),
         QStringLiteral(
             "CREATE TABLE review_positions (review_id INTEGER NOT NULL REFERENCES reviews(id), "
             "position_index INTEGER NOT NULL CHECK(position_index>=0), evidence_id INTEGER NOT "
             "NULL, PRIMARY KEY(review_id,position_index), FOREIGN KEY(review_id,evidence_id) "
             "REFERENCES review_evidence(review_id,id))"),
         QStringLiteral(
             "CREATE TABLE review_pairs (review_id INTEGER NOT NULL REFERENCES reviews(id), "
             "position_index INTEGER NOT NULL CHECK(position_index>0), before_id INTEGER NOT NULL, "
             "after_id INTEGER NOT NULL, paired BLOB NOT NULL, PRIMARY "
             "KEY(review_id,position_index), FOREIGN KEY(review_id,before_id) REFERENCES "
             "review_evidence(review_id,id), FOREIGN KEY(review_id,after_id) REFERENCES "
             "review_evidence(review_id,id))"),
         QStringLiteral(
             "CREATE TABLE review_transitions (review_id INTEGER NOT NULL REFERENCES reviews(id), "
             "revision INTEGER NOT NULL CHECK(revision>=0), previous INTEGER CHECK(previous "
             "BETWEEN 0 AND 9), status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 9), failure "
             "INTEGER NOT NULL CHECK(failure BETWEEN 0 AND 4), created_at INTEGER NOT NULL, "
             "PRIMARY KEY(review_id,revision))")}};
}
} // namespace chess_review::persistence::detail
