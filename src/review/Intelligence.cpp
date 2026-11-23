#include "review/Intelligence.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <variant>
namespace chess_review::review {
namespace {
int value(chess::PieceType type) {
    constexpr std::array<int, 6> values{1, 3, 3, 5, 9, 0};
    return values[static_cast<std::size_t>(type)];
}
chess::Color opposite(chess::Color color) {
    return color == chess::Color::White ? chess::Color::Black : chess::Color::White;
}
int balance(const chess::Position& position, chess::Color side) {
    int result = 0;
    for (const auto& piece : position.boardIdentity().pieces) {
        if (piece) {
            result += (piece->color == side ? 1 : -1) * value(piece->type);
        }
    }
    return result;
}
bool sameFact(const TacticalFact& a, const TacticalFact& b) {
    return a.kind == b.kind && a.source == b.source && a.beneficiary == b.beneficiary &&
           a.actor == b.actor && a.square == b.square && a.targets == b.targets &&
           a.material == b.material && a.retainedOpportunity == b.retainedOpportunity;
}
std::vector<chess::Move> playedLine(const MoveEvidence& pair) {
    std::vector<chess::Move> line{pair.playedMove};
    if (!pair.after.candidates.empty()) {
        const auto& continuation = pair.after.candidates.front().continuation;
        const auto count = std::min(continuation.size(), tacticalLineLimit - 1);
        line.insert(line.end(), continuation.begin(),
                    continuation.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return line;
}
bool opportunity(const TacticalFact& fact, chess::Color side) {
    return (fact.modelVersion == 1 || fact.retainedOpportunity) && fact.beneficiary == side &&
           ((fact.kind == TacticalKind::Material && fact.material > 0) ||
            fact.kind == TacticalKind::Promotion || fact.kind == TacticalKind::Checkmate);
}
bool requiresInputRepair(const ClassificationResult& result) {
    if (result.issue == ClassificationIssue::InvalidConfiguration) {
        return true;
    }
    if (!result.evidenceIssue) {
        return false;
    }
    switch (*result.evidenceIssue) {
    case EvidenceIssue::Incomplete:
    case EvidenceIssue::Bounded:
    case EvidenceIssue::MateClaim:
    case EvidenceIssue::MissingEvaluation:
    case EvidenceIssue::MixedConversion:
    case EvidenceIssue::NegativeLoss:
    case EvidenceIssue::ContradictoryCandidates:
        return false;
    case EvidenceIssue::Invalid:
    case EvidenceIssue::ContextMismatch:
    case EvidenceIssue::IncompatibleEngine:
    case EvidenceIssue::IncompatibleModel:
    case EvidenceIssue::RestrictedSearch:
        return true;
    }
    return true;
}
std::optional<double> runnerUp(const MoveEvidence& pair, const ConversionConfig& config,
                               std::uint32_t expectedModel = 1) {
    if (pair.before.candidates.size() < 2) {
        return {};
    }
    const auto score = convertEvaluation(pair.before.candidates[1].evaluation,
                                         pair.before.evaluation.perspective, config, expectedModel);
    return score.expected ? std::optional(score.expected->value) : std::nullopt;
}
int staticExchangeOnSquare(chess::Position board, chess::Square target, chess::Color side,
                           int initialBalance) {
    int bestOrLast = balance(board, side) - initialBalance;
    for (int depth = 0; depth < 16; ++depth) {
        const auto legal = board.legalMoves();
        const chess::Move* cheapestCapture = nullptr;
        int cheapestVal = 100;
        for (const auto& m : legal) {
            if (m.to != target) {
                continue;
            }
            const auto attacker = board.pieceAt(m.from);
            if (!attacker) {
                continue;
            }
            int v = attacker->type == chess::PieceType::King ? 10 : value(attacker->type);
            if (v < cheapestVal) {
                cheapestVal = v;
                cheapestCapture = &m;
            }
        }
        if (!cheapestCapture) {
            break;
        }
        if (std::holds_alternative<chess::Error>(board.play(*cheapestCapture))) {
            break;
        }
        bestOrLast = balance(board, side) - initialBalance;
    }
    return bestOrLast;
}
bool isTrivialRecapture(const chess::Position& root, chess::Move playedMove) {
    const auto side = root.sideToMove();
    if (const auto victim = root.pieceAt(playedMove.to);
        victim && value(victim->type) >= 3 && !root.isAttacked(playedMove.to, opposite(side))) {
        auto after = root;
        if (std::holds_alternative<std::monostate>(after.play(playedMove)) &&
            !after.isAttacked(playedMove.to, opposite(side))) {
            return true;
        }
    }
    if (root.context().moves.empty()) {
        return false;
    }
    auto previous = root;
    const auto last = previous.undo();
    if (!last || playedMove.to != last->to || !root.pieceAt(last->to)) {
        return false;
    }
    const bool downMaterial = balance(root, side) < balance(previous, side);
    const bool undefended = !root.isAttacked(last->to, opposite(side));
    return downMaterial || undefended;
}
} // namespace
bool validSpecialConfig(const SpecialLabelConfig& c) {
    const std::array values{c.maximumLoss, c.maximumLossDrift,    c.alternativeGap, c.missLoss,
                            c.viableScore, c.alreadyWinningScore, c.margin};
    return std::all_of(values.begin(), values.end(),
                       [](double x) { return std::isfinite(x) && x >= 0 && x <= 1; }) &&
           c.alternativeGap > c.margin && c.missLoss > c.margin && c.viableScore > c.margin &&
           c.alreadyWinningScore > c.viableScore + c.margin && c.alreadyWinningScore < 1 &&
           c.sacrificeValue >= 1 && c.sacrificeValue <= 9;
}
std::vector<TacticalFact> inspectTacticalLine(const chess::Position& root,
                                              std::span<const chess::Move> input, FactSource source,
                                              std::uint32_t modelVersion) {
    if ((modelVersion != 1 && modelVersion != 2 && modelVersion != 3 && modelVersion != 4) ||
        (source != FactSource::Played && source != FactSource::Best) || input.empty() ||
        input.size() > tacticalLineLimit) {
        return {};
    }
    const auto line = input.first(std::min(input.size(), tacticalLineLimit));
    auto board = root;
    const auto side = root.sideToMove();
    const auto initialBalance = balance(root, side);
    std::vector<TacticalFact> facts;
    struct PromotionWitness {
        std::size_t fact;
        chess::Square square;
        bool alive = true;
    };
    std::vector<PromotionWitness> promotions;
    int captures = 0;
    int rooks = 0;
    int minors = 0;
    bool promoted = false;
    int concession = 0;
    int concessionPly = -1;
    chess::PieceType concessionActor = chess::PieceType::Pawn;
    chess::Square concessionSquare{};
    std::optional<chess::Position> afterFirst;
    std::optional<int> deltaAfterPly2;
    const auto firstActor = root.pieceAt(line.front().from);
    if (!firstActor) {
        return {};
    }
    concessionActor = firstActor->type;
    concessionSquare = line.front().to;
    for (std::size_t ply = 0; ply < line.size(); ++ply) {
        const auto& move = line[ply];
        // play() validates legality and preserves the board on failure; a separate
        // isLegal() call would generate the same legal moves twice for every ply.
        const auto actor = board.pieceAt(move.from);
        const auto victim = board.pieceAt(move.to);
        const auto actorSide = board.sideToMove();
        if (victim) {
            ++captures;
            rooks += victim->type == chess::PieceType::Rook ? 1 : 0;
            minors +=
                victim->type == chess::PieceType::Bishop || victim->type == chess::PieceType::Knight
                    ? 1
                    : 0;
        }
        if (modelVersion >= 2) {
            for (auto& promotion : promotions) {
                if (!promotion.alive) {
                    continue;
                }
                if (victim && promotion.square == move.to) {
                    promotion.alive = false;
                } else if (promotion.square == move.from) {
                    promotion.square = move.to;
                }
            }
        }
        promoted = promoted || move.promotion.has_value();
        const bool hanging = victim && victim->type != chess::PieceType::Pawn &&
                             !board.isAttacked(move.to, victim->color);
        if (std::holds_alternative<chess::Error>(board.play(move))) {
            return {};
        }
        if (ply == 0) {
            afterFirst = board;
        }
        const auto prefix = [&] {
            return std::vector<chess::Move>(line.begin(),
                                            line.begin() + static_cast<std::ptrdiff_t>(ply + 1));
        };
        // Moving the capturer can uncover an x-ray defender. The pre-capture
        // geometry alone cannot establish that the captured piece was hanging.
        if (hanging && (modelVersion < 3 || !board.isAttacked(move.to, victim->color))) {
            facts.push_back({TacticalKind::HangingPiece,
                             source,
                             actorSide,
                             actor->type,
                             move.to,
                             {move.to},
                             value(victim->type),
                             prefix()});
        }
        if (move.promotion) {
            facts.push_back({TacticalKind::Promotion,
                             source,
                             actorSide,
                             *move.promotion,
                             move.to,
                             {},
                             value(*move.promotion) - 1,
                             prefix()});
        }
        if (move.promotion && modelVersion >= 2) {
            promotions.push_back({facts.size() - 1, move.to, true});
        }
        if (ply == 0) {
            std::vector<chess::Square> targets;
            bool kingTarget = false;
            bool legalCapture = false;
            // Attack geometry is necessary but not sufficient: a pinned attacker must not
            // become a named fork. For king forks, being in check supplies that witness.
            for (int index = 0; index < 64; ++index) {
                const auto square = *chess::Square::fromCoordinates(index % 8, index / 8);
                const auto piece = board.pieceAt(square);
                if (piece && piece->color != actorSide && piece->type != chess::PieceType::Pawn &&
                    board.attacks(move.to, square)) {
                    targets.push_back(square);
                    kingTarget = kingTarget || piece->type == chess::PieceType::King;
                }
            }
            // Only a checking fork is named here: it is concrete and does not require
            // changing side-to-move or inventing a null move for legality checks.
            legalCapture = kingTarget && board.inCheck();
            if (targets.size() >= 2 && legalCapture &&
                !board.isAttacked(move.to, opposite(actorSide))) {
                facts.push_back({TacticalKind::Fork, source, actorSide,
                                 modelVersion >= 2 ? board.pieceAt(move.to)->type : actor->type,
                                 move.to, std::move(targets), 0, prefix()});
            }
        }
        const auto delta = balance(board, side) - initialBalance;
        if (ply == 1 && victim && delta < 0 &&
            (move.to == line.front().to ||
             (modelVersion >= 4 && value(victim->type) >= 3 && delta <= -2))) {
            concession = -delta;
            concessionPly = static_cast<int>(ply);
            concessionActor = move.to == line.front().to ? firstActor->type : victim->type;
            concessionSquare = move.to;
        }
        if (ply == 2) {
            deltaAfterPly2 = delta;
            // An immediate balancing recapture is an exchange, not a sacrifice witness.
            if (concession > 0 && delta >= 0) {
                concession = 0;
            }
        }
        if (board.inCheck() && board.legalMoves().empty()) {
            facts.push_back({TacticalKind::Checkmate,
                             source,
                             actorSide,
                             actor->type,
                             move.to,
                             {},
                             0,
                             prefix()});
        }
    }
    const int delta = balance(board, side) - initialBalance;
    bool quietEnd = false;
    if (modelVersion >= 2 && !board.inCheck()) {
        const auto replies = board.legalMoves();
        quietEnd = !replies.empty() &&
                   std::none_of(replies.begin(), replies.end(), [&](const auto& reply) {
                       const auto piece = board.pieceAt(reply.from);
                       return reply.promotion || board.pieceAt(reply.to) ||
                              (piece->type == chess::PieceType::Pawn &&
                               reply.from.file() != reply.to.file());
                   });
    }
    const auto saved = [&] { return std::vector<chess::Move>(line.begin(), line.end()); };
    if (delta != 0) {
        facts.push_back({TacticalKind::Material,
                         source,
                         delta > 0 ? side : opposite(side),
                         firstActor->type,
                         line.front().to,
                         {},
                         std::abs(delta),
                         saved()});
        if (std::abs(delta) == 2 && captures == 2 && rooks == 1 && minors == 1 && !promoted) {
            facts.push_back({TacticalKind::Exchange,
                             source,
                             delta > 0 ? side : opposite(side),
                             firstActor->type,
                             line.front().to,
                             {},
                             2,
                             saved()});
        }
    }
    std::optional<chess::Color> mateWinner;
    for (const auto& fact : facts) {
        if (fact.kind == TacticalKind::Checkmate) {
            mateWinner = fact.beneficiary;
        }
    }
    const bool matingResolution = mateWinner == side;
    if (modelVersion < 4) {
        if (concession > 0 && concessionPly >= 0 && line.size() >= 5 &&
            ((delta >= 0 && (modelVersion == 1 || quietEnd)) || matingResolution)) {
            facts.push_back({TacticalKind::Sacrifice,
                             source,
                             side,
                             firstActor->type,
                             line.front().to,
                             {},
                             concession,
                             saved()});
        }
    } else {
        bool addedSacrifice = false;
        if (concession > 0 && concessionPly >= 0 && line.size() >= 3 &&
            (matingResolution || (deltaAfterPly2 && *deltaAfterPly2 < 0))) {
            facts.push_back({TacticalKind::Sacrifice,
                             source,
                             side,
                             concessionActor,
                             concessionSquare,
                             {},
                             concession,
                             saved()});
            addedSacrifice = true;
        }
        // Path B: declined / counter-sacrifice detection directly from board after move 0.
        if (!addedSacrifice && afterFirst) {
            const auto legalAfter = afterFirst->legalMoves();
            const auto movedPiece = afterFirst->pieceAt(line.front().to);
            // B1: moved piece offered on destination square
            if (movedPiece && value(movedPiece->type) >= 3) {
                int worstCapDelta = 0;
                bool hasCaptureOnDest = false;
                for (const auto& oppMove : legalAfter) {
                    if (oppMove.to != line.front().to) {
                        continue;
                    }
                    auto capBoard = *afterFirst;
                    if (std::holds_alternative<chess::Error>(capBoard.play(oppMove))) {
                        continue;
                    }
                    hasCaptureOnDest = true;
                    worstCapDelta =
                        std::min(worstCapDelta, balance(capBoard, side) - initialBalance);
                }
                if (hasCaptureOnDest) {
                    const int rawDrop = -worstCapDelta;
                    const int seeDelta =
                        staticExchangeOnSquare(*afterFirst, line.front().to, side, initialBalance);
                    if (rawDrop >= 2 && seeDelta <= -1) {
                        facts.push_back({TacticalKind::Sacrifice,
                                         source,
                                         side,
                                         firstActor->type,
                                         line.front().to,
                                         {},
                                         std::max(rawDrop, -seeDelta),
                                         saved()});
                        addedSacrifice = true;
                    }
                }
            }
            // B2: counter-sacrifice leaving another non-pawn piece en prise
            if (!addedSacrifice) {
                int bestSacMaterial = 0;
                chess::PieceType bestSacActor = chess::PieceType::Pawn;
                chess::Square bestSacSquare{};
                for (const auto& oppMove : legalAfter) {
                    if (oppMove.to == line.front().to) {
                        continue;
                    }
                    const auto victim = afterFirst->pieceAt(oppMove.to);
                    if (!victim || victim->color != side || value(victim->type) < 3) {
                        continue;
                    }
                    auto capBoard = *afterFirst;
                    if (std::holds_alternative<chess::Error>(capBoard.play(oppMove))) {
                        continue;
                    }
                    const int capDelta = balance(capBoard, side) - initialBalance;
                    int bestReplyGain = 0;
                    for (const auto& reply : capBoard.legalMoves()) {
                        const auto replyVictim = capBoard.pieceAt(reply.to);
                        int gain = replyVictim ? value(replyVictim->type) : 0;
                        if (reply.promotion) {
                            gain += value(*reply.promotion) - 1;
                        }
                        bestReplyGain = std::max(bestReplyGain, gain);
                    }
                    const int netAfterBestReply = capDelta + bestReplyGain;
                    if (netAfterBestReply <= -2 && -netAfterBestReply > bestSacMaterial) {
                        bestSacMaterial = -netAfterBestReply;
                        bestSacActor = victim->type;
                        bestSacSquare = oppMove.to;
                    }
                }
                if (bestSacMaterial >= 2) {
                    facts.push_back({TacticalKind::Sacrifice,
                                     source,
                                     side,
                                     bestSacActor,
                                     bestSacSquare,
                                     {},
                                     bestSacMaterial,
                                     saved()});
                }
            }
        }
    }
    for (auto& fact : facts) {
        fact.linePlies = static_cast<int>(fact.line.size());
        fact.modelVersion = modelVersion;
        if (modelVersion >= 2) {
            fact.retainedOpportunity = fact.kind == TacticalKind::Checkmate ||
                                       (quietEnd && (fact.kind == TacticalKind::Material ||
                                                     fact.kind == TacticalKind::Exchange));
        }
    }
    if (modelVersion >= 2) {
        for (const auto& promotion : promotions) {
            auto& fact = facts[promotion.fact];
            fact.retainedOpportunity =
                promotion.alive && (quietEnd || mateWinner == fact.beneficiary);
        }
    }
    return facts;
}
namespace {
std::vector<TacticalFact> factsFromPair(const chess::Position& root, const MoveEvidence& pair,
                                        std::uint32_t modelVersion) {
    auto result = inspectTacticalLine(root, playedLine(pair), FactSource::Played, modelVersion);
    if (!pair.before.candidates.empty()) {
        const auto& continuation = pair.before.candidates.front().continuation;
        auto best = inspectTacticalLine(
            root, std::span(continuation).first(std::min(continuation.size(), tacticalLineLimit)),
            FactSource::Best, modelVersion);
        result.insert(result.end(), best.begin(), best.end());
    }
    return result;
}
} // namespace
std::vector<TacticalFact> tacticalFacts(const chess::Position& root, const MoveEvidence& supplied,
                                        const Configuration& config, std::uint32_t modelVersion) {
    if (modelVersion != 1 && modelVersion != 2 && modelVersion != 3 && modelVersion != 4) {
        return {};
    }
    const std::uint32_t expectedModel = modelVersion >= 4 ? 2U : 1U;
    const auto pair = pairEvidence(root, supplied.playedMove, supplied.before, supplied.after,
                                   config.conversion, expectedModel);
    if ((pair.issue && pair.issue != EvidenceIssue::MateClaim) ||
        (pair.beforeEvaluation.issue && pair.beforeEvaluation.issue != EvidenceIssue::MateClaim) ||
        (pair.afterEvaluation.issue && pair.afterEvaluation.issue != EvidenceIssue::MateClaim)) {
        return {};
    }
    return factsFromPair(root, pair, modelVersion);
}
void addIntelligenceCandidates(ScanAssessment& scan, const chess::Position& root,
                               const MoveEvidence& pair, const Configuration& config) {
    const auto models =
        scan.pipelineVersion ? intelligenceModelsForPipeline(*scan.pipelineVersion) : std::nullopt;
    if (!models || scan.state != ScanState::Usable || !validSpecialConfig(config.special)) {
        return;
    }
    const auto facts = tacticalFacts(root, pair, config, models->motif);
    const auto add = [&](RefinementReason reason) {
        if (std::find(scan.reasons.begin(), scan.reasons.end(), reason) == scan.reasons.end()) {
            scan.reasons.push_back(reason);
        }
    };
    const bool plausibleSacrifice = !scan.pipelineVersion || *scan.pipelineVersion < 14 ||
                                    !scan.loss ||
                                    *scan.loss <= config.classification.inaccuracyLoss;
    for (const auto& fact : facts) {
        // A remote material balance at a truncated PV horizon is not a cheap
        // tactical opportunity signal. Admit immediate motifs or short concrete gains.
        const bool immediate = fact.linePlies == 1 &&
                               ((fact.kind == TacticalKind::HangingPiece && fact.material >= 3) ||
                                fact.kind == TacticalKind::Fork ||
                                (fact.kind == TacticalKind::Promotion &&
                                 (models->motif == 1 || fact.retainedOpportunity)));
        const bool shortGain =
            (models->motif == 1 || fact.retainedOpportunity) && fact.linePlies <= 5 &&
            (fact.kind == TacticalKind::Checkmate || fact.kind == TacticalKind::Exchange ||
             (fact.kind == TacticalKind::Material && fact.material >= 3));
        if (fact.source == FactSource::Best && fact.beneficiary == root.sideToMove() &&
            (immediate || shortGain)) {
            add(RefinementReason::TacticalOpportunity);
        }
        if (fact.source == FactSource::Played && fact.kind == TacticalKind::Sacrifice &&
            fact.material >= config.special.sacrificeValue && plausibleSacrifice) {
            add(RefinementReason::SacrificeSignal);
        }
    }
    // MultiPV=1 cannot establish uniqueness. Check evasions and constrained choices
    // are cheap investigation signals; deeper alternatives must verify the claim.
    const auto legalCount = root.legalMoves().size();
    if (legalCount > 1 && (root.inCheck() || legalCount <= 3)) {
        add(RefinementReason::UniqueResource);
    }
}
SpecialAssessment verifySpecial(const chess::Position& root, const MoveEvidence& supplied,
                                const MoveEvidence* older, const Configuration& config,
                                std::uint32_t modelVersion) {
    SpecialAssessment out;
    out.modelVersion = modelVersion;
    const auto rule = [modelVersion](const char* name) {
        return std::string(name) + ".v" + std::to_string(modelVersion);
    };
    out.thresholds = config.special;
    out.status = SpecialStatus::Insufficient;
    if (modelVersion != 1 && modelVersion != 2 && modelVersion != 3 && modelVersion != 4) {
        out.verificationPossible = false;
        out.ruleId = "special.unsupported_model";
        return out;
    }
    if (!validSpecialConfig(config.special) || !std::isfinite(config.refinement.maximumDrift) ||
        config.refinement.maximumDrift < 0 || config.refinement.maximumDrift > 1 ||
        !std::isfinite(config.refinement.boundaryMargin) || config.refinement.boundaryMargin < 0 ||
        config.refinement.boundaryMargin > 1 || config.refinement.multiPv < 1 ||
        config.refinement.multiPv > 256 || !std::isfinite(config.conversion.fallbackScaleCp) ||
        config.conversion.fallbackScaleCp < 1 || config.conversion.fallbackScaleCp > 10000) {
        out.verificationPossible = false;
        out.ruleId = rule("special.invalid_configuration");
        return out;
    }
    const std::uint32_t classModel = modelVersion >= 4 ? 3U : 2U;
    const std::uint32_t expectedModel = modelVersion >= 4 ? 2U : 1U;
    const std::uint32_t stabilityModel = modelVersion >= 4 ? 4U : 3U;
    const auto latestClass =
        classify(root, supplied, config.classification, config.conversion, classModel);
    if (latestClass.issue || !latestClass.loss) {
        if (latestClass.issue == ClassificationIssue::InvalidConfiguration) {
            out.verificationPossible = false;
            out.ruleId = rule("special.invalid_configuration");
            return out;
        }
        out.verificationPossible = !requiresInputRepair(latestClass);
        out.ruleId = rule("special.numeric_evidence_required");
        return out;
    }
    if (*latestClass.loss > config.special.maximumLoss &&
        *latestClass.loss < config.special.missLoss) {
        out.status = SpecialStatus::Rejected;
        out.ruleId = rule("special.quality_requirements_not_met");
        return out;
    }
    if (root.legalMoves().size() <= 1 || supplied.before.candidates.size() < 2) {
        out.verificationPossible = false;
        out.ruleId = rule("special.alternative_coverage_unavailable");
        return out;
    }
    if (modelVersion >= 2) {
        const auto second = runnerUp(supplied, config.conversion, expectedModel);
        const bool possibleMiss = *latestClass.loss >= config.special.missLoss;
        const double minViable = modelVersion >= 4 ? std::min(0.40, config.special.viableScore)
                                                   : config.special.viableScore;
        const bool possibleResource =
            *latestClass.loss <= config.special.maximumLoss && latestClass.playedMoveRank == 1 &&
            second && latestClass.after.expected->value >= minViable &&
            latestClass.before.expected->value - *second >= config.special.alternativeGap;
        if (!possibleMiss && !possibleResource) {
            out.status = SpecialStatus::Rejected;
            out.ruleId = rule("special.quality_requirements_not_met");
            return out;
        }
    }
    if (!older) {
        out.status = SpecialStatus::Pending;
        out.ruleId = rule("special.independent_verification_pending");
        return out;
    }
    const auto oldClass =
        classify(root, *older, config.classification, config.conversion, classModel);
    if (requiresInputRepair(oldClass)) {
        out.verificationPossible = false;
        out.ruleId = rule("special.evidence_invalid");
        return out;
    }
    const auto latest = pairEvidence(root, supplied.playedMove, supplied.before, supplied.after,
                                     config.conversion, expectedModel);
    const auto previous = pairEvidence(root, older->playedMove, older->before, older->after,
                                       config.conversion, expectedModel);
    const auto stable =
        assessStability(oldClass, latestClass, previous, latest, config.refinement, stabilityModel);
    if (!stable.stable || !oldClass.loss) {
        out.ruleId = rule("special.independent_evidence_insufficient");
        return out;
    }
    const auto& c = config.special;
    const auto second = runnerUp(latest, config.conversion, expectedModel);
    const auto oldSecond = runnerUp(previous, config.conversion, expectedModel);
    if (modelVersion < 4) {
        const auto comparison = compareAlternatives(root, latest, &previous, config);
        if (comparison.reason != ComparisonReason::Confirmed) {
            out.ruleId = rule("special.independent_evidence_insufficient");
            return out;
        }
    } else {
        if (!second || !oldSecond) {
            out.ruleId = rule("special.independent_evidence_insufficient");
            return out;
        }
        const double gapCheck = latestClass.before.expected->value - *second;
        const double oldGapCheck = oldClass.before.expected->value - *oldSecond;
        const double secondDrift = std::abs(*second - *oldSecond);
        const bool decisiveRunnerUp =
            secondDrift <= std::max(config.refinement.maximumDrift, 0.08) &&
            gapCheck >= c.alternativeGap + c.margin && oldGapCheck >= c.alternativeGap + c.margin &&
            *second <= c.alreadyWinningScore - c.margin &&
            *oldSecond <= c.alreadyWinningScore - c.margin;
        if (secondDrift > config.refinement.maximumDrift && !decisiveRunnerUp) {
            out.ruleId = rule("special.independent_evidence_insufficient");
            return out;
        }
    }
    const auto facts = factsFromPair(root, latest, modelVersion);
    const auto oldFacts = factsFromPair(root, previous, modelVersion);
    for (const auto& fact : facts) {
        if (std::any_of(oldFacts.begin(), oldFacts.end(),
                        [&](const auto& old) { return sameFact(fact, old); })) {
            out.witnesses.push_back(fact);
        }
    }
    const bool native =
        latestClass.before.expected->source != ConversionSource::ApproximateCentipawns &&
        latestClass.after.expected->source != ConversionSource::ApproximateCentipawns &&
        oldClass.before.expected->source != ConversionSource::ApproximateCentipawns &&
        oldClass.after.expected->source != ConversionSource::ApproximateCentipawns;
    const auto difference = [native](double a, double b) {
        const auto x = std::round(a * 2000);
        const auto y = std::round(b * 2000);
        if (native && a == x / 2000 && b == y / 2000) {
            return (x - y) / 2000;
        }
        return a - b;
    };
    if (std::abs(difference(*latestClass.loss, *oldClass.loss)) > c.maximumLossDrift) {
        out.ruleId = rule("special.loss_drift");
        return out;
    }
    const auto clears = [&](double a, double b) { return difference(a, b) > c.margin; };
    const bool quality = *latestClass.loss <= c.maximumLoss && *oldClass.loss <= c.maximumLoss;
    const bool best = latestClass.playedMoveRank == 1 && oldClass.playedMoveRank == 1;
    if (second) {
        out.alternativeGap = difference(latestClass.before.expected->value, *second);
    }
    if (modelVersion >= 4) {
        const double gap = *out.alternativeGap;
        const double oldGap = difference(oldClass.before.expected->value, *oldSecond);
        const bool unique = gap >= c.alternativeGap && oldGap >= c.alternativeGap;
        const bool sacViable = latestClass.after.expected->value >= c.viableScore &&
                               oldClass.after.expected->value >= c.viableScore;
        const bool sacNotTrivial =
            *second < c.alreadyWinningScore && *oldSecond < c.alreadyWinningScore;
        const auto sacMatch = [&](const std::vector<TacticalFact>& list,
                                  const TacticalFact* sameSquareAs = nullptr) {
            return std::find_if(list.begin(), list.end(), [&](const auto& f) {
                return f.source == FactSource::Played && f.kind == TacticalKind::Sacrifice &&
                       f.material >= c.sacrificeValue &&
                       (!sameSquareAs || f.square == sameSquareAs->square);
            });
        };
        const auto latestSac = sacMatch(facts);
        const bool sacrifice =
            latestSac != facts.end() && sacMatch(oldFacts, &*latestSac) != oldFacts.end();
        const bool greatViable =
            latestClass.after.expected->value >= 0.40 && oldClass.after.expected->value >= 0.40;
        const bool defensiveSave = *second <= c.viableScore && *oldSecond <= c.viableScore;
        const bool decisiveFind =
            (latestClass.after.expected->value >= 0.62 && oldClass.after.expected->value >= 0.62 &&
             *second < 0.60 && *oldSecond < 0.60) ||
            (gap >= 0.15 && oldGap >= 0.15);
        const bool notTrivialGreat =
            *second < c.alreadyWinningScore && *oldSecond < c.alreadyWinningScore;
        if (quality && best && unique && sacViable && sacNotTrivial && sacrifice) {
            if (std::none_of(out.witnesses.begin(), out.witnesses.end(), [](const auto& f) {
                    return f.source == FactSource::Played && f.kind == TacticalKind::Sacrifice;
                })) {
                out.witnesses.push_back(*latestSac);
            }
            out.label = Classification::Brilliant;
            out.ruleId = rule("special.brilliant.tactical_sacrifice");
            out.status = SpecialStatus::Verified;
            return out;
        }
        if (quality && best && unique && greatViable && notTrivialGreat &&
            (defensiveSave || decisiveFind) && root.legalMoves().size() > 1 &&
            !isTrivialRecapture(root, latest.playedMove)) {
            out.label = Classification::Great;
            out.ruleId = rule("special.great.unique_resource");
            out.status = SpecialStatus::Verified;
            return out;
        }
        if (*latestClass.loss >= c.missLoss && *oldClass.loss >= c.missLoss &&
            latestClass.before.expected->value >= 0.58 && oldClass.before.expected->value >= 0.58) {
            const bool missedMate = latest.mate.before && latest.mate.before->value > 0 &&
                                    previous.mate.before && previous.mate.before->value > 0 &&
                                    !(latest.mate.after && latest.mate.after->value > 0 &&
                                      previous.mate.after && previous.mate.after->value > 0);
            const bool nonBlunderedMiss =
                latestClass.after.expected->value >= 0.40 && oldClass.after.expected->value >= 0.40;
            const bool concreteOpportunity =
                std::any_of(out.witnesses.begin(), out.witnesses.end(), [&](const auto& f) {
                    if (f.source != FactSource::Best || !opportunity(f, latest.mover)) {
                        return false;
                    }
                    const auto forfeits = [&](const auto& witnesses) {
                        return std::none_of(
                            witnesses.begin(), witnesses.end(), [&](const auto& played) {
                                return played.source == FactSource::Played &&
                                       opportunity(played, latest.mover) && played.kind == f.kind &&
                                       played.material >= f.material;
                            });
                    };
                    return forfeits(facts) && forfeits(oldFacts);
                });
            const bool missedWinningShot = latestClass.before.expected->value >= 0.65 &&
                                           oldClass.before.expected->value >= 0.65 &&
                                           gap >= c.alternativeGap && oldGap >= c.alternativeGap;
            if (nonBlunderedMiss && (missedMate || concreteOpportunity || missedWinningShot)) {
                out.label = Classification::Miss;
                out.ruleId = rule("special.miss.verified_opportunity");
                out.status = SpecialStatus::Verified;
                return out;
            }
        }
        out.status = SpecialStatus::Rejected;
        out.ruleId = rule("special.requirements_not_met");
        return out;
    }
    const bool unique =
        second && oldSecond &&
        clears(difference(latestClass.before.expected->value, *second), c.alternativeGap) &&
        clears(difference(oldClass.before.expected->value, *oldSecond), c.alternativeGap);
    const bool viable = clears(latestClass.after.expected->value, c.viableScore) &&
                        clears(oldClass.after.expected->value, c.viableScore);
    const bool critical =
        second && oldSecond && clears(c.viableScore, *second) && clears(c.viableScore, *oldSecond);
    const bool notTrivial = clears(c.alreadyWinningScore, latestClass.before.expected->value) &&
                            clears(c.alreadyWinningScore, oldClass.before.expected->value);
    const bool sacrifice =
        std::any_of(out.witnesses.begin(), out.witnesses.end(), [&](const auto& f) {
            return f.source == FactSource::Played && f.kind == TacticalKind::Sacrifice &&
                   f.material >= c.sacrificeValue;
        });
    bool missedDisagreement = false;
    if (quality && best && unique && viable && notTrivial && sacrifice) {
        out.label = Classification::Brilliant;
        out.ruleId = rule("special.brilliant.tactical_sacrifice");
    } else if (quality && best && unique && viable && critical && root.legalMoves().size() > 1) {
        out.label = Classification::Great;
        out.ruleId = rule("special.great.unique_resource");
    } else if (clears(*latestClass.loss, c.missLoss) && clears(*oldClass.loss, c.missLoss)) {
        const bool missed =
            std::any_of(out.witnesses.begin(), out.witnesses.end(), [&](const auto& f) {
                if (f.source != FactSource::Best || !opportunity(f, latest.mover)) {
                    return false;
                }
                const auto forfeits = [&](const auto& witnesses) {
                    return std::none_of(
                        witnesses.begin(), witnesses.end(), [&](const auto& played) {
                            return played.source == FactSource::Played &&
                                   opportunity(played, latest.mover) && played.kind == f.kind &&
                                   played.material >= f.material;
                        });
                };
                const bool currentMiss = forfeits(facts);
                const bool previousMiss = modelVersion == 1 || forfeits(oldFacts);
                missedDisagreement =
                    missedDisagreement || (modelVersion >= 2 && currentMiss != previousMiss);
                return currentMiss && previousMiss;
            });
        if (missed) {
            out.label = Classification::Miss;
            out.ruleId = rule("special.miss.verified_opportunity");
        }
    }
    // A tactical witness appearing in only one of the otherwise stable attempts
    // is unresolved verification, not a confidently rejected candidate.
    const bool tacticalDisagreement = std::any_of(facts.begin(), facts.end(), [&](const auto& f) {
        const bool relevant =
            (quality && best && unique && viable && notTrivial && f.source == FactSource::Played &&
             f.kind == TacticalKind::Sacrifice && f.material >= c.sacrificeValue) ||
            (clears(*latestClass.loss, c.missLoss) && f.source == FactSource::Best &&
             opportunity(f, latest.mover));
        return relevant && std::none_of(oldFacts.begin(), oldFacts.end(),
                                        [&](const auto& old) { return sameFact(f, old); });
    });
    const bool olderOnly =
        modelVersion >= 2 && std::any_of(oldFacts.begin(), oldFacts.end(), [&](const auto& f) {
            const bool relevant =
                (quality && best && unique && viable && notTrivial &&
                 f.source == FactSource::Played && f.kind == TacticalKind::Sacrifice &&
                 f.material >= c.sacrificeValue) ||
                (clears(*oldClass.loss, c.missLoss) && f.source == FactSource::Best &&
                 opportunity(f, latest.mover));
            return relevant && std::none_of(facts.begin(), facts.end(), [&](const auto& current) {
                       return sameFact(f, current);
                   });
        });
    if (!out.label && (tacticalDisagreement || olderOnly || missedDisagreement)) {
        out.status = SpecialStatus::Insufficient;
        out.ruleId = rule("special.tactical_witness_disagreement");
        return out;
    }
    out.status = out.label ? SpecialStatus::Verified : SpecialStatus::Rejected;
    if (!out.label) {
        out.ruleId = rule("special.requirements_not_met");
    }
    return out;
}
} // namespace chess_review::review
