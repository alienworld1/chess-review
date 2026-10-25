#pragma once

#include "engine/Analysis.h"

namespace chess_review::engine {
// Immutable input. Callers supply the intended effective configuration, not profile metadata.
struct CacheQuery {
    chess::PositionContext position;
    EngineFingerprint fingerprint;
    SearchLimit limit = NodeLimit{10000};
    int multiPv = 1;
    bool wantWdl = false;
    std::optional<std::vector<chess::Move>> searchMoves;
};
// CPU-only validation; invoke on a worker for histories and PVs of arbitrary length.
chess::Result<chess::Position> restorePosition(const chess::PositionContext& context);
bool validFingerprint(const EngineFingerprint& fingerprint);
// Policy qualification only; it does not validate the digest or authorize reuse.
bool verifiedResources(const EngineFingerprint& fingerprint);
std::optional<Error> validateCacheQuery(const CacheQuery& query);
// Optional replay witness, borrowed for this call only. Used after exact context
// equality; mismatches replay normally. It never substitutes a FEN-only identity.
std::optional<Error> validateCacheResult(const AnalysisResult& result,
                                         const chess::Position* knownRoot = nullptr);
bool cacheDominates(const AnalysisResult& result, const CacheQuery& query,
                    const chess::Position* knownRoot = nullptr);
std::uint64_t cacheStrength(const AnalysisResult& result);
} // namespace chess_review::engine
