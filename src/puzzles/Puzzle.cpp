#include "puzzles/Puzzle.h"
#include "review/Intelligence.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace chess_review::puzzles {
namespace {
struct Value {
    int mate = 0; // Winning side (+1 / -1 / 0).
    int mateDistance = 0;
    double score = 0;
    std::optional<int> ticks;
    review::ConversionSource source = review::ConversionSource::ApproximateCentipawns;
};
int pieceValue(chess::PieceType type) {
    constexpr std::array<int, 6> values{1, 3, 3, 5, 9, 0};
    return values[static_cast<std::size_t>(type)];
}
chess::Color opposite(chess::Color color) {
    return color == chess::Color::White ? chess::Color::Black : chess::Color::White;
}
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
        const auto rawAbs = raw.score->value == std::numeric_limits<std::int64_t>::min()
                                ? std::numeric_limits<std::int64_t>::max()
                                : std::abs(raw.score->value);
        const int distance =
            static_cast<int>(std::min<std::int64_t>(rawAbs, std::numeric_limits<int>::max()));
        return Value{raw.score->value > 0 ? 1 : -1, distance, 0, {}, {}};
    }
    const auto converted = review::convertEvaluation(raw, raw.perspective, config);
    if (converted.issue || !converted.expected) {
        return {};
    }
    const auto& score = *converted.expected;
    return Value{0, 0, score.value,
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
bool closeV2(const Value& a, const Value& b, double tolerance, int maxMateMoves) {
    if (a.mate || b.mate) {
        if (a.mate != b.mate) {
            return false;
        }
        if (a.mate > 0 && (a.mateDistance <= maxMateMoves || b.mateDistance <= maxMateMoves)) {
            return a.mateDistance == b.mateDistance;
        }
        return true;
    }
    return close(a, b, tolerance);
}
bool worseV2(const Value& best, const Value& candidate, double gap, int maxMateMoves) {
    if (best.mate || candidate.mate) {
        if (best.mate != candidate.mate) {
            return best.mate > candidate.mate;
        }
        if (best.mate > 0 && best.mateDistance <= maxMateMoves) {
            return candidate.mateDistance > maxMateMoves ||
                   candidate.mateDistance >= best.mateDistance + 2;
        }
        return false;
    }
    return worse(best, candidate, gap);
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
bool defensivePositionV2(const review::PositionEvidence& evidence, const Policy& policy) {
    if (evidence.candidates.size() < 2) {
        return false;
    }
    const auto best = value(evidence.evaluation, policy.review.conversion);
    const auto second = value(evidence.candidates[1].evaluation, policy.review.conversion);
    return best && second && best->mate == 0 && best->score >= 0.45 &&
           (second->mate < 0 || (second->mate == 0 && second->score <= 0.40 &&
                                 worseV2(*best, *second, policy.review.special.alternativeGap,
                                         policy.maximumPlayerMoves)));
}
bool isBalancingRecapture(const chess::Position& position, chess::Move move) {
    if (position.context().moves.empty() || move.promotion) {
        return false;
    }
    auto previous = position;
    const auto last = previous.undo();
    if (!last || move.to != last->to || !position.pieceAt(last->to)) {
        return false;
    }
    const auto side = position.sideToMove();
    if (material(position, side) >= material(previous, side)) {
        return false;
    }
    auto after = position;
    if (std::holds_alternative<chess::Error>(after.play(move))) {
        return false;
    }
    return !after.inCheck() && material(after, side) <= material(previous, side);
}
bool hasFork(const chess::Position& root, chess::Move move, bool* isChecking = nullptr) {
    if (isChecking) {
        *isChecking = false;
    }
    auto after = root;
    const auto side = root.sideToMove();
    const auto defender = opposite(side);
    if (std::holds_alternative<chess::Error>(after.play(move))) {
        return false;
    }
    const auto moverPiece = after.pieceAt(move.to);
    if (!moverPiece) {
        return false;
    }
    const int moverValue = pieceValue(moverPiece->type);
    const bool checking = after.inCheck();
    if (isChecking) {
        *isChecking = checking;
    }
    std::vector<chess::Square> targets;
    bool higherValueTarget = false;
    bool undefendedTarget = false;
    for (int index = 0; index < 64; ++index) {
        const auto square = *chess::Square::fromCoordinates(index % 8, index / 8);
        const auto piece = after.pieceAt(square);
        if (!piece || piece->color != defender || piece->type == chess::PieceType::Pawn ||
            !after.attacks(move.to, square)) {
            continue;
        }
        targets.push_back(square);
        if (piece->type == chess::PieceType::King) {
            if (checking) {
                higherValueTarget = true;
            }
            continue;
        }
        if (pieceValue(piece->type) > moverValue) {
            higherValueTarget = true;
        }
        if (!after.isAttacked(square, defender)) {
            undefendedTarget = true;
        }
    }
    if (targets.size() < 2) {
        return false;
    }
    if (after.isAttacked(move.to, defender)) {
        return checking || higherValueTarget;
    }
    if ((moverPiece->type == chess::PieceType::Queen ||
         moverPiece->type == chess::PieceType::Rook) &&
        !checking) {
        return undefendedTarget || higherValueTarget;
    }
    return true;
}
bool isHangingCapture(const chess::Position& root, chess::Move move) {
    const auto victim = root.pieceAt(move.to);
    if (!victim || pieceValue(victim->type) < 3 || root.isAttacked(move.to, victim->color) ||
        isBalancingRecapture(root, move)) {
        return false;
    }
    auto after = root;
    if (std::holds_alternative<chess::Error>(after.play(move))) {
        return false;
    }
    return !after.isAttacked(move.to, victim->color);
}
std::optional<int> detectMaterialWin(const chess::Position& root, std::span<const chess::Move> pv,
                                     chess::Color side, const Policy& policy) {
    if (pv.empty() || isBalancingRecapture(root, pv.front())) {
        return {};
    }
    const std::size_t maxPlies = std::min<std::size_t>(
        pv.size(), static_cast<std::size_t>(policy.maximumPlayerMoves) * 2 + 4);
    auto board = root;
    const int start = material(root, side);
    std::vector<int> deltas;
    deltas.reserve(maxPlies);
    for (std::size_t ply = 0; ply < maxPlies; ++ply) {
        if (std::holds_alternative<chess::Error>(board.play(pv[ply]))) {
            return {};
        }
        deltas.push_back(material(board, side) - start);
    }
    const auto victim = root.pieceAt(pv[0].to);
    const bool immediateUndefended =
        victim && !root.isAttacked(pv[0].to, opposite(side)) && !pv[0].promotion;
    if (deltas.size() == 1) {
        if (deltas[0] >= policy.immediateMaterialGain) {
            return policy.immediateMaterialGain;
        }
        return {};
    }
    const bool matedAtEnd =
        board.inCheck() && board.legalMoves().empty() && board.sideToMove() != side;
    const int endDelta =
        matedAtEnd ? std::numeric_limits<int>::max() / 4
                   : (deltas.size() % 2 == 1 ? deltas.back()
                                             : std::max(deltas.back(), deltas[deltas.size() - 2]));
    const std::size_t scanLimit = std::min<std::size_t>(
        deltas.size(), static_cast<std::size_t>(policy.maximumPlayerMoves) * 2);
    for (std::size_t evenPly = 2; evenPly <= scanLimit; evenPly += 2) {
        const int deltaAtP = deltas[evenPly - 1];
        const bool nextPairHolds =
            evenPly + 2 > deltas.size() ||
            std::max(deltas[evenPly], deltas[evenPly + 1]) >= policy.minimumMaterialGain;
        if (deltaAtP < policy.minimumMaterialGain || endDelta < policy.minimumMaterialGain ||
            !nextPairHolds) {
            continue;
        }
        bool laterIncreases = false;
        for (std::size_t q = evenPly + 2; q <= scanLimit; q += 2) {
            if (deltas[q - 1] > deltaAtP) {
                laterIncreases = true;
                break;
            }
        }
        const int target = (evenPly == 2 && immediateUndefended && !laterIncreases)
                               ? policy.immediateMaterialGain
                               : policy.minimumMaterialGain;
        const bool nextPairTarget =
            evenPly + 2 > deltas.size() || std::max(deltas[evenPly], deltas[evenPly + 1]) >= target;
        if (deltaAtP >= target && endDelta >= target && nextPairTarget) {
            return target;
        }
    }
    return {};
}
bool hasTacticalSignature(const chess::Position& root, std::span<const chess::Move> pv,
                          chess::Color side) {
    if (pv.empty()) {
        return false;
    }
    const auto first = pv.front();
    if (first.promotion || hasFork(root, first)) {
        return true;
    }
    auto afterFirst = root;
    if (std::holds_alternative<chess::Error>(afterFirst.play(first))) {
        return false;
    }
    if (afterFirst.inCheck()) {
        return true;
    }
    if (root.pieceAt(first.to) && !isBalancingRecapture(root, first)) {
        return true;
    }
    const auto prefix = pv.first(std::min(pv.size(), review::tacticalLineLimit));
    const auto facts = review::inspectTacticalLine(root, prefix, review::FactSource::Best);
    return std::any_of(facts.begin(), facts.end(), [&](const auto& f) {
        return f.beneficiary == side && f.kind == review::TacticalKind::Sacrifice &&
               f.material >= 2;
    });
}
int maxEnPrisePieceValue(const chess::Position& position, chess::Color side) {
    const auto enemy = opposite(side);
    int worst = 0;
    for (int index = 0; index < 64; ++index) {
        const auto sq = *chess::Square::fromCoordinates(index % 8, index / 8);
        const auto piece = position.pieceAt(sq);
        if (!piece || piece->color != side || piece->type == chess::PieceType::King) {
            continue;
        }
        if (!position.isAttacked(sq, enemy)) {
            continue;
        }
        const int mine = pieceValue(piece->type);
        if (!position.isAttacked(sq, side)) {
            worst = std::max(worst, mine);
            continue;
        }
        for (int oppIndex = 0; oppIndex < 64; ++oppIndex) {
            const auto oppSq = *chess::Square::fromCoordinates(oppIndex % 8, oppIndex / 8);
            const auto opp = position.pieceAt(oppSq);
            if (opp && opp->color == enemy && position.attacks(oppSq, sq) &&
                pieceValue(opp->type) < mine) {
                worst = std::max(worst, mine - pieceValue(opp->type));
            }
        }
    }
    return worst;
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
std::optional<std::vector<chess::Move>> solutionsV2(const review::PositionEvidence& evidence,
                                                    const Policy& policy, bool separation,
                                                    bool solver, int maxMoves) {
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
        if (!solver) {
            continue;
        }
        if (closeV2(*best, *score, c.classification.excellentLoss, maxMoves)) {
            result.push_back(candidate.continuation.front());
        } else if (separation && !worseV2(*best, *score, c.special.alternativeGap, maxMoves)) {
            return {};
        }
    }
    if (!solver) {
        return std::vector{evidence.candidates.front().continuation.front()};
    }
    if (result.empty() || (separation && result.size() > 3)) {
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
bool evalPairStableV2(const Value& x, const Value& y, double baseDrift, int maxMoves,
                      bool exactMateDistance) {
    if (x.mate > 0 && y.mate > 0) {
        if (x.mateDistance <= maxMoves || y.mateDistance <= maxMoves) {
            return exactMateDistance ? (x.mateDistance == y.mateDistance)
                                     : (std::abs(x.mateDistance - y.mateDistance) <= 1);
        }
        return true;
    }
    if (x.mate < 0 && y.mate < 0) {
        return true;
    }
    if ((x.mate > 0 && y.mate == 0 && y.score >= 0.85) ||
        (y.mate > 0 && x.mate == 0 && x.score >= 0.85)) {
        return true;
    }
    if (x.mate == 0 && y.mate == 0) {
        const double drift =
            (x.score >= 0.72 && y.score >= 0.72) ? std::max(baseDrift, 0.12) : baseDrift;
        return close(x, y, drift);
    }
    return false;
}
bool stableV2(const Pair& pair, std::span<const chess::Move> solverAccepted, const Policy& policy,
              bool solver, bool root, int maxMoves) {
    if (pair.older->attemptIdentity == pair.newer->attemptIdentity ||
        pair.older->engineIdentity != pair.newer->engineIdentity) {
        return false;
    }
    const double baseDrift = std::max(policy.review.refinement.maximumDrift, 0.05);
    if (solver) {
        for (const auto move : solverAccepted) {
            const auto findMove = [&](const review::PositionEvidence& e) {
                return std::find_if(e.candidates.begin(), e.candidates.end(), [&](const auto& c) {
                    return !c.continuation.empty() && c.continuation.front() == move;
                });
            };
            const auto a = findMove(*pair.older);
            const auto b = findMove(*pair.newer);
            if (a == pair.older->candidates.end() || b == pair.newer->candidates.end()) {
                return false;
            }
            const auto x = value(a->evaluation, policy.review.conversion);
            const auto y = value(b->evaluation, policy.review.conversion);
            if (!x || !y || !evalPairStableV2(*x, *y, baseDrift, maxMoves, root)) {
                return false;
            }
        }
        return true;
    }
    const auto oldBest = value(pair.older->evaluation, policy.review.conversion);
    const auto newBest = value(pair.newer->evaluation, policy.review.conversion);
    if (!oldBest || !newBest) {
        return false;
    }
    const bool bothLosing = (oldBest->mate < 0 || oldBest->score <= 0.28) &&
                            (newBest->mate < 0 || newBest->score <= 0.28);
    if (!bothLosing && !close(*oldBest, *newBest, baseDrift)) {
        return false;
    }
    if (pair.newer->candidates.empty() || pair.newer->candidates.front().continuation.empty()) {
        return false;
    }
    const auto chosen = pair.newer->candidates.front().continuation.front();
    const auto foundOld = std::find_if(
        pair.older->candidates.begin(), pair.older->candidates.end(),
        [&](const auto& c) { return !c.continuation.empty() && c.continuation.front() == chosen; });
    if (!bothLosing && foundOld != pair.older->candidates.end()) {
        const auto oldChosen = value(foundOld->evaluation, policy.review.conversion);
        if (!oldChosen || !close(*oldBest, *oldChosen, baseDrift)) {
            return false;
        }
    }
    return true;
}
bool pairReadyForGrading(const review::PositionEvidence& older,
                         const review::PositionEvidence& newer, const Policy& policy) {
    if (older.attemptIdentity == newer.attemptIdentity ||
        older.engineIdentity != newer.engineIdentity || older.candidates.empty() ||
        newer.candidates.empty()) {
        return false;
    }
    const auto x = value(older.evaluation, policy.review.conversion);
    const auto y = value(newer.evaluation, policy.review.conversion);
    if (!x || !y) {
        return false;
    }
    const double baseDrift = std::max(policy.review.refinement.maximumDrift, 0.05);
    return evalPairStableV2(*x, *y, baseDrift, policy.maximumPlayerMoves, false);
}
bool hasSeparationV2(const review::PositionEvidence& evidence,
                     std::span<const chess::Move> accepted, const Policy& policy, double minGap,
                     int maxMoves) {
    const auto best = value(evidence.evaluation, policy.review.conversion);
    if (!best) {
        return false;
    }
    for (const auto& candidate : evidence.candidates) {
        if (candidate.continuation.empty()) {
            continue;
        }
        const auto m = candidate.continuation.front();
        if (std::find(accepted.begin(), accepted.end(), m) != accepted.end()) {
            continue;
        }
        const auto score = value(candidate.evaluation, policy.review.conversion);
        return score && worseV2(*best, *score, minGap, maxMoves);
    }
    return false;
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
        if (!supportsModelVersion(definition_.modelVersion) || !validPolicy(definition_.policy) ||
            root_.context() != definition_.position ||
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
        const int maximumMoves =
            continuation_ ? continuation_->remainingPlayerMoves : policy.maximumPlayerMoves;
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
                if (definition_.modelVersion == 1) {
                    auto a = solutions(*older, policy, separated, solver);
                    auto b = solutions(*found, policy, separated, solver);
                    if (a && b && sameMoves(*a, *b) && stable(pair, policy)) {
                        pair.accepted = std::move(*b);
                        return pair;
                    }
                } else {
                    auto a = solutionsV2(*older, policy, separated, solver, maximumMoves);
                    auto b = solutionsV2(*found, policy, separated, solver, maximumMoves);
                    if (solver) {
                        if (a && b && sameMoves(*a, *b) &&
                            stableV2(pair, *b, policy, true, root, maximumMoves)) {
                            pair.accepted = std::move(*b);
                            return pair;
                        }
                    } else if (a && b && stableV2(pair, {}, policy, false, root, maximumMoves)) {
                        pair.accepted = std::move(*b);
                        return pair;
                    }
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
        const auto& policy = definition_.policy;
        const auto bestA = value(pair.older->evaluation, policy.review.conversion);
        const auto bestB = value(pair.newer->evaluation, policy.review.conversion);
        if (definition_.modelVersion == 1) {
            if (bestA && bestB && bestA->mate > 0 && bestB->mate > 0) {
                result_.solution.goal = Goal::Mate;
                return true;
            }
            const auto factsA = review::inspectTacticalLine(
                position, pair.older->candidates.front().continuation, review::FactSource::Best);
            const auto factsB = review::inspectTacticalLine(
                position, pair.newer->candidates.front().continuation, review::FactSource::Best);
            for (const auto& b : factsB) {
                const int minimum =
                    b.linePlies <= 1 ? policy.immediateMaterialGain : policy.minimumMaterialGain;
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
            if (pair.accepted.size() == 1 && defensivePosition(*pair.older, policy) &&
                defensivePosition(*pair.newer, policy)) {
                result_.solution.goal = Goal::Defence;
                return true;
            }
            return false;
        }
        const int maxMoves = policy.maximumPlayerMoves;
        if (bestA && bestB && bestA->mate > 0 && bestB->mate > 0 &&
            bestA->mateDistance <= maxMoves && bestB->mateDistance <= maxMoves) {
            result_.solution.goal = Goal::Mate;
            return true;
        }
        const auto& pvA = pair.older->candidates.front().continuation;
        const auto& pvB = pair.newer->candidates.front().continuation;
        if (pvA.empty() || pvB.empty() || isBalancingRecapture(position, pvB.front())) {
            return false;
        }
        const auto targetA = detectMaterialWin(position, pvA, side_, policy);
        const auto targetB = detectMaterialWin(position, pvB, side_, policy);
        if (targetA && targetB) {
            result_.solution.goal = Goal::Material;
            result_.solution.materialTarget = std::max(*targetA, *targetB);
            return true;
        }
        if (hasFork(position, pvA.front()) && hasFork(position, pvB.front()) && bestA && bestB &&
            (bestA->mate > 0 || bestA->score >= 0.65) &&
            (bestB->mate > 0 || bestB->score >= 0.65)) {
            result_.solution.goal = Goal::Material;
            result_.solution.materialTarget = policy.minimumMaterialGain;
            return true;
        }
        if (pair.accepted.size() <= 2 && defensivePositionV2(*pair.older, policy) &&
            defensivePositionV2(*pair.newer, policy)) {
            result_.solution.goal = Goal::Defence;
            return true;
        }
        const double tacticGap = std::max(policy.review.special.alternativeGap, 0.12);
        const bool winning = bestA && bestB && (bestA->mate > 0 || bestA->score >= 0.70) &&
                             (bestB->mate > 0 || bestB->score >= 0.70);
        const bool separated =
            hasSeparationV2(*pair.older, pair.accepted, policy, tacticGap, maxMoves) &&
            hasSeparationV2(*pair.newer, pair.accepted, policy, tacticGap, maxMoves);
        const bool forcedLongMate = bestA && bestB && bestA->mate > 0 && bestB->mate > 0;
        if (winning && separated &&
            (forcedLongMate || (hasTacticalSignature(position, pvA, side_) &&
                                hasTacticalSignature(position, pvB, side_)))) {
            result_.solution.goal = Goal::Tactic;
            return true;
        }
        return false;
    }
    bool uniquelyForcing(const Pair& pair) const {
        const auto& policy = definition_.policy;
        const int maxMoves =
            continuation_ ? continuation_->remainingPlayerMoves : policy.maximumPlayerMoves;
        return pair.accepted.size() <= 2 &&
               hasSeparationV2(*pair.newer, pair.accepted, policy,
                               policy.review.special.alternativeGap, maxMoves);
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
        const int maximumMoves = continuation_ ? continuation_->remainingPlayerMoves
                                               : definition_.policy.maximumPlayerMoves;
        if (definition_.modelVersion == 1) {
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
        } else if (!path.empty() && solver && !position.inCheck()) {
            const bool bothQuiet = quiet(position, *pair->older) && quiet(position, *pair->newer);
            const bool forcing = uniquelyForcing(*pair);
            const int gain = material(position, side_) - startingMaterial_;
            const auto a = value(pair->older->evaluation, definition_.policy.review.conversion);
            const auto b = value(pair->newer->evaluation, definition_.policy.review.conversion);
            const bool winningEval =
                a && b && (a->mate > 0 || a->score >= 0.60) && (b->mate > 0 || b->score >= 0.60);
            const bool strongWinningEval =
                a && b && (a->mate > 0 || a->score >= 0.68) && (b->mate > 0 || b->score >= 0.68);
            const bool safeGain =
                gain >= result_.solution.materialTarget &&
                (gain - maxEnPrisePieceValue(position, side_) >= result_.solution.materialTarget);
            const bool defended =
                goal == Goal::Defence && a && b && (a->mate > 0 || a->score >= 0.45) &&
                (b->mate > 0 || b->score >= 0.45) &&
                (bothQuiet || (!defensivePositionV2(*pair->older, definition_.policy) &&
                               !defensivePositionV2(*pair->newer, definition_.policy)));
            const bool complete =
                (goal == Goal::Practice && (bothQuiet || (playerMoves >= 2 && !forcing))) ||
                defended ||
                (goal == Goal::Material && gain >= result_.solution.materialTarget && winningEval &&
                 (bothQuiet || safeGain || !forcing)) ||
                (goal == Goal::Tactic && strongWinningEval && !forcing &&
                 (bothQuiet || playerMoves >= 2));
            if (complete) {
                result_.solution.nodes[index].complete = true;
                return index;
            }
        }
        if (solver && (playerMoves >= maximumMoves ||
                       (definition_.modelVersion >= 2 && pair->accepted.size() > 3))) {
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
            if (definition_.modelVersion == 1) {
                if (goal == Goal::Mate) {
                    hint = review::RetryIdea::Checkmate;
                } else if (goal == Goal::Material) {
                    hint = review::RetryIdea::Material;
                }
            } else {
                bool checkingFork = false;
                if (goal == Goal::Mate) {
                    hint = review::RetryIdea::Checkmate;
                } else if (hasFork(position, accepted.front(), &checkingFork) && checkingFork) {
                    hint = review::RetryIdea::Fork;
                } else if (accepted.front().promotion) {
                    hint = review::RetryIdea::Promotion;
                } else if (goal == Goal::Material && isHangingCapture(position, accepted.front())) {
                    hint = review::RetryIdea::HangingPiece;
                } else if (goal == Goal::Material) {
                    hint = review::RetryIdea::Material;
                } else {
                    hint = review::RetryIdea::General;
                }
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
std::optional<Goal> candidateGoal(const chess::Position& root, const review::MoveEvidence* older,
                                  const review::MoveEvidence& newer, const Policy& policy,
                                  CandidateContext context) {
    if (!validPolicy(policy) || root.legalMoves().size() < 2) {
        return {};
    }
    if (older && (older->playedMove != newer.playedMove ||
                  older->before.engineIdentity != newer.before.engineIdentity ||
                  older->before.attemptIdentity == newer.before.attemptIdentity)) {
        return {};
    }
    const auto b = review::pairEvidence(root, newer.playedMove, newer.before, newer.after,
                                        policy.review.conversion);
    if (b.issue && b.issue != review::EvidenceIssue::MateClaim) {
        return {};
    }
    std::optional<review::MoveEvidence> a;
    if (older) {
        a = review::pairEvidence(root, older->playedMove, older->before, older->after,
                                 policy.review.conversion);
        if (a->issue && a->issue != review::EvidenceIssue::MateClaim) {
            return {};
        }
    }
    if (b.before.candidates.empty() || b.before.candidates.front().continuation.empty()) {
        return {};
    }
    if (a && (a->before.candidates.empty() || a->before.candidates.front().continuation.empty())) {
        return {};
    }
    const auto side = root.sideToMove();
    const auto bestB = value(b.before.evaluation, policy.review.conversion);
    if (!bestB) {
        return {};
    }
    std::optional<Value> bestA;
    if (a) {
        bestA = value(a->before.evaluation, policy.review.conversion);
        if (!bestA) {
            return {};
        }
    }
    if (bestB->mate > 0 && (!bestA || bestA->mate > 0)) {
        if (context.isError || bestB->mateDistance <= policy.maximumPlayerMoves) {
            return Goal::Mate;
        }
    }
    const auto& pvB = b.before.candidates.front().continuation;
    if (isBalancingRecapture(root, pvB.front())) {
        return {};
    }
    if (b.before.candidates.size() >= 2 && defensivePositionV2(b.before, policy) &&
        (!a || a->before.candidates.size() < 2 || defensivePositionV2(a->before, policy))) {
        return Goal::Defence;
    }
    if (!a && b.before.candidates.size() < 2 && context.isError &&
        (root.inCheck() || root.legalMoves().size() <= 4) && bestB->mate == 0 &&
        bestB->score >= 0.45) {
        if (b.after.terminal == review::TerminalOutcome::Checkmate) {
            return Goal::Defence;
        }
        auto afterRaw = b.after.evaluation;
        if (afterRaw.score && afterRaw.score->value != std::numeric_limits<std::int64_t>::min()) {
            afterRaw.score->value = -afterRaw.score->value;
        }
        if (afterRaw.wdl) {
            std::swap(afterRaw.wdl->wins, afterRaw.wdl->losses);
        }
        afterRaw.perspective = side;
        const auto afterVal = value(afterRaw, policy.review.conversion);
        if (afterVal && (afterVal->mate < 0 || (afterVal->mate == 0 && afterVal->score <= 0.38))) {
            return Goal::Defence;
        }
    }
    const auto hasMaterialOrTacticalFact =
        [&](const chess::Position& pos, std::span<const chess::Move> pv, const Value& bestVal) {
            if (detectMaterialWin(pos, pv, side, policy).has_value()) {
                return true;
            }
            if (hasFork(pos, pv.front()) && (bestVal.mate > 0 || bestVal.score >= 0.62)) {
                return true;
            }
            const auto victim = pos.pieceAt(pv.front().to);
            if (isHangingCapture(pos, pv.front()) && victim &&
                pieceValue(victim->type) >= policy.immediateMaterialGain) {
                return true;
            }
            const auto facts = review::inspectTacticalLine(
                pos, pv.first(std::min(pv.size(), review::tacticalLineLimit)),
                review::FactSource::Best);
            return std::any_of(facts.begin(), facts.end(), [&](const auto& f) {
                return f.beneficiary == side && (f.kind == review::TacticalKind::Promotion ||
                                                 f.kind == review::TacticalKind::Exchange);
            });
        };
    const bool olderWinning = !bestA || bestA->mate > 0 || bestA->score >= 0.58;
    if (olderWinning && hasMaterialOrTacticalFact(root, pvB, *bestB)) {
        if (!a ||
            hasMaterialOrTacticalFact(root, a->before.candidates.front().continuation, *bestA)) {
            return Goal::Material;
        }
    }
    const bool winning = bestB->mate > 0 || bestB->score >= 0.68;
    const bool olderStrong = !bestA || bestA->mate > 0 || bestA->score >= 0.62;
    const auto hasCandidateGap = [&](const review::PositionEvidence& ev, const Value& top) {
        if (ev.candidates.size() < 2) {
            return true;
        }
        const auto worst = value(ev.candidates.back().evaluation, policy.review.conversion);
        return worst && worseV2(top, *worst, policy.review.special.alternativeGap,
                                policy.maximumPlayerMoves);
    };
    if (winning && olderStrong && (context.isSpecial || hasCandidateGap(b.before, *bestB)) &&
        (hasTacticalSignature(root, pvB, side) || context.isSpecial)) {
        if (!a || context.isSpecial ||
            hasTacticalSignature(root, a->before.candidates.front().continuation, side)) {
            return Goal::Tactic;
        }
    }
    return {};
}
std::optional<Goal> candidateGoal(const chess::Position& root, const review::MoveEvidence& older,
                                  const review::MoveEvidence& newer, const Policy& policy) {
    return candidateGoal(root, &older, newer, policy, {});
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
        !pairReadyForGrading(olderRoot, newerRoot, policy) ||
        olderChild.attemptIdentity == newerChild.attemptIdentity ||
        olderRoot.engineIdentity != olderChild.engineIdentity ||
        newerRoot.engineIdentity != newerChild.engineIdentity) {
        return AlternativeGrade::Unavailable;
    }
    if (childWidth > 0 && (!usable(child, olderChild, childWidth, policy) ||
                           !usable(child, newerChild, childWidth, policy) ||
                           !pairReadyForGrading(olderChild, newerChild, policy))) {
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
                           child.inCheck() ? 1 : 0,
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
        const bool strong = closeV2(*best, *played, policy.review.classification.excellentLoss,
                                    policy.maximumPlayerMoves);
        if (!strong && !worseV2(*best, *played, policy.review.classification.excellentLoss,
                                policy.maximumPlayerMoves)) {
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
        !usable(root, newer, width, policy) || !pairReadyForGrading(older, newer, policy)) {
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
        const auto grade = closeV2(*best, *candidate, policy.review.classification.excellentLoss,
                                   policy.maximumPlayerMoves)
                               ? AlternativeGrade::Strong
                           : worseV2(*best, *candidate, policy.review.classification.excellentLoss,
                                     policy.maximumPlayerMoves)
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
