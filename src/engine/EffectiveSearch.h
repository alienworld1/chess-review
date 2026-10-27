#pragma once
#include "engine/Analysis.h"
#include <variant>

namespace chess_review::engine {
// CPU-only; call on a worker. Uses actual search preparation, including default options.
// MultiPV=1, unrestricted root; fingerprint is independent of position and node budget.
std::variant<EngineFingerprint, Error> quickReviewFingerprint(const ValidatedEngine& engine);
} // namespace chess_review::engine
