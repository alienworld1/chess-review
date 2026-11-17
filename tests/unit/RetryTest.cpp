#include "review/Retry.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <limits>
using namespace chess_review;
using namespace chess_review::review;
namespace {
chess::Move parsed(const chess::Position& root, const char* uci) {
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
    e.candidates = {{1, e.evaluation, {parsed(root, uci)}, e.strength}};
    return e;
}
MoveEvidence original(const chess::Position& root, bool verified, int gap) {
    const bool white = root.sideToMove() == chess::Color::White;
    const auto played = parsed(root, white ? "e2e4" : "e7e5");
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
    const auto nodes = verified ? 5000000U : 1000000U;
    auto before = proof(root, 600, white ? "g1f3" : "g8f6", verified ? "vb" : "db", nodes);
    auto alt = before.evaluation;
    alt.wdl = Wdl{600 - gap, 0, 400 + gap};
    before.candidates.push_back({2, alt, {parsed(root, white ? "d2d4" : "d7d5")}, before.strength});
    return pairEvidence(root, played, before,
                        proof(after, 550, white ? "g8f6" : "b1c3", verified ? "va" : "da", nodes));
}
MoveEvidence hintEvidence(const chess::Position& root, std::initializer_list<const char*> line,
                          const char* playedUci, bool verified) {
    const auto nodes = verified ? 5000000U : 1000000U;
    auto before = proof(root, 600, *line.begin(), verified ? "vb" : "db", nodes);
    before.candidates.front().continuation.clear();
    auto replay = root;
    for (const auto uci : line) {
        const auto move = parsed(replay, uci);
        before.candidates.front().continuation.push_back(move);
        REQUIRE(std::holds_alternative<std::monostate>(replay.play(move)));
    }
    const auto played = parsed(root, playedUci);
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(played)));
    const auto reply = std::get<std::string>(after.uci(after.legalMoves().front()));
    return pairEvidence(root, played, before,
                        proof(after, 550, reply.c_str(), verified ? "va" : "da", nodes));
}
} // namespace
TEST_CASE("Retry accepts demonstrated alternatives at exact WDL boundaries",
          "[unit][review][retry]") {
    for (bool black : {false, true}) {
        auto root = chess::Position::initial();
        if (black) {
            REQUIRE(std::holds_alternative<std::monostate>(root.play(parsed(root, "e2e4"))));
        }
        for (auto [gap, grade] : {std::pair{0, RetryGrade::Strong},
                                  {10, RetryGrade::Strong},
                                  {11, RetryGrade::Okay},
                                  {49, RetryGrade::Okay},
                                  {50, RetryGrade::TryAgain}}) {
            auto d = original(root, false, gap), v = original(root, true, gap);
            const auto result = gradeRetry(root, parsed(root, black ? "d7d5" : "d2d4"), d, v, {});
            REQUIRE(result.grade);
            CHECK(*result.grade == grade);
            CHECK(*result.gap == Catch::Approx(static_cast<double>(gap) / 1000));
            CHECK(result.success() == (grade == RetryGrade::Strong));
            CHECK(gradeRetry(root, parsed(root, black ? "g8f6" : "g1f3"), d, v, {}).grade ==
                  RetryGrade::Best);
        }
    }
}
TEST_CASE("Retry withholds unsupported conflicting and incompatible evidence",
          "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    auto d = original(root, false, 10), v = original(root, true, 10);
    const auto move = parsed(root, "d2d4");
    REQUIRE(gradeRetry(root, move, d, v, {}).grade);
    CHECK(!gradeRetry(root, parsed(root, "b1c3"), d, v, {}).grade);
    CHECK(!gradeRetry(root, move, d, d, {}).grade);
    auto config = Configuration{};
    config.refinement.maximumDrift = std::numeric_limits<double>::quiet_NaN();
    CHECK(!gradeRetry(root, move, d, v, config).grade);
    config = {};
    config.classification.excellentLoss = -1;
    CHECK(!gradeRetry(root, move, d, v, config).grade);
    auto bad = v;
    bad.before.candidates[1].evaluation.wdl = Wdl{589, 0, 411};
    CHECK(!gradeRetry(root, move, d, bad, {}).grade);
    bad = v;
    bad.before.evaluation.score->bound = Bound::Lower;
    CHECK(!gradeRetry(root, move, d, bad, {}).grade);
    bad = v;
    bad.before.engineIdentity = "other";
    CHECK(!gradeRetry(root, move, d, bad, {}).grade);
    bad = v;
    bad.before.position.moves.push_back(move);
    CHECK(!gradeRetry(root, move, d, bad, {}).grade);
    bad = v;
    bad.before.evaluation.score = RawScore{ScoreKind::Mate, 4};
    CHECK(!gradeRetry(root, move, d, bad, {}).grade);
}
TEST_CASE("Unknown retries require independent comparable child evidence",
          "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    const auto d = original(root, false, 10), v = original(root, true, 10);
    const auto move = parsed(root, "b1c3");
    auto after = root;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(move)));
    auto dc = proof(after, 410, "g8f6", "dc", 1000000);
    auto vc = proof(after, 410, "g8f6", "vc", 5000000);
    CHECK(gradeRetry(root, move, d, v, {}, &dc, &vc).grade == RetryGrade::Strong);
    CHECK(!gradeRetry(root, move, d, v, {}, &dc, &dc).grade);
    CHECK(!gradeRetry(root, parsed(root, "d2d4"), d, v, {}, &dc, nullptr).grade);
    CHECK(!gradeRetry(root, parsed(root, "d2d4"), d, v, {}, nullptr, &vc).grade);
    vc.evaluation.wdl = Wdl{500, 0, 500};
    vc.candidates[0].evaluation = vc.evaluation;
    CHECK(!gradeRetry(root, move, d, v, {}, &dc, &vc).grade);
    vc = dc;
    vc.attemptIdentity = "vc";
    vc.strength.nodes = 5000000;
    vc.position.moves.clear();
    CHECK(!gradeRetry(root, move, d, v, {}, &dc, &vc).grade);
}

TEST_CASE("Retry rejects mixing engines across otherwise coherent passes",
          "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    const auto deep = original(root, false, 10);
    auto verified = original(root, true, 10);
    verified.before.engineIdentity = verified.after.engineIdentity = "another-engine";
    CHECK(!gradeRetry(root, parsed(root, "d2d4"), deep, verified, {}).grade);
}
TEST_CASE("Retry drift includes the exact native WDL boundary", "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    const auto deep = original(root, false, 13);
    const auto verified = original(root, true, 33);
    CHECK(gradeRetry(root, parsed(root, "d2d4"), deep, verified, {}).grade == RetryGrade::Okay);
}
TEST_CASE("Retry hints fall back honestly and reject unsupported best moves",
          "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    const auto deep = original(root, false, 10);
    auto verified = original(root, true, 10);
    const auto hint = retryHint(root, deep, verified, {});
    REQUIRE(hint);
    CHECK(hint->idea == RetryIdea::General);
    CHECK(hint->bestMove == parsed(root, "g1f3"));
    CHECK(hint->piece == chess::PieceType::Knight);
    CHECK(!retryHint(root, deep, deep, {}));
    verified.before.candidates.front().continuation.clear();
    CHECK(!retryHint(root, deep, verified, {}));
    verified = original(root, true, 10);
    verified.before.engineIdentity = "other";
    CHECK(!retryHint(root, deep, verified, {}));
}
TEST_CASE("Retry idea hints require matching concrete witnesses in both passes",
          "[unit][review][retry]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1"));
    const auto deep = hintEvidence(root, {"d1d5", "h8g8"}, "a1b1", false);
    const auto verified = hintEvidence(root, {"d1d5", "h8g8"}, "a1b1", true);
    const auto hint = retryHint(root, deep, verified, {});
    REQUIRE(hint);
    CHECK(hint->idea == RetryIdea::HangingPiece);
    // Identical best first moves and scores alone do not verify the later idea.
    const auto shortPass = hintEvidence(root, {"d1d2"}, "a1b1", false);
    const auto longPass =
        hintEvidence(root, {"d1d2", "d5e6", "d2e2", "h8g8", "e2e6"}, "a1b1", true);
    const auto fallback = retryHint(root, shortPass, longPass, {});
    REQUIRE(fallback);
    CHECK(fallback->idea == RetryIdea::General);
}
TEST_CASE("Retry hints use corrected tactical rules without rewriting historical evidence",
          "[unit][review][retry]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("7k/8/8/3q4/8/3R4/K7/3r4 w - - 0 1"));
    const auto deep = hintEvidence(root, {"d3d5"}, "a2b2", false);
    const auto verified = hintEvidence(root, {"d3d5"}, "a2b2", true);
    const auto before = verified.before.position;
    const auto hint = retryHint(root, deep, verified, {});
    REQUIRE(hint);
    CHECK(hint->idea == RetryIdea::General);
    CHECK(verified.before.position == before);
}
TEST_CASE("Retry hints prefer mate over intermediate tactical consequences",
          "[unit][review][retry]") {
    const auto root = std::get<chess::Position>(
        chess::Position::fromFen("5rk1/5ppp/8/8/8/3B1N2/5PPP/3Q2K1 w - - 0 1"));
    const auto deep =
        hintEvidence(root, {"d3h7", "g8h7", "f3g5", "h7g8", "d1h5", "g7g6", "h5h7"}, "d1e2", false);
    const auto verified =
        hintEvidence(root, {"d3h7", "g8h7", "f3g5", "h7g8", "d1h5", "g7g6", "h5h7"}, "d1e2", true);
    const auto hint = retryHint(root, deep, verified, {});
    REQUIRE(hint);
    CHECK(hint->idea == RetryIdea::Checkmate);
}
TEST_CASE("Retry hints distinguish a checking fork from a lost promotion",
          "[unit][review][retry]") {
    const auto fork =
        std::get<chess::Position>(chess::Position::fromFen("3q3k/8/8/4N3/8/8/8/K7 w - - 0 1"));
    const auto deep = hintEvidence(fork, {"e5f7"}, "a1b1", false);
    const auto verified = hintEvidence(fork, {"e5f7"}, "a1b1", true);
    const auto idea = retryHint(fork, deep, verified, {});
    REQUIRE(idea);
    CHECK(idea->idea == RetryIdea::Fork);
    const auto promotion =
        std::get<chess::Position>(chess::Position::fromFen("1r5k/P7/8/8/8/8/8/7K w - - 0 1"));
    const auto first = hintEvidence(promotion, {"a7a8q", "b8a8"}, "h1g1", false);
    const auto second = hintEvidence(promotion, {"a7a8q", "b8a8"}, "h1g1", true);
    const auto fallback = retryHint(promotion, first, second, {});
    REQUIRE(fallback);
    CHECK(fallback->idea == RetryIdea::General);
    CHECK(fallback->bestMove.promotion == chess::PieceType::Queen);
}

TEST_CASE("Retry hints reject corrupted raw evidence instead of trusting derived claims",
          "[unit][review][retry]") {
    const auto root = chess::Position::initial();
    const auto deep = original(root, false, 10);
    const auto verified = original(root, true, 10);
    for (int corruption = 0; corruption < 7; ++corruption) {
        auto bad = verified;
        switch (corruption) {
        case 0:
            bad.before.candidates.front().continuation.push_back(parsed(root, "g1f3"));
            break;
        case 1:
            bad.before.evaluation.wdl = Wdl{1001, 0, -1};
            break;
        case 2:
            bad.before.evaluation.score->bound = Bound::Lower;
            break;
        case 3:
            bad.before.strength.nodes = 0;
            break;
        case 4:
            bad.before.position.moves.push_back(parsed(root, "e2e4"));
            break;
        case 5:
            bad.before.candidates.front().rank = 2;
            break;
        case 6:
            bad.before.engineIdentity.clear();
            break;
        }
        INFO(corruption);
        CHECK(!retryHint(root, deep, bad, {}));
    }
}
