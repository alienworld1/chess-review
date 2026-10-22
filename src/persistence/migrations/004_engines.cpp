#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration enginesMigration() {
    return {
        4,
        QStringLiteral("engines"),
        {QStringLiteral(
             "CREATE TABLE engines (id INTEGER PRIMARY KEY, display_name TEXT NOT NULL, "
             "executable_path TEXT NOT NULL UNIQUE, executable_sha256 BLOB NOT NULL UNIQUE "
             "CHECK(length(executable_sha256)=32), reported_name TEXT NOT NULL, reported_author "
             "TEXT, detected_version TEXT, capabilities_json BLOB NOT NULL, option_overrides_json "
             "BLOB NOT NULL, is_default INTEGER NOT NULL CHECK(is_default IN (0,1)), created_at "
             "TEXT NOT NULL, last_validated_at TEXT NOT NULL)"),
         QStringLiteral(
             "CREATE UNIQUE INDEX engines_one_default ON engines(is_default) WHERE is_default=1")}};
}
} // namespace chess_review::persistence::detail
