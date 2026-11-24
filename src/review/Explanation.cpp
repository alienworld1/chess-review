#include "review/Explanation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <variant>

namespace chess_review::review {
namespace {

int pieceValue(chess::PieceType type) {
    constexpr std::array<int, 6> values{1, 3, 3, 5, 9, 0};
    return values[static_cast<std::size_t>(type)];
}

chess::Color opposite(chess::Color color) {
    return color == chess::Color::White ? chess::Color::Black : chess::Color::White;
}

int materialBalance(const chess::Position& position, chess::Color side) {
    int result = 0;
    for (const auto& piece : position.boardIdentity().pieces) {
        if (piece) {
            result += (piece->color == side ? 1 : -1) * pieceValue(piece->type);
        }
    }
    return result;
}

std::optional<chess::PieceType> capturedPieceType(const chess::Position& board, chess::Move move) {
    if (const auto victim = board.pieceAt(move.to)) {
        return victim->type;
    }
    const auto actor = board.pieceAt(move.from);
    if (actor && actor->type == chess::PieceType::Pawn && move.from.file() != move.to.file()) {
        return chess::PieceType::Pawn;
    }
    return std::nullopt;
}

std::optional<chess::Square> findKing(const chess::Position& pos, chess::Color color) {
    for (int index = 0; index < 64; ++index) {
        const auto sq = *chess::Square::fromCoordinates(index % 8, index / 8);
        if (const auto p = pos.pieceAt(sq);
            p && p->color == color && p->type == chess::PieceType::King) {
            return sq;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> rayDirection(chess::Square from, chess::Square to) {
    const int df = to.file() - from.file();
    const int dr = to.rank() - from.rank();
    if (df == 0 && dr == 0) {
        return std::nullopt;
    }
    if (df == 0) {
        return std::pair{0, dr > 0 ? 1 : -1};
    }
    if (dr == 0) {
        return std::pair{df > 0 ? 1 : -1, 0};
    }
    if (std::abs(df) == std::abs(dr)) {
        return std::pair{df > 0 ? 1 : -1, dr > 0 ? 1 : -1};
    }
    return std::nullopt;
}

bool sliderMatchesDirection(chess::PieceType type, std::pair<int, int> dir) {
    const bool diagonal = dir.first != 0 && dir.second != 0;
    if (type == chess::PieceType::Queen) {
        return true;
    }
    return diagonal ? type == chess::PieceType::Bishop : type == chess::PieceType::Rook;
}

bool isPinnedOffTarget(const chess::Position& pos, chess::Square pieceSq, chess::Color pieceColor,
                       chess::Square targetSq) {
    const auto kingSq = findKing(pos, pieceColor);
    if (!kingSq || *kingSq == pieceSq) {
        return false;
    }
    const auto dir = rayDirection(*kingSq, pieceSq);
    if (!dir) {
        return false;
    }
    int f = kingSq->file() + dir->first;
    int r = kingSq->rank() + dir->second;
    bool foundSelf = false;
    while (f >= 0 && f < 8 && r >= 0 && r < 8) {
        const auto sq = *chess::Square::fromCoordinates(f, r);
        if (!foundSelf) {
            if (sq == pieceSq) {
                foundSelf = true;
            } else if (pos.pieceAt(sq)) {
                return false;
            }
        } else if (const auto p = pos.pieceAt(sq)) {
            if (p->color != pieceColor && sliderMatchesDirection(p->type, *dir)) {
                return rayDirection(*kingSq, targetSq) != dir;
            }
            return false;
        }
        f += dir->first;
        r += dir->second;
    }
    return false;
}

int staticExchange(chess::Position board, chess::Square target, chess::Color perspective,
                   int referenceBalance) {
    std::array<int, 17> balances{};
    std::array<chess::Color, 17> turn{};
    std::size_t count = 1;
    balances[0] = materialBalance(board, perspective) - referenceBalance;
    turn[0] = board.sideToMove();
    for (int depth = 0; depth < 16 && count < balances.size(); ++depth) {
        if (!board.pieceAt(target)) {
            break;
        }
        const auto legal = board.legalMoves();
        const chess::Move* cheapest = nullptr;
        int cheapestVal = 100;
        for (const auto& m : legal) {
            if (m.to != target) {
                continue;
            }
            if (m.promotion && *m.promotion != chess::PieceType::Queen) {
                continue;
            }
            const auto attacker = board.pieceAt(m.from);
            if (!attacker) {
                continue;
            }
            const int v =
                attacker->type == chess::PieceType::King ? 10 : pieceValue(attacker->type);
            if (v < cheapestVal) {
                cheapestVal = v;
                cheapest = &m;
            }
        }
        if (!cheapest) {
            break;
        }
        if (std::holds_alternative<chess::Error>(board.play(*cheapest))) {
            break;
        }
        balances[count] = materialBalance(board, perspective) - referenceBalance;
        turn[count] = board.sideToMove();
        ++count;
    }
    int score = balances[count - 1];
    for (std::size_t i = count - 1; i > 0; --i) {
        const std::size_t idx = i - 1;
        const int standPat = balances[idx];
        score = turn[idx] == perspective ? std::max(standPat, score) : std::min(standPat, score);
    }
    return score;
}

int seeOnAfter(const chess::Position& after, chess::Square target, chess::Color mover) {
    return staticExchange(after, target, mover, materialBalance(after, mover));
}

bool staticSquareDanger(const chess::Position& pos, chess::Square sq, chess::Color pieceColor) {
    const auto target = pos.pieceAt(sq);
    if (!target || target->color != pieceColor || target->type == chess::PieceType::Pawn ||
        target->type == chess::PieceType::King) {
        return false;
    }
    const auto opp = opposite(pieceColor);
    std::array<int, 16> attackers{};
    std::array<int, 16> defenders{};
    std::size_t attCount = 0;
    std::size_t defCount = 0;
    bool enemyHasKing = false;
    bool friendlyHasKing = false;
    for (int index = 0; index < 64; ++index) {
        const auto s = *chess::Square::fromCoordinates(index % 8, index / 8);
        if (s == sq) {
            continue;
        }
        const auto p = pos.pieceAt(s);
        if (!p || !pos.attacks(s, sq)) {
            continue;
        }
        if (p->color == opp) {
            if (isPinnedOffTarget(pos, s, opp, sq)) {
                continue;
            }
            if (p->type == chess::PieceType::King) {
                enemyHasKing = true;
            } else if (attCount < attackers.size()) {
                attackers[attCount++] = pieceValue(p->type);
            }
        } else {
            if (isPinnedOffTarget(pos, s, pieceColor, sq)) {
                continue;
            }
            if (p->type == chess::PieceType::King) {
                friendlyHasKing = true;
            } else if (defCount < defenders.size()) {
                defenders[defCount++] = pieceValue(p->type);
            }
        }
    }
    if (attCount == 0 && !(enemyHasKing && defCount == 0 && !friendlyHasKing)) {
        return false;
    }
    std::sort(attackers.begin(), attackers.begin() + static_cast<std::ptrdiff_t>(attCount));
    std::sort(defenders.begin(), defenders.begin() + static_cast<std::ptrdiff_t>(defCount));

    std::array<int, 33> balances{};
    std::array<bool, 33> turnIsFriendly{};
    std::size_t count = 1;
    balances[0] = 0;
    turnIsFriendly[0] = false;
    int onSquare = pieceValue(target->type);
    std::size_t attIdx = 0;
    std::size_t defIdx = 0;
    bool enemyKingUsed = false;
    bool friendlyKingUsed = false;

    while (count < balances.size()) {
        if (!turnIsFriendly[count - 1]) {
            int val = 0;
            if (attIdx < attCount) {
                val = attackers[attIdx++];
            } else if (enemyHasKing && !enemyKingUsed && defIdx == defCount && !friendlyHasKing) {
                enemyKingUsed = true;
                val = 10;
            } else {
                break;
            }
            balances[count] = balances[count - 1] - onSquare;
            onSquare = val;
            turnIsFriendly[count] = true;
            ++count;
        } else {
            int val = 0;
            if (defIdx < defCount) {
                val = defenders[defIdx++];
            } else if (friendlyHasKing && !friendlyKingUsed && attIdx == attCount &&
                       !enemyHasKing) {
                friendlyKingUsed = true;
                val = 10;
            } else {
                break;
            }
            balances[count] = balances[count - 1] + onSquare;
            onSquare = val;
            turnIsFriendly[count] = false;
            ++count;
        }
    }
    int score = balances[count - 1];
    for (std::size_t i = count - 1; i > 0; --i) {
        const std::size_t idx = i - 1;
        const int standPat = balances[idx];
        score = turnIsFriendly[idx] ? std::max(standPat, score) : std::min(standPat, score);
    }
    return score <= -2;
}

std::optional<MoveExplanation> detectFork(const chess::Position& before, chess::Move move,
                                          const chess::Position& after, chess::Color mover) {
    const auto moved = after.pieceAt(move.to);
    if (!moved || moved->color != mover || seeOnAfter(after, move.to, mover) < 0) {
        return std::nullopt;
    }
    const auto opp = opposite(mover);
    struct Target {
        chess::Square square;
        chess::PieceType type;
        bool isKing;
        bool undefended;
        bool isNew;
    };
    std::array<Target, 8> targets{};
    std::size_t count = 0;
    bool hasNewTarget = false;
    for (int index = 0; index < 64; ++index) {
        const auto sq = *chess::Square::fromCoordinates(index % 8, index / 8);
        const auto p = after.pieceAt(sq);
        if (!p || p->color != opp || p->type == chess::PieceType::Pawn) {
            continue;
        }
        if (!after.attacks(move.to, sq) || isPinnedOffTarget(after, move.to, mover, sq)) {
            continue;
        }
        const bool isKing = p->type == chess::PieceType::King;
        if (isKing && !after.inCheck()) {
            continue;
        }
        const bool undefended = !after.isAttacked(sq, opp);
        const bool higherValue =
            moved->type != chess::PieceType::King && pieceValue(p->type) > pieceValue(moved->type);
        const bool qualifies = isKing || higherValue || undefended;
        if (!qualifies) {
            continue;
        }
        const bool isNew = move.promotion.has_value() || !before.attacks(move.from, sq);
        hasNewTarget = hasNewTarget || isNew;
        if (count < targets.size()) {
            targets[count++] = {sq, p->type, isKing, undefended, isNew};
        }
    }
    if (count < 2 || !hasNewTarget) {
        return std::nullopt;
    }
    std::sort(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(count),
              [](const Target& a, const Target& b) {
                  if (a.isKing != b.isKing) {
                      return a.isKing;
                  }
                  if (pieceValue(a.type) != pieceValue(b.type)) {
                      return pieceValue(a.type) > pieceValue(b.type);
                  }
                  return a.undefended && !b.undefended;
              });
    MoveExplanation result;
    result.kind = ExplanationKind::ForksPieces;
    result.beneficiary = mover;
    result.actor = moved->type;
    result.checksKing = targets[0].isKing;
    result.targetPiece = targets[0].type;
    result.square = targets[0].square;
    result.secondaryPiece = targets[1].type;
    result.secondarySquare = targets[1].square;
    return result;
}

std::optional<MoveExplanation> detectPin(const chess::Position& before, chess::Move move,
                                         const chess::Position& after, chess::Color mover) {
    const auto moved = after.pieceAt(move.to);
    if (!moved || moved->color != mover ||
        (moved->type != chess::PieceType::Bishop && moved->type != chess::PieceType::Rook &&
         moved->type != chess::PieceType::Queen)) {
        return std::nullopt;
    }
    if (seeOnAfter(after, move.to, mover) < 0) {
        return std::nullopt;
    }
    const auto opp = opposite(mover);
    constexpr std::array<std::pair<int, int>, 8> allDirs{
        {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};
    std::optional<MoveExplanation> best;
    int bestRank = -1;
    for (const auto dir : allDirs) {
        if (!sliderMatchesDirection(moved->type, dir)) {
            continue;
        }
        std::optional<chess::Square> sq1;
        std::optional<chess::Piece> p1;
        std::optional<chess::Square> sq2;
        std::optional<chess::Piece> p2;
        int f = move.to.file() + dir.first;
        int r = move.to.rank() + dir.second;
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            const auto sq = *chess::Square::fromCoordinates(f, r);
            if (const auto p = after.pieceAt(sq)) {
                if (!sq1) {
                    sq1 = sq;
                    p1 = p;
                } else {
                    sq2 = sq;
                    p2 = p;
                    break;
                }
            }
            f += dir.first;
            r += dir.second;
        }
        if (!sq1 || !p1 || !sq2 || !p2 || p1->color != opp || p2->color != opp ||
            p1->type == chess::PieceType::Pawn || p1->type == chess::PieceType::King) {
            continue;
        }
        const bool absPin = p2->type == chess::PieceType::King;
        const bool relPin =
            (p2->type == chess::PieceType::Queen || p2->type == chess::PieceType::Rook) &&
            pieceValue(p2->type) > pieceValue(p1->type) &&
            (pieceValue(p2->type) > pieceValue(moved->type) || !after.isAttacked(*sq2, opp));
        if (!absPin && !relPin) {
            continue;
        }
        // Verify this pin did not already exist from move.from along the same ray.
        if (!move.promotion && rayDirection(move.from, *sq1) == dir) {
            std::optional<chess::Square> prev1;
            std::optional<chess::Square> prev2;
            int pf = move.from.file() + dir.first;
            int pr = move.from.rank() + dir.second;
            while (pf >= 0 && pf < 8 && pr >= 0 && pr < 8) {
                const auto sq = *chess::Square::fromCoordinates(pf, pr);
                if (before.pieceAt(sq)) {
                    if (!prev1) {
                        prev1 = sq;
                    } else {
                        prev2 = sq;
                        break;
                    }
                }
                pf += dir.first;
                pr += dir.second;
            }
            if (prev1 == sq1 && prev2 == sq2) {
                continue;
            }
        }
        const int rank = absPin ? 100 : pieceValue(p2->type);
        if (rank > bestRank) {
            bestRank = rank;
            MoveExplanation candidate;
            candidate.kind = ExplanationKind::CreatesPin;
            candidate.beneficiary = mover;
            candidate.actor = moved->type;
            candidate.targetPiece = p1->type;
            candidate.square = *sq1;
            candidate.secondaryPiece = p2->type;
            candidate.secondarySquare = *sq2;
            best = candidate;
        }
    }
    return best;
}

std::optional<MoveExplanation> detectThreat(const chess::Position& before, chess::Move move,
                                            const chess::Position& after, chess::Color mover) {
    const auto moved = after.pieceAt(move.to);
    if (!moved || moved->color != mover || seeOnAfter(after, move.to, mover) < 0) {
        return std::nullopt;
    }
    const auto opp = opposite(mover);
    std::optional<MoveExplanation> best;
    int bestVal = -1;
    bool bestUndefended = false;
    for (int index = 0; index < 64; ++index) {
        const auto sq = *chess::Square::fromCoordinates(index % 8, index / 8);
        const auto p = after.pieceAt(sq);
        if (!p || p->color != opp || p->type == chess::PieceType::Pawn ||
            p->type == chess::PieceType::King) {
            continue;
        }
        if (!after.attacks(move.to, sq) || isPinnedOffTarget(after, move.to, mover, sq)) {
            continue;
        }
        if (!move.promotion && before.attacks(move.from, sq)) {
            continue;
        }
        const bool undefended = !after.isAttacked(sq, opp);
        const bool higherValue =
            moved->type != chess::PieceType::King && pieceValue(p->type) > pieceValue(moved->type);
        if (!higherValue && !undefended && !staticSquareDanger(after, sq, opp)) {
            continue;
        }
        const int val = pieceValue(p->type);
        if (val > bestVal || (val == bestVal && undefended && !bestUndefended)) {
            bestVal = val;
            bestUndefended = undefended;
            MoveExplanation candidate;
            candidate.kind = ExplanationKind::CreatesThreat;
            candidate.beneficiary = mover;
            candidate.actor = moved->type;
            candidate.targetPiece = p->type;
            candidate.square = sq;
            candidate.undefended = undefended;
            best = candidate;
        }
    }
    return best;
}

std::optional<MoveExplanation> detectDiscoveredAttack(const chess::Position& before,
                                                      chess::Move move,
                                                      const chess::Position& after,
                                                      chess::Color mover) {
    const auto opp = opposite(mover);
    std::optional<MoveExplanation> best;
    int bestVal = -1;
    bool bestUndefended = false;
    for (int sIdx = 0; sIdx < 64; ++sIdx) {
        const auto sliderSq = *chess::Square::fromCoordinates(sIdx % 8, sIdx / 8);
        if (sliderSq == move.to) {
            continue;
        }
        const auto slider = after.pieceAt(sliderSq);
        if (!slider || slider->color != mover ||
            (slider->type != chess::PieceType::Bishop && slider->type != chess::PieceType::Rook &&
             slider->type != chess::PieceType::Queen)) {
            continue;
        }
        if (seeOnAfter(after, sliderSq, mover) < 0) {
            continue;
        }
        for (int tIdx = 0; tIdx < 64; ++tIdx) {
            const auto sq = *chess::Square::fromCoordinates(tIdx % 8, tIdx / 8);
            const auto p = after.pieceAt(sq);
            if (!p || p->color != opp || p->type == chess::PieceType::Pawn ||
                p->type == chess::PieceType::King) {
                continue;
            }
            if (before.attacks(sliderSq, sq) || !after.attacks(sliderSq, sq) ||
                isPinnedOffTarget(after, sliderSq, mover, sq)) {
                continue;
            }
            const bool undefended = !after.isAttacked(sq, opp);
            if (pieceValue(p->type) <= pieceValue(slider->type) && !undefended) {
                continue;
            }
            const int val = pieceValue(p->type);
            if (val > bestVal || (val == bestVal && undefended && !bestUndefended)) {
                bestVal = val;
                bestUndefended = undefended;
                MoveExplanation candidate;
                candidate.kind = ExplanationKind::CreatesDiscoveredAttack;
                candidate.beneficiary = mover;
                candidate.actor = slider->type;
                candidate.targetPiece = p->type;
                candidate.square = sq;
                candidate.undefended = undefended;
                best = candidate;
            }
        }
    }
    return best;
}

bool isMinorStartingSquare(chess::Color color, chess::PieceType type, chess::Square sq) {
    const int homeRank = color == chess::Color::White ? 0 : 7;
    if (sq.rank() != homeRank) {
        return false;
    }
    if (type == chess::PieceType::Knight) {
        return sq.file() == 1 || sq.file() == 6;
    }
    if (type == chess::PieceType::Bishop) {
        return sq.file() == 2 || sq.file() == 5;
    }
    return false;
}

bool mateScoreFor(const RawEvaluation& eval, chess::Color mover, bool positiveForMover) {
    if (!eval.score || eval.score->kind != ScoreKind::Mate || eval.score->value == 0) {
        return false;
    }
    const bool same = eval.perspective == mover;
    const bool positive = same ? eval.score->value > 0 : eval.score->value < 0;
    return positiveForMover ? positive : !positive;
}

std::optional<chess::Move> resolveBestMove(const chess::Position& before, const MoveEvidence& pair,
                                           const ClassificationResult& classification) {
    if (classification.bestMove && before.isLegal(*classification.bestMove)) {
        return classification.bestMove;
    }
    if (!pair.before.candidates.empty() && !pair.before.candidates.front().continuation.empty()) {
        const auto cand = pair.before.candidates.front().continuation.front();
        if (before.isLegal(cand)) {
            return cand;
        }
    }
    return std::nullopt;
}

std::optional<chess::Move> resolveReplyMove(const chess::Position& after,
                                            const MoveEvidence& pair) {
    if (!pair.after.candidates.empty() && !pair.after.candidates.front().continuation.empty()) {
        const auto reply = pair.after.candidates.front().continuation.front();
        if (after.isLegal(reply)) {
            return reply;
        }
    }
    return std::nullopt;
}

std::optional<MoveExplanation> shortRetainedLineGain(std::span<const TacticalFact> facts,
                                                     FactSource source, chess::Color beneficiary,
                                                     int maxPlies, bool isLossOrMiss) {
    const TacticalFact* exchange = nullptr;
    const TacticalFact* material = nullptr;
    for (const auto& fact : facts) {
        if (fact.source != source || fact.beneficiary != beneficiary || !fact.retainedOpportunity ||
            fact.linePlies > maxPlies) {
            continue;
        }
        if (fact.kind == TacticalKind::Exchange && !exchange) {
            exchange = &fact;
        } else if (fact.kind == TacticalKind::Material && fact.material >= 2 &&
                   (!material || fact.material > material->material)) {
            material = &fact;
        }
    }
    if (exchange) {
        MoveExplanation out;
        out.kind = isLossOrMiss
                       ? (source == FactSource::Played ? ExplanationKind::LosesExchangeInLine
                                                       : ExplanationKind::MissesExchangeInLine)
                       : ExplanationKind::WinsExchangeInLine;
        out.beneficiary = beneficiary;
        out.material = 2;
        return out;
    }
    if (material) {
        MoveExplanation out;
        out.kind = isLossOrMiss
                       ? (source == FactSource::Played ? ExplanationKind::LosesMaterialInLine
                                                       : ExplanationKind::MissesMaterialInLine)
                       : ExplanationKind::WinsMaterialInLine;
        out.beneficiary = beneficiary;
        out.material = material->material;
        return out;
    }
    return std::nullopt;
}

} // namespace

MoveExplanation explainMove(const chess::Position& before, const MoveEvidence& pair,
                            const ClassificationResult& classification,
                            std::span<const TacticalFact> tacticalFacts) {
    const auto mover = before.sideToMove();
    const auto opp = opposite(mover);
    MoveExplanation generic;
    generic.beneficiary = mover;

    if (!classification.label) {
        return generic;
    }
    const auto label = *classification.label;
    if (label == Classification::Book || label == Classification::Brilliant ||
        label == Classification::Great || classification.ruleId == "forced.best.v3" ||
        before.legalMoves().size() <= 1) {
        return generic;
    }

    const auto move = pair.playedMove;
    const auto beforeActor = before.pieceAt(move.from);
    if (!beforeActor || beforeActor->color != mover) {
        return generic;
    }
    auto after = before;
    if (std::holds_alternative<chess::Error>(after.play(move))) {
        return generic;
    }
    const auto afterActor = after.pieceAt(move.to);
    if (!afterActor) {
        return generic;
    }
    const chess::PieceType actorType = afterActor->type;

    const bool mateBeforeForMover = mateScoreFor(pair.before.evaluation, mover, true);
    const bool mateAfterForMover = mateScoreFor(pair.after.evaluation, mover, true);
    const bool mateAfterAgainstMover = mateScoreFor(pair.after.evaluation, mover, false);

    if (!review::isError(label)) {
        if (after.inCheck() && after.legalMoves().empty()) {
            MoveExplanation out;
            out.kind = ExplanationKind::DeliversCheckmate;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            return out;
        }
        if (mateAfterForMover) {
            MoveExplanation out;
            out.kind = ExplanationKind::LeadsToMate;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            out.checksKing = after.inCheck();
            return out;
        }
        if (move.promotion) {
            MoveExplanation out;
            out.kind = ExplanationKind::PromotesPawn;
            out.beneficiary = mover;
            out.actor = *move.promotion;
            out.square = move.to;
            return out;
        }
        if (const auto fork = detectFork(before, move, after, mover)) {
            return *fork;
        }

        const auto victim = capturedPieceType(before, move);
        bool isEqualTrade = false;
        if (victim) {
            std::optional<chess::PieceType> prevCaptured;
            if (!before.context().moves.empty()) {
                auto prev = before;
                if (const auto last = prev.undo(); last && last->to == move.to) {
                    prevCaptured = capturedPieceType(prev, *last);
                }
            }
            const int seeFromBefore =
                staticExchange(after, move.to, mover, materialBalance(before, mover));
            if (*victim != chess::PieceType::Pawn && !before.isAttacked(move.to, opp) &&
                !after.isAttacked(move.to, opp) &&
                (!prevCaptured || pieceValue(*victim) >= pieceValue(*prevCaptured) + 2)) {
                MoveExplanation out;
                out.kind = ExplanationKind::CapturesFreePiece;
                out.beneficiary = mover;
                out.actor = actorType;
                out.targetPiece = *victim;
                out.square = move.to;
                return out;
            }
            if (prevCaptured) {
                MoveExplanation out;
                out.kind = ExplanationKind::Recaptures;
                out.beneficiary = mover;
                out.actor = actorType;
                out.targetPiece = *victim;
                out.square = move.to;
                return out;
            }
            if (*victim != chess::PieceType::Pawn && seeFromBefore >= 2 &&
                (pieceValue(*victim) > pieceValue(actorType) ||
                 seeFromBefore >= pieceValue(*victim))) {
                MoveExplanation out;
                out.kind = (*victim == chess::PieceType::Rook &&
                            (actorType == chess::PieceType::Bishop ||
                             actorType == chess::PieceType::Knight) &&
                            seeFromBefore == 2 && after.isAttacked(move.to, opp))
                               ? ExplanationKind::WinsExchangeImmediate
                               : ExplanationKind::WinsMaterialImmediate;
                out.beneficiary = mover;
                out.actor = actorType;
                out.targetPiece = *victim;
                out.square = move.to;
                out.material = seeFromBefore;
                return out;
            }
            isEqualTrade = *victim != chess::PieceType::Pawn &&
                           actorType != chess::PieceType::Pawn &&
                           pieceValue(*victim) == pieceValue(actorType);
        }

        if (label == Classification::Best || label == Classification::Excellent) {
            if (const auto lineGain =
                    shortRetainedLineGain(tacticalFacts, FactSource::Played, mover, 5, false)) {
                return *lineGain;
            }
        }

        if (actorType == chess::PieceType::King &&
            std::abs(move.from.file() - move.to.file()) == 2) {
            MoveExplanation out;
            out.kind = ExplanationKind::Castles;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            out.kingside = move.to.file() > move.from.file();
            return out;
        }
        if (isEqualTrade) {
            MoveExplanation out;
            out.kind = ExplanationKind::TradesPieces;
            out.beneficiary = mover;
            out.actor = actorType;
            out.targetPiece = *victim;
            out.square = move.to;
            return out;
        }
        if (const auto pin = detectPin(before, move, after, mover)) {
            return *pin;
        }
        if (actorType != chess::PieceType::Pawn && actorType != chess::PieceType::King &&
            staticSquareDanger(before, move.from, mover) &&
            seeOnAfter(after, move.to, mover) >= 0) {
            MoveExplanation out;
            out.kind = ExplanationKind::SavesPiece;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            return out;
        }
        if (const auto threat = detectThreat(before, move, after, mover)) {
            return *threat;
        }
        if (const auto discovered = detectDiscoveredAttack(before, move, after, mover)) {
            return *discovered;
        }
        if (seeOnAfter(after, move.to, mover) >= 0) {
            std::optional<MoveExplanation> bestDefend;
            int bestDefendVal = -1;
            for (int index = 0; index < 64; ++index) {
                const auto allySq = *chess::Square::fromCoordinates(index % 8, index / 8);
                if (allySq == move.to) {
                    continue;
                }
                const auto ally = after.pieceAt(allySq);
                if (!ally || ally->color != mover || ally->type == chess::PieceType::Pawn ||
                    ally->type == chess::PieceType::King) {
                    continue;
                }
                if (!after.attacks(move.to, allySq) ||
                    isPinnedOffTarget(after, move.to, mover, allySq)) {
                    continue;
                }
                if (!move.promotion && before.attacks(move.from, allySq) &&
                    !isPinnedOffTarget(before, move.from, mover, allySq)) {
                    continue;
                }
                if (!staticSquareDanger(before, allySq, mover) ||
                    seeOnAfter(after, allySq, mover) < 0) {
                    continue;
                }
                const int val = pieceValue(ally->type);
                if (val > bestDefendVal) {
                    bestDefendVal = val;
                    MoveExplanation candidate;
                    candidate.kind = ExplanationKind::DefendsPiece;
                    candidate.beneficiary = mover;
                    candidate.actor = actorType;
                    candidate.targetPiece = ally->type;
                    candidate.square = allySq;
                    bestDefend = candidate;
                }
            }
            if (bestDefend) {
                return *bestDefend;
            }
        }
        if (before.inCheck()) {
            MoveExplanation out;
            out.kind = ExplanationKind::EscapesCheck;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            out.blocksCheck = actorType != chess::PieceType::King;
            return out;
        }
        if (after.inCheck()) {
            MoveExplanation out;
            out.kind = ExplanationKind::GivesCheck;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            out.checksKing = true;
            return out;
        }
        if (isMinorStartingSquare(mover, beforeActor->type, move.from) &&
            move.to.rank() != move.from.rank() && seeOnAfter(after, move.to, mover) >= 0) {
            MoveExplanation out;
            out.kind = ExplanationKind::DevelopsPiece;
            out.beneficiary = mover;
            out.actor = actorType;
            out.square = move.to;
            out.central = move.to.file() >= 2 && move.to.file() <= 5 && move.to.rank() >= 2 &&
                          move.to.rank() <= 5;
            return out;
        }
        return generic;
    }

    // Error / miss themes
    if (label != Classification::Miss && mateAfterAgainstMover) {
        MoveExplanation out;
        out.kind = ExplanationKind::AllowsMate;
        out.beneficiary = opp;
        return out;
    }
    if (mateBeforeForMover && !mateAfterForMover) {
        MoveExplanation out;
        out.kind = ExplanationKind::MissesMate;
        out.beneficiary = mover;
        return out;
    }

    const auto playedVictim = capturedPieceType(before, move);
    const auto playedCapturedBetterFreePiece = [&](int minVal) {
        return playedVictim && *playedVictim != chess::PieceType::Pawn &&
               pieceValue(*playedVictim) >= minVal && !before.isAttacked(move.to, opp) &&
               !after.isAttacked(move.to, opp);
    };
    const auto playedRetainsMaterial = [&](int minMaterial) {
        return std::any_of(tacticalFacts.begin(), tacticalFacts.end(), [&](const TacticalFact& f) {
            return f.source == FactSource::Played && f.beneficiary == mover &&
                   f.retainedOpportunity &&
                   (f.kind == TacticalKind::Material || f.kind == TacticalKind::Exchange) &&
                   f.material >= minMaterial;
        });
    };

    const auto bestMove = resolveBestMove(before, pair, classification);
    std::optional<chess::Position> afterBest;
    if (bestMove && *bestMove != move) {
        afterBest = before;
        if (std::holds_alternative<chess::Error>(afterBest->play(*bestMove))) {
            afterBest.reset();
        }
    }

    const auto checkMissedOpportunities = [&]() -> std::optional<MoveExplanation> {
        if (bestMove && afterBest) {
            if (const auto bestVictim = capturedPieceType(before, *bestMove);
                bestVictim && *bestVictim != chess::PieceType::Pawn &&
                !before.isAttacked(bestMove->to, opp) &&
                !afterBest->isAttacked(bestMove->to, opp) &&
                !playedCapturedBetterFreePiece(pieceValue(*bestVictim))) {
                MoveExplanation out;
                out.kind = ExplanationKind::MissesFreePiece;
                out.beneficiary = mover;
                out.targetPiece = *bestVictim;
                out.square = bestMove->to;
                return out;
            }
            if (const auto missedFork = detectFork(before, *bestMove, *afterBest, mover)) {
                MoveExplanation out = *missedFork;
                out.kind = ExplanationKind::MissesFork;
                return out;
            }
            if (bestMove->promotion) {
                MoveExplanation out;
                out.kind = ExplanationKind::MissesPromotion;
                out.beneficiary = mover;
                out.actor = *bestMove->promotion;
                out.square = bestMove->to;
                return out;
            }
        }
        return std::nullopt;
    };

    if (label == Classification::Miss) {
        if (const auto missed = checkMissedOpportunities()) {
            return *missed;
        }
        if (const auto lineMiss =
                shortRetainedLineGain(tacticalFacts, FactSource::Best, mover, 6, true)) {
            if (lineMiss->kind != ExplanationKind::MissesMaterialInLine ||
                !playedRetainsMaterial(lineMiss->material)) {
                if (lineMiss->kind != ExplanationKind::MissesExchangeInLine ||
                    !playedRetainsMaterial(2)) {
                    return *lineMiss;
                }
            }
        }
        for (const auto& fact : tacticalFacts) {
            if (fact.source == FactSource::Best && fact.beneficiary == mover &&
                fact.retainedOpportunity && fact.linePlies <= 6 &&
                fact.kind == TacticalKind::Promotion) {
                MoveExplanation out;
                out.kind = ExplanationKind::MissesPromotion;
                out.beneficiary = mover;
                out.actor = fact.actor;
                out.square = fact.square;
                return out;
            }
        }
        return generic;
    }

    // Blunder / Mistake / Inaccuracy
    const auto replyMove = resolveReplyMove(after, pair);
    std::optional<chess::Position> afterReply;
    if (replyMove) {
        afterReply = after;
        if (std::holds_alternative<chess::Error>(afterReply->play(*replyMove))) {
            afterReply.reset();
        }
    }

    if (replyMove && afterReply) {
        if (const auto lost = after.pieceAt(replyMove->to);
            lost && lost->color == mover && lost->type != chess::PieceType::Pawn) {
            const int seeDelta =
                staticExchange(after, replyMove->to, mover, materialBalance(before, mover));
            if (seeDelta <= -2) {
                const auto replyActor = after.pieceAt(replyMove->from);
                const bool replyMinor =
                    replyActor && (replyActor->type == chess::PieceType::Bishop ||
                                   replyActor->type == chess::PieceType::Knight);
                const bool isExchange =
                    lost->type == chess::PieceType::Rook && replyMinor && seeDelta == -2;
                MoveExplanation out;
                out.beneficiary = opp;
                out.targetPiece = lost->type;
                out.square = replyMove->to;
                out.material = -seeDelta;
                if (replyMove->to == move.to) {
                    if (!playedVictim && !after.isAttacked(replyMove->to, mover)) {
                        out.kind = ExplanationKind::HangsPiece;
                    } else if (isExchange) {
                        out.kind = ExplanationKind::AllowsExchangeImmediate;
                    } else {
                        out.kind = ExplanationKind::LosesPieceImmediate;
                    }
                } else {
                    if (staticSquareDanger(before, replyMove->to, mover)) {
                        out.kind = ExplanationKind::FailsToSavePiece;
                    } else if (!after.isAttacked(replyMove->to, mover)) {
                        out.kind = ExplanationKind::HangsPiece;
                    } else if (isExchange) {
                        out.kind = ExplanationKind::AllowsExchangeImmediate;
                    } else {
                        out.kind = ExplanationKind::LosesPieceImmediate;
                    }
                }
                return out;
            }
        }
        if (const auto oppFork = detectFork(after, *replyMove, *afterReply, opp)) {
            MoveExplanation out = *oppFork;
            out.kind = ExplanationKind::AllowsFork;
            out.square = replyMove->to;
            return out;
        }
        if (replyMove->promotion) {
            MoveExplanation out;
            out.kind = ExplanationKind::AllowsPromotion;
            out.beneficiary = opp;
            out.actor = *replyMove->promotion;
            out.square = replyMove->to;
            return out;
        }
    }

    if (const auto missed = checkMissedOpportunities()) {
        return *missed;
    }

    if (label == Classification::Blunder || label == Classification::Mistake) {
        if (const auto lineLoss =
                shortRetainedLineGain(tacticalFacts, FactSource::Played, opp, 6, true)) {
            return *lineLoss;
        }
        if (const auto lineMiss =
                shortRetainedLineGain(tacticalFacts, FactSource::Best, mover, 6, true)) {
            if ((lineMiss->kind == ExplanationKind::MissesExchangeInLine &&
                 !playedRetainsMaterial(2)) ||
                (lineMiss->kind == ExplanationKind::MissesMaterialInLine &&
                 !playedRetainsMaterial(lineMiss->material))) {
                return *lineMiss;
            }
        }
    }

    return generic;
}

} // namespace chess_review::review
