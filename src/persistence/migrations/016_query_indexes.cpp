#include "persistence/private/Migrations.h"

namespace chess_review::persistence::detail {

Migration queryIndexesMigration() {
    return {
        16,
        QStringLiteral("query_indexes"),
        {QStringLiteral("CREATE INDEX review_jobs_status ON review_jobs(status, review_id)"),
         QStringLiteral(
             "CREATE INDEX games_result_imported ON games(result, imported_at DESC, id DESC)")}};
}

} // namespace chess_review::persistence::detail
