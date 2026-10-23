#pragma once
#include "engine/EngineFingerprint.h"

namespace chess_review::engine {
// Inputs come from validated runtime discovery and prepared, effective option values.
EngineFingerprint makeFingerprint(const ValidatedEngine& engine,
                                  std::vector<EffectiveOption> options);
// Version-1 canonical encoding; not a persistence codec. Ignores the digest field.
QByteArray fingerprintDigest(const EngineFingerprint& fingerprint);
} // namespace chess_review::engine
