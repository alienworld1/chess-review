#include "persistence/private/Migrations.h"

namespace chess_review::persistence::detail {
Migration openingsMigration() {
    return {12,
            QStringLiteral("game_openings"),
            {QStringLiteral("CREATE TABLE game_openings ("
                            "game_id INTEGER PRIMARY KEY REFERENCES games(id) ON DELETE CASCADE, "
                            "dataset_version TEXT NOT NULL, policy_version INTEGER NOT NULL, "
                            "eco TEXT, name TEXT, CHECK ((eco IS NULL) = (name IS NULL)))")}};
}
} // namespace chess_review::persistence::detail
