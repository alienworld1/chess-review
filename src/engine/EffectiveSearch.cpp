#include "engine/EffectiveSearch.h"
#include "engine/private/Fingerprint.h"
#include "engine/private/SearchProtocol.h"
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
