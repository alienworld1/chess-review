#include "review/Classification.h"
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
PositionEvidence evidence(const chess::Position& position, int expectedWins, const char* best) {
    PositionEvidence out;
    out.position = position.context();
    out.engineIdentity = "engine";
    out.complete = true;
    out.strength = {12, 100000, {}};
    out.evaluation = {position.sideToMove(), RawScore{ScoreKind::Centipawns, 0, Bound::Exact},
                      Wdl{expectedWins, 0, 1000 - expectedWins}};
    out.candidates = {{1, out.evaluation, {move(position, best)}, out.strength}};
    return out;
}
MoveEvidence pair(const chess::Position& position, int lossPermille, bool best = false) {
    const auto played =
        move(position, position.sideToMove() == chess::Color::White ? "e2e4" : "e7e5");
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    return pairEvidence(
        position, played,
        evidence(position, 500,
                 best ? (position.sideToMove() == chess::Color::White ? "e2e4" : "e7e5")
                      : (position.sideToMove() == chess::Color::White ? "g1f3" : "g8f6")),
        evidence(next, 500 + lossPermille,
                 next.sideToMove() == chess::Color::White ? "b1c3" : "g8f6"));
}
} // namespace
TEST_CASE("Normal labels require verified evidence and ordered rules",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    for (const auto [loss, label] : {std::pair{0, Classification::Excellent},
                                     {5, Classification::Excellent},
                                     {30, Classification::Good},
                                     {75, Classification::Inaccuracy},
                                     {150, Classification::Mistake},
                                     {250, Classification::Blunder}}) {
        const auto result = classify(position, pair(position, loss), {});
        REQUIRE(result.label);
        CHECK(*result.label == label);
        CHECK(!result.stable);
        CHECK(!result.playedMoveRank);
        CHECK(result.bestMove == move(position, "g1f3"));
        CHECK(result.alternatives.empty());
    }
    CHECK(classify(position, pair(position, 30, true), {}).label == Classification::Best);
    const auto conflict = classify(position, pair(position, 250, true), {});
    CHECK(!conflict.label);
    CHECK(conflict.issue == ClassificationIssue::BestMoveConflict);
    CHECK(conflict.playedMoveRank == 1);
    CHECK(conflict.loss == Catch::Approx(.25));
}
TEST_CASE("Loss boundaries use exact inclusive comparisons", "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    for (const auto [which, severity] : {std::pair{0, Classification::Inaccuracy},
                                         {1, Classification::Mistake},
                                         {2, Classification::Blunder}}) {
        auto configuration = ClassificationConfig{};
        const auto paired = pair(position, which == 0 ? 50 : which == 1 ? 100 : 200);
        REQUIRE(paired.loss);
        double& threshold = which == 0   ? configuration.inaccuracyLoss
                            : which == 1 ? configuration.mistakeLoss
                                         : configuration.blunderLoss;
        const double loss = *classify(position, paired, {}).loss;
        threshold = loss;
        CHECK(classify(position, paired, configuration).label == severity);
        threshold = std::nextafter(threshold, 1.0);
        CHECK(classify(position, paired, configuration).label != severity);
        threshold = std::nextafter(loss, 0.0);
        CHECK(classify(position, paired, configuration).label == severity);
    }
    const auto paired = pair(position, 10);
    auto configuration = ClassificationConfig{};
    const double excellentLoss = *classify(position, paired, {}).loss;
    configuration.excellentLoss = excellentLoss;
    CHECK(classify(position, paired, configuration).label == Classification::Excellent);
    configuration.excellentLoss = std::nextafter(excellentLoss, 0.0);
    CHECK(classify(position, paired, configuration).label == Classification::Good);
    configuration.inaccuracyLoss = .005;
    CHECK(classify(position, paired, configuration).label == Classification::Inaccuracy);
}
TEST_CASE("Classification revalidates evidence rather than trusting derived claims",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    auto paired = pair(position, 0);
    SECTION("forged rank") {
        paired.playedMoveRank = 1;
        CHECK(classify(position, paired, {}).label == Classification::Excellent);
    }
    SECTION("forged loss") {
        paired.loss = .9;
        CHECK(classify(position, paired, {}).label == Classification::Excellent);
    }
    SECTION("bounded") {
        paired.before.evaluation.score->bound = Bound::Lower;
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        CHECK(!classify(position, paired, {}).label);
    }
    SECTION("mate") {
        paired.before.evaluation.score->kind = ScoreKind::Mate;
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        CHECK(classify(position, paired, {}).evidenceIssue == EvidenceIssue::MateClaim);
    }
    SECTION("weak") {
        paired.before.complete = false;
        CHECK(classify(position, paired, {}).evidenceIssue == EvidenceIssue::Incomplete);
    }
    SECTION("restricted") {
        paired.before.restricted = true;
        CHECK(classify(position, paired, {}).evidenceIssue == EvidenceIssue::RestrictedSearch);
    }
    SECTION("context") {
        paired.after.position.moves.clear();
        CHECK(classify(position, paired, {}).evidenceIssue == EvidenceIssue::ContextMismatch);
    }
    SECTION("mixed") {
        paired.after.evaluation.wdl.reset();
        paired.after.candidates.front().evaluation = paired.after.evaluation;
        CHECK(classify(position, paired, {}).evidenceIssue == EvidenceIssue::MixedConversion);
    }
    SECTION("negative") {
        CHECK(classify(position, pair(position, -1), {}).evidenceIssue ==
              EvidenceIssue::NegativeLoss);
    }
    SECTION("invalid configuration") {
        auto c = ClassificationConfig{};
        c.excellentLoss = std::numeric_limits<double>::quiet_NaN();
        CHECK(classify(position, paired, c).issue == ClassificationIssue::InvalidConfiguration);
    }
}
TEST_CASE("Equivalent alternatives require coherent measured candidate evidence",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    auto paired = pair(position, 5);
    auto candidate = paired.before.candidates.front();
    candidate.rank = 2;
    candidate.continuation = {move(position, "e2e4")};
    candidate.evaluation.wdl = Wdl{495, 0, 505};
    paired.before.candidates.push_back(candidate);
    auto result = classify(position, paired, {});
    REQUIRE(result.alternatives.size() == 1);
    CHECK(result.playedMoveRank == 2);
    CHECK(result.alternatives.front().nearlyEquivalent);
    CHECK(result.alternatives.front().gap == Catch::Approx(.005));
    paired.before.candidates.back().evaluation.wdl = Wdl{600, 0, 400};
    result = classify(position, paired, {});
    CHECK(!result.label);
    CHECK(result.evidenceIssue == EvidenceIssue::ContradictoryCandidates);
    CHECK(result.alternatives.empty());
}
TEST_CASE("Mirrored mover perspectives classify consistently", "[unit][review][classification]") {
    auto black = chess::Position::initial();
    REQUIRE(std::holds_alternative<std::monostate>(black.play(move(black, "g1f3"))));
    const auto white = chess::Position::initial();
    const auto a = classify(white, pair(white, 75), {});
    const auto b = classify(black, pair(black, 75), {});
    CHECK(a.label == b.label);
    CHECK(a.loss == b.loss);
    CHECK(b.before.expected->perspective == chess::Color::Black);
}
TEST_CASE("Local terminal outcomes remain subject to conservative pairing",
          "[unit][review][classification]") {
    auto position =
        std::get<chess::Position>(chess::Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"));
    const auto played = move(position, "f7e6");
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    auto before = evidence(position, 750, "f7g7");
    const auto paired = pairEvidence(position, played, before, terminalEvidence(next));
    const auto result = classify(position, paired, {});
    CHECK(result.label == Classification::Blunder);
    CHECK(result.after.expected->source == ConversionSource::Terminal);
}

TEST_CASE("Native WDL respects the configured decimal boundaries",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    CHECK(classify(position, pair(position, 10), {}).label == Classification::Excellent);
    CHECK(classify(position, pair(position, 50), {}).label == Classification::Inaccuracy);
    CHECK(classify(position, pair(position, 100), {}).label == Classification::Mistake);
    CHECK(!classify(position, pair(position, 50, true), {}).label);
    auto paired = pair(position, 0);
    auto alternative = paired.before.candidates.front();
    alternative.rank = 2;
    alternative.continuation = {move(position, "e2e4")};
    alternative.evaluation.wdl = Wdl{490, 0, 510};
    paired.before.candidates.push_back(alternative);
    const auto result = classify(position, paired, {});
    REQUIRE(result.alternatives.size() == 1);
    CHECK(result.alternatives.front().nearlyEquivalent);
}

TEST_CASE("Released classification model remains reproducible", "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    CHECK(classify(position, pair(position, 50), {}, {}, 1).label == Classification::Good);
    CHECK(classify(position, pair(position, 10), {}, {}, 1).label == Classification::Good);
    CHECK(classify(position, pair(position, 50, true), {}, {}, 1).label == Classification::Best);
    CHECK(classify(position, pair(position, 0), {}, {}, 999).issue ==
          ClassificationIssue::Evidence);
}

TEST_CASE("Exact WDL loss is independent of baseline and includes draws",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    for (int wins = 100; wins <= 900; wins += 100) {
        auto paired = pair(position, 50);
        paired.before.evaluation.wdl = Wdl{wins, 0, 1000 - wins};
        paired.after.evaluation.wdl = Wdl{1050 - wins, 0, wins - 50};
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        paired.after.candidates.front().evaluation = paired.after.evaluation;
        const auto result = classify(position, paired, {});
        CHECK(result.label == Classification::Inaccuracy);
        CHECK(result.loss == 0.05);
    }
    auto paired = pair(position, 50);
    paired.before.evaluation.wdl = Wdl{480, 40, 480};
    paired.after.evaluation.wdl = Wdl{530, 40, 430};
    paired.before.candidates.front().evaluation = paired.before.evaluation;
    paired.after.candidates.front().evaluation = paired.after.evaluation;
    CHECK(classify(position, paired, {}).loss == 0.05);
    CHECK(classify(position, paired, {}, ConversionConfig{300, false}).loss == 0.0);
}
