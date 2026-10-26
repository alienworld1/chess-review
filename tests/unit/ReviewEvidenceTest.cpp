#include "review/Evidence.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
using namespace chess_review;
using namespace chess_review::review;
namespace {
chess::Move move(const chess::Position& position, const char* uci) {
    return std::get<chess::Move>(position.parseUci(uci));
}
PositionEvidence evidence(const chess::Position& position, std::int64_t cp = 0) {
    auto result = terminalEvidence(position);
    if (result.terminal) {
        return result;
    }
    result.complete = true;
    result.engineIdentity = "engine-a";
    result.evaluation = {
        position.sideToMove(), RawScore{ScoreKind::Centipawns, cp, Bound::Exact}, {}};
    result.candidates.push_back(
        {1, result.evaluation, {position.legalMoves().front()}, {12, 100000, {}}});
    return result;
}
MoveEvidence pair(std::int64_t beforeCp, std::int64_t afterCp) {
    const auto position = chess::Position::initial();
    const auto played = move(position, "e2e4");
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    return pairEvidence(position, played, evidence(position, beforeCp), evidence(next, afterCp));
}
} // namespace
TEST_CASE("Expected scores carry perspective and a versioned conversion source", "[unit][review]") {
    RawEvaluation raw{chess::Color::White, RawScore{ScoreKind::Centipawns, 300, Bound::Exact}, {}};
    auto converted = convertEvaluation(raw, chess::Color::White);
    REQUIRE(converted.expected);
    CHECK(converted.expected->value == Catch::Approx(0.75));
    CHECK(converted.expected->source == ConversionSource::ApproximateCentipawns);
    CHECK(converted.expected->modelVersion == expectedScoreModelVersion);
    CHECK(convertEvaluation(raw, chess::Color::Black).expected->value == Catch::Approx(0.25));
    raw.wdl = Wdl{200, 600, 200};
    CHECK(convertEvaluation(raw, chess::Color::White).expected->value == Catch::Approx(0.5));
    CHECK(convertEvaluation(raw, chess::Color::White).expected->source ==
          ConversionSource::NativeWdl);
    CHECK(convertEvaluation(raw, chess::Color::White, {300, false}).expected->value ==
          Catch::Approx(0.75));
    raw.wdl = Wdl{1000, 1000, 1000};
    CHECK(convertEvaluation(raw, chess::Color::White).issue == EvidenceIssue::Invalid);
    raw.wdl.reset();
    for (auto cp :
         {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        raw.score->value = cp;
        auto value = convertEvaluation(raw, chess::Color::Black).expected->value;
        CHECK(std::isfinite(value));
        CHECK(value >= 0);
        CHECK(value <= 1);
    }
    raw.score->bound = Bound::Lower;
    CHECK(convertEvaluation(raw, chess::Color::White).issue == EvidenceIssue::Bounded);
    raw.score = RawScore{ScoreKind::Mate, 3, Bound::Exact};
    CHECK(convertEvaluation(raw, chess::Color::White).issue == EvidenceIssue::MateClaim);
    CHECK(convertEvaluation(raw, chess::Color::White, {0, true}).issue == EvidenceIssue::Invalid);
    CHECK(convertEvaluation({}, chess::Color::White).issue == EvidenceIssue::MissingEvaluation);
}
TEST_CASE("Pairing preserves mover perspective and withholds apparent gains", "[unit][review]") {
    const auto result = pair(300, 0);
    REQUIRE(result.loss);
    CHECK(*result.loss == Catch::Approx(0.25));
    CHECK(result.afterEvaluation.expected->perspective == chess::Color::White);
    auto gain = pair(0, -300);
    CHECK(gain.issue == EvidenceIssue::NegativeLoss);
    CHECK(!gain.loss);
    REQUIRE(gain.signedDelta);
    CHECK(*gain.signedDelta < 0);
    const auto root = std::get<chess::Position>(
        chess::Position::fromFen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"));
    auto next = root;
    const auto played = move(root, "e7e5");
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    auto black = pairEvidence(root, played, evidence(root, 300), evidence(next));
    CHECK(black.mover == chess::Color::Black);
    REQUIRE(black.loss);
    CHECK(*black.loss == Catch::Approx(0.25));
}
TEST_CASE("Evidence pairing rejects incompatible and insufficient evidence", "[unit][review]") {
    const auto position = chess::Position::initial();
    const auto played = move(position, "e2e4");
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    auto before = evidence(position, 300);
    auto after = evidence(next, 0);
    SECTION("mixed methods") {
        after.evaluation.wdl = Wdl{500, 0, 500};
        after.candidates[0].evaluation = after.evaluation;
        CHECK(pairEvidence(position, played, before, after).issue ==
              EvidenceIssue::MixedConversion);
    }
    SECTION("incomplete") {
        after.complete = false;
        CHECK(pairEvidence(position, played, before, after).issue == EvidenceIssue::Incomplete);
    }
    SECTION("engine") {
        after.engineIdentity = "other";
        CHECK(pairEvidence(position, played, before, after).issue ==
              EvidenceIssue::IncompatibleEngine);
    }
    SECTION("model") {
        after.modelVersion = 2;
        CHECK(pairEvidence(position, played, before, after).issue ==
              EvidenceIssue::IncompatibleModel);
    }
    SECTION("bound") {
        after.evaluation.score->bound = Bound::Upper;
        after.candidates[0].evaluation = after.evaluation;
        CHECK(pairEvidence(position, played, before, after).issue == EvidenceIssue::Bounded);
    }
    SECTION("mate") {
        after.evaluation.score = RawScore{ScoreKind::Mate, -3, Bound::Lower};
        after.candidates[0].evaluation = after.evaluation;
        const auto result = pairEvidence(position, played, before, after);
        CHECK(!result.loss);
        REQUIRE(result.mate.after);
        CHECK(result.mate.after->value == 3);
        CHECK(result.mate.after->bound == Bound::Upper);
    }
    SECTION("history") {
        const auto sameBoard = std::get<chess::Position>(chess::Position::fromFen(next.fen()));
        after.position = sameBoard.context();
        CHECK(pairEvidence(position, played, before, after).issue ==
              EvidenceIssue::ContextMismatch);
    }
    SECTION("illegal") {
        CHECK(pairEvidence(position, move(next, "e7e5"), before, after).issue ==
              EvidenceIssue::ContextMismatch);
    }
    SECTION("restricted") {
        before.restricted = true;
        CHECK(pairEvidence(position, played, before, after).issue ==
              EvidenceIssue::RestrictedSearch);
    }
    SECTION("rank") {
        before.candidates = {{1, before.evaluation, {played}, {}}};
        CHECK(pairEvidence(position, played, before, after).playedMoveRank == 1);
        before.candidates.push_back(before.candidates.front());
        const auto result = pairEvidence(position, played, before, after);
        CHECK(!result.playedMoveRank);
        CHECK(!result.loss);
        CHECK(result.issue == EvidenceIssue::Invalid);
    }
    SECTION("empty candidates") {
        before.candidates.clear();
        CHECK(!pairEvidence(position, played, before, after).loss);
    }
    SECTION("forged terminal") {
        after.terminal = TerminalOutcome::Checkmate;
        CHECK(pairEvidence(position, played, before, after).issue == EvidenceIssue::Invalid);
    }
}
TEST_CASE("Board-proven terminal evidence needs no engine claim", "[unit][review]") {
    const auto position =
        std::get<chess::Position>(chess::Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"));
    for (const auto* uci : {"f7g7", "f7e6"}) {
        auto next = position;
        const auto played = move(position, uci);
        REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
        const auto after = terminalEvidence(next);
        REQUIRE(after.terminal);
        const auto result = pairEvidence(position, played, evidence(position, 300), after);
        REQUIRE(result.afterEvaluation.expected);
        CHECK(result.afterEvaluation.expected->source == ConversionSource::Terminal);
        CHECK(result.afterEvaluation.expected->value == (next.inCheck() ? 1 : 0.5));
        auto missingIdentity = evidence(position, 300);
        missingIdentity.engineIdentity.clear();
        CHECK(pairEvidence(position, played, missingIdentity, after).issue ==
              EvidenceIssue::IncompatibleEngine);
    }
}
