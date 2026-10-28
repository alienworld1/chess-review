#include "engine/EffectiveSearch.h"
#include "engine/private/Fingerprint.h"
#include "engine/private/SearchProtocol.h"
#include <algorithm>
#include <utility>

namespace chess_review::engine {
std::variant<EngineFingerprint, Error> quickReviewFingerprint(const ValidatedEngine& engine) {
    if (!validEngine(engine)) {
        return Error{ErrorCode::InvalidRecord,
                     QStringLiteral("Invalid review engine configuration.")};
    }
    AnalysisRequest request;
    request.id = QUuid::createUuid();
    request.wantWdl = true;
    auto prepared = prepareSearch(request, engine.capabilities);
    if (const auto* error = std::get_if<Error>(&prepared)) {
        return *error;
    }
    return makeFingerprint(engine, std::get<PreparedSearch>(std::move(prepared)).effectiveOptions);
}
} // namespace chess_review::engine

namespace chess_review::engine {
EngineFingerprint reviewWidthFingerprint(EngineFingerprint base, int width) {
    for (auto& option : base.options) {
        if (option.name == QStringLiteral("multipv")) {
            option.value = QString::number(width);
        }
    }
    base.digest = fingerprintDigest(base);
    return base;
}
bool reviewWidthCompatible(const EngineFingerprint& base, const EngineFingerprint& actual,
                           int width) {
    if (width < 1) {
        return false;
    }
    if (width > 1 && std::none_of(base.options.begin(), base.options.end(), [](const auto& option) {
            return option.name == QStringLiteral("multipv");
        })) {
        return false;
    }
    return reviewWidthFingerprint(base, width) == actual;
}
} // namespace chess_review::engine
