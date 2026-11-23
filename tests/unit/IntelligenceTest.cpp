#include "review/Intelligence.h"
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
#include <variant>
using namespace chess_review;
using namespace chess_review::review;
namespace {
chess::Position position(const char* fen) {
    auto result = chess::Position::fromFen(fen);
    INFO(fen);
    REQUIRE(std::holds_alternative<chess::Position>(result));
    return std::get<chess::Position>(std::move(result));
}
std::vector<chess::Move> line(chess::Position root, std::initializer_list<const char*> moves) {
    std::vector<chess::Move> result;
    for (const auto text : moves) {
        INFO(text);
        auto parsed = root.parseUci(text);
        REQUIRE(std::holds_alternative<chess::Move>(parsed));
        const auto move = std::get<chess::Move>(parsed);
        REQUIRE(std::holds_alternative<std::monostate>(root.play(move)));
        result.push_back(move);
    }
    return result;
}
PositionEvidence evidence(const chess::Position& root, int wins, const std::vector<chess::Move>& pv,
                          bool newer, const char* suffix) {
    PositionEvidence e;
    e.position = root.context();
    e.complete = true;
    e.engineIdentity = "engine";
    e.attemptIdentity = std::string(newer ? "verify-" : "deep-") + suffix;
    e.strength = {12, newer ? 5000000U : 1000000U, {}};
    e.evaluation = {root.sideToMove(), RawScore{ScoreKind::Centipawns, 0},
                    Wdl{wins, 0, 1000 - wins}};
    e.candidates = {{1, e.evaluation, pv, e.strength}};
    return e;
}
Configuration wdlConfig(bool legacyThresholds = false) {
    Configuration c;
    c.conversion.preferWdl = true;
    if (legacyThresholds) {
        c.classification.excellentLoss = 0.01;
        c.special.maximumLoss = 0.01;
        c.special.alternativeGap = 0.15;
        c.special.missLoss = 0.15;
        c.special.alreadyWinningScore = 0.95;
    }
    return c;
}
MoveEvidence pair(const chess::Position& root, const std::vector<chess::Move>& best,
                  const std::vector<chess::Move>& played, const chess::Move& alternative,
                  int beforeWins, int afterWins, int alternativeWins, bool newer,
                  std::uint32_t expectedModel = 1) {
    auto before = evidence(root, beforeWins, best, newer, "before");
    auto second = before.evaluation;
    second.wdl = Wdl{alternativeWins, 0, 1000 - alternativeWins};
    before.candidates.push_back({2, second, {alternative}, before.strength});
    auto child = root;
    REQUIRE(std::holds_alternative<std::monostate>(child.play(played.front())));
    std::vector<chess::Move> reply(played.begin() + 1, played.end());
    REQUIRE(!(reply.empty()));
    return pairEvidence(root, played.front(), before,
                        evidence(child, 1000 - afterWins, reply, newer, "after"), {300, true},
                        expectedModel);
}
bool has(const std::vector<TacticalFact>& facts, TacticalKind kind) {
    return std::any_of(facts.begin(), facts.end(),
                       [&](const auto& fact) { return fact.kind == kind; });
}
} // namespace
TEST_CASE("Special labels verify a critical unique resource and preserve ordinary evidence",
          "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto alt = line(root, {"d2d4"}).front();
    const auto a = pair(root, pv, pv, alt, 600, 600, 400, false);
    const auto b = pair(root, pv, pv, alt, 600, 600, 400, true);
    REQUIRE(!(a.issue));
    REQUIRE(!(b.issue));
    const auto cfg = wdlConfig(true);
    const auto result = verifySpecial(root, b, &a, cfg);
    REQUIRE(result.status == SpecialStatus::Verified);
    CHECK(result.label == Classification::Great);
    CHECK(result.alternativeGap == Catch::Approx(.2));
    CHECK(classify(root, b, cfg.classification, cfg.conversion).label == Classification::Best);
    CHECK(verifySpecial(root, b, nullptr, cfg).status == SpecialStatus::Pending);
    CHECK(verifySpecial(root, a, &a, cfg).needsEvidence());
    auto missing = b;
    missing.before.candidates.resize(1);
    const auto noCoverage = verifySpecial(root, missing, &a, cfg);
    CHECK(!(noCoverage.label));
    CHECK(!(noCoverage.needsEvidence()));
    auto forged = b;
    forged.before.engineIdentity = "another engine";
    CHECK(!(verifySpecial(root, forged, &a, cfg).label));
}
TEST_CASE("Special cutoffs require clearance and agreement", "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto alt = line(root, {"d2d4"}).front();
    const auto legacy = wdlConfig(true);
    for (const auto scores : {std::array{510, 510, 350}, std::array{600, 600, 490},
                              std::array{600, 580, 400}, std::array{800, 800, 600}}) {
        const auto a = pair(root, pv, pv, alt, scores[0], scores[1], scores[2], false);
        const auto b = pair(root, pv, pv, alt, scores[0], scores[1], scores[2], true);
        CHECK(!(verifySpecial(root, b, &a, legacy, 3).label));
    }
    auto config = Configuration{};
    config.special.margin = std::numeric_limits<double>::quiet_NaN();
    CHECK(!(validSpecialConfig(config.special)));
}
TEST_CASE("Miss requires a concrete opportunity forfeited by the played move",
          "[unit][review][intelligence]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d5", "h8g8"});
    const auto played = line(root, {"a1b1", "d5e5"});
    const auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    const auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    REQUIRE(!(a.issue));
    const auto legacy = wdlConfig(true);
    const auto result = verifySpecial(root, b, &a, legacy);
    REQUIRE(result.status == SpecialStatus::Verified);
    CHECK(result.label == Classification::Miss);
    CHECK(has(result.witnesses, TacticalKind::Material));
    CHECK(classify(root, b, legacy.classification, legacy.conversion).label ==
          Classification::Blunder);
    const auto quietRoot = chess::Position::initial();
    const auto quietBest = line(quietRoot, {"e2e4", "e7e5"});
    const auto quietPlayed = line(quietRoot, {"d2d4", "d7d5"});
    const auto q1 =
        pair(quietRoot, quietBest, quietPlayed, quietPlayed.front(), 800, 550, 550, false);
    const auto q2 =
        pair(quietRoot, quietBest, quietPlayed, quietPlayed.front(), 800, 550, 550, true);
    CHECK(!(verifySpecial(quietRoot, q2, &q1, legacy, 3).label));
}
TEST_CASE("Brilliant requires a resolved tactical sacrifice and nontrivial soundness",
          "[unit][review][intelligence]") {
    const auto root = position("5rk1/5ppp/8/8/8/3B1N2/5PPP/3Q2K1 w - - 0 1");
    const auto pv = line(root, {"d3h7", "g8h7", "f3g5", "h7g8", "d1h5", "g7g6", "h5h7"});
    const auto facts = inspectTacticalLine(root, pv, FactSource::Played);
    REQUIRE(has(facts, TacticalKind::Checkmate));
    REQUIRE(has(facts, TacticalKind::Sacrifice));
    const auto alt = line(root, {"d1e2"}).front();
    const auto a = pair(root, pv, pv, alt, 600, 600, 400, false);
    const auto b = pair(root, pv, pv, alt, 600, 600, 400, true);
    REQUIRE(!(a.issue));
    const auto legacy = wdlConfig(true);
    const auto result = verifySpecial(root, b, &a, legacy);
    REQUIRE(result.label == Classification::Brilliant);
    const auto won1 = pair(root, pv, pv, alt, 960, 960, 400, false);
    const auto won2 = pair(root, pv, pv, alt, 960, 960, 400, true);
    CHECK(verifySpecial(root, won2, &won1, legacy, 3).label != Classification::Brilliant);
    auto shortened = pv;
    shortened.resize(2);
    CHECK(!(
        has(inspectTacticalLine(root, shortened, FactSource::Played, 3), TacticalKind::Sacrifice)));
}
TEST_CASE("Tactical facts are legal, bounded and distinguish line consequences",
          "[unit][review][intelligence]") {
    const auto ep = position("7k/8/8/3pP3/8/8/8/K7 w - d6 0 2");
    const auto capture = line(ep, {"e5d6", "h8g8"});
    const auto facts = inspectTacticalLine(ep, capture, FactSource::Played);
    REQUIRE(has(facts, TacticalKind::Material));
    CHECK(facts.back().material == 1);
    CHECK(!(has(facts, TacticalKind::Exchange)));
    auto illegal = capture;
    illegal.push_back(capture.front());
    CHECK(inspectTacticalLine(ep, illegal, FactSource::Played).empty());
    const auto promotion = position("7k/P7/8/8/8/8/8/K7 w - - 0 1");
    CHECK(has(inspectTacticalLine(promotion, line(promotion, {"a7a8n"}), FactSource::Best),
              TacticalKind::Promotion));
    const auto fork = position("3q3k/8/8/8/8/4N3/8/K7 w - - 0 1");
    CHECK(has(inspectTacticalLine(fork, line(fork, {"e3f5"}), FactSource::Played),
              TacticalKind::Fork) == false);
    const auto checking = position("3q3k/8/8/4N3/8/8/8/K7 w - - 0 1");
    CHECK(has(inspectTacticalLine(checking, line(checking, {"e5f7"}), FactSource::Played),
              TacticalKind::Fork));
}
TEST_CASE("New adaptive signals leave historical selection unchanged",
          "[unit][review][intelligence]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d5", "h8g8"});
    const auto a = pair(root, best, best, line(root, {"a1b1"}).front(), 600, 600, 400, false);
    const auto cfg = wdlConfig(true);
    for (const auto policy : {9U, 10U}) {
        auto scan = assessQuickScan(classify(root, a, cfg.classification, cfg.conversion), a,
                                    cfg.refinement, policy);
        addIntelligenceCandidates(scan, root, a, cfg);
        const bool tactical =
            std::find(scan.reasons.begin(), scan.reasons.end(),
                      RefinementReason::TacticalOpportunity) != scan.reasons.end();
        CHECK(tactical == (policy == 10));
        const std::array<RefinementCandidate, 1> candidates{{{1, scan}}};
        CHECK(std::holds_alternative<std::vector<int>>(
            refinementQueue(candidates, policy, Quality::Standard)));
    }
}

TEST_CASE("Special tactical disagreement requests bounded verification rather than a label",
          "[unit][review][intelligence]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d5", "h8g8"});
    const auto played = line(root, {"a1b1", "d5e5"});
    auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    const auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    // Same candidate identity and stable scores, but only the newer continuation
    // reaches the material capture. This is not a verified missed chance yet.
    // Keep candidate identity fixed: its older witness ends before a later capture.
    const auto delayed = line(root, {"d1d2", "d5e6", "d2e2", "h8g8", "e2e6"});
    a.before.candidates.front().continuation = {delayed.front()};
    auto latest = b;
    latest.before.candidates.front().continuation = delayed;
    const auto legacy = wdlConfig(true);
    const auto result = verifySpecial(root, latest, &a, legacy, 3);
    CHECK(!(result.label));
    CHECK(result.needsEvidence());
    CHECK(result.ruleId == "special.tactical_witness_disagreement.v3");
}

TEST_CASE("Named motifs reject pinned threats and distinguish trades from pawn gains",
          "[unit][review][intelligence]") {
    const auto pinned = position("4r3/8/8/8/3q1k2/8/8/2N1K3 w - - 0 1");
    const auto block = line(pinned, {"c1e2"});
    CHECK(!(has(inspectTacticalLine(pinned, block, FactSource::Played), TacticalKind::Fork)));
    const auto defended = position("3r3k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    CHECK(!(has(inspectTacticalLine(defended, line(defended, {"d1d5"}), FactSource::Played),
                TacticalKind::HangingPiece)));
    const auto trade = position("8/4k1p1/5r2/8/8/2B5/8/4K3 w - - 0 1");
    const auto traded =
        inspectTacticalLine(trade, line(trade, {"c3f6", "g7f6"}), FactSource::Played);
    CHECK(has(traded, TacticalKind::Exchange));
    CHECK(!(has(traded, TacticalKind::Sacrifice)));
    const auto pawns = position("7k/8/8/8/8/p7/p7/R6K w - - 0 1");
    const auto collected =
        inspectTacticalLine(pawns, line(pawns, {"a1a2", "h8g8", "a2a3"}), FactSource::Played);
    CHECK(has(collected, TacticalKind::Material));
    CHECK(!(has(collected, TacticalKind::Exchange)));
}

TEST_CASE("Remote PV material balances do not deepen quiet opening moves",
          "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "d7d5", "e4d5", "g8f6", "b1c3", "f6g4", "g1f3"});
    const auto a = pair(root, pv, pv, line(root, {"d2d4"}).front(), 600, 600, 400, false);
    REQUIRE(!a.issue);
    const auto cfg = wdlConfig(true);
    CHECK(has(tacticalFacts(root, a, cfg), TacticalKind::Material));
    auto scan = assessQuickScan(classify(root, a, cfg.classification, cfg.conversion), a,
                                cfg.refinement, 10);
    REQUIRE(!scan.needsRefinement());
    addIntelligenceCandidates(scan, root, a, cfg);
    CHECK(!scan.needsRefinement());
}

TEST_CASE("Ordinary moves outside special quality ranges request no extra verification",
          "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto best = line(root, {"e2e4", "e7e5"});
    const auto played = line(root, {"d2d4", "d7d5"});
    const auto supplied = pair(root, best, played, played.front(), 600, 560, 400, true);
    const auto result = verifySpecial(root, supplied, nullptr, wdlConfig(true));
    CHECK(result.status == SpecialStatus::Rejected);
    CHECK(!result.needsEvidence());
}

TEST_CASE("A promotion fork names the promoted actor", "[unit][review][adversarial]") {
    const auto root = position("8/P1k5/1r6/8/8/8/8/7K w - - 0 1");
    const auto facts = inspectTacticalLine(root, line(root, {"a7a8n"}), FactSource::Played);
    const auto fork = std::find_if(facts.begin(), facts.end(),
                                   [](const auto& f) { return f.kind == TacticalKind::Fork; });
    REQUIRE(fork != facts.end());
    CHECK(fork->actor == chess::PieceType::Knight);
    const auto legacy = inspectTacticalLine(root, line(root, {"a7a8n"}), FactSource::Played, 1);
    const auto oldFork = std::find_if(legacy.begin(), legacy.end(), [](const auto& fact) {
        return fact.kind == TacticalKind::Fork;
    });
    REQUIRE(oldFork != legacy.end());
    CHECK(oldFork->actor == chess::PieceType::Pawn);
}
TEST_CASE("A promotion lost in the same line is not a verified missed opportunity",
          "[unit][review][adversarial]") {
    const auto root = position("1r5k/P7/8/8/8/8/8/7K w - - 0 1");
    const auto best = line(root, {"a7a8q", "b8a8"});
    const auto played = line(root, {"h1g1", "b8b7"});
    const auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    const auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    REQUIRE(!a.issue);
    const auto legacy = wdlConfig(true);
    CHECK(!verifySpecial(root, b, &a, legacy, 3).label);
    CHECK(verifySpecial(root, b, &a, legacy, 1).label == Classification::Miss);
}
TEST_CASE("A PV ending before an immediate recapture cannot verify Miss",
          "[unit][review][adversarial]") {
    const auto root = position("3r3k/8/8/3p4/8/8/8/K2Q4 w - - 0 1");
    const auto best = line(root, {"d1d5"});
    const auto played = line(root, {"a1b1", "d8d7"});
    const auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    const auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    REQUIRE(!a.issue);
    const auto legacy = wdlConfig(true);
    CHECK(!verifySpecial(root, b, &a, legacy, 3).label);
    CHECK(verifySpecial(root, b, &a, legacy, 1).label == Classification::Miss);
}
TEST_CASE("A mate conversion gap cannot authorize facts from invalid paired evidence",
          "[unit][review][adversarial]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d5", "h8g8"});
    const auto played = line(root, {"a1b1", "d5e5"});
    auto supplied = pair(root, best, played, played.front(), 800, 550, 550, false);
    supplied.before.evaluation.score = RawScore{ScoreKind::Mate, 3};
    supplied.before.candidates.front().evaluation = supplied.before.evaluation;
    supplied.after.restricted = true;
    const auto cfg = wdlConfig(true);
    CHECK(tacticalFacts(root, supplied, cfg).empty());
    supplied.after.restricted = false;
    supplied.before.candidates.back().continuation = {chess::Move{}};
    CHECK(tacticalFacts(root, supplied, cfg).empty());
}

TEST_CASE("Both verification attempts must show that the played move forfeits the opportunity",
          "[unit][review][adversarial]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d5", "h8g8"});
    const auto previousPlayed = line(root, {"a1b1", "d5d2", "d1d2", "h8g8"});
    const auto latestPlayed = line(root, {"a1b1", "d5e5"});
    const auto a = pair(root, best, previousPlayed, previousPlayed.front(), 800, 550, 550, false);
    const auto b = pair(root, best, latestPlayed, latestPlayed.front(), 800, 550, 550, true);
    REQUIRE(!a.issue);
    const auto legacy = wdlConfig(true);
    const auto result = verifySpecial(root, b, &a, legacy, 3);
    CHECK(!result.label);
    CHECK(result.needsEvidence());
    CHECK(verifySpecial(root, b, &a, legacy, 1).label == Classification::Miss);
}
TEST_CASE("An older-only tactical witness remains uncertain in the hardened model",
          "[unit][review][adversarial]") {
    const auto root = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
    const auto best = line(root, {"d1d2", "d5e6", "d2e2", "h8g8", "e2e6"});
    const auto played = line(root, {"a1b1", "d5e5"});
    const auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    b.before.candidates.front().continuation.resize(1);
    const auto legacy = wdlConfig(true);
    CHECK(verifySpecial(root, b, &a, legacy, 3).needsEvidence());
    CHECK(verifySpecial(root, b, &a, legacy, 1).status == SpecialStatus::Rejected);
}
TEST_CASE("Unknown intelligence models fail closed", "[unit][review][adversarial]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto a = pair(root, pv, pv, line(root, {"d2d4"}).front(), 600, 600, 400, false);
    const auto result = verifySpecial(root, a, nullptr, {}, 99);
    CHECK(!result.label);
    CHECK(!result.needsEvidence());
    CHECK(tacticalFacts(root, a, {}, 99).empty());
    CHECK(inspectTacticalLine(root, pv, FactSource::Played, 99).empty());
    CHECK(!intelligenceModelsForPipeline(99));
}

TEST_CASE("Surviving promotions remain supported after the promoted piece moves",
          "[unit][review][adversarial]") {
    const auto root = position("7k/P7/8/8/8/8/8/7K w - - 0 1");
    const auto best = line(root, {"a7a8q", "h8h7", "a8b8", "h7g6"});
    const auto played = line(root, {"h1g1", "h8g8"});
    const auto a = pair(root, best, played, played.front(), 800, 550, 550, false);
    const auto b = pair(root, best, played, played.front(), 800, 550, 550, true);
    CHECK(verifySpecial(root, b, &a, wdlConfig(true), 3).label == Classification::Miss);
    const auto facts = inspectTacticalLine(root, best, FactSource::Best);
    const auto promotion = std::find_if(facts.begin(), facts.end(), [](const auto& fact) {
        return fact.kind == TacticalKind::Promotion;
    });
    REQUIRE(promotion != facts.end());
    CHECK(promotion->retainedOpportunity);
}

TEST_CASE("Caller supplied derived metadata cannot alter special verification",
          "[unit][review][adversarial]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto alternative = line(root, {"d2d4"}).front();
    const auto older = pair(root, pv, pv, alternative, 600, 600, 400, false);
    auto latest = pair(root, pv, pv, alternative, 600, 600, 400, true);
    latest.mover = chess::Color::Black;
    latest.loss = 1;
    latest.signedDelta = -1;
    latest.beforeEvaluation = {};
    latest.afterEvaluation = {};
    latest.issue = EvidenceIssue::Invalid;
    CHECK(verifySpecial(root, latest, &older, wdlConfig(true)).label == Classification::Great);
}

TEST_CASE("Invalid configuration or provenance is not permission for more engine work",
          "[unit][review][adversarial]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto a = pair(root, pv, pv, line(root, {"d2d4"}).front(), 600, 600, 400, false);
    for (int field = 0; field < 4; ++field) {
        Configuration config;
        if (field == 0) {
            config.special.margin = std::numeric_limits<double>::quiet_NaN();
        }
        if (field == 1) {
            config.refinement.maximumDrift = std::numeric_limits<double>::quiet_NaN();
        }
        if (field == 2) {
            config.classification.excellentLoss = -1;
        }
        if (field == 3) {
            config.conversion.fallbackScaleCp = 0;
        }
        const auto result = verifySpecial(root, a, nullptr, config);
        CHECK(!result.label);
        CHECK(!result.needsEvidence());
    }
    auto malformed = a;
    malformed.before.position.halfmoveClock++;
    CHECK(!verifySpecial(root, malformed, nullptr, {}).needsEvidence());
    const auto latest = pair(root, pv, pv, line(root, {"d2d4"}).front(), 600, 600, 400, true);
    CHECK(!verifySpecial(root, latest, &malformed, {}).needsEvidence());
}

TEST_CASE("A capture revealing a defender is not a hanging-piece witness",
          "[unit][review][adversarial]") {
    const auto root = position("7k/8/8/3q4/8/3R4/K7/3r4 w - - 0 1");
    const auto pv = line(root, {"d3d5"});
    const auto facts = inspectTacticalLine(root, pv, FactSource::Played);
    CHECK(!has(facts, TacticalKind::HangingPiece));
    CHECK(has(inspectTacticalLine(root, pv, FactSource::Played, 2), TacticalKind::HangingPiece));
}

TEST_CASE("Intelligence model v4 detects declined, counter, and mating sacrifices as Brilliant",
          "[unit][review][intelligence][v4]") {
    auto cpPair = [&](const chess::Position& root, const std::vector<chess::Move>& best,
                      const std::vector<chess::Move>& played, chess::Move alt,
                      std::int64_t beforeCp, std::int64_t afterCp, std::int64_t secondCp,
                      bool newer, std::optional<RawScore> beforeOverride = std::nullopt,
                      std::optional<RawScore> afterOverride = std::nullopt) {
        auto makeEv = [&](const chess::Position& pos, std::int64_t cp,
                          const std::vector<chess::Move>& pv, const char* suffix,
                          std::optional<RawScore> scoreOverride) {
            PositionEvidence e;
            e.position = pos.context();
            e.complete = true;
            e.engineIdentity = "engine";
            e.attemptIdentity = std::string(newer ? "verify-" : "deep-") + suffix;
            e.strength = {16, newer ? 400000U : 200000U, {}};
            e.evaluation = {
                pos.sideToMove(),
                scoreOverride.value_or(RawScore{ScoreKind::Centipawns, cp, Bound::Exact}),
                {}};
            e.candidates = {{1, e.evaluation, pv, e.strength}};
            return e;
        };
        auto before = makeEv(root, beforeCp, best, "before", beforeOverride);
        RawEvaluation secondEval{
            root.sideToMove(), RawScore{ScoreKind::Centipawns, secondCp, Bound::Exact}, {}};
        before.candidates.push_back({2, secondEval, {alt}, before.strength});
        auto child = root;
        REQUIRE(std::holds_alternative<std::monostate>(child.play(played.front())));
        std::vector<chess::Move> reply(played.begin() + 1, played.end());
        auto after = makeEv(child, -afterCp, reply, "after", afterOverride);
        return pairEvidence(root, played.front(), before, after, ConversionConfig{}, 2);
    };

    SECTION("3-ply mating queen sacrifice stays Brilliant even when before is already won") {
        // Position from Morphy vs Duke Karl / Count Isouard before 16. Qb8+ Nxb8 Rd8#
        const auto root = position("4kb1r/p2nqppp/5n2/1B2p1B1/4P3/1Q6/PPP2PPP/2KR4 w k - 1 16");
        // Clear b5 bishop first? Look: 15. Bxd7+ left white bishop on d7, taken by Nxd7 (on d7)!
        // Actually after 15... Nxd7, White's queen on b3 moves through empty b5 to b8:
        const auto operaRoot = position("4kb1r/p2nqppp/5n2/4p1B1/4P3/1Q6/PPP2PPP/2KR4 w k - 1 16");
        const auto pv = line(operaRoot, {"b3b8", "d7b8", "d1d8"});
        const auto alt = line(operaRoot, {"g5f6"}).front();
        const auto facts = inspectTacticalLine(operaRoot, pv, FactSource::Played, 4);
        CHECK(has(facts, TacticalKind::Sacrifice));
        const RawScore mateTwo{ScoreKind::Mate, 2, Bound::Exact};
        const RawScore mateOne{ScoreKind::Mate, -1, Bound::Exact};
        const auto a = cpPair(operaRoot, pv, pv, alt, 0, 0, 400, false, mateTwo, mateOne);
        const auto b = cpPair(operaRoot, pv, pv, alt, 0, 0, 410, true, mateTwo, mateOne);
        REQUIRE(!a.issue);
        REQUIRE(!b.issue);
        const auto result = verifySpecial(operaRoot, b, &a, Configuration{}, 4);
        CHECK(result.status == SpecialStatus::Verified);
        CHECK(result.label == Classification::Brilliant);
        (void)root;
    }

    SECTION("declined knight sacrifice on destination square is Brilliant") {
        const auto root =
            position("r1bqkb1r/1ppp1ppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 1");
        // White plays Nxc5/Nxb5-style sacrifice onto a pawn-defended square where PV declines.
        const auto pos =
            position("r2qkbnr/ppp2ppp/2np4/4p3/2B1P1b1/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 2 5");
        // c3d5 isn't defended; let's use classic c7/b5 square: Nb5 when a6 or c6 attacks b5!
        const auto b5Root =
            position("r1bqk2r/pp2bppp/2p2n2/3pp3/4P3/2NP1N2/PPP2PPP/R1BQKB1R w KQkq - 0 6");
        const auto pv = line(b5Root, {"c3b5", "d5e4"}); // c6 can capture b5, PV declines!
        const auto alt = line(b5Root, {"c3e2"}).front();
        const auto facts = inspectTacticalLine(b5Root, pv, FactSource::Played, 4);
        REQUIRE(has(facts, TacticalKind::Sacrifice));
        const auto a = cpPair(b5Root, pv, pv, alt, 420, 420, 150, false);
        const auto b = cpPair(b5Root, pv, pv, alt, 430, 430, 155, true);
        const auto result = verifySpecial(b5Root, b, &a, Configuration{}, 4);
        CHECK(result.status == SpecialStatus::Verified);
        CHECK(result.label == Classification::Brilliant);
        (void)root;
    }

    SECTION("declined counter-sacrifice leaving a major piece en prise is Brilliant") {
        // Fischer's 17... Be6 leaving Queen on b6 en prise to c3/Bxc3; PV plays Qxb6? No, PV
        // declines.
        const auto root = position("r3r1k1/pp3pbp/1qp1b1p1/2B5/2BP4/Q1n2N2/P4PPP/3R1K2 w - - 0 18");
        // Test symmetric setup from White's perspective: Qd3 left en prise while Bishop
        // retreats/moves.
        const auto counter = position("4k3/8/8/8/1r6/1Q1B4/8/4K3 w - - 0 1");
        // Rb4 threatens Qb3; White plays Bf5 leaving Qb3 en prise to Rb4xb3, PV plays Kf8.
        const auto pv = line(counter, {"d3f5", "e8f8"});
        const auto alt = line(counter, {"b3b4"}).front();
        const auto facts = inspectTacticalLine(counter, pv, FactSource::Played, 4);
        REQUIRE(has(facts, TacticalKind::Sacrifice));
        const auto a = cpPair(counter, pv, pv, alt, 300, 300, 100, false);
        const auto b = cpPair(counter, pv, pv, alt, 310, 310, 105, true);
        CHECK(verifySpecial(counter, b, &a, Configuration{}, 4).label == Classification::Brilliant);
    }

    SECTION("equal queen exchange is not classified as a counter-sacrifice") {
        const auto root = position("4k3/8/8/p7/1q6/1Q6/P7/4K3 w - - 0 1");
        const auto pv = line(root, {"e1e2", "e8e7"});
        const auto facts = inspectTacticalLine(root, pv, FactSource::Played, 4);
        CHECK(!has(facts, TacticalKind::Sacrifice));
    }

    SECTION("Great allows draw saves and excludes trivial recaptures; Miss excludes blunders") {
        // Defensive draw save at 0.50 when runner-up loses (-200cp -> ~0.32).
        const auto saveRoot = chess::Position::initial();
        const auto savePv = line(saveRoot, {"e2e4", "e7e5"});
        const auto saveAlt = line(saveRoot, {"d2d4"}).front();
        const auto s1 = cpPair(saveRoot, savePv, savePv, saveAlt, 0, 0, -200, false);
        const auto s2 = cpPair(saveRoot, savePv, savePv, saveAlt, 0, 0, -210, true);
        CHECK(verifySpecial(saveRoot, s2, &s1, Configuration{}, 4).label == Classification::Great);

        // Trivial recapture after Black just took a White knight on d5 should NOT get Great.
        auto capRoot = position("4k3/8/8/3n4/8/8/8/3QK3 b - - 0 1");
        // Actually play c6xd5 so root has prior move history on d5 and White recaptures down or
        // even.
        capRoot = position("4k3/8/2p5/3N4/8/8/8/3QK3 b - - 0 1");
        REQUIRE(
            std::holds_alternative<std::monostate>(capRoot.play(line(capRoot, {"c6d5"}).front())));
        const auto recPv = line(capRoot, {"d1d5", "e8e7"});
        const auto recAlt = line(capRoot, {"e1e2"}).front();
        const auto r1 = cpPair(capRoot, recPv, recPv, recAlt, 400, 400, 0, false);
        const auto r2 = cpPair(capRoot, recPv, recPv, recAlt, 405, 405, 5, true);
        const auto recRes = verifySpecial(capRoot, r2, &r1, Configuration{}, 4);
        CHECK(recRes.label != Classification::Great);

        // Missing mate while hanging mate yourself (<0.40) stays Blunder, not Miss.
        const auto missRoot = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
        const auto mateBest = line(missRoot, {"d1d5", "h8g8"});
        const auto blunderPlay = line(missRoot, {"a1b1", "d5d1"});
        const RawScore mBefore{ScoreKind::Mate, 3, Bound::Exact};
        const RawScore mAfter{ScoreKind::Mate, 1, Bound::Exact};
        const auto m1 = cpPair(missRoot, mateBest, blunderPlay, blunderPlay.front(), 0, -900, -900,
                               false, mBefore, mAfter);
        const auto m2 = cpPair(missRoot, mateBest, blunderPlay, blunderPlay.front(), 0, -900, -900,
                               true, mBefore, mAfter);
        CHECK(!verifySpecial(missRoot, m2, &m1, Configuration{}, 4).label);
    }

    SECTION("runner-up stability tolerates bounded drift on decisive gaps only") {
        const auto root = chess::Position::initial();
        const auto pv = line(root, {"e2e4", "e7e5"});
        const auto alt = line(root, {"d2d4"}).front();
        // ~0.027 runner-up drift (-114cp vs -84cp) with decisive gap (~0.32 >= 0.11) passes v4.
        const auto ok1 = cpPair(root, pv, pv, alt, 270, 270, -114, false);
        const auto ok2 = cpPair(root, pv, pv, alt, 271, 271, -84, true);
        const auto okRes = verifySpecial(root, ok2, &ok1, Configuration{}, 4);
        CHECK(okRes.status == SpecialStatus::Verified);
        CHECK(okRes.label == Classification::Great);

        // Drift above 0.08 fails stability even when both gaps are large.
        const auto wide1 = cpPair(root, pv, pv, alt, 270, 270, -300, false);
        const auto wide2 = cpPair(root, pv, pv, alt, 271, 271, -40, true);
        const auto wideRes = verifySpecial(root, wide2, &wide1, Configuration{}, 4);
        CHECK(wideRes.status == SpecialStatus::Insufficient);
        CHECK(wideRes.ruleId == "special.independent_evidence_insufficient.v4");

        // Borderline gap without margin buffer cannot relax runner-up drift above maximumDrift.
        const auto tight1 = cpPair(root, pv, pv, alt, 0, 0, -142, false);
        const auto tight2 = cpPair(root, pv, pv, alt, 0, 0, -112, true);
        const auto tightRes = verifySpecial(root, tight2, &tight1, Configuration{}, 4);
        CHECK(tightRes.status == SpecialStatus::Insufficient);
        CHECK(tightRes.ruleId == "special.independent_evidence_insufficient.v4");
    }

    SECTION("delayed capture of an undefended piece on a safe square is not Great") {
        // Black rook on e7 is completely undefended and e7 stays unattacked after Qxe7+.
        const auto freeRoot = position("k7/4r3/8/8/8/8/4Q3/4K3 w - - 0 1");
        const auto freePv = line(freeRoot, {"e2e7", "a8b8"});
        const auto freeAlt = line(freeRoot, {"e1d1"}).front();
        const auto f1 = cpPair(freeRoot, freePv, freePv, freeAlt, 400, 400, 0, false);
        const auto f2 = cpPair(freeRoot, freePv, freePv, freeAlt, 405, 405, 5, true);
        const auto freeRes = verifySpecial(freeRoot, f2, &f1, Configuration{}, 4);
        CHECK(freeRes.status == SpecialStatus::Rejected);
        CHECK(freeRes.label != Classification::Great);
    }
}
