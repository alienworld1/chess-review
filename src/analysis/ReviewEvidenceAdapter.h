#pragma once
#include "engine/Analysis.h"
#include "review/Evidence.h"
#include <memory>
#include <variant>
namespace chess_review::analysis {
struct ReviewEvidenceRecord {
    review::PositionEvidence evidence;
    std::shared_ptr<const engine::AnalysisProvenance> provenance;
    engine::SearchLimit requestedLimit;
    int requestedMultiPv;
    int effectiveMultiPv;
    bool wdlRequested;
    bool wdlSupported;
};
enum class EvidenceAdapterIssue { InvalidAnalysis, IncompleteAttempt };
using EvidenceResult = std::variant<ReviewEvidenceRecord, EvidenceAdapterIssue>;
// CPU-only; replay/validation belongs off the GUI thread. Does not authorize cache reuse.
// A borrowed board witness is used only after complete history equality; mismatch falls back to
// replay.
[[nodiscard]] EvidenceResult reviewEvidence(const engine::AnalysisResult& result,
                                            const chess::Position* knownRoot = nullptr);
} // namespace chess_review::analysis
