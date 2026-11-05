#include "review/Comparison.h"
#include "review/Classification.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace chess_review::review {
namespace {
int ticks(const RawEvaluation& raw, chess::Color side) {
    return 2 * (raw.perspective == side ? raw.wdl->wins : raw.wdl->losses) + raw.wdl->draws;
}
bool stronger(const PositionEvidence& a, const PositionEvidence& b, ComparisonModel model) {
    if (a.position != b.position) {
        return false;
    }
    if (a.terminal && b.terminal) {
        return a.terminal == b.terminal;
    }
    return a.engineIdentity == b.engineIdentity && !a.attemptIdentity.empty() &&
           !b.attemptIdentity.empty() && a.attemptIdentity != b.attemptIdentity &&
           a.strength.nodes && b.strength.nodes &&
           (model == ComparisonModel::Version1 || *a.strength.nodes > 0) &&
           *b.strength.nodes > *a.strength.nodes && a.strength.depth && b.strength.depth &&
           *b.strength.depth >= *a.strength.depth;
}
std::optional<int> scoreTicks(const MoveComparison& row, const MoveEvidence& pair,
                              chess::Color mover) {
    if (!row.expected || row.expected->source == ConversionSource::ApproximateCentipawns) {
        return {};
    }
    if (row.source == ComparisonSource::PlayedChild) {
        if (row.expected->source == ConversionSource::Terminal) {
            return static_cast<int>(row.expected->value * 2000);
        }
        return ticks(pair.after.evaluation, mover);
    }
    const auto found =
        std::find_if(pair.before.candidates.begin(), pair.before.candidates.end(),
                     [&](const auto& c) { return c.continuation.front() == row.move; });
    return ticks(found->evaluation, mover);
}
AlternativeComparison single(const chess::Position& root, const MoveEvidence& pair,
                             const Configuration& config, ComparisonModel model) {
    AlternativeComparison out;
    const auto classified = classify(root, pair, config.classification, config.conversion);
    out.reason = ComparisonReason::Insufficient;
    if (classified.issue || !classified.loss || !classified.bestMove) {
        return out;
    }
    const auto add = [&](chess::Move move, const CandidateEvidence* candidate) {
        MoveComparison row;
        row.move = move;
        row.played = move == pair.playedMove;
        if (candidate) {
            row.rank = candidate->rank;
            row.expected =
                convertEvaluation(candidate->evaluation, root.sideToMove(), config.conversion)
                    .expected;
            row.gap = classified.before.expected->source == ConversionSource::NativeWdl
                          ? static_cast<double>(ticks(pair.before.evaluation, root.sideToMove()) -
                                                ticks(candidate->evaluation, root.sideToMove())) /
                                2000
                          : classified.before.expected->value - row.expected->value;
        } else {
            row.source = ComparisonSource::PlayedChild;
            row.expected = classified.after.expected;
            row.gap = classified.loss;
        }
        row.equivalent = *row.gap <= config.classification.excellentLoss;
        if (row.played && candidate && std::isfinite(config.refinement.maximumDrift)) {
            const bool exact = row.expected->source == ConversionSource::NativeWdl &&
                               (classified.after.expected->source == ConversionSource::NativeWdl ||
                                (model == ComparisonModel::Version2 &&
                                 classified.after.expected->source == ConversionSource::Terminal));
            const double disagreement =
                exact ? static_cast<double>(std::abs(
                            ticks(candidate->evaluation, root.sideToMove()) -
                            (classified.after.expected->source == ConversionSource::Terminal
                                 ? static_cast<int>(classified.after.expected->value * 2000)
                                 : ticks(pair.after.evaluation, root.sideToMove())))) /
                            2000
                      : std::abs(row.expected->value - classified.after.expected->value);
            const bool qualityConflict =
                model == ComparisonModel::Version2 &&
                *row.equivalent != (*classified.loss <= config.classification.excellentLoss);
            if (disagreement > config.refinement.maximumDrift || qualityConflict) {
                row.expected.reset();
                row.gap.reset();
                row.equivalent.reset();
                row.reason = ComparisonReason::Disagreement;
            }
        }
        out.moves.push_back(std::move(row));
    };
    const auto played =
        std::find_if(pair.before.candidates.begin(), pair.before.candidates.end(),
                     [&](const auto& c) { return c.continuation.front() == pair.playedMove; });
    add(pair.playedMove, played == pair.before.candidates.end() ? nullptr : &*played);
    for (const auto& candidate : pair.before.candidates) {
        if (candidate.continuation.front() != pair.playedMove) {
            add(candidate.continuation.front(), &candidate);
        }
    }
    out.reason = root.legalMoves().size() <= 1       ? ComparisonReason::NoAlternative
                 : pair.before.candidates.size() < 2 ? ComparisonReason::Coverage
                                                     : ComparisonReason::Pending;
    if (out.reason == ComparisonReason::Coverage || out.reason == ComparisonReason::NoAlternative) {
        for (auto& row : out.moves) {
            row.equivalent.reset();
            row.reason = out.reason;
        }
    }
    if (out.reason == ComparisonReason::Pending &&
        std::any_of(out.moves.begin(), out.moves.end(),
                    [](const auto& row) { return row.reason == ComparisonReason::Disagreement; })) {
        out.reason = ComparisonReason::Disagreement;
    }
    return out;
}
} // namespace
AlternativeComparison compareAlternatives(const chess::Position& root, const MoveEvidence& latest,
                                          const MoveEvidence* older, const Configuration& config,
                                          ComparisonModel model) {
    if ((model != ComparisonModel::Version1 && model != ComparisonModel::Version2) ||
        (model == ComparisonModel::Version2 &&
         (!std::isfinite(config.refinement.maximumDrift) || config.refinement.maximumDrift < 0 ||
          config.refinement.maximumDrift > 1))) {
        AlternativeComparison unavailable;
        unavailable.reason = ComparisonReason::Insufficient;
        return unavailable;
    }
    // single() calls the original-board classifier, which validates both raw witnesses.
    // Read only those witnesses and classified values; caller-supplied derived pair facts
    // (including mover/loss) are never trusted. Avoid a second full PV replay/copy here.
    auto out = single(root, latest, config, model);
    if (out.moves.empty() || !out.needsEvidence()) {
        return out;
    }
    if (!older || !std::isfinite(config.refinement.maximumDrift) ||
        config.refinement.maximumDrift < 0 || config.refinement.maximumDrift > 1) {
        return out;
    }
    const auto previous = single(root, *older, config, model);
    const bool independent = !previous.moves.empty() && older->playedMove == latest.playedMove &&
                             stronger(older->before, latest.before, model) &&
                             stronger(older->after, latest.after, model);
    bool unique = true;
    if (model == ComparisonModel::Version2) {
        const std::array witnesses{&older->before, &older->after, &latest.before, &latest.after};
        for (std::size_t i = 0; i < witnesses.size(); ++i) {
            for (std::size_t j = i + 1; j < witnesses.size(); ++j) {
                if (!witnesses[i]->terminal && !witnesses[j]->terminal &&
                    witnesses[i]->attemptIdentity == witnesses[j]->attemptIdentity) {
                    unique = false;
                }
            }
        }
    }
    const bool assessable = out.reason == ComparisonReason::Pending;
    if (assessable) {
        out.reason = ComparisonReason::Confirmed;
    }
    for (auto& row : out.moves) {
        const auto found = std::find_if(previous.moves.begin(), previous.moves.end(),
                                        [&](const auto& r) { return r.move == row.move; });
        row.reason = ComparisonReason::Insufficient;
        if (!row.expected || (found != previous.moves.end() && !found->expected)) {
            row.reason = ComparisonReason::Disagreement;
        } else if (independent && unique && found == previous.moves.end()) {
            row.reason = ComparisonReason::MissingCandidate;
        } else if (independent && unique) {
            const auto& prior = *found;
            if (prior.source != row.source || prior.expected->source != row.expected->source ||
                prior.expected->config != row.expected->config ||
                prior.equivalent != row.equivalent) {
                row.reason = ComparisonReason::Disagreement;
            } else {
                const auto a = scoreTicks(prior, *older, root.sideToMove()),
                           b = scoreTicks(row, latest, root.sideToMove());
                const double scoreDrift =
                    a && b ? static_cast<double>(std::abs(*a - *b)) / 2000
                           : std::abs(prior.expected->value - row.expected->value);
                // Native gaps are exact half-permille counts; subtract counts before conversion.
                const double gapDrift =
                    a && b ? static_cast<double>(std::abs(
                                 (ticks(older->before.evaluation, root.sideToMove()) - *a) -
                                 (ticks(latest.before.evaluation, root.sideToMove()) - *b))) /
                                 2000
                           : std::abs(*prior.gap - *row.gap);
                row.reason = scoreDrift > config.refinement.maximumDrift ||
                                     gapDrift > config.refinement.maximumDrift
                                 ? ComparisonReason::ScoreDrift
                                 : ComparisonReason::Confirmed;
            }
        }
        if (assessable && row.reason != ComparisonReason::Confirmed) {
            out.reason = row.reason;
        }
    }
    return out;
}
} // namespace chess_review::review
