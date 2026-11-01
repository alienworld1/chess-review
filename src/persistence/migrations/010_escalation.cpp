#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration escalationMigration() {
    return {
        10,
        QStringLiteral("review_escalation"),
        {QStringLiteral("CREATE TABLE review_escalation_positions (review_id INTEGER NOT NULL "
                        "REFERENCES reviews(id), stage INTEGER NOT NULL CHECK(stage IN (3,4)), "
                        "position_index INTEGER NOT NULL CHECK(position_index>=0), evidence_id "
                        "INTEGER NOT NULL, PRIMARY KEY(review_id,stage,position_index), FOREIGN "
                        "KEY(review_id,evidence_id) REFERENCES review_evidence(review_id,id))")}};
}
} // namespace chess_review::persistence::detail
