#include "app/PuzzleAlternative.h"
#include "analysis/ReviewEvidenceAdapter.h"
#include "engine/AnalysisCache.h"
#include "engine/EffectiveSearch.h"
#include <algorithm>

namespace chess_review::app {
namespace {
std::optional<review::PositionEvidence> evidence(const engine::AnalysisResult& raw,
                                                 const persistence::PuzzleDocument& document) {
    if (!raw.evidence.provenance ||
        !engine::reviewWidthCompatible(document.fingerprint, raw.evidence.provenance->fingerprint,
                                       raw.evidence.requestedMultiPv)) {
        return {};
    }
    const auto* nodes = std::get_if<engine::NodeLimit>(&raw.evidence.limit);
    auto converted = analysis::reviewEvidence(raw);
    auto* record = std::get_if<analysis::ReviewEvidenceRecord>(&converted);
    if (!nodes || !record || !record->evidence.complete) {
        return {};
    }
    record->evidence.engineIdentity = document.fingerprint.digest.toHex().toStdString();
    record->evidence.strength.nodes = nodes->nodes;
    return record->evidence;
}
} // namespace
PuzzleAlternativeResult evaluatePuzzleAlternative(const PuzzleAlternative& attempt) {
    if (!attempt.puzzle || !attempt.position) {
        return {};
    }
    const auto& d = attempt.puzzle->document;
    auto child = *attempt.position;
    if (std::holds_alternative<chess::Error>(child.play(attempt.move))) {
        return {};
    }
    const auto childContext = child.context();
    const auto legal = child.legalMoves();
    if (legal.empty() && child.inCheck()) {
        // The board proves the objective, even if MultiPV omitted or underrated this move.
        puzzles::Solution mate{d.solution.goal,
                               d.solution.materialTarget,
                               {{{}, {}, true, review::RetryIdea::General}}};
        return {puzzles::AlternativeGrade::Strong, {}, std::move(mate)};
    }
    const auto rootContext = attempt.position->context();
    std::vector<review::PositionEvidence> roots;
    for (const auto& raw : d.passes) {
        if (raw.evidence.provenance && raw.evidence.provenance->position == rootContext) {
            auto e = evidence(raw, d);
            if (!e) {
                return {};
            }
            roots.push_back(std::move(*e));
        }
    }
    if (roots.size() < 2) {
        return {};
    }
    const auto known = puzzles::gradeKnownMove(*attempt.position, attempt.move, d.definition.policy,
                                               roots[roots.size() - 2], roots.back());
    if (known == puzzles::AlternativeGrade::TryAgain) {
        return {known, {}, {}};
    }
    const auto solver = d.definition.position.board.sideToMove;
    std::vector<review::PositionEvidence> passes;
    for (const auto& raw : attempt.passes) {
        auto e = evidence(raw, d);
        if (!e) {
            return {};
        }
        passes.push_back(std::move(*e));
    }
    const auto baseline = d.definition.policy.baselineNodes;
    review::PositionEvidence terminal = review::terminalEvidence(child);
    terminal.engineIdentity = roots.back().engineIdentity;
    terminal.sessionIdentity = "local-proof";
    terminal.attemptIdentity = "local-proof";
    const review::PositionEvidence* older = &terminal;
    const review::PositionEvidence* newer = &terminal;
    if (!legal.empty()) {
        for (std::size_t i = 0; i < 2; ++i) {
            const auto nodes = baseline * (i == 0 ? 2 : 4);
            if (passes.size() <= i) {
                return {puzzles::AlternativeGrade::Unavailable,
                        puzzles::SearchNeed{
                            child, nodes, static_cast<int>(std::min<std::size_t>(4, legal.size()))},
                        {}};
            }
            if (passes[i].position != childContext || passes[i].strength.nodes != nodes) {
                return {};
            }
        }
        older = &passes[0];
        newer = &passes[1];
    }
    // Terminal outcomes are proven locally and need no distinct engine attempts.
    auto secondTerminal = terminal;
    if (legal.empty()) {
        secondTerminal.attemptIdentity = "local-proof-2";
        newer = &secondTerminal;
    }
    const auto grade =
        puzzles::gradeAlternative(*attempt.position, attempt.move, d.definition.policy,
                                  roots[roots.size() - 2], roots.back(), *older, *newer);
    if (grade != puzzles::AlternativeGrade::Strong) {
        return {grade, {}, {}};
    }
    auto original = engine::restorePosition(d.definition.position);
    const auto* originalRoot = std::get_if<chess::Position>(&original);
    if (!originalRoot) {
        return {};
    }
    puzzles::Definition definition{childContext, puzzles::Origin::Manual, d.definition.policy,
                                   d.definition.modelVersion};
    definition.policy.rootMinimumNodes = 0;
    const puzzles::ContinuationGoal goal{
        d.solution.goal, solver, puzzles::material(*originalRoot, solver),
        d.solution.materialTarget,
        d.definition.policy.maximumPlayerMoves - attempt.playerMoves - 1};
    if (goal.remainingPlayerMoves < 0) {
        return {};
    }
    const auto verified = puzzles::verifyContinuation(child, definition, passes, goal);
    if (verified.outcome == puzzles::Outcome::Pending && verified.next) {
        return {puzzles::AlternativeGrade::Unavailable, verified.next, {}};
    }
    if (verified.outcome != puzzles::Outcome::Accepted) {
        return {};
    }
    return {puzzles::AlternativeGrade::Strong, {}, verified.solution};
}
} // namespace chess_review::app
