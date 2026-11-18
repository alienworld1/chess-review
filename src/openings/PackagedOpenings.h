#pragma once

#include "openings/OpeningIndex.h"

namespace chess_review::openings {
inline constexpr char datasetVersion[] = "2026.04.16";
inline constexpr int recognitionPolicyVersion = 1;

// First call loads on the calling worker. The immutable result lives for the process.
// A missing or malformed resource returns null.
[[nodiscard]] const OpeningIndex* loadPackagedOpenings();
} // namespace chess_review::openings
