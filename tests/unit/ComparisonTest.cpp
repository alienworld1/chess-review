#include "review/Comparison.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
#include <variant>
using namespace chess_review;
using namespace chess_review::review;
namespace {
chess::Move move(const chess::Position& root, const char* uci) {
    return std::get<chess::Move>(root.parseUci(uci));
}
PositionEvidence proof(const chess::Position& root, int wins, const char* uci, const char* attempt,
                       std::uint64_t nodes) {
    PositionEvidence e;
    e.position = root.context();
    e.complete = true;
    e.engineIdentity = "engine";
    e.attemptIdentity = attempt;
    e.strength = {12, nodes, {}};
    e.evaluation = {root.sideToMove(), RawScore{ScoreKind::Centipawns, 0},
                    Wdl{wins, 0, 1000 - wins}};
    e.candidates = {{1, e.evaluation, {move(root, uci)}, e.strength}};
    return e;
}
MoveEvidence pair(const chess::Position& root, bool verified, int gap = 10) {
    const bool white = root.sideToMove() == chess::Color::White;
    const auto played = move(root, white ? "e2e4" : "e7e5");
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
    const auto nodes = verified ? 5000000U : 1000000U;
    auto before = proof(root, 600, white ? "g1f3" : "g8f6", verified ? "vb" : "db", nodes);
    auto alternative = before.evaluation;
    alternative.wdl = Wdl{600 - gap, 0, 400 + gap};
    before.candidates.push_back(
        {2, alternative, {move(root, white ? "d2d4" : "d7d5")}, before.strength});
    return pairEvidence(root, played, before,
                        proof(after, 450, white ? "g8f6" : "b1c3", verified ? "va" : "da", nodes));
}
} // namespace
TEST_CASE("Comparison confirms identities and exact equivalence for both sides",
          "[unit][review][comparison]") {
    for (bool black : {false, true}) {
        auto root = chess::Position::initial();
        if (black) {
            REQUIRE(std::holds_alternative<std::monostate>(root.play(move(root, "e2e4"))));
        }
        for (const int gap : {0, 10, 11}) {
            const auto a = pair(root, false, gap), b = pair(root, true, gap);
            const auto result = compareAlternatives(root, b, &a, {});
            REQUIRE(result.reason == ComparisonReason::Confirmed);
            REQUIRE(result.moves.size() == 3);
            CHECK(result.moves.front().played);
            CHECK(result.moves.front().source == ComparisonSource::PlayedChild);
            CHECK(result.moves.front().gap == Catch::Approx(.05));
            CHECK(result.moves.back().gap == static_cast<double>(gap) / 1000);
            CHECK(result.moves.back().equivalent == (gap <= 10));
        }
    }
}
TEST_CASE("Comparison needs independent compatible stronger evidence",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    const auto a = pair(root, false), b = pair(root, true);
    CHECK(compareAlternatives(root, b, nullptr, {}).reason == ComparisonReason::Pending);
    CHECK(compareAlternatives(root, a, &a, {}).needsEvidence());
    auto bad = b;
    bad.before.engineIdentity = "other";
    CHECK(compareAlternatives(root, bad, &a, {}).needsEvidence());
    bad = b;
    bad.before.position.halfmoveClock++;
    CHECK(compareAlternatives(root, bad, &a, {}).moves.empty());
    bad = b;
    bad.before.candidates.back().evaluation.score->bound = Bound::Lower;
    CHECK(compareAlternatives(root, bad, &a, {}).moves.empty());
    bad = b;
    bad.before.candidates.back().continuation = {move(root, "b1c3")};
    CHECK(compareAlternatives(root, bad, &a, {}).reason == ComparisonReason::MissingCandidate);
    bad = b;
    bad.before.candidates.back().evaluation.wdl = Wdl{589, 0, 411};
    CHECK(compareAlternatives(root, bad, &a, {}).reason == ComparisonReason::Disagreement);
    auto config = Configuration{};
    config.refinement.maximumDrift = std::numeric_limits<double>::quiet_NaN();
    CHECK(compareAlternatives(root, b, &a, config).needsEvidence());
    bad = b;
    bad.before.evaluation.score = RawScore{ScoreKind::Mate, 4};
    CHECK(compareAlternatives(root, bad, &a, {}).moves.empty());
}
TEST_CASE("Comparison permits equivalent rank swaps and bounds score drift exactly",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false, 0), b = pair(root, true, 0);
    std::swap(b.before.candidates[0].continuation, b.before.candidates[1].continuation);
    CHECK(compareAlternatives(root, b, &a, {}).reason == ComparisonReason::Confirmed);
    b = pair(root, true, 0);
    for (auto& c : b.before.candidates) {
        c.evaluation.wdl = Wdl{620, 0, 380};
    }
    b.before.evaluation = b.before.candidates.front().evaluation;
    CHECK(compareAlternatives(root, b, &a, {}).reason == ComparisonReason::Confirmed);
    for (auto& c : b.before.candidates) {
        c.evaluation.wdl = Wdl{621, 0, 379};
    }
    b.before.evaluation = b.before.candidates.front().evaluation;
    CHECK(compareAlternatives(root, b, &a, {}).reason == ComparisonReason::ScoreDrift);
}
TEST_CASE("Width one remains explicitly unassessed without comparison searches",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    a.before.candidates.resize(1);
    b.before.candidates.resize(1);
    const auto result = compareAlternatives(root, b, &a, {});
    CHECK(result.reason == ComparisonReason::Coverage);
    CHECK(!result.needsEvidence());
    REQUIRE(result.moves.size() == 2);
    CHECK(result.moves.front().source == ComparisonSource::PlayedChild);
}
TEST_CASE("Comparison retains promotion identity and approximate provenance",
          "[unit][review][comparison]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("7k/P7/8/8/8/8/8/7K w - - 0 1"));
    const auto played = move(root, "h1g1");
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
    auto before = proof(root, 600, "a7a8q", "first", 1000000);
    auto candidate = before.candidates.front();
    candidate.rank = 2;
    candidate.continuation = {move(root, "a7a8r")};
    before.candidates.push_back(candidate);
    auto child = proof(after, 450, "h8g7", "child", 1000000);
    auto a = pairEvidence(root, played, before, child);
    before.attemptIdentity = "second";
    child.attemptIdentity = "child2";
    before.strength.nodes = child.strength.nodes = 5000000;
    std::swap(before.candidates[0].continuation, before.candidates[1].continuation);
    auto b = pairEvidence(root, played, before, child);
    auto result = compareAlternatives(root, b, &a, {});
    REQUIRE(result.reason == ComparisonReason::Confirmed);
    REQUIRE(result.moves.size() == 3);
    CHECK(result.moves[1].move != result.moves[2].move);
    auto config = Configuration{};
    config.conversion.preferWdl = false;
    result = compareAlternatives(root, b, &a, config);
    REQUIRE(!result.moves.empty());
    REQUIRE(result.moves.back().expected);
    CHECK(result.moves.back().expected->source == ConversionSource::ApproximateCentipawns);
}
TEST_CASE("Comparison supports proven terminal children and a sole legal move",
          "[unit][review][comparison]") {
    SECTION("terminal child") {
        const auto root =
            std::get<chess::Position>(chess::Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"));
        const auto played = move(root, "f7e6");
        auto after = root;
        REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
        auto before = proof(root, 750, "f7g7", "first", 1000000);
        auto alternative = before.candidates.front();
        alternative.rank = 2;
        alternative.continuation = {move(root, "f7h7")};
        before.candidates.push_back(alternative);
        const auto a = pairEvidence(root, played, before, terminalEvidence(after));
        before.attemptIdentity = "second";
        before.strength.nodes = 5000000;
        const auto b = pairEvidence(root, played, before, terminalEvidence(after));
        const auto result = compareAlternatives(root, b, &a, {});
        REQUIRE(result.reason == ComparisonReason::Confirmed);
        REQUIRE(result.moves.front().expected);
        CHECK(result.moves.front().expected->source == ConversionSource::Terminal);
        CHECK(result.moves.front().gap == .25);
    }
    SECTION("sole legal move") {
        const auto root =
            std::get<chess::Position>(chess::Position::fromFen("7k/8/5K2/8/8/8/8/7R b - - 0 1"));
        REQUIRE(root.legalMoves().size() == 1);
        const auto played = root.legalMoves().front();
        const auto uci = std::get<std::string>(root.uci(played));
        auto before = proof(root, 500, uci.c_str(), "first", 1000000);
        auto after = root;
        REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
        const auto next = std::get<std::string>(after.uci(after.legalMoves().front()));
        const auto a =
            pairEvidence(root, played, before, proof(after, 500, next.c_str(), "child", 1000000));
        const auto result = compareAlternatives(root, a, nullptr, {});
        CHECK(result.reason == ComparisonReason::NoAlternative);
        CHECK(!result.needsEvidence());
    }
}
TEST_CASE("Contradictory played-root and child values withhold numerical comparison",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    for (auto* evidence : {&a, &b}) {
        evidence->before.candidates.back().continuation = {evidence->playedMove};
    }
    const auto result = compareAlternatives(root, b, &a, {});
    REQUIRE(!result.moves.empty());
    CHECK(result.moves.front().reason == ComparisonReason::Disagreement);
    CHECK(!result.moves.front().gap);
    CHECK(result.needsEvidence());
}
