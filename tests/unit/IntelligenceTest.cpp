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
MoveEvidence pair(const chess::Position& root, const std::vector<chess::Move>& best,
                  const std::vector<chess::Move>& played, const chess::Move& alternative,
                  int beforeWins, int afterWins, int alternativeWins, bool newer) {
    auto before = evidence(root, beforeWins, best, newer, "before");
    auto second = before.evaluation;
    second.wdl = Wdl{alternativeWins, 0, 1000 - alternativeWins};
    before.candidates.push_back({2, second, {alternative}, before.strength});
    auto child = root;
    REQUIRE(std::holds_alternative<std::monostate>(child.play(played.front())));
    std::vector<chess::Move> reply(played.begin() + 1, played.end());
    REQUIRE(!(reply.empty()));
    return pairEvidence(root, played.front(), before,
                        evidence(child, 1000 - afterWins, reply, newer, "after"));
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
    const auto result = verifySpecial(root, b, &a, {});
    REQUIRE(result.status == SpecialStatus::Verified);
    CHECK(result.label == Classification::Great);
    CHECK(result.alternativeGap == Catch::Approx(.2));
    CHECK(classify(root, b, {}).label == Classification::Best);
    CHECK(verifySpecial(root, b, nullptr, {}).status == SpecialStatus::Pending);
    CHECK(verifySpecial(root, a, &a, {}).needsEvidence());
    auto missing = b;
    missing.before.candidates.resize(1);
    const auto noCoverage = verifySpecial(root, missing, &a, {});
    CHECK(!(noCoverage.label));
    CHECK(!(noCoverage.needsEvidence()));
    auto forged = b;
    forged.before.engineIdentity = "another engine";
    CHECK(!(verifySpecial(root, forged, &a, {}).label));
}
TEST_CASE("Special cutoffs require clearance and agreement", "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto alt = line(root, {"d2d4"}).front();
    for (const auto scores : {std::array{510, 510, 350}, std::array{600, 600, 490},
                              std::array{600, 580, 400}, std::array{800, 800, 600}}) {
        const auto a = pair(root, pv, pv, alt, scores[0], scores[1], scores[2], false);
        const auto b = pair(root, pv, pv, alt, scores[0], scores[1], scores[2], true);
        CHECK(!(verifySpecial(root, b, &a, {}).label));
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
    const auto result = verifySpecial(root, b, &a, {});
    REQUIRE(result.status == SpecialStatus::Verified);
    CHECK(result.label == Classification::Miss);
    CHECK(has(result.witnesses, TacticalKind::Material));
    CHECK(classify(root, b, {}).label == Classification::Blunder);
    const auto quietRoot = chess::Position::initial();
    const auto quietBest = line(quietRoot, {"e2e4", "e7e5"});
    const auto quietPlayed = line(quietRoot, {"d2d4", "d7d5"});
    const auto q1 =
        pair(quietRoot, quietBest, quietPlayed, quietPlayed.front(), 800, 550, 550, false);
    const auto q2 =
        pair(quietRoot, quietBest, quietPlayed, quietPlayed.front(), 800, 550, 550, true);
    CHECK(!(verifySpecial(quietRoot, q2, &q1, {}).label));
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
    const auto result = verifySpecial(root, b, &a, {});
    REQUIRE(result.label == Classification::Brilliant);
    const auto won1 = pair(root, pv, pv, alt, 960, 960, 400, false);
    const auto won2 = pair(root, pv, pv, alt, 960, 960, 400, true);
    CHECK(verifySpecial(root, won2, &won1, {}).label != Classification::Brilliant);
    auto shortened = pv;
    shortened.resize(2);
    CHECK(
        !(has(inspectTacticalLine(root, shortened, FactSource::Played), TacticalKind::Sacrifice)));
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
    for (const auto policy : {9U, 10U}) {
        auto scan = assessQuickScan(classify(root, a, {}), a, {}, policy);
        addIntelligenceCandidates(scan, root, a, {});
        const bool tactical =
            std::find(scan.reasons.begin(), scan.reasons.end(),
                      RefinementReason::TacticalOpportunity) != scan.reasons.end();
        CHECK(tactical == (policy == 10));
        const std::array<RefinementCandidate, 1> candidates{{{1, scan}}};
        CHECK(std::holds_alternative<std::vector<int>>(refinementQueue(candidates, policy)));
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
    const auto result = verifySpecial(root, latest, &a, {});
    CHECK(!(result.label));
    CHECK(result.needsEvidence());
    CHECK(result.ruleId == "special.tactical_witness_disagreement.v1");
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
    CHECK(has(tacticalFacts(root, a, {}), TacticalKind::Material));
    auto scan = assessQuickScan(classify(root, a, {}), a, {}, 10);
    REQUIRE(!scan.needsRefinement());
    addIntelligenceCandidates(scan, root, a, {});
    CHECK(!scan.needsRefinement());
}

TEST_CASE("Ordinary moves outside special quality ranges request no extra verification",
          "[unit][review][intelligence]") {
    const auto root = chess::Position::initial();
    const auto pv = line(root, {"e2e4", "e7e5"});
    const auto supplied = pair(root, pv, pv, line(root, {"d2d4"}).front(), 600, 570, 400, true);
    const auto result = verifySpecial(root, supplied, nullptr, {});
    CHECK(result.status == SpecialStatus::Rejected);
    CHECK(!result.needsEvidence());
}
