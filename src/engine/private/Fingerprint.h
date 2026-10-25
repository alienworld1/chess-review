#pragma once
#include "engine/EngineFingerprint.h"

namespace chess_review::engine {
// Inputs come from validated runtime discovery and prepared, effective option values.
EngineFingerprint makeFingerprint(const ValidatedEngine& engine,
                                  std::vector<EffectiveOption> options);
// Canonical v1/v2 encoding; v2 appends resource-policy version. Ignores digest.
QByteArray fingerprintDigest(const EngineFingerprint& fingerprint);
} // namespace chess_review::engine
