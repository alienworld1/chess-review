#include "persistence/private/Migrations.h"
namespace chess_review::persistence::detail {
Migration engineSettingsMigration() {
    return {15,
            QStringLiteral("engine-settings"),
            {QStringLiteral("ALTER TABLE engines ADD COLUMN archived_path TEXT")}};
}
} // namespace chess_review::persistence::detail
