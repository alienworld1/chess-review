#include "review/Summary.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
using namespace chess_review::review;
TEST_CASE("Accuracy is independent, bounded and excludes unavailable evidence",
          "[unit][review][summary]") {
    CHECK(moveAccuracy(0) == 100);
    CHECK(moveAccuracy(1) == 0);
    CHECK(!moveAccuracy(std::nullopt));
    CHECK(!moveAccuracy(-0.001));
    CHECK(!moveAccuracy(std::numeric_limits<double>::quiet_NaN()));
    const std::array<double, 2> scores{100, 50};
    CHECK(gameAccuracy(scores) == Catch::Approx((75 + 200.0 / 3) / 2));
    const std::array<double, 2> zero{100, 0};
    CHECK(gameAccuracy(zero) == 25);
    CHECK(!gameAccuracy({}));
    CHECK(*moveAccuracy(.10) < *moveAccuracy(.05));
}
TEST_CASE("Refinement triggers are inclusive and preserve mate uncertainty",
          "[unit][review][summary]") {
    ClassificationResult result;
    MoveEvidence pair;
    result.loss = .04;
    CHECK(needsRefinement(result, pair, {}));
    result.loss = .039;
    CHECK(!needsRefinement(result, pair, {}));
    result.loss = .09;
    CHECK(needsRefinement(result, pair, {}));
    result.thresholds.mistakeLoss = std::nextafter(.10, 1.0);
    CHECK(!needsRefinement(result, pair, {}));
    result.thresholds.mistakeLoss = .10;
    result.loss = .2;
    CHECK(needsRefinement(result, pair, {}));
    result.loss.reset();
    CHECK(!needsRefinement(result, pair, {}));
    pair.mate.before = RawScore{ScoreKind::Mate, 4};
    CHECK(needsRefinement(result, pair, {}));
}
TEST_CASE("Stability requires independent stronger evidence and agreement",
          "[unit][review][summary]") {
    ClassificationResult deep;
    deep.label = Classification::Mistake;
    deep.bestMove = chess_review::chess::Move{};
    deep.before.expected = ExpectedScore{
        .5, chess_review::chess::Color::White, ConversionSource::ApproximateCentipawns, 1, {}};
    deep.after.expected = ExpectedScore{
        .4, chess_review::chess::Color::White, ConversionSource::ApproximateCentipawns, 1, {}};
    auto verified = deep;
    MoveEvidence a;
    a.before.complete = a.after.complete = true;
    a.before.strength = a.after.strength = {12, 1000000, {}};
    a.before.attemptIdentity = "deep-before";
    a.after.attemptIdentity = "deep-after";
    auto b = a;
    b.before.strength.nodes = b.after.strength.nodes = 5000000;
    b.before.attemptIdentity = "verify-before";
    b.after.attemptIdentity = "verify-after";
    CHECK(assessStability(deep, verified, a, b, {}).stable);
    verified.label = Classification::Blunder;
    CHECK(assessStability(deep, verified, a, b, {}).reason == StabilityReason::Disagreement);
    verified = deep;
    verified.before.expected->value = .53;
    CHECK(assessStability(deep, verified, a, b, {}).reason == StabilityReason::ScoreDrift);
    CHECK(!assessStability(deep, deep, a, a, {}).stable);
    a.mate.before = RawScore{ScoreKind::Mate, 3};
    b.mate.before = RawScore{ScoreKind::Mate, 5};
    deep.label.reset();
    verified.label.reset();
    CHECK(assessStability(deep, verified, a, b, {}).persistentMate);
    CHECK(!assessStability(deep, verified, a, b, {}).stable);
}
TEST_CASE("Accuracy uses exact WDL deltas independently of label arithmetic",
          "[unit][review][summary]") {
    MoveEvidence pair;
    pair.mover = chess_review::chess::Color::White;
    pair.loss = .050000000000000044;
    pair.beforeEvaluation.expected =
        ExpectedScore{.55, pair.mover, ConversionSource::NativeWdl, 1, {}};
    pair.afterEvaluation.expected =
        ExpectedScore{.5, pair.mover, ConversionSource::NativeWdl, 1, {}};
    pair.before.evaluation.perspective = pair.mover;
    pair.after.evaluation.perspective = pair.mover;
    pair.before.evaluation.wdl = Wdl{550, 0, 450};
    pair.after.evaluation.wdl = Wdl{500, 0, 500};
    CHECK(accuracyLoss(pair) == .05);
    const auto accuracy = moveAccuracy(accuracyLoss(pair));
    CHECK(moveAccuracy(accuracyLoss(pair)) == accuracy);
    pair.issue = EvidenceIssue::Bounded;
    CHECK(!accuracyLoss(pair));
}
TEST_CASE("Native WDL stability drift includes the exact two-point boundary",
          "[unit][review][summary]") {
    ClassificationResult deep;
    deep.label = Classification::Mistake;
    deep.bestMove = chess_review::chess::Move{};
    deep.before.expected =
        ExpectedScore{.5, chess_review::chess::Color::White, ConversionSource::NativeWdl, 1, {}};
    deep.after.expected =
        ExpectedScore{.4, chess_review::chess::Color::White, ConversionSource::NativeWdl, 1, {}};
    auto verified = deep;
    verified.before.expected->value = .52;
    verified.after.expected->value = .42;
    MoveEvidence first;
    first.before.complete = first.after.complete = true;
    first.before.strength = first.after.strength = {12, 1000000, {}};
    first.before.attemptIdentity = "deep-before";
    first.after.attemptIdentity = "deep-after";
    first.before.evaluation.wdl = Wdl{500, 0, 500};
    first.after.evaluation.wdl = Wdl{400, 0, 600};
    auto second = first;
    second.before.attemptIdentity = "verify-before";
    second.after.attemptIdentity = "verify-after";
    second.before.strength.nodes = second.after.strength.nodes = 5000000;
    second.before.evaluation.wdl = Wdl{520, 0, 480};
    second.after.evaluation.wdl = Wdl{420, 0, 580};
    CHECK(assessStability(deep, verified, first, second, {}).stable);
    second.before.evaluation.wdl = Wdl{521, 0, 479};
    verified.before.expected->value = .521;
    CHECK(assessStability(deep, verified, first, second, {}).reason == StabilityReason::ScoreDrift);
}
TEST_CASE("Summary facts select chronological errors and actual mate transitions",
          "[unit][review][summary]") {
    using chess_review::chess::Color;
    SummaryBuilder builder(Color::Black);
    SummaryMoveFacts opponent;
    opponent.positionIndex = 1;
    opponent.mover = Color::White;
    opponent.processed = true;
    opponent.label = Classification::Blunder;
    opponent.loss = .25;
    CHECK(!builder.add(opponent).selected);
    auto player = opponent;
    player.mover = Color::Black;
    player.positionIndex = 2;
    const auto selected = builder.add(player);
    CHECK(selected.selected);
    CHECK(selected.opponentContext == 1);
    const auto facts = builder.finish();
    CHECK(facts.keyMoments == 1);
    CHECK(facts.sides[0].evaluated == 1);
    CHECK(facts.sides[1].counts[static_cast<std::size_t>(Classification::Blunder)] == 1);
    CHECK(facts.largestLossPosition == 2);
    CHECK(facts.sides[1].accuracy == moveAccuracy(.25));
    SummaryBuilder mates(Color::White);
    SummaryMoveFacts move;
    move.mover = Color::White;
    move.processed = true;
    move.positionIndex = 1;
    move.before = {Color::White, RawScore{ScoreKind::Mate, 4}, {}};
    move.after = {Color::Black, RawScore{ScoreKind::Mate, -3}, {}};
    CHECK(!mates.add(move).selected);
    move.positionIndex = 3;
    move.before.score.reset();
    CHECK(mates.add(move).selected);
    move.positionIndex = 5;
    move.before = move.after;
    move.after.score.reset();
    CHECK(mates.add(move).selected);
    CHECK(mates.finish().keyMoments == 2);
}
