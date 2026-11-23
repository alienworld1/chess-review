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
Configuration wdlConfig() {
    Configuration c;
    c.conversion.preferWdl = true;
    c.classification.excellentLoss = 0.01;
    return c;
}
MoveEvidence pair(const chess::Position& root, bool verified, int gap = 10,
                  std::uint32_t expectedModel = 1) {
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
                        proof(after, 450, white ? "g8f6" : "b1c3", verified ? "va" : "da", nodes),
                        {300, true}, expectedModel);
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
            const auto result = compareAlternatives(root, b, &a, wdlConfig());
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
    const auto cfg = wdlConfig();
    CHECK(compareAlternatives(root, b, nullptr, cfg).reason == ComparisonReason::Pending);
    CHECK(compareAlternatives(root, a, &a, cfg).needsEvidence());
    auto bad = b;
    bad.before.engineIdentity = "other";
    CHECK(compareAlternatives(root, bad, &a, cfg).needsEvidence());
    bad = b;
    bad.before.position.halfmoveClock++;
    CHECK(compareAlternatives(root, bad, &a, cfg).moves.empty());
    bad = b;
    bad.before.candidates.back().evaluation.score->bound = Bound::Lower;
    CHECK(compareAlternatives(root, bad, &a, cfg).moves.empty());
    bad = b;
    bad.before.candidates.back().continuation = {move(root, "b1c3")};
    CHECK(compareAlternatives(root, bad, &a, cfg).reason == ComparisonReason::MissingCandidate);
    bad = b;
    bad.before.candidates.back().evaluation.wdl = Wdl{589, 0, 411};
    CHECK(compareAlternatives(root, bad, &a, cfg).reason == ComparisonReason::Disagreement);
    auto config = cfg;
    config.refinement.maximumDrift = std::numeric_limits<double>::quiet_NaN();
    CHECK(compareAlternatives(root, b, &a, config).needsEvidence());
    bad = b;
    bad.before.evaluation.score = RawScore{ScoreKind::Mate, 4};
    bad.before.candidates.front().evaluation = bad.before.evaluation;
    CHECK(compareAlternatives(root, bad, &a, cfg, ComparisonModel::Version2).moves.empty());
    const auto mateOk = compareAlternatives(root, bad, &a, {}, ComparisonModel::Version3);
    CHECK(mateOk.reason == ComparisonReason::Disagreement); // 4 wins on Mate (1.0 vs 0.5) drifts
}
TEST_CASE("Comparison permits equivalent rank swaps and bounds score drift exactly",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    const auto cfg = wdlConfig();
    auto a = pair(root, false, 0), b = pair(root, true, 0);
    std::swap(b.before.candidates[0].continuation, b.before.candidates[1].continuation);
    CHECK(compareAlternatives(root, b, &a, cfg).reason == ComparisonReason::Confirmed);
    b = pair(root, true, 0);
    for (auto& c : b.before.candidates) {
        c.evaluation.wdl = Wdl{620, 0, 380};
    }
    b.before.evaluation = b.before.candidates.front().evaluation;
    CHECK(compareAlternatives(root, b, &a, cfg).reason == ComparisonReason::Confirmed);
    for (auto& c : b.before.candidates) {
        c.evaluation.wdl = Wdl{621, 0, 379};
    }
    b.before.evaluation = b.before.candidates.front().evaluation;
    CHECK(compareAlternatives(root, b, &a, cfg).reason == ComparisonReason::ScoreDrift);
}
TEST_CASE("Width one remains explicitly unassessed without comparison searches",
          "[unit][review][comparison]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    a.before.candidates.resize(1);
    b.before.candidates.resize(1);
    const auto result = compareAlternatives(root, b, &a, wdlConfig());
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
    auto a = pairEvidence(root, played, before, child, {300, true}, 1);
    before.attemptIdentity = "second";
    child.attemptIdentity = "child2";
    before.strength.nodes = child.strength.nodes = 5000000;
    std::swap(before.candidates[0].continuation, before.candidates[1].continuation);
    auto b = pairEvidence(root, played, before, child, {300, true}, 1);
    auto result = compareAlternatives(root, b, &a, wdlConfig());
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
        const auto a = pairEvidence(root, played, before, terminalEvidence(after), {300, true}, 1);
        before.attemptIdentity = "second";
        before.strength.nodes = 5000000;
        const auto b = pairEvidence(root, played, before, terminalEvidence(after), {300, true}, 1);
        const auto result = compareAlternatives(root, b, &a, wdlConfig());
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
            pairEvidence(root, played, before, proof(after, 500, next.c_str(), "child", 1000000),
                         {300, true}, 1);
        const auto result = compareAlternatives(root, a, nullptr, wdlConfig());
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
    const auto result = compareAlternatives(root, b, &a, wdlConfig());
    REQUIRE(!result.moves.empty());
    CHECK(result.moves.front().reason == ComparisonReason::Disagreement);
    CHECK(!result.moves.front().gap);
    CHECK(result.needsEvidence());
}
TEST_CASE("Played comparisons use exact native and terminal disagreement boundaries",
          "[unit][review][comparison][audit]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"));
    const auto played = move(root, "f7e6");
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
    const auto cfg = wdlConfig();
    for (const int wins : {519, 520, 521}) {
        auto before = proof(root, 750, "f7g7", "first", 1000000);
        auto candidate = before.candidates.front();
        candidate.rank = 2;
        candidate.continuation = {played};
        candidate.evaluation.wdl = Wdl{wins, 0, 1000 - wins};
        before.candidates.push_back(candidate);
        const auto a = pairEvidence(root, played, before, terminalEvidence(after), {300, true}, 1);
        before.attemptIdentity = "second";
        before.strength.nodes = 5000000;
        const auto b = pairEvidence(root, played, before, terminalEvidence(after), {300, true}, 1);
        const auto result = compareAlternatives(root, b, &a, cfg);
        REQUIRE(!result.moves.empty());
        CHECK(result.moves.front().reason ==
              (wins <= 520 ? ComparisonReason::Confirmed : ComparisonReason::Disagreement));
        CHECK(result.moves.front().gap.has_value() == (wins <= 520));
        const auto legacy = compareAlternatives(root, b, &a, cfg, ComparisonModel::Version1);
        CHECK(legacy.moves.front().reason ==
              (wins < 520 ? ComparisonReason::Confirmed : ComparisonReason::Disagreement));
    }
}
TEST_CASE("Root and child equivalence disagreement cannot confirm played quality",
          "[unit][review][comparison][audit]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    for (auto* evidence : {&a, &b}) {
        evidence->before.candidates.back().continuation = {evidence->playedMove};
        evidence->after.evaluation.wdl = Wdl{420, 0, 580};
        evidence->after.candidates.front().evaluation = evidence->after.evaluation;
    }
    const auto cfg = wdlConfig();
    const auto result = compareAlternatives(root, b, &a, cfg);
    REQUIRE(!result.moves.empty());
    CHECK(result.moves.front().reason == ComparisonReason::Disagreement);
    CHECK(!result.moves.front().gap);
    CHECK(result.needsEvidence());
    CHECK(compareAlternatives(root, b, &a, cfg, ComparisonModel::Version1).reason ==
          ComparisonReason::Confirmed);
}
TEST_CASE("Comparison rejects reused attempts zero strength and invalid configuration",
          "[unit][review][comparison][audit]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    SECTION("cross position reuse") { b.after.attemptIdentity = a.before.attemptIdentity; }
    SECTION("within pair reuse") { a.after.attemptIdentity = a.before.attemptIdentity; }
    SECTION("zero first strength") { a.before.strength.nodes = 0; }
    SECTION("invalid drift with width one") {
        a.before.candidates.resize(1);
        b.before.candidates.resize(1);
        for (const double drift : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
            auto config = Configuration{};
            config.refinement.maximumDrift = drift;
            const auto result = compareAlternatives(root, b, &a, config);
            CHECK(result.reason == ComparisonReason::Insufficient);
            CHECK(result.moves.empty());
        }
        return;
    }
    const auto result = compareAlternatives(root, b, &a, {});
    CHECK(result.reason == ComparisonReason::Insufficient);
    CHECK(result.needsEvidence());
}

TEST_CASE("Comparison ignores supplied derived facts and rejects malformed raw witnesses",
          "[unit][review][comparison][audit]") {
    const auto root = chess::Position::initial();
    auto a = pair(root, false), b = pair(root, true);
    SECTION("derived facts") {
        for (auto* evidence : {&a, &b}) {
            evidence->mover = chess::Color::Black;
            evidence->loss = 1;
            evidence->playedMoveRank = 1;
            evidence->beforeEvaluation = {};
            evidence->afterEvaluation = {};
        }
        CHECK(compareAlternatives(root, b, &a, wdlConfig()).reason == ComparisonReason::Confirmed);
        return;
    }
    SECTION("duplicate root") {
        b.before.candidates.back().continuation = b.before.candidates.front().continuation;
    }
    SECTION("oversized PV") { b.before.candidates.front().continuation.resize(1025); }
    SECTION("oversized width") { b.before.candidates.resize(257); }
    SECTION("invalid WDL") {
        b.before.candidates.back().evaluation.wdl->wins = std::numeric_limits<int>::max();
    }
    SECTION("missing score") { b.before.candidates.back().evaluation.score.reset(); }
    SECTION("bad rank") { b.before.candidates.back().rank = std::numeric_limits<int>::min(); }
    SECTION("wrong child context") { b.after.position.halfmoveClock++; }
    CHECK(compareAlternatives(root, b, &a, {}).moves.empty());
}

TEST_CASE("Comparison model version three allows equal-node exact-mate confirmation",
          "[unit][review][comparison]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("6k1/5ppp/8/8/8/8/8/3R2K1 w - - 0 1"));
    const auto mateMove = move(root, "d1d8");
    const auto altMove = move(root, "d1a1");
    auto matePos = root;
    REQUIRE(std::holds_alternative<std::monostate>(matePos.play(mateMove)));

    auto makeMateBefore = [&](const char* attempt) {
        auto e = proof(root, 1000, "d1d8", attempt, 491);
        e.strength.depth = 245;
        e.evaluation = {chess::Color::White, RawScore{ScoreKind::Mate, 1, Bound::Exact}, {}};
        e.candidates.front().evaluation = e.evaluation;
        e.candidates.front().strength = e.strength;
        RawEvaluation altEval{
            chess::Color::White, RawScore{ScoreKind::Centipawns, 0, Bound::Exact}, {}};
        e.candidates.push_back({2, altEval, {altMove}, e.strength});
        return e;
    };
    const auto a = pairEvidence(root, mateMove, makeMateBefore("attempt-a"),
                                terminalEvidence(matePos), ConversionConfig{}, 2);
    const auto b = pairEvidence(root, mateMove, makeMateBefore("attempt-b"),
                                terminalEvidence(matePos), ConversionConfig{}, 2);
    CHECK(compareAlternatives(root, b, &a, {}, ComparisonModel::Version2).reason ==
          ComparisonReason::Insufficient);
    CHECK(compareAlternatives(root, b, &a, {}, ComparisonModel::Version3).reason ==
          ComparisonReason::Confirmed);

    // Non-terminal child equal-node exact mate (e.g., 16. Qb8+ -> Mate 2 / Mate -1 at 491 nodes).
    const auto mate2Root =
        std::get<chess::Position>(chess::Position::fromFen("4k3/3n4/8/8/8/8/1Q6/3R2K1 w - - 0 1"));
    const auto queenMove = move(mate2Root, "b2b8");
    auto mate2AfterPos = mate2Root;
    REQUIRE(std::holds_alternative<std::monostate>(mate2AfterPos.play(queenMove)));
    auto makeMate2Pair = [&](const std::string& suffix, std::uint64_t beforeNodes) {
        const auto beforeAttempt = "b-" + suffix;
        auto before = proof(mate2Root, 1000, "b2b8", beforeAttempt.c_str(), beforeNodes);
        before.strength.depth = 14;
        before.evaluation = {chess::Color::White, RawScore{ScoreKind::Mate, 2, Bound::Exact}, {}};
        before.candidates.front().evaluation = before.evaluation;
        before.candidates.front().strength = before.strength;
        RawEvaluation altEval{
            chess::Color::White, RawScore{ScoreKind::Centipawns, 400, Bound::Exact}, {}};
        before.candidates.push_back({2, altEval, {move(mate2Root, "b2b3")}, before.strength});

        const auto afterAttempt = "a-" + suffix;
        auto after = proof(mate2AfterPos, 0, "d7b8", afterAttempt.c_str(), 491);
        after.strength.depth = 245;
        after.evaluation = {chess::Color::Black, RawScore{ScoreKind::Mate, -1, Bound::Exact}, {}};
        after.candidates.front().evaluation = after.evaluation;
        after.candidates.front().strength = after.strength;
        return pairEvidence(mate2Root, queenMove, before, after, ConversionConfig{}, 2);
    };
    const auto m2a = makeMate2Pair("1", 200000U);
    const auto m2b = makeMate2Pair("2", 400000U);
    CHECK(compareAlternatives(mate2Root, m2b, &m2a, {}, ComparisonModel::Version2).reason ==
          ComparisonReason::Insufficient);
    CHECK(compareAlternatives(mate2Root, m2b, &m2a, {}, ComparisonModel::Version3).reason ==
          ComparisonReason::Confirmed);
}
