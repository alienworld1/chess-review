#include "openings/PackagedOpenings.h"
#include <QFile>

static void initializeOpeningResources() { Q_INIT_RESOURCE(openings_lookup); }

namespace chess_review::openings {
const OpeningIndex* loadPackagedOpenings() {
    static const auto index = []() -> std::optional<OpeningIndex> {
        initializeOpeningResources();
        constexpr qint64 maxLookupBytes = 1024 * 1024;
        QFile file(QStringLiteral(":/openings/opening-references.tsv"));
        if (!file.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        const auto bytes = file.read(maxLookupBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > maxLookupBytes ||
            !bytes.isValidUtf8()) {
            return std::nullopt;
        }
        return OpeningIndex::build({bytes.constData(), static_cast<std::size_t>(bytes.size())});
    }();
    return index ? &*index : nullptr;
}
} // namespace chess_review::openings
