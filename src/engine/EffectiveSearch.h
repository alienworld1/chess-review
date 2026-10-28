#pragma once
#include "engine/Analysis.h"
#include <variant>

namespace chess_review::engine {
// CPU-only; call on a worker. Uses actual search preparation, including default options.
// MultiPV=1, unrestricted root; fingerprint is independent of position and node budget.
std::variant<EngineFingerprint, Error> quickReviewFingerprint(const ValidatedEngine& engine);
} // namespace chess_review::engine

namespace chess_review::engine {
// Review-only identity transform. Cache queries still use the exact resulting fingerprint.
EngineFingerprint reviewWidthFingerprint(EngineFingerprint base, int width);
bool reviewWidthCompatible(const EngineFingerprint& base, const EngineFingerprint& actual,
                           int width);
} // namespace chess_review::engine
