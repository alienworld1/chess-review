#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration classificationsMigration() {
    return {
        7,
        QStringLiteral("normal_classifications"),
        {QStringLiteral(
            "CREATE TABLE review_classifications (review_id INTEGER NOT NULL REFERENCES "
            "reviews(id), "
            "position_index INTEGER NOT NULL CHECK(position_index>0), before_id INTEGER NOT NULL, "
            "after_id INTEGER NOT NULL, document BLOB NOT NULL, PRIMARY "
            "KEY(review_id,position_index), "
            "FOREIGN KEY(review_id,before_id) REFERENCES review_evidence(review_id,id), "
            "FOREIGN KEY(review_id,after_id) REFERENCES review_evidence(review_id,id))")}};
}
} // namespace chess_review::persistence::detail
