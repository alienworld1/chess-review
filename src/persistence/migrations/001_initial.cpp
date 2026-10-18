#include "persistence/private/Migrations.h"

namespace chess_review::persistence::detail {

Migration initialMigration() {
    return {1,
            QStringLiteral("initial"),
            {QStringLiteral("CREATE TABLE schema_migrations ("
                            "version INTEGER PRIMARY KEY CHECK (version > 0), "
                            "name TEXT NOT NULL, "
                            "applied_at TEXT NOT NULL)")}};
}

} // namespace chess_review::persistence::detail
