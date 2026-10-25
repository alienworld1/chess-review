#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration analysisMigration() {
    return {
        5,
        QStringLiteral("analysis_cache"),
        {QStringLiteral("CREATE TABLE analysis_results (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "session BLOB NOT NULL CHECK(length(session)=16), attempt BLOB NOT NULL "
                        "CHECK(length(attempt)=16), "
                        "position_key BLOB NOT NULL CHECK(length(position_key)=32), "
                        "engine_fingerprint BLOB NOT NULL CHECK(length(engine_fingerprint)=32), "
                        "position_identity BLOB NOT NULL, raw_result_blob BLOB NOT NULL, "
                        "result_format_version INTEGER NOT NULL, UNIQUE(session,attempt))"),
         QStringLiteral("CREATE INDEX analysis_compatibility ON "
                        "analysis_results(position_key,engine_fingerprint,id)")}};
}
} // namespace chess_review::persistence::detail
