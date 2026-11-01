#include "review/Summary.h"
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
    for (const auto& [loss, label] : {std::pair{0, Classification::Excellent},
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
    for (const auto& [which, severity] : {std::pair{0, Classification::Inaccuracy},
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

TEST_CASE("Quick scan separates pending, usable and retained evidence", "[unit][review][scan]") {
    CHECK(ScanAssessment{}.state == ScanState::AwaitingEvidence);
    auto position = chess::Position::initial();
    SECTION("black mover") {
        REQUIRE(std::holds_alternative<std::monostate>(position.play(move(position, "e2e4"))));
    }
    const auto paired = pair(position, 30);
    const auto result = classify(position, paired, {});
    const auto scan = assessQuickScan(result, paired, {});
    CHECK(scan.state == ScanState::Usable);
    CHECK(scan.loss == Catch::Approx(.03));
    CHECK(scan.signedDelta == paired.signedDelta);
    CHECK(!scan.issue);
    CHECK(scan.reasons.empty());
}
TEST_CASE("Quick scan repairs recoverable gaps only under policy two", "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    auto raw = pair(position, 30);
    auto issue = EvidenceIssue::Incomplete;
    auto reason = RefinementReason::InsufficientStrength;
    SECTION("insufficient strength") { raw.before.complete = false; }
    SECTION("bound") {
        raw.before.evaluation.score->bound = Bound::Lower;
        issue = EvidenceIssue::Bounded;
        reason = RefinementReason::BoundedEvaluation;
    }
    SECTION("missing score and WDL") {
        raw.before.complete = false;
        raw.before.evaluation.score.reset();
        raw.before.evaluation.wdl.reset();
        issue = EvidenceIssue::Incomplete;
        reason = RefinementReason::MissingEvaluation;
    }
    SECTION("WDL without score is still missing candidate evidence") {
        raw.before.complete = false;
        raw.before.evaluation.score.reset();
        issue = EvidenceIssue::Incomplete;
        reason = RefinementReason::MissingEvaluation;
    }
    SECTION("mixed conversions") {
        raw.after.evaluation.wdl.reset();
        issue = EvidenceIssue::MixedConversion;
        reason = RefinementReason::MixedConversion;
    }
    raw.before.candidates.front().evaluation = raw.before.evaluation;
    raw.after.candidates.front().evaluation = raw.after.evaluation;
    const auto paired = pairEvidence(position, raw.playedMove, raw.before, raw.after);
    const auto result = classify(position, paired, {});
    const auto scan = assessQuickScan(result, paired, {});
    CHECK(scan.state == ScanState::Unusable);
    CHECK(scan.issue == issue);
    if (reason == RefinementReason::MissingEvaluation) {
        CHECK(scan.reasons == std::vector{RefinementReason::InsufficientStrength, reason});
    } else {
        CHECK(scan.reasons == std::vector{reason});
    }
    CHECK(!scan.loss);
    CHECK(!scan.signedDelta);
    const auto legacy = assessQuickScan(result, paired, {}, 1);
    CHECK(legacy.needsRefinement() == needsRefinement(result, paired, {}));
    CHECK(!legacy.needsRefinement());
}
TEST_CASE("Quick scan retains combined triggers and exact legacy decisions",
          "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    for (int loss = -1; loss <= 300; ++loss) {
        const auto paired = pair(position, loss, true);
        const auto result = classify(position, paired, {});
        const auto legacy = assessQuickScan(result, paired, {}, 1);
        CHECK(legacy.needsRefinement() == needsRefinement(result, paired, {}));
        // Golden policy-1 decisions for rank-1 native-WDL evidence: low-loss
        // threshold proximity, the Inaccuracy margin, then search conflict.
        CHECK(legacy.needsRefinement() == (loss <= 20 || loss >= 40));
        const auto scan = assessQuickScan(result, paired, {}, 2);
        CHECK(scan.needsRefinement() == legacy.needsRefinement());
        if (loss == 200) {
            CHECK(scan.reasons == std::vector{RefinementReason::SearchConflict,
                                              RefinementReason::MajorLoss,
                                              RefinementReason::ThresholdProximity});
        }
    }
}
TEST_CASE("Quick scan preserves mate signals without inventing numeric loss",
          "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    auto raw = pair(position, 30);
    raw.before.evaluation.score = RawScore{ScoreKind::Mate, 3};
    raw.before.candidates.front().evaluation = raw.before.evaluation;
    raw.after.complete = false;
    const auto paired = pairEvidence(position, raw.playedMove, raw.before, raw.after);
    const auto scan = assessQuickScan(classify(position, paired, {}), paired, {});
    CHECK(scan.state == ScanState::Unusable);
    CHECK(scan.reasons ==
          std::vector{RefinementReason::MateSignal, RefinementReason::InsufficientStrength});
    CHECK(!scan.loss);
    CHECK(!scan.signedDelta);
}
TEST_CASE("Quick scan rejects invalid inputs instead of scheduling repair",
          "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    auto raw = pair(position, 30);
    auto policy = std::uint32_t{2};
    auto configuration = ClassificationConfig{};
    auto refinement = RefinementConfig{};
    SECTION("context mismatch") { raw.after.position.moves.clear(); }
    SECTION("engine mismatch") { raw.after.engineIdentity = "another engine"; }
    SECTION("unsupported evidence") { raw.before.modelVersion = 99; }
    SECTION("restricted search") { raw.before.restricted = true; }
    SECTION("invalid candidate") { raw.before.candidates.front().rank = 2; }
    SECTION("invalid WDL with missing score") {
        raw.before.evaluation.score.reset();
        raw.before.evaluation.wdl->wins = -1;
        raw.before.candidates.front().evaluation = raw.before.evaluation;
    }
    SECTION("invalid other side with missing score") {
        raw.before.evaluation.score.reset();
        raw.before.candidates.front().evaluation = raw.before.evaluation;
        raw.after.candidates.front().rank = 2;
    }
    SECTION("invalid classification config") { configuration.inaccuracyLoss = -1; }
    SECTION("invalid refinement config") {
        refinement.boundaryMargin = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("unsupported policy") { policy = 99; }
    const auto paired = pairEvidence(position, raw.playedMove, raw.before, raw.after);
    const auto scan =
        assessQuickScan(classify(position, paired, configuration), paired, refinement, policy);
    CHECK(scan.state == ScanState::Invalid);
    CHECK(!scan.needsRefinement());
}

TEST_CASE("Quick scan rejects unsupported and malformed derived values", "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    auto paired = pair(position, 30);
    auto result = classify(position, paired, {});
    SECTION("unsupported classification model") { result.modelVersion = 99; }
    SECTION("wrong mover") { result.mover = chess::Color::Black; }
    SECTION("unsupported conversion model") { result.before.expected->modelVersion = 99; }
    SECTION("wrong expected-score perspective") {
        result.before.expected->perspective = chess::Color::Black;
    }
    SECTION("nonfinite expected score") {
        result.before.expected->value = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("out-of-range expected score") { result.after.expected->value = 2; }
    SECTION("missing numeric evidence") { result.before.expected.reset(); }
    SECTION("nonfinite selection threshold") {
        result.thresholds.inaccuracyLoss = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("unordered selection thresholds") { result.thresholds.mistakeLoss = .01; }
    SECTION("inconsistent loss") {
        result.loss = .4;
        result.signedDelta = .4;
    }
    SECTION("inconsistent pair delta") { paired.signedDelta = .4; }
    SECTION("negative loss") { result.loss = -.01; }
    SECTION("nonfinite loss") { result.loss = std::numeric_limits<double>::quiet_NaN(); }
    SECTION("out-of-range loss") { result.loss = 2; }
    SECTION("unknown issue") {
        const auto value = GENERATE(99);
        paired.issue = static_cast<EvidenceIssue>(value);
    }
    SECTION("unknown classification issue") {
        const auto value = GENERATE(99);
        result.issue = static_cast<ClassificationIssue>(value);
    }
    const auto scan = assessQuickScan(result, paired, {});
    CHECK(scan.state == ScanState::Invalid);
    CHECK(!scan.needsRefinement());
}

TEST_CASE("Policy three selects errors without selecting routine excellent moves",
          "[unit][review][adaptive]") {
    auto position = chess::Position::initial();
    const auto root = GENERATE(0, 1, 2);
    if (root == 1) {
        REQUIRE(std::holds_alternative<std::monostate>(position.play(move(position, "e2e4"))));
    }
    if (root == 2) {
        position = std::get<chess::Position>(
            chess::Position::fromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 40"));
    }
    for (const int loss : {0, 10, 20, 39, 40, 49, 50, 51, 100, 200}) {
        const auto paired = pair(position, loss);
        const auto classified = classify(position, paired, {});
        const auto scan = assessQuickScan(classified, paired, {}, 3);
        REQUIRE(scan.state == ScanState::Usable);
        CHECK(scan.qualifyingLoss.has_value() == (loss >= 50));
        CHECK(scan.needsRefinement() == (loss >= 40));
        const auto legacy = assessQuickScan(classified, paired, {}, 2);
        if (loss == 0) {
            CHECK(legacy.needsRefinement());
        }
    }
    const auto paired = pair(position, 50);
    auto configuration = ClassificationConfig{};
    configuration.inaccuracyLoss = std::nextafter(.05, 1.0);
    CHECK(
        !assessQuickScan(classify(position, paired, configuration), paired, {}, 3).qualifyingLoss);
    configuration.inaccuracyLoss = .05;
    CHECK(
        assessQuickScan(classify(position, paired, configuration), paired, {}, 3).qualifyingLoss ==
        .05);
    // Recorded model 1's floating subtraction cannot change the new selection boundary.
    CHECK(assessQuickScan(classify(position, paired, configuration, {}, 1), paired, {}, 3)
              .qualifyingLoss == .05);
}
TEST_CASE("Opportunity context uses only the preceding usable quick loss",
          "[unit][review][adaptive]") {
    const auto position = chess::Position::initial();
    const auto error = pair(position, 60);
    const auto previous = assessQuickScan(classify(position, error, {}), error, {}, 3);
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(error.playedMove)));
    const auto routine = pair(next, 0);
    const auto reply = assessQuickScan(classify(next, routine, {}), routine, {}, 3);
    REQUIRE(!reply.needsRefinement());
    auto selected = reply;
    addOpportunityContext(selected, previous, 50);
    CHECK(selected.opportunityPosition == 50);
    CHECK(selected.opportunityLoss == .06);
    CHECK(selected.reasons == std::vector{RefinementReason::OpportunityReply});
    for (const auto state :
         {ScanState::AwaitingEvidence, ScanState::Unusable, ScanState::Invalid}) {
        auto unavailable = previous;
        unavailable.state = state;
        selected = reply;
        addOpportunityContext(selected, unavailable, 50);
        CHECK(!selected.needsRefinement());
    }
    selected = reply;
    addOpportunityContext(selected, previous, 0);
    CHECK(!selected.needsRefinement());
    selected = reply;
    addOpportunityContext(selected, selected, 1);
    CHECK(!selected.needsRefinement());
    // A reply that also errs retains its own loss and can create another opportunity.
    selected = previous;
    addOpportunityContext(selected, previous, 1);
    CHECK(selected.qualifyingLoss == .06);
    CHECK(selected.opportunityLoss == .06);
}
TEST_CASE("Adaptive queue orders chess impact then expands unique adjacent positions",
          "[unit][review][adaptive]") {
    const auto candidate = [](int index, RefinementReason reason,
                              std::optional<double> loss = std::nullopt) {
        ScanAssessment scan;
        scan.pipelineVersion = 3;
        scan.state = ScanState::Usable;
        scan.reasons = {reason};
        if (reason == RefinementReason::ExpectedScoreLoss) {
            scan.qualifyingLoss = loss;
        } else if (reason == RefinementReason::OpportunityReply) {
            scan.opportunityLoss = loss;
            scan.opportunityPosition = index - 1;
        }
        return RefinementCandidate{index, scan};
    };
    std::vector<RefinementCandidate> candidates{
        candidate(1, RefinementReason::ThresholdProximity),
        candidate(3, RefinementReason::MissingEvaluation),
        candidate(5, RefinementReason::OpportunityReply, .1),
        candidate(6, RefinementReason::OpportunityReply, .2),
        candidate(8, RefinementReason::ExpectedScoreLoss, .05),
        candidate(10, RefinementReason::ExpectedScoreLoss, .2),
        candidate(12, RefinementReason::SearchConflict),
        candidate(14, RefinementReason::MateSignal),
        candidate(16, RefinementReason::ExpectedScoreLoss, .2)};
    const auto expected =
        std::vector<int>{13, 14, 11, 12, 9, 10, 15, 16, 7, 8, 5, 6, 4, 2, 3, 0, 1};
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 3)) == expected);
    std::reverse(candidates.begin(), candidates.end());
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 3)) == expected);
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 2)) ==
          std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    CHECK(std::get<std::vector<int>>(refinementQueue({}, 3)).empty());
}

TEST_CASE("Adaptive selection uses the captured fallback conversion and error cutoff",
          "[unit][review][adaptive]") {
    const auto position = chess::Position::initial();
    auto original = pair(position, 0);
    original.before.evaluation.wdl.reset();
    original.after.evaluation.wdl.reset();
    original.before.evaluation.score->value = 40;
    original.after.evaluation.score->value = 0;
    original.before.candidates.front().evaluation = original.before.evaluation;
    original.after.candidates.front().evaluation = original.after.evaluation;
    const auto paired =
        pairEvidence(position, original.playedMove, original.before, original.after);
    auto classified = classify(position, paired, {});
    REQUIRE(classified.loss);
    REQUIRE(classified.before.expected->source == ConversionSource::ApproximateCentipawns);
    classified.thresholds.inaccuracyLoss = *classified.loss;
    REQUIRE(classified.thresholds.inaccuracyLoss < classified.thresholds.mistakeLoss);
    CHECK(assessQuickScan(classified, paired, {}, 3).qualifyingLoss == classified.loss);
    classified.thresholds.inaccuracyLoss = std::nextafter(*classified.loss, 1.0);
    CHECK(!assessQuickScan(classified, paired, {}, 3).qualifyingLoss);
}

TEST_CASE("Adaptive cutoff does not merge adjacent floating values", "[unit][review][audit]") {
    const auto position = chess::Position::initial();
    auto original = pair(position, 0);
    original.before.evaluation.wdl.reset();
    original.after.evaluation.wdl.reset();
    original.before.evaluation.score->value = 9;
    original.after.evaluation.score->value = 0;
    original.before.candidates.front().evaluation = original.before.evaluation;
    original.after.candidates.front().evaluation = original.after.evaluation;
    auto paired = pairEvidence(position, original.playedMove, original.before, original.after);
    if (GENERATE(false, true)) {
        paired = pair(position, 43);
    }
    const auto initial = classify(position, paired, {});
    REQUIRE(initial.loss);
    auto thresholds = ClassificationConfig{};
    thresholds.inaccuracyLoss = std::nextafter(*initial.loss, 1.0);
    const auto classified = classify(position, paired, thresholds);
    REQUIRE(classified.loss < thresholds.inaccuracyLoss);
    const auto released = assessQuickScan(classified, paired, {}, 3);
    CHECK(released.qualifyingLoss.has_value());
    const auto scan = assessQuickScan(classified, paired, {}, 4);
    CHECK(!scan.qualifyingLoss);
    auto reply = ScanAssessment{};
    reply.pipelineVersion = 4;
    reply.state = ScanState::Usable;
    addOpportunityContext(reply, scan, 1);
    CHECK(!reply.opportunityLoss);
}
TEST_CASE("Opportunity context cannot accumulate or propagate nonfinite losses",
          "[unit][review][audit]") {
    const auto position = chess::Position::initial();
    const auto error = pair(position, 60);
    auto previous = assessQuickScan(classify(position, error, {}), error, {}, 4);
    auto current = previous;
    addOpportunityContext(current, previous, 1);
    addOpportunityContext(current, previous, 1);
    CHECK(std::count(current.reasons.begin(), current.reasons.end(),
                     RefinementReason::OpportunityReply) == 1);
    previous.qualifyingLoss = std::numeric_limits<double>::quiet_NaN();
    current = {};
    current.pipelineVersion = 4;
    current.state = ScanState::Usable;
    addOpportunityContext(current, previous, 1);
    CHECK(!current.opportunityLoss);
    CHECK(!current.needsRefinement());
}

TEST_CASE("Adaptive queue rejects unsupported and malformed plans before sorting",
          "[unit][review][audit]") {
    ScanAssessment scan;
    scan.pipelineVersion = 4;
    scan.state = ScanState::Usable;
    scan.loss = .1;
    scan.qualifyingLoss = .1;
    scan.reasons = {RefinementReason::ExpectedScoreLoss};
    RefinementCandidate candidate{2, scan};
    const auto build = [&](std::uint32_t policy = 4) {
        return refinementQueue(std::span{&candidate, 1}, policy);
    };
    SECTION("unknown policy") {
        CHECK(std::get<RefinementQueueIssue>(build(99)) == RefinementQueueIssue::UnsupportedPolicy);
        return;
    }
    SECTION("invalid index") { candidate.positionIndex = 0; }
    SECTION("nonfinite sort key") {
        candidate.scan.qualifyingLoss = GENERATE(std::numeric_limits<double>::quiet_NaN(),
                                                 std::numeric_limits<double>::infinity(),
                                                 -std::numeric_limits<double>::infinity());
    }
    SECTION("out of range") { candidate.scan.qualifyingLoss = GENERATE(-.1, 0.0, 1.1); }
    SECTION("missing key") { candidate.scan.qualifyingLoss.reset(); }
    SECTION("missing reason") { candidate.scan.reasons.clear(); }
    SECTION("invalid state") { candidate.scan.state = ScanState::Invalid; }
    SECTION("awaiting selected") { candidate.scan.state = ScanState::AwaitingEvidence; }
    SECTION("missing captured policy") { candidate.scan.pipelineVersion.reset(); }
    SECTION("mixed captured policy") { candidate.scan.pipelineVersion = 3; }
    SECTION("unknown reason") {
        candidate.scan.reasons.push_back(static_cast<RefinementReason>(GENERATE(99)));
    }
    SECTION("opportunity index is not adjacent") {
        candidate.scan.opportunityLoss = .2;
        candidate.scan.opportunityPosition = 2;
        candidate.scan.reasons.push_back(RefinementReason::OpportunityReply);
    }
    SECTION("opportunity lacks index") {
        candidate.scan.opportunityLoss = .2;
        candidate.scan.reasons.push_back(RefinementReason::OpportunityReply);
    }
    SECTION("index lacks opportunity") { candidate.scan.opportunityPosition = 1; }
    SECTION("nonfinite displayed loss") {
        candidate.scan.loss = std::numeric_limits<double>::quiet_NaN();
    }
    CHECK(std::get<RefinementQueueIssue>(build()) == RefinementQueueIssue::InvalidCandidate);
}
TEST_CASE("Opportunity reassessment clears stale and mismatched context", "[unit][review][audit]") {
    const auto position = chess::Position::initial();
    const auto error = pair(position, 60);
    const auto previous = assessQuickScan(classify(position, error, {}), error, {}, 4);
    auto current = previous;
    addOpportunityContext(current, previous, 1);
    REQUIRE(current.opportunityPosition == 1);
    auto other = previous;
    SECTION("different policy") { other.pipelineVersion = 3; }
    SECTION("missing policy") { other.pipelineVersion.reset(); }
    SECTION("no preceding loss") { other.qualifyingLoss.reset(); }
    SECTION("unknown current state") { current.state = static_cast<ScanState>(GENERATE(99)); }
    SECTION("negative preceding loss") { other.qualifyingLoss = -.1; }
    SECTION("infinite preceding loss") {
        other.qualifyingLoss = std::numeric_limits<double>::infinity();
    }
    addOpportunityContext(current, other, 1);
    CHECK(!current.opportunityLoss);
    CHECK(!current.opportunityPosition);
    CHECK(std::count(current.reasons.begin(), current.reasons.end(),
                     RefinementReason::OpportunityReply) == 0);
    CHECK(current.qualifyingLoss == previous.qualifyingLoss);
}
