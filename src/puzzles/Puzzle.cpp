#include "puzzles/Puzzle.h"
#include "review/Intelligence.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace chess_review::puzzles {
namespace {
struct Value {
    int mate = 0; // Only the winning side matters; mate distance is not a grading tolerance.
    double score = 0;
    std::optional<int> ticks;
    review::ConversionSource source = review::ConversionSource::ApproximateCentipawns;
};
std::optional<Value> value(const review::RawEvaluation& raw,
                           const review::ConversionConfig& config) {
    if (raw.score && raw.score->kind == review::ScoreKind::Mate) {
        if (raw.score->bound != review::Bound::Exact || raw.score->value == 0 ||
            (raw.wdl && (raw.wdl->wins < 0 || raw.wdl->draws < 0 || raw.wdl->losses < 0 ||
                         raw.wdl->wins > 1000 || raw.wdl->draws > 1000 || raw.wdl->losses > 1000 ||
                         raw.wdl->wins + raw.wdl->draws + raw.wdl->losses != 1000))) {
            return {};
        }
        if (raw.wdl && (raw.wdl->draws != 0 ||
                        (raw.score->value > 0 ? raw.wdl->wins != 1000 : raw.wdl->losses != 1000))) {
            return {};
        }
        return Value{raw.score->value > 0 ? 1 : -1, 0, {}, {}};
    }
    const auto converted = review::convertEvaluation(raw, raw.perspective, config);
    if (converted.issue || !converted.expected) {
        return {};
    }
    const auto& score = *converted.expected;
    return Value{0, score.value,
                 score.source == review::ConversionSource::NativeWdl
                     ? std::optional{2 * raw.wdl->wins + raw.wdl->draws}
                     : std::nullopt,
                 score.source};
}
bool close(const Value& a, const Value& b, double tolerance) {
    if (a.mate || b.mate) {
        return a.mate == b.mate;
    }
    if (a.source != b.source) {
        return false;
    }
    if (a.ticks && b.ticks) {
        return std::abs(*a.ticks - *b.ticks) <= tolerance * 2000;
    }
    return std::abs(a.score - b.score) <= tolerance;
}
bool worse(const Value& best, const Value& candidate, double gap) {
    if (best.mate || candidate.mate) {
        return best.mate > candidate.mate;
    }
    if (best.source != candidate.source) {
        return false;
    }
    if (best.ticks && candidate.ticks) {
        return *best.ticks - *candidate.ticks >= gap * 2000;
    }
    return best.score - candidate.score >= gap;
}
bool defensivePosition(const review::PositionEvidence& evidence, const Policy& policy) {
    if (evidence.candidates.size() < 2) {
        return false;
    }
    const auto best = value(evidence.evaluation, policy.review.conversion);
    const auto second = value(evidence.candidates[1].evaluation, policy.review.conversion);
    return best && second && best->mate == 0 && best->score >= 0.5 &&
           (second->mate < 0 || (second->mate == 0 && second->score <= 0.4));
}
struct Pair {
    const review::PositionEvidence* older = nullptr;
    const review::PositionEvidence* newer = nullptr;
    std::vector<chess::Move> accepted;
};
bool usable(const chess::Position& root, const review::PositionEvidence& evidence, int width,
            const Policy& policy) {
    const auto issue = review::validatePositionEvidence(root, evidence, policy.review.conversion);
    return (!issue || issue == review::EvidenceIssue::MateClaim) &&
           evidence.candidates.size() == static_cast<std::size_t>(width);
}
std::optional<std::vector<chess::Move>> solutions(const review::PositionEvidence& evidence,
                                                  const Policy& policy, bool separation,
                                                  bool solver) {
    const auto& c = policy.review;
    const auto best = value(evidence.evaluation, c.conversion);
    if (!best || (solver && best->mate < 0)) {
        return {};
    }
    std::vector<chess::Move> result;
    std::optional<Value> previous;
    for (const auto& candidate : evidence.candidates) {
        const auto score = value(candidate.evaluation, c.conversion);
        if (!score || (previous && worse(*score, *previous, 0.0000001))) {
            return {};
        }
        previous = score;
        if (close(*best, *score, c.classification.excellentLoss)) {
            result.push_back(candidate.continuation.front());
        } else if (separation && !worse(*best, *score, c.special.alternativeGap)) {
            return {};
        }
    }
    if (!solver) {
        return std::vector{evidence.candidates.front().continuation.front()};
    }
    if (result.empty() || result.size() > 3) {
        return {};
    }
    return result;
}
bool sameMoves(std::span<const chess::Move> a, std::span<const chess::Move> b) {
    return a.size() == b.size() && std::all_of(a.begin(), a.end(), [&](chess::Move move) {
               return std::find(b.begin(), b.end(), move) != b.end();
           });
}
bool stable(const Pair& pair, const Policy& policy) {
    if (pair.older->attemptIdentity == pair.newer->attemptIdentity ||
        pair.older->engineIdentity != pair.newer->engineIdentity) {
        return false;
    }
    for (const auto& a : pair.older->candidates) {
        const auto found = std::find_if(
            pair.newer->candidates.begin(), pair.newer->candidates.end(),
            [&](const auto& b) { return b.continuation.front() == a.continuation.front(); });
        if (found == pair.newer->candidates.end()) {
            return false;
        }
        const auto x = value(a.evaluation, policy.review.conversion);
        const auto y = value(found->evaluation, policy.review.conversion);
        if (!x || !y || !close(*x, *y, policy.review.refinement.maximumDrift)) {
            return false;
        }
    }
    return true;
}
bool quiet(const chess::Position& position, const review::PositionEvidence& evidence) {
    if (position.inCheck() || evidence.candidates.empty()) {
        return false;
    }
    const auto move = evidence.candidates.front().continuation.front();
    // Captures (including en passant), promotions, and checks can conceal a recapture.
    auto child = position;
    const int before = material(position, position.sideToMove());
    if (std::holds_alternative<chess::Error>(child.play(move))) {
        return false;
    }
    return !move.promotion && material(child, position.sideToMove()) == before && !child.inCheck();
}
class VerificationRun {
  public:
    VerificationRun(const chess::Position& root, const Definition& definition,
                    std::span<const review::PositionEvidence> passes,
                    std::optional<ContinuationGoal> goal = std::nullopt)
        : root_(root), definition_(definition), passes_(passes), side_(root.sideToMove()),
          startingMaterial_(material(root, side_)), continuation_(goal) {
        if (goal) {
            side_ = goal->solver;
            startingMaterial_ = goal->startingMaterial;
        }
    }
    Verification run() {
        if (!validPolicy(definition_.policy) || root_.context() != definition_.position ||
            (definition_.origin != Origin::Manual && definition_.origin != Origin::Generated) ||
            (!continuation_ && root_.legalMoves().empty()) || passes_.size() > 192 ||
            (continuation_ && (continuation_->remainingPlayerMoves < 0 ||
                               continuation_->remainingPlayerMoves > 4))) {
            result_.outcome = Outcome::Rejected;
            return result_;
        }
        for (const auto& pass : passes_) {
            if (!pass.strength.nodes || *pass.strength.nodes > maximumCost() - cost_) {
                result_.outcome = Outcome::Insufficient;
                return result_;
            }
            cost_ += *pass.strength.nodes;
        }
        const auto first = node(root_, {}, 0);
        if (first && result_.outcome == Outcome::Pending && !result_.next) {
            result_.outcome = Outcome::Accepted;
        }
        return result_;
    }

  private:
    std::uint64_t maximumCost() const {
        return definition_.policy.baselineNodes *
               static_cast<std::uint64_t>(definition_.policy.maximumBudgetUnits);
    }
    std::optional<Pair> evidence(const chess::Position& position, bool root) {
        const auto& policy = definition_.policy;
        const auto count = position.legalMoves().size();
        const int width = static_cast<int>(std::min<std::size_t>(4, count));
        if (root && definition_.origin == Origin::Generated && count < 2) {
            result_.outcome = Outcome::Rejected;
            return {};
        }
        const auto context = position.context();
        const auto baseline =
            root ? std::max(policy.baselineNodes, policy.rootMinimumNodes) : policy.baselineNodes;
        const review::PositionEvidence* older = nullptr;
        for (const auto factor : {2ULL, 4ULL, 8ULL}) {
            if (baseline > std::numeric_limits<std::uint64_t>::max() / factor) {
                break;
            }
            const auto requested = baseline * factor;
            const auto found = std::find_if(passes_.begin(), passes_.end(), [&](const auto& pass) {
                return pass.position == context && pass.strength.nodes &&
                       *pass.strength.nodes >= requested &&
                       (!older || pass.attemptIdentity != older->attemptIdentity);
            });
            if (found == passes_.end()) {
                if (requested > maximumCost() - cost_) {
                    break;
                }
                result_.next = SearchNeed{position, requested, width};
                return {};
            }
            if (!usable(position, *found, width, policy)) {
                result_.outcome = Outcome::Insufficient;
                return {};
            }
            if (older) {
                Pair pair{older, &*found, {}};
                const bool separated = root && definition_.origin == Origin::Generated;
                const bool solver = position.sideToMove() == side_;
                auto a = solutions(*older, policy, separated, solver);
                auto b = solutions(*found, policy, separated, solver);
                if (a && b && sameMoves(*a, *b) && stable(pair, policy)) {
                    pair.accepted = std::move(*b);
                    return pair;
                }
            }
            older = &*found;
        }
        result_.outcome = Outcome::Insufficient;
        return {};
    }
    bool selectGoal(const chess::Position& position, const Pair& pair) {
        if (continuation_) {
            result_.solution.goal = continuation_->goal;
            result_.solution.materialTarget = continuation_->materialTarget;
            return true;
        }
        if (definition_.origin == Origin::Manual) {
            return true;
        }
        const auto bestA = value(pair.older->evaluation, definition_.policy.review.conversion);
        const auto bestB = value(pair.newer->evaluation, definition_.policy.review.conversion);
        if (bestA && bestB && bestA->mate > 0 && bestB->mate > 0) {
            result_.solution.goal = Goal::Mate;
            return true;
        }
        const auto factsA = review::inspectTacticalLine(
            position, pair.older->candidates.front().continuation, review::FactSource::Best);
        const auto factsB = review::inspectTacticalLine(
            position, pair.newer->candidates.front().continuation, review::FactSource::Best);
        for (const auto& b : factsB) {
            const int minimum = b.linePlies <= 1 ? definition_.policy.immediateMaterialGain
                                                 : definition_.policy.minimumMaterialGain;
            if (b.kind != review::TacticalKind::Material || !b.retainedOpportunity ||
                b.beneficiary != side_ || b.material < minimum) {
                continue;
            }
            if (std::any_of(factsA.begin(), factsA.end(), [&](const auto& a) {
                    return a.kind == b.kind && a.beneficiary == b.beneficiary &&
                           a.retainedOpportunity && a.material >= minimum;
                })) {
                // An immediate balancing recapture is not a new tactical lesson.
                if (!position.context().moves.empty()) {
                    auto previous = position;
                    const auto last = previous.undo();
                    const auto first = pair.newer->candidates.front().continuation.front();
                    if (last && first.to == last->to && position.pieceAt(first.to) &&
                        material(position, side_) < material(previous, side_)) {
                        return false;
                    }
                }
                result_.solution.goal = Goal::Material;
                result_.solution.materialTarget = minimum;
                return true;
            }
        }
        if (pair.accepted.size() == 1 && defensivePosition(*pair.older, definition_.policy) &&
            defensivePosition(*pair.newer, definition_.policy)) {
            result_.solution.goal = Goal::Defence;
            return true;
        }
        return false;
    }
    std::optional<std::size_t> node(chess::Position position, std::vector<chess::Move> path,
                                    int playerMoves) {
        if (result_.solution.nodes.size() >=
            static_cast<std::size_t>(definition_.policy.maximumPositions)) {
            result_.outcome = Outcome::Insufficient;
            return {};
        }
        const auto index = result_.solution.nodes.size();
        result_.solution.nodes.push_back({path, {}, false, review::RetryIdea::General});
        const auto legal = position.legalMoves();
        if (legal.empty()) {
            if (!path.empty() && position.inCheck() && position.sideToMove() != side_) {
                result_.solution.nodes[index].complete = true;
                return index;
            }
            result_.outcome = Outcome::Rejected;
            return {};
        }
        const auto pair = evidence(position, path.empty());
        if (!pair) {
            return {};
        }
        if (path.empty() && !selectGoal(position, *pair)) {
            result_.outcome = Outcome::Rejected;
            return {};
        }
        const auto goal = result_.solution.goal;
        const bool solver = position.sideToMove() == side_;
        if (!path.empty() && solver && quiet(position, *pair->older) &&
            quiet(position, *pair->newer)) {
            const int gain = material(position, side_) - startingMaterial_;
            const auto a = value(pair->older->evaluation, definition_.policy.review.conversion);
            const auto b = value(pair->newer->evaluation, definition_.policy.review.conversion);
            const bool defended = goal == Goal::Defence && a && b && !a->mate && !b->mate &&
                                  a->score >= 0.5 && b->score >= 0.5;
            if (goal == Goal::Practice || defended ||
                (goal == Goal::Material && gain >= result_.solution.materialTarget)) {
                result_.solution.nodes[index].complete = true;
                return index;
            }
        }
        const int maximumMoves = continuation_ ? continuation_->remainingPlayerMoves
                                               : definition_.policy.maximumPlayerMoves;
        if (solver && playerMoves >= maximumMoves) {
            result_.outcome = Outcome::Insufficient;
            return {};
        }
        auto accepted = pair->accepted;
        if (!solver) {
            // One independently supported best defence, never an arbitrary PV reply.
            const auto best = pair->newer->candidates.front().continuation.front();
            if (std::find(accepted.begin(), accepted.end(), best) == accepted.end()) {
                result_.outcome = Outcome::Insufficient;
                return {};
            }
            accepted = {best};
        }
        for (const auto move : accepted) {
            auto child = position;
            if (std::holds_alternative<chess::Error>(child.play(move))) {
                result_.outcome = Outcome::Rejected;
                return {};
            }
            auto childPath = path;
            childPath.push_back(move);
            const auto next =
                node(std::move(child), std::move(childPath), playerMoves + (solver ? 1 : 0));
            if (!next) {
                return {};
            }
            result_.solution.nodes[index].moves.push_back({move, *next});
        }
        if (solver) {
            auto& hint = result_.solution.nodes[index].idea;
            if (goal == Goal::Mate) {
                hint = review::RetryIdea::Checkmate;
            } else if (goal == Goal::Material) {
                hint = review::RetryIdea::Material;
            }
        }
        return index;
    }
    const chess::Position& root_;
    const Definition& definition_;
    std::span<const review::PositionEvidence> passes_;
    chess::Color side_;
    int startingMaterial_;
    std::uint64_t cost_ = 0;
    Verification result_;
    std::optional<ContinuationGoal> continuation_;
};
} // namespace
bool validPolicy(const Policy& policy) {
    const auto& c = policy.review;
    return policy.baselineNodes > 0 && policy.baselineNodes <= 1000000000000ULL / 64 &&
           policy.rootMinimumNodes <= 1000000000000ULL / 8 && policy.maximumPlayerMoves >= 1 &&
           policy.maximumPlayerMoves <= 4 && policy.maximumPositions >= 1 &&
           policy.maximumPositions <= 64 && policy.maximumBudgetUnits >= 6 &&
           policy.maximumBudgetUnits <= 64 && policy.minimumMaterialGain >= 2 &&
           policy.minimumMaterialGain <= 9 &&
           policy.immediateMaterialGain >= policy.minimumMaterialGain &&
           policy.immediateMaterialGain <= 9 && std::isfinite(c.classification.excellentLoss) &&
           c.classification.excellentLoss >= 0 &&
           c.classification.excellentLoss < c.special.alternativeGap &&
           std::isfinite(c.special.alternativeGap) && c.special.alternativeGap <= 1 &&
           std::isfinite(c.refinement.maximumDrift) && c.refinement.maximumDrift >= 0 &&
           c.refinement.maximumDrift <= 1 && std::isfinite(c.conversion.fallbackScaleCp) &&
           c.conversion.fallbackScaleCp >= 1 && c.conversion.fallbackScaleCp <= 10000;
}
int material(const chess::Position& position, chess::Color side) {
    constexpr int values[]{1, 3, 3, 5, 9, 0};
    int total = 0;
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const auto piece = position.pieceAt(*chess::Square::fromCoordinates(file, rank));
            if (piece) {
                total += values[static_cast<int>(piece->type)] * (piece->color == side ? 1 : -1);
            }
        }
    }
    return total;
}
Verification verify(const chess::Position& root, const Definition& definition,
                    std::span<const review::PositionEvidence> passes) {
    return VerificationRun(root, definition, passes).run();
}
std::optional<Goal> candidateGoal(const chess::Position& root, const review::MoveEvidence& older,
                                  const review::MoveEvidence& newer, const Policy& policy) {
    if (!validPolicy(policy) || root.legalMoves().size() < 2 ||
        older.playedMove != newer.playedMove ||
        older.before.engineIdentity != newer.before.engineIdentity ||
        older.before.attemptIdentity == newer.before.attemptIdentity) {
        return {};
    }
    const auto a = review::pairEvidence(root, older.playedMove, older.before, older.after,
                                        policy.review.conversion);
    const auto b = review::pairEvidence(root, newer.playedMove, newer.before, newer.after,
                                        policy.review.conversion);
    if ((a.issue && a.issue != review::EvidenceIssue::MateClaim) ||
        (b.issue && b.issue != review::EvidenceIssue::MateClaim)) {
        return {};
    }
    const auto mateMiss = [](const review::MoveEvidence& pair) {
        const auto& before = pair.before.evaluation.score;
        const auto& after = pair.after.evaluation.score;
        return before && before->kind == review::ScoreKind::Mate && before->value > 0 &&
               (pair.after.terminal == review::TerminalOutcome::Stalemate ||
                (after && !(after->kind == review::ScoreKind::Mate && after->value < 0)));
    };
    if (mateMiss(a) && mateMiss(b)) {
        return Goal::Mate;
    }
    if (defensivePosition(a.before, policy) && defensivePosition(b.before, policy)) {
        return Goal::Defence;
    }
    const auto first = review::tacticalFacts(root, a, policy.review);
    const auto second = review::tacticalFacts(root, b, policy.review);
    for (const auto& fact : second) {
        if (fact.source != review::FactSource::Best || fact.beneficiary != root.sideToMove()) {
            continue;
        }
        const auto minimum =
            fact.linePlies <= 1 ? policy.immediateMaterialGain : policy.minimumMaterialGain;
        const bool material = fact.kind == review::TacticalKind::Material &&
                              fact.retainedOpportunity && fact.material >= minimum;
        const bool tactic =
            fact.kind == review::TacticalKind::Fork ||
            fact.kind == review::TacticalKind::HangingPiece ||
            (fact.retainedOpportunity && (fact.kind == review::TacticalKind::Promotion ||
                                          fact.kind == review::TacticalKind::Sacrifice));
        if (!(material || tactic)) {
            continue;
        }
        if (std::any_of(first.begin(), first.end(), [&](const auto& old) {
                return old.kind == fact.kind && old.source == fact.source &&
                       old.beneficiary == fact.beneficiary && old.square == fact.square &&
                       old.targets == fact.targets &&
                       (material ? old.retainedOpportunity && old.material >= minimum
                                 : old.retainedOpportunity == fact.retainedOpportunity);
            })) {
            return Goal::Material;
        }
    }
    return {};
}
Verification verifyContinuation(const chess::Position& root, const Definition& definition,
                                std::span<const review::PositionEvidence> passes,
                                ContinuationGoal goal) {
    return VerificationRun(root, definition, passes, goal).run();
}
AlternativeGrade gradeAlternative(const chess::Position& root, chess::Move move,
                                  const Policy& policy, const review::PositionEvidence& olderRoot,
                                  const review::PositionEvidence& newerRoot,
                                  const review::PositionEvidence& olderChild,
                                  const review::PositionEvidence& newerChild) {
    auto child = root;
    if (!validPolicy(policy) || std::holds_alternative<chess::Error>(child.play(move))) {
        return AlternativeGrade::Unavailable;
    }
    const int rootWidth = static_cast<int>(std::min<std::size_t>(4, root.legalMoves().size()));
    const int childWidth = static_cast<int>(std::min<std::size_t>(4, child.legalMoves().size()));
    if (!usable(root, olderRoot, rootWidth, policy) ||
        !usable(root, newerRoot, rootWidth, policy) ||
        !stable({&olderRoot, &newerRoot, {}}, policy) ||
        olderChild.attemptIdentity == newerChild.attemptIdentity ||
        olderRoot.engineIdentity != olderChild.engineIdentity ||
        newerRoot.engineIdentity != newerChild.engineIdentity) {
        return AlternativeGrade::Unavailable;
    }
    if (childWidth > 0 && (!usable(child, olderChild, childWidth, policy) ||
                           !usable(child, newerChild, childWidth, policy) ||
                           !stable({&olderChild, &newerChild, {}}, policy))) {
        return AlternativeGrade::Unavailable;
    }
    if (childWidth == 0 && child.inCheck()) {
        return AlternativeGrade::Strong;
    }
    std::optional<bool> acceptable;
    for (const auto& [before, after] :
         {std::pair{&olderRoot, &olderChild}, std::pair{&newerRoot, &newerChild}}) {
        const auto best = value(before->evaluation, policy.review.conversion);
        std::optional<Value> played;
        if (childWidth == 0) {
            played = Value{child.inCheck() ? 1 : 0,
                           child.inCheck() ? 1.0 : 0.5,
                           {},
                           review::ConversionSource::Terminal};
        } else {
            auto raw = after->evaluation;
            if (raw.score) {
                if (raw.score->value == std::numeric_limits<std::int64_t>::min()) {
                    return AlternativeGrade::Unavailable;
                }
                raw.score->value = -raw.score->value;
            }
            if (raw.wdl) {
                std::swap(raw.wdl->wins, raw.wdl->losses);
            }
            raw.perspective = root.sideToMove();
            played = value(raw, policy.review.conversion);
        }
        if (!best || !played) {
            return AlternativeGrade::Unavailable;
        }
        if (childWidth == 0 && !best->mate) {
            played->source = best->source;
            if (best->ticks) {
                played->ticks = 1000;
            }
        }
        const bool strong = close(*best, *played, policy.review.classification.excellentLoss);
        if (!strong && !worse(*best, *played, policy.review.classification.excellentLoss)) {
            return AlternativeGrade::Unavailable;
        }
        if (acceptable && *acceptable != strong) {
            return AlternativeGrade::Unavailable;
        }
        acceptable = strong;
    }
    return *acceptable ? AlternativeGrade::Strong : AlternativeGrade::TryAgain;
}
AlternativeGrade gradeKnownMove(const chess::Position& root, chess::Move move, const Policy& policy,
                                const review::PositionEvidence& older,
                                const review::PositionEvidence& newer) {
    const int width = static_cast<int>(std::min<std::size_t>(4, root.legalMoves().size()));
    if (!validPolicy(policy) || !root.isLegal(move) || !usable(root, older, width, policy) ||
        !usable(root, newer, width, policy) || !stable({&older, &newer, {}}, policy)) {
        return AlternativeGrade::Unavailable;
    }
    std::optional<AlternativeGrade> result;
    for (const auto* pass : {&older, &newer}) {
        const auto found = std::find_if(
            pass->candidates.begin(), pass->candidates.end(),
            [&](const auto& candidate) { return candidate.continuation.front() == move; });
        if (found == pass->candidates.end()) {
            return AlternativeGrade::Unavailable;
        }
        const auto best = value(pass->evaluation, policy.review.conversion);
        const auto candidate = value(found->evaluation, policy.review.conversion);
        if (!best || !candidate) {
            return AlternativeGrade::Unavailable;
        }
        const auto grade = close(*best, *candidate, policy.review.classification.excellentLoss)
                               ? AlternativeGrade::Strong
                           : worse(*best, *candidate, policy.review.classification.excellentLoss)
                               ? AlternativeGrade::TryAgain
                               : AlternativeGrade::Unavailable;
        if (result && *result != grade) {
            return AlternativeGrade::Unavailable;
        }
        result = grade;
    }
    return result.value_or(AlternativeGrade::Unavailable);
}
} // namespace chess_review::puzzles
