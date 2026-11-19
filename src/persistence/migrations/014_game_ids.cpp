#include "persistence/private/Migrations.h"

namespace chess_review::persistence::detail {
Migration gameIdsMigration() {
    return {14,
            QStringLiteral("monotonic_game_ids"),
            {QStringLiteral("CREATE TABLE game_id_sequence (id INTEGER PRIMARY KEY CHECK(id=1), "
                            "last_id INTEGER NOT NULL CHECK(last_id>=0))"),
             QStringLiteral("INSERT INTO game_id_sequence SELECT 1,max(0,coalesce(max(id),0)) "
                            "FROM games"),
             QStringLiteral("CREATE TRIGGER track_game_id AFTER INSERT ON games BEGIN "
                            "UPDATE game_id_sequence SET last_id=max(last_id,NEW.id) WHERE id=1; "
                            "END"),
             QStringLiteral("CREATE INDEX puzzles_game ON puzzles(source_game_id)")}};
}
} // namespace chess_review::persistence::detail
