#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration refinementMigration() {
    return {
        8,
        QStringLiteral("review_refinement"),
        {QStringLiteral("CREATE TABLE review_stage_positions (review_id INTEGER NOT NULL "
                        "REFERENCES reviews(id), stage INTEGER NOT NULL CHECK(stage IN (1,2)), "
                        "position_index INTEGER NOT NULL CHECK(position_index>=0), evidence_id "
                        "INTEGER NOT NULL, PRIMARY KEY(review_id,stage,position_index), FOREIGN "
                        "KEY(review_id,evidence_id) REFERENCES review_evidence(review_id,id))")}};
}
} // namespace chess_review::persistence::detail
