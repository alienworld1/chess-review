#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration reviewWorkspaceMigration() {
    return {
        9,
        QStringLiteral("review_workspace"),
        {QStringLiteral("CREATE TABLE review_workspace (id INTEGER PRIMARY KEY CHECK(id=1), "
                        "format_version INTEGER NOT NULL CHECK(format_version=1), game_id INTEGER "
                        "REFERENCES games(id), review_id INTEGER REFERENCES reviews(id), reopen "
                        "INTEGER NOT NULL CHECK(reopen IN (0,1)))"),
         QStringLiteral("CREATE TABLE review_selection (game_id INTEGER PRIMARY KEY REFERENCES "
                        "games(id), review_id INTEGER NOT NULL REFERENCES reviews(id))"),
         QStringLiteral("CREATE TABLE review_moments (review_id INTEGER PRIMARY KEY REFERENCES "
                        "reviews(id), position_index INTEGER NOT NULL CHECK(position_index>=0))")}};
}
} // namespace chess_review::persistence::detail
