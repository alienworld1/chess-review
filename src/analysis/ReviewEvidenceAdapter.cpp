#include "analysis/ReviewEvidenceAdapter.h"
#include "engine/AnalysisCache.h"
#include <utility>

namespace chess_review::analysis {
namespace {
review::RawEvaluation evaluation(const engine::AnalysisLine& line, chess::Color perspective) {
    review::RawEvaluation raw;
    raw.perspective = perspective;
    if (line.score) {
        raw.score = review::RawScore{
            (line.score->type == engine::ScoreType::Centipawns ? review::ScoreKind::Centipawns
                                                               : review::ScoreKind::Mate),
            line.score->value,
            (line.score->bound == engine::ScoreBound::Exact
                 ? review::Bound::Exact
                 : (line.score->bound == engine::ScoreBound::Lower ? review::Bound::Lower
                                                                   : review::Bound::Upper))};
    }
    if (line.wdl) {
        raw.wdl = review::Wdl{line.wdl->wins, line.wdl->draws, line.wdl->losses};
    }
    return raw;
}
review::AnalysisStrength strength(const engine::SearchStatistics& stats) {
    return {stats.depth, stats.nodes, stats.timeMs};
}
} // namespace
EvidenceResult reviewEvidence(const engine::AnalysisResult& result,
                              const chess::Position* knownRoot) {
    if (result.outcome != engine::AnalysisOutcome::Completed) {
        return EvidenceAdapterIssue::IncompleteAttempt;
    }
    const auto& snapshot = result.evidence;
    if (!snapshot.provenance) {
        return EvidenceAdapterIssue::InvalidAnalysis;
    }
    const auto& provenance = *snapshot.provenance;
    auto restored = knownRoot && knownRoot->context() == provenance.position
                        ? chess::Result<chess::Position>(*knownRoot)
                        : engine::restorePosition(provenance.position);
    if (std::holds_alternative<chess::Error>(restored)) {
        return EvidenceAdapterIssue::InvalidAnalysis;
    }
    const auto& position = std::get<chess::Position>(restored);
    if (engine::validateCacheResult(result, &position)) {
        return EvidenceAdapterIssue::InvalidAnalysis;
    }
    auto evidence = review::terminalEvidence(position);
    evidence.engineIdentity = provenance.fingerprint.digest.toHex().toStdString();
    evidence.sessionIdentity = snapshot.session.toString(QUuid::WithoutBraces).toStdString();
    evidence.attemptIdentity = snapshot.request.toString(QUuid::WithoutBraces).toStdString();
    evidence.createdAtUtc = provenance.createdAt.toString(Qt::ISODateWithMs).toStdString();
    evidence.restricted = provenance.searchMoves.has_value();
    evidence.strength = strength(snapshot.statistics);
    if (!snapshot.lines.empty() && snapshot.lines.front().rank == 1) {
        evidence.evaluation = evaluation(snapshot.lines.front(), position.sideToMove());
    }
    const auto record = [&] {
        return ReviewEvidenceRecord{
            std::move(evidence),       snapshot.provenance,       snapshot.limit,
            snapshot.requestedMultiPv, snapshot.effectiveMultiPv, snapshot.wdlRequested,
            snapshot.wdlSupported};
    };
    if (evidence.terminal) {
        return record();
    }
    evidence.strength.depth = snapshot.commonDepth;
    evidence.complete = snapshot.achievedWidth == snapshot.requestedMultiPv &&
                        snapshot.commonDepth && *snapshot.commonDepth > 0;
    if (const auto* nodes = std::get_if<engine::NodeLimit>(&snapshot.limit)) {
        evidence.complete = evidence.complete && snapshot.statistics.nodes &&
                            *snapshot.statistics.nodes >= nodes->nodes;
    } else if (const auto* depth = std::get_if<engine::DepthLimit>(&snapshot.limit)) {
        evidence.complete = evidence.complete && snapshot.commonDepth &&
                            static_cast<std::uint32_t>(*snapshot.commonDepth) >= depth->depth;
    } else if (const auto* time = std::get_if<engine::TimeLimit>(&snapshot.limit)) {
        evidence.complete = evidence.complete && snapshot.statistics.timeMs &&
                            *snapshot.statistics.timeMs >= time->milliseconds;
    } else {
        evidence.complete = false;
    }
    for (const auto& line : snapshot.lines) {
        evidence.candidates.push_back({line.rank, evaluation(line, position.sideToMove()), line.pv,
                                       strength(line.statistics)});
    }
    if (!snapshot.bestMove || snapshot.lines.empty() || snapshot.lines.front().pv.empty() ||
        *snapshot.bestMove != snapshot.lines.front().pv.front()) {
        evidence.complete = false;
    }
    return record();
}
} // namespace chess_review::analysis
