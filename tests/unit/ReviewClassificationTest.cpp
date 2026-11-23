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
constexpr ConversionConfig kWdlConversion{300, true};
ClassificationConfig legacyThresholds() {
    ClassificationConfig c;
    c.excellentLoss = 0.01;
    return c;
}
MoveEvidence pair(const chess::Position& position, int lossPermille, bool best = false,
                  std::uint32_t expectedModel = 1) {
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
                 next.sideToMove() == chess::Color::White ? "b1c3" : "g8f6"),
        kWdlConversion, expectedModel);
}
} // namespace
TEST_CASE("Normal labels require verified evidence and ordered rules",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    for (const auto& [loss, label] : {std::pair{0, Classification::Excellent},
                                      {5, Classification::Excellent},
                                      {30, Classification::Good},
                                      {75, Classification::Inaccuracy},
                                      {150, Classification::Mistake},
                                      {250, Classification::Blunder}}) {
        const auto result = classify(position, pair(position, loss), c, kWdlConversion, 2);
        REQUIRE(result.label);
        CHECK(*result.label == label);
        CHECK(!result.stable);
        CHECK(!result.playedMoveRank);
        CHECK(result.bestMove == move(position, "g1f3"));
        CHECK(result.alternatives.empty());
    }
    CHECK(classify(position, pair(position, 30, true), c, kWdlConversion, 2).label ==
          Classification::Best);
    const auto conflict = classify(position, pair(position, 250, true), c, kWdlConversion, 2);
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
        auto configuration = legacyThresholds();
        const auto paired = pair(position, which == 0 ? 50 : which == 1 ? 100 : 200);
        REQUIRE(paired.loss);
        double& threshold = which == 0   ? configuration.inaccuracyLoss
                            : which == 1 ? configuration.mistakeLoss
                                         : configuration.blunderLoss;
        const double loss = *classify(position, paired, configuration, kWdlConversion, 2).loss;
        threshold = loss;
        CHECK(classify(position, paired, configuration, kWdlConversion, 2).label == severity);
        threshold = std::nextafter(threshold, 1.0);
        CHECK(classify(position, paired, configuration, kWdlConversion, 2).label != severity);
        threshold = std::nextafter(loss, 0.0);
        CHECK(classify(position, paired, configuration, kWdlConversion, 2).label == severity);
    }
    const auto paired = pair(position, 10);
    auto configuration = legacyThresholds();
    const double excellentLoss = *classify(position, paired, configuration, kWdlConversion, 2).loss;
    configuration.excellentLoss = excellentLoss;
    CHECK(classify(position, paired, configuration, kWdlConversion, 2).label ==
          Classification::Excellent);
    configuration.excellentLoss = std::nextafter(excellentLoss, 0.0);
    CHECK(classify(position, paired, configuration, kWdlConversion, 2).label ==
          Classification::Good);
    configuration.inaccuracyLoss = .005;
    CHECK(classify(position, paired, configuration, kWdlConversion, 2).label ==
          Classification::Inaccuracy);
}
TEST_CASE("Classification revalidates evidence rather than trusting derived claims",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    auto paired = pair(position, 0);
    SECTION("forged rank") {
        paired.playedMoveRank = 1;
        CHECK(classify(position, paired, c, kWdlConversion, 2).label == Classification::Excellent);
    }
    SECTION("forged loss") {
        paired.loss = .9;
        CHECK(classify(position, paired, c, kWdlConversion, 2).label == Classification::Excellent);
    }
    SECTION("bounded") {
        paired.before.evaluation.score->bound = Bound::Lower;
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        CHECK(!classify(position, paired, c, kWdlConversion, 2).label);
    }
    SECTION("mate") {
        paired.before.evaluation.score->kind = ScoreKind::Mate;
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        CHECK(classify(position, paired, c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::MateClaim);
    }
    SECTION("weak") {
        paired.before.complete = false;
        CHECK(classify(position, paired, c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::Incomplete);
    }
    SECTION("restricted") {
        paired.before.restricted = true;
        CHECK(classify(position, paired, c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::RestrictedSearch);
    }
    SECTION("context") {
        paired.after.position.moves.clear();
        CHECK(classify(position, paired, c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::ContextMismatch);
    }
    SECTION("mixed") {
        paired.after.evaluation.wdl.reset();
        paired.after.candidates.front().evaluation = paired.after.evaluation;
        CHECK(classify(position, paired, c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::MixedConversion);
    }
    SECTION("negative") {
        CHECK(classify(position, pair(position, -1), c, kWdlConversion, 2).evidenceIssue ==
              EvidenceIssue::NegativeLoss);
    }
    SECTION("invalid configuration") {
        auto bad = c;
        bad.excellentLoss = std::numeric_limits<double>::quiet_NaN();
        CHECK(classify(position, paired, bad, kWdlConversion, 2).issue ==
              ClassificationIssue::InvalidConfiguration);
    }
}
TEST_CASE("Equivalent alternatives require coherent measured candidate evidence",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    auto paired = pair(position, 5);
    auto candidate = paired.before.candidates.front();
    candidate.rank = 2;
    candidate.continuation = {move(position, "e2e4")};
    candidate.evaluation.wdl = Wdl{495, 0, 505};
    paired.before.candidates.push_back(candidate);
    auto result = classify(position, paired, c, kWdlConversion, 2);
    REQUIRE(result.alternatives.size() == 1);
    CHECK(result.playedMoveRank == 2);
    CHECK(result.alternatives.front().nearlyEquivalent);
    CHECK(result.alternatives.front().gap == Catch::Approx(.005));
    paired.before.candidates.back().evaluation.wdl = Wdl{600, 0, 400};
    result = classify(position, paired, c, kWdlConversion, 2);
    CHECK(!result.label);
    CHECK(result.evidenceIssue == EvidenceIssue::ContradictoryCandidates);
    CHECK(result.alternatives.empty());
}
TEST_CASE("Mirrored mover perspectives classify consistently", "[unit][review][classification]") {
    auto black = chess::Position::initial();
    REQUIRE(std::holds_alternative<std::monostate>(black.play(move(black, "g1f3"))));
    const auto white = chess::Position::initial();
    const auto a = classify(white, pair(white, 75), {}, kWdlConversion);
    const auto b = classify(black, pair(black, 75), {}, kWdlConversion);
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
    const auto paired =
        pairEvidence(position, played, before, terminalEvidence(next), kWdlConversion, 1);
    const auto result = classify(position, paired, {}, kWdlConversion);
    CHECK(result.label == Classification::Blunder);
    CHECK(result.after.expected->source == ConversionSource::Terminal);
}

TEST_CASE("Native WDL respects the configured decimal boundaries",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    CHECK(classify(position, pair(position, 10), c, kWdlConversion, 2).label ==
          Classification::Excellent);
    CHECK(classify(position, pair(position, 50), c, kWdlConversion, 2).label ==
          Classification::Inaccuracy);
    CHECK(classify(position, pair(position, 100), c, kWdlConversion, 2).label ==
          Classification::Mistake);
    CHECK(!classify(position, pair(position, 50, true), c, kWdlConversion, 2).label);
    auto paired = pair(position, 0);
    auto alternative = paired.before.candidates.front();
    alternative.rank = 2;
    alternative.continuation = {move(position, "e2e4")};
    alternative.evaluation.wdl = Wdl{490, 0, 510};
    paired.before.candidates.push_back(alternative);
    const auto result = classify(position, paired, c, kWdlConversion, 2);
    REQUIRE(result.alternatives.size() == 1);
    CHECK(result.alternatives.front().nearlyEquivalent);
}

TEST_CASE("Released classification model remains reproducible", "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    CHECK(classify(position, pair(position, 50), c, kWdlConversion, 1).label ==
          Classification::Good);
    CHECK(classify(position, pair(position, 10), c, kWdlConversion, 1).label ==
          Classification::Good);
    CHECK(classify(position, pair(position, 50, true), c, kWdlConversion, 1).label ==
          Classification::Best);
    CHECK(classify(position, pair(position, 0), c, kWdlConversion, 999).issue ==
          ClassificationIssue::Evidence);
}

TEST_CASE("Exact WDL loss is independent of baseline and includes draws",
          "[unit][review][classification]") {
    const auto position = chess::Position::initial();
    const auto c = legacyThresholds();
    for (int wins = 100; wins <= 900; wins += 100) {
        auto paired = pair(position, 50);
        paired.before.evaluation.wdl = Wdl{wins, 0, 1000 - wins};
        paired.after.evaluation.wdl = Wdl{1050 - wins, 0, wins - 50};
        paired.before.candidates.front().evaluation = paired.before.evaluation;
        paired.after.candidates.front().evaluation = paired.after.evaluation;
        const auto result = classify(position, paired, c, kWdlConversion, 2);
        CHECK(result.label == Classification::Inaccuracy);
        CHECK(result.loss == 0.05);
    }
    auto paired = pair(position, 50);
    paired.before.evaluation.wdl = Wdl{480, 40, 480};
    paired.after.evaluation.wdl = Wdl{530, 40, 430};
    paired.before.candidates.front().evaluation = paired.before.evaluation;
    paired.after.candidates.front().evaluation = paired.after.evaluation;
    CHECK(classify(position, paired, c, kWdlConversion, 2).loss == 0.05);
    CHECK(classify(position, paired, c, ConversionConfig{300, false}, 2).loss == 0.0);
}

TEST_CASE("Quick scan separates pending, usable and retained evidence", "[unit][review][scan]") {
    CHECK(ScanAssessment{}.state == ScanState::AwaitingEvidence);
    auto position = chess::Position::initial();
    SECTION("black mover") {
        REQUIRE(std::holds_alternative<std::monostate>(position.play(move(position, "e2e4"))));
    }
    const auto paired = pair(position, 35, false, 2);
    const auto result = classify(position, paired, {}, kWdlConversion);
    const auto scan = assessQuickScan(result, paired, {});
    CHECK(scan.state == ScanState::Usable);
    CHECK(scan.loss == Catch::Approx(.035));
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
    const auto paired =
        pairEvidence(position, raw.playedMove, raw.before, raw.after, kWdlConversion, 1);
    const auto result = classify(position, paired, legacyThresholds(), kWdlConversion, 2);
    const auto scan = assessQuickScan(result, paired, {}, 13);
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
    const auto c = legacyThresholds();
    for (int loss = -1; loss <= 300; ++loss) {
        const auto paired = pair(position, loss, true);
        const auto result = classify(position, paired, c, kWdlConversion, 2);
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
    const auto paired =
        pairEvidence(position, raw.playedMove, raw.before, raw.after, kWdlConversion, 1);
    const auto scan = assessQuickScan(
        classify(position, paired, legacyThresholds(), kWdlConversion, 2), paired, {}, 13);
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
    const auto paired =
        pairEvidence(position, raw.playedMove, raw.before, raw.after, kWdlConversion, 1);
    const auto scan = assessQuickScan(classify(position, paired, configuration, kWdlConversion, 2),
                                      paired, refinement, policy);
    CHECK(scan.state == ScanState::Invalid);
    CHECK(!scan.needsRefinement());
}

TEST_CASE("Quick scan rejects unsupported and malformed derived values", "[unit][review][scan]") {
    const auto position = chess::Position::initial();
    auto paired = pair(position, 30);
    auto result = classify(position, paired, legacyThresholds(), kWdlConversion, 2);
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
    const auto scan = assessQuickScan(result, paired, {}, 13);
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
    const auto c = legacyThresholds();
    for (const int loss : {0, 10, 20, 39, 40, 49, 50, 51, 100, 200}) {
        const auto paired = pair(position, loss);
        const auto classified = classify(position, paired, c, kWdlConversion, 2);
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
    auto configuration = legacyThresholds();
    configuration.inaccuracyLoss = std::nextafter(.05, 1.0);
    CHECK(!assessQuickScan(classify(position, paired, configuration, kWdlConversion, 2), paired, {},
                           3)
               .qualifyingLoss);
    configuration.inaccuracyLoss = .05;
    CHECK(
        assessQuickScan(classify(position, paired, configuration, kWdlConversion, 2), paired, {}, 3)
            .qualifyingLoss == .05);
    // Recorded model 1's floating subtraction cannot change the new selection boundary.
    CHECK(
        assessQuickScan(classify(position, paired, configuration, kWdlConversion, 1), paired, {}, 3)
            .qualifyingLoss == .05);
}
TEST_CASE("Opportunity context uses only the preceding usable quick loss",
          "[unit][review][adaptive]") {
    const auto position = chess::Position::initial();
    const auto error = pair(position, 60);
    const auto previous = assessQuickScan(
        classify(position, error, legacyThresholds(), kWdlConversion, 2), error, {}, 3);
    auto next = position;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(error.playedMove)));
    const auto routine = pair(next, 0);
    const auto reply = assessQuickScan(
        classify(next, routine, legacyThresholds(), kWdlConversion, 2), routine, {}, 3);
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
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 3, Quality::Standard)) ==
          expected);
    std::reverse(candidates.begin(), candidates.end());
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 3, Quality::Standard)) ==
          expected);
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 2, Quality::Standard)) ==
          std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    CHECK(std::get<std::vector<int>>(refinementQueue({}, 3, Quality::Standard)).empty());
    for (auto& item : candidates) {
        item.scan.pipelineVersion = 13;
    }
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 13, Quality::Thorough)) ==
          expected);
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 13, Quality::Standard)) ==
          std::vector<int>{13, 14, 9, 10, 15, 16, 7, 8});
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 13, Quality::Fast)) ==
          std::vector<int>{13, 14, 9, 10});
    for (auto& item : candidates) {
        if (item.positionIndex == 14) {
            item.scan.repeatedMateSignal = true;
        }
        if (item.positionIndex == 12) {
            item.scan.issue = EvidenceIssue::NegativeLoss;
        }
    }
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 13, Quality::Thorough)) ==
          expected);
    CHECK(std::get<std::vector<int>>(refinementQueue(candidates, 13, Quality::Standard)) ==
          std::vector<int>{9, 10, 15, 16, 7, 8, 5, 6});
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
    const auto paired = pairEvidence(position, original.playedMove, original.before, original.after,
                                     kWdlConversion, 1);
    auto classified = classify(position, paired, legacyThresholds(), kWdlConversion, 2);
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
    auto paired = pairEvidence(position, original.playedMove, original.before, original.after,
                               kWdlConversion, 1);
    if (GENERATE(false, true)) {
        paired = pair(position, 43);
    }
    const auto initial = classify(position, paired, legacyThresholds(), kWdlConversion, 2);
    REQUIRE(initial.loss);
    auto thresholds = legacyThresholds();
    thresholds.inaccuracyLoss = std::nextafter(*initial.loss, 1.0);
    const auto classified = classify(position, paired, thresholds, kWdlConversion, 2);
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
    auto previous = assessQuickScan(
        classify(position, error, legacyThresholds(), kWdlConversion, 2), error, {}, 4);
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
        return refinementQueue(std::span{&candidate, 1}, policy, Quality::Standard);
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
    const auto previous = assessQuickScan(
        classify(position, error, legacyThresholds(), kWdlConversion, 2), error, {}, 4);
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

namespace {
MoveEvidence stabilityPair(const chess::Position& root, int loss, bool verified, bool best = false,
                           std::uint32_t expectedModel = 1) {
    auto p = pair(root, loss, best, expectedModel);
    p.before.attemptIdentity = verified ? "verified-before" : "deep-before";
    p.after.attemptIdentity = verified ? "verified-after" : "deep-after";
    p.before.strength.nodes = p.after.strength.nodes = verified ? 5000000 : 1000000;
    return pairEvidence(root, p.playedMove, p.before, p.after, kWdlConversion, expectedModel);
}
StabilityAssessment stability(const chess::Position& root, const MoveEvidence& a,
                              const MoveEvidence& b,
                              const ClassificationConfig& c = legacyThresholds(),
                              const RefinementConfig& r = {}, std::uint32_t model = 3) {
    return assessStability(classify(root, a, c, kWdlConversion, 2),
                           classify(root, b, c, kWdlConversion, 2), a, b, r, model);
}
} // namespace
TEST_CASE("Threshold stability protects all applicable bands symmetrically",
          "[unit][review][threshold]") {
    const bool black = GENERATE(false, true);
    auto root = chess::Position::initial();
    if (black) {
        REQUIRE(std::holds_alternative<std::monostate>(root.play(move(root, "e2e4"))));
    }
    const int cutoff = GENERATE(10, 50, 100, 200);
    const int offset = GENERATE(-10, -9, 0, 9, 10);
    const auto a = stabilityPair(root, cutoff + offset, false);
    const auto b = stabilityPair(root, cutoff + offset, true);
    const auto result = stability(root, a, b);
    REQUIRE(result.reason == StabilityReason::ThresholdProximity);
    REQUIRE(!result.stable);
    REQUIRE(result.modelVersion == 3);
    REQUIRE(!result.thresholds.empty());
    CHECK(result.thresholds.front().losses[0] == static_cast<double>(cutoff + offset) / 1000);
    CHECK(result.thresholds.front().margin == .01);
    CHECK(stability(root, a, b, legacyThresholds(), {}, 1).stable);
    if (cutoff != 10 || offset > 0) {
        const int clearLoss = cutoff + (offset < 0 ? -11 : 11);
        CHECK(stability(root, stabilityPair(root, clearLoss, false),
                        stabilityPair(root, clearLoss, true))
                  .stable);
    }
}
TEST_CASE("Either witness can keep an agreeing label provisional", "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    for (const bool olderNear : {false, true}) {
        const auto result = stability(root, stabilityPair(root, olderNear ? 101 : 115, false),
                                      stabilityPair(root, olderNear ? 115 : 101, true));
        CHECK(result.reason == StabilityReason::ThresholdProximity);
        CHECK(needsEscalation(true, true, result));
        CHECK(escalationStop(true, result, 2, 5000000) == EscalationStop::RoundLimit);
    }
    const auto a = stabilityPair(root, 101, false), b = stabilityPair(root, 125, true);
    CHECK(stability(root, a, b).reason == StabilityReason::ScoreDrift);
    CHECK(stability(root, a, stabilityPair(root, 99, true)).reason ==
          StabilityReason::Disagreement);
}
TEST_CASE("Threshold clearance preserves exact and neighboring configuration values",
          "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    const auto a = stabilityPair(root, 110, false), b = stabilityPair(root, 110, true);
    RefinementConfig r;
    CHECK(!stability(root, a, b, legacyThresholds(), r).stable);
    r.boundaryMargin = std::nextafter(.01, 0.0);
    CHECK(stability(root, a, b, legacyThresholds(), r).stable);
    r.boundaryMargin = std::nextafter(.01, 1.0);
    CHECK(!stability(root, a, b, legacyThresholds(), r).stable);
    r.boundaryMargin = 0;
    auto c = legacyThresholds();
    c.mistakeLoss = .11;
    CHECK(!stability(root, a, b, c, r).stable);
    c.mistakeLoss = std::nextafter(.11, 0.0);
    CHECK(stability(root, a, b, c, r).stable);
    c.mistakeLoss = std::nextafter(.11, 1.0);
    CHECK(stability(root, a, b, c, r).stable);
    r.boundaryMargin = .005;
    CHECK(stability(root, a, b, legacyThresholds(), r).stable);
}
TEST_CASE("Threshold protection follows rank and classifier precedence",
          "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    CHECK(stability(root, stabilityPair(root, 10, false, true), stabilityPair(root, 10, true, true))
              .stable);
    CHECK(
        !stability(root, stabilityPair(root, 40, false, true), stabilityPair(root, 40, true, true))
             .stable);
    auto c = legacyThresholds();
    c.excellentLoss = .15;
    CHECK(
        stability(root, stabilityPair(root, 150, false), stabilityPair(root, 150, true), c).stable);
    for (const int loss : {0, 5, 10}) {
        CHECK(!stability(root, stabilityPair(root, loss, false), stabilityPair(root, loss, true))
                   .stable);
    }
    // Touching/overlapping uncertainty bands may legitimately prevent confirmation.
    c = legacyThresholds();
    c.mistakeLoss = .06;
    CHECK(
        !stability(root, stabilityPair(root, 55, false), stabilityPair(root, 55, true), c).stable);
}
TEST_CASE("Threshold stability fails closed on invalid models configuration and loss",
          "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    const auto a = stabilityPair(root, 150, false), b = stabilityPair(root, 150, true);
    CHECK(stability(root, a, b).stable);
    CHECK(!stability(root, a, b, legacyThresholds(), {}, 99).stable);
    auto r = RefinementConfig{};
    for (const double margin : {-1.0, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
        r.boundaryMargin = margin;
        CHECK(!stability(root, a, b, legacyThresholds(), r).stable);
    }
    const auto c = legacyThresholds();
    auto d = classify(root, a, c, kWdlConversion, 2), v = classify(root, b, c, kWdlConversion, 2);
    d.loss.reset();
    CHECK(!assessStability(d, v, a, b, {}, 2).stable);
    d = classify(root, a, c, kWdlConversion, 2);
    d.loss = .3;
    CHECK(!assessStability(d, v, a, b, {}, 2).stable);
    d = classify(root, a, c, kWdlConversion, 2);
    d.thresholds.excellentLoss = v.thresholds.excellentLoss =
        std::numeric_limits<double>::infinity();
    CHECK(!assessStability(d, v, a, b, {}, 2).stable);
}
TEST_CASE("Policy eight selects Excellent ambiguity without changing older quick plans",
          "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    const auto c = legacyThresholds();
    for (const bool best : {false, true}) {
        const auto a = stabilityPair(root, 10, false, best);
        const auto result = classify(root, a, c, kWdlConversion, 2);
        CHECK(!assessQuickScan(result, a, {}, 7).needsRefinement());
        CHECK(assessQuickScan(result, a, {}, 8).needsRefinement() == !best);
    }
    const auto a = stabilityPair(root, 150, false);
    auto result = classify(root, a, c, kWdlConversion, 2);
    result.thresholds.excellentLoss = std::numeric_limits<double>::quiet_NaN();
    CHECK(assessQuickScan(result, a, {}, 8).state == ScanState::Invalid);
}
TEST_CASE("Fallback threshold stability uses canonical unscaled losses",
          "[unit][review][threshold]") {
    const auto root = chess::Position::initial();
    const auto fallback = [&](bool verified) {
        auto p = stabilityPair(root, 150, verified);
        p.before.evaluation.wdl.reset();
        p.after.evaluation.wdl.reset();
        p.before.evaluation.score->value = 100;
        p.after.evaluation.score->value = -50;
        p.before.candidates.front().evaluation = p.before.evaluation;
        p.after.candidates.front().evaluation = p.after.evaluation;
        return pairEvidence(root, p.playedMove, p.before, p.after, kWdlConversion, 1);
    };
    const auto a = fallback(false), b = fallback(true);
    REQUIRE(a.loss);
    auto c = legacyThresholds();
    c.inaccuracyLoss = *a.loss;
    c.mistakeLoss = *a.loss + .1;
    c.blunderLoss = *a.loss + .2;
    RefinementConfig r;
    r.boundaryMargin = 0;
    CHECK(!stability(root, a, b, c, r).stable);
    c.inaccuracyLoss = std::nextafter(*a.loss, 0.0);
    CHECK(stability(root, a, b, c, r).stable);
}
TEST_CASE("Threshold stability accepts local terminal proof and half-permille losses",
          "[unit][review][threshold]") {
    const auto root =
        std::get<chess::Position>(chess::Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"));
    const auto played = move(root, "f7e6");
    auto next = root;
    REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
    const auto make = [&](bool verified) {
        auto before = evidence(root, 750, "f7g7");
        before.attemptIdentity = verified ? "verified" : "deep";
        before.strength.nodes = verified ? 5000000 : 1000000;
        return pairEvidence(root, played, before, terminalEvidence(next), kWdlConversion, 1);
    };
    CHECK(stability(root, make(false), make(true)).stable);
    auto c = legacyThresholds();
    c.blunderLoss = .25;
    CHECK(stability(root, make(false), make(true), c).reason ==
          StabilityReason::ThresholdProximity);

    const auto initial = chess::Position::initial();
    const auto makeHalf = [&](bool verified) {
        auto p = stabilityPair(initial, 100, verified);
        p.after.evaluation.wdl = Wdl{600, 1, 399};
        p.after.candidates.front().evaluation = p.after.evaluation;
        return pairEvidence(initial, p.playedMove, p.before, p.after, kWdlConversion, 1);
    };
    const auto a = makeHalf(false), b = makeHalf(true);
    REQUIRE(classify(initial, a, legacyThresholds(), kWdlConversion, 2).loss == .1005);
    RefinementConfig r;
    r.boundaryMargin = .0005;
    CHECK(!stability(initial, a, b, legacyThresholds(), r).stable);
    r.boundaryMargin = std::nextafter(.0005, 0.0);
    CHECK(stability(initial, a, b, legacyThresholds(), r).stable);
}
TEST_CASE("Stability rejects rank and score metadata inconsistent with its paired witness",
          "[unit][review][threshold][audit]") {
    const auto root = chess::Position::initial();
    const auto c = legacyThresholds();
    const auto a = stabilityPair(root, 10, false), b = stabilityPair(root, 10, true);
    auto d = classify(root, a, c, kWdlConversion, 2), v = classify(root, b, c, kWdlConversion, 2);
    d.playedMoveRank = v.playedMoveRank = 1;
    CHECK(!assessStability(d, v, a, b, {}, 2).stable);
    const auto clearA = stabilityPair(root, 150, false), clearB = stabilityPair(root, 150, true);
    d = classify(root, clearA, c, kWdlConversion, 2);
    v = classify(root, clearB, c, kWdlConversion, 2);
    d.before.expected->value = v.before.expected->value = .8;
    CHECK(!assessStability(d, v, clearA, clearB, {}, 2).stable);
}
TEST_CASE("Unknown pipelines have no stability model", "[unit][review][threshold][audit]") {
    CHECK(!stabilityModelForPipeline(0));
    CHECK(!stabilityModelForPipeline(99));
    CHECK(!stabilityModelForPipeline(std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("Portable threshold arithmetic preserves captured policy eight decisions",
          "[unit][review][threshold][audit]") {
    const auto root = chess::Position::initial();
    const auto half = [&](bool verified) {
        auto p = stabilityPair(root, 0, verified);
        p.after.evaluation.wdl = Wdl{500, 1, 499};
        p.after.candidates.front().evaluation = p.after.evaluation;
        return pairEvidence(root, p.playedMove, p.before, p.after, kWdlConversion, 1);
    };
    const auto a = half(false), b = half(true);
    auto c = legacyThresholds();
    c.excellentLoss = std::nextafter(.0045, 1.0);
    RefinementConfig r;
    r.boundaryMargin = .004;
    const auto d = classify(root, a, c, kWdlConversion, 2),
               v = classify(root, b, c, kWdlConversion, 2);
    REQUIRE(d.loss == .0005);
    const bool legacyNear =
        std::abs(static_cast<long double>(*d.loss) - static_cast<long double>(c.excellentLoss)) <=
        static_cast<long double>(r.boundaryMargin);
    CHECK(assessStability(d, v, a, b, r, 2).stable == !legacyNear);
    CHECK(assessStability(d, v, a, b, r, 3).reason == StabilityReason::ThresholdProximity);
    CHECK(assessQuickScan(d, a, r, 8).needsRefinement() == legacyNear);
    CHECK(assessQuickScan(d, a, r, 9).needsRefinement());
    CHECK(stabilityModelForPipeline(8) == 2);
    CHECK(stabilityModelForPipeline(9) == 3);
}

TEST_CASE("Classification model three, stability model four, and pipeline fourteen rules",
          "[unit][review][v3][v4][pipeline14]") {
    // 1. Sole legal move is Best (forced.best.v3) with zero loss.
    const auto forcedRoot =
        std::get<chess::Position>(chess::Position::fromFen("7k/8/5K2/8/8/8/8/7R b - - 0 1"));
    REQUIRE(forcedRoot.legalMoves().size() == 1);
    const auto forcedMove = forcedRoot.legalMoves().front();
    auto forcedAfterPos = forcedRoot;
    REQUIRE(std::holds_alternative<std::monostate>(forcedAfterPos.play(forcedMove)));
    auto forcedBefore =
        evidence(forcedRoot, 500, std::get<std::string>(forcedRoot.uci(forcedMove)).c_str());
    forcedBefore.evaluation.score = RawScore{ScoreKind::Centipawns, -800, Bound::Exact};
    forcedBefore.candidates.front().evaluation = forcedBefore.evaluation;
    auto forcedAfter = evidence(
        forcedAfterPos, 500,
        std::get<std::string>(forcedAfterPos.uci(forcedAfterPos.legalMoves().front())).c_str());
    forcedAfter.evaluation.score = RawScore{ScoreKind::Centipawns, 500, Bound::Exact};
    forcedAfter.candidates.front().evaluation = forcedAfter.evaluation;
    const auto forcedPair = pairEvidence(forcedRoot, forcedMove, forcedBefore, forcedAfter);
    const auto forcedClass = classify(forcedRoot, forcedPair, {});
    CHECK(forcedClass.label == Classification::Best);
    CHECK(forcedClass.loss == 0.0);
    CHECK(forcedClass.ruleId == "forced.best.v3");

    // 2. Rank-1 played move with large child disagreement stays Best (no BestMoveConflict),
    // while retaining signedDelta so quick scan flags SearchConflict.
    const auto init = chess::Position::initial();
    const auto bestPair = pair(init, 250, true, 2);
    const auto bestClass = classify(init, bestPair, {}, kWdlConversion, 3);
    CHECK(bestClass.label == Classification::Best);
    CHECK(bestClass.loss == 0.0);
    CHECK(bestClass.signedDelta == Catch::Approx(0.25));
    CHECK(bestClass.ruleId == "verified_candidate.best.v3");
    const auto bestScan = assessQuickScan(bestClass, bestPair, {}, 14);
    CHECK(std::find(bestScan.reasons.begin(), bestScan.reasons.end(),
                    RefinementReason::SearchConflict) != bestScan.reasons.end());

    // 3. Stability v4: agreeing mates are stable, and 0.0 loss with excellentLoss=0.02 skips
    // ThresholdProximity.
    auto mateA = stabilityPair(init, 0, false, false, 2);
    mateA.before.evaluation = {chess::Color::White, RawScore{ScoreKind::Mate, 3, Bound::Exact}, {}};
    mateA.before.candidates.front().evaluation = mateA.before.evaluation;
    mateA.after.evaluation = {chess::Color::Black, RawScore{ScoreKind::Mate, -2, Bound::Exact}, {}};
    mateA.after.candidates.front().evaluation = mateA.after.evaluation;
    mateA = pairEvidence(init, mateA.playedMove, mateA.before, mateA.after);
    auto mateB = stabilityPair(init, 0, true, false, 2);
    mateB.before.evaluation = {chess::Color::White, RawScore{ScoreKind::Mate, 4, Bound::Exact}, {}};
    mateB.before.candidates.front().evaluation = mateB.before.evaluation;
    mateB.after.evaluation = {chess::Color::Black, RawScore{ScoreKind::Mate, -3, Bound::Exact}, {}};
    mateB.after.candidates.front().evaluation = mateB.after.evaluation;
    mateB = pairEvidence(init, mateB.playedMove, mateB.before, mateB.after);
    const auto mateStab =
        assessStability(classify(init, mateA, {}), classify(init, mateB, {}), mateA, mateB, {}, 4);
    CHECK(mateStab.stable);

    const auto zeroA = stabilityPair(init, 0, false, false, 2);
    const auto zeroB = stabilityPair(init, 0, true, false, 2);
    const auto zeroStab =
        assessStability(classify(init, zeroA, {}, kWdlConversion, 3),
                        classify(init, zeroB, {}, kWdlConversion, 3), zeroA, zeroB, {}, 4);
    CHECK(zeroStab.stable);

    // 4. Pipeline 14: SacrificeSignal is Tier 0 ahead of ExpectedScoreLoss, and Fast/Standard
    // budgets allow 6 / 12 moves (12 / 24 positions).
    std::vector<RefinementCandidate> queue;
    for (int i = 1; i <= 15; ++i) {
        ScanAssessment s;
        s.pipelineVersion = 14;
        s.state = ScanState::Usable;
        s.loss = 0.25;
        s.qualifyingLoss = 0.25;
        s.reasons = {RefinementReason::ExpectedScoreLoss};
        queue.push_back({i * 2, s});
    }
    queue.back().scan.reasons = {RefinementReason::SacrificeSignal};
    queue.back().scan.qualifyingLoss.reset();
    queue.back().scan.loss = 0.0;
    queue.back().scan.signedDelta = -0.02;
    const auto fastQ = std::get<std::vector<int>>(refinementQueue(queue, 14, Quality::Fast));
    const auto stdQ = std::get<std::vector<int>>(refinementQueue(queue, 14, Quality::Standard));
    CHECK(fastQ.size() == 12);
    CHECK(stdQ.size() == 24);
    CHECK(fastQ.front() == 29);

    // 5. Stability v4 confirms two distinct independent exact-mate searches with identical node
    // counts.
    auto equalMateA = mateA;
    auto equalMateB = mateB;
    equalMateA.before.strength.nodes = equalMateA.after.strength.nodes = 120;
    equalMateB.before.strength.nodes = equalMateB.after.strength.nodes = 120;
    CHECK(!assessStability(classify(init, equalMateA, {}), classify(init, equalMateB, {}),
                           equalMateA, equalMateB, {}, 3)
               .stable);
    CHECK(assessStability(classify(init, equalMateA, {}), classify(init, equalMateB, {}),
                          equalMateA, equalMateB, {}, 4)
              .stable);

    // 6. Immediate checkmate in v3 is Best even when another mate-in-1 occupied rank 1.
    const auto mateRoot =
        std::get<chess::Position>(chess::Position::fromFen("6k1/5ppp/8/8/8/8/Q7/4R1K1 w - - 0 1"));
    const auto playedMate = move(mateRoot, "a2a8");
    auto mateAfterPos = mateRoot;
    REQUIRE(std::holds_alternative<std::monostate>(mateAfterPos.play(playedMate)));
    REQUIRE(mateAfterPos.inCheck());
    REQUIRE(mateAfterPos.legalMoves().empty());
    auto mateBefore = evidence(mateRoot, 1000, "e1e8");
    const auto altCpMatePair = pairEvidence(mateRoot, playedMate, mateBefore,
                                            terminalEvidence(mateAfterPos), kWdlConversion, 2);
    CHECK(classify(mateRoot, altCpMatePair, {}, kWdlConversion, 2).label ==
          Classification::Excellent);
    const auto cpMateBestClass = classify(mateRoot, altCpMatePair, {}, kWdlConversion, 3);
    CHECK(cpMateBestClass.label == Classification::Best);
    CHECK(cpMateBestClass.loss == 0.0);
    CHECK(cpMateBestClass.ruleId == "checkmate.best.v3");
    mateBefore.evaluation.score = RawScore{ScoreKind::Mate, 1, Bound::Exact};
    mateBefore.candidates.front().evaluation = mateBefore.evaluation;
    const auto altMatePair = pairEvidence(mateRoot, playedMate, mateBefore,
                                          terminalEvidence(mateAfterPos), kWdlConversion, 2);
    const auto bestMateClass = classify(mateRoot, altMatePair, {}, kWdlConversion, 3);
    CHECK(bestMateClass.label == Classification::Best);
    CHECK(bestMateClass.loss == 0.0);
    CHECK(bestMateClass.ruleId == "checkmate.best.v3");

    // 7. Stability v4 tolerates minor ApproximateCentipawns refinement (up to 0.04) when loss is
    // stable, while still rejecting >0.04 swings, excessive loss drift, and zero maximumDrift.
    auto makeCpPair = [&](std::int64_t beforeCp, std::int64_t afterOppCp, bool verified,
                          bool rankOne = true) {
        const auto played = move(init, "e2e4");
        auto next = init;
        REQUIRE(std::holds_alternative<std::monostate>(next.play(played)));
        auto b = evidence(init, 500, rankOne ? "e2e4" : "g1f3");
        b.attemptIdentity = verified ? "cp-verify-before" : "cp-deep-before";
        b.strength = {verified ? 17 : 15, verified ? 400000U : 200000U, {}};
        b.evaluation = {
            chess::Color::White, RawScore{ScoreKind::Centipawns, beforeCp, Bound::Exact}, {}};
        b.candidates.front().evaluation = b.evaluation;
        b.candidates.front().strength = b.strength;
        auto a = evidence(next, 500, "e7e5");
        a.attemptIdentity = verified ? "cp-verify-after" : "cp-deep-after";
        a.strength = b.strength;
        a.evaluation = {
            chess::Color::Black, RawScore{ScoreKind::Centipawns, afterOppCp, Bound::Exact}, {}};
        a.candidates.front().evaluation = a.evaluation;
        a.candidates.front().strength = a.strength;
        return pairEvidence(init, played, b, a, ConversionConfig{}, 2);
    };
    const auto cpA = makeCpPair(266, -246, false);
    const auto cpB = makeCpPair(295, -260, true);
    CHECK(
        assessStability(classify(init, cpA, {}), classify(init, cpB, {}), cpA, cpB, {}, 4).stable);
    RefinementConfig zeroDrift;
    zeroDrift.maximumDrift = 0.0;
    CHECK(assessStability(classify(init, cpA, {}), classify(init, cpB, {}), cpA, cpB, zeroDrift, 4)
              .reason == StabilityReason::ScoreDrift);

    const auto bigSwingA = makeCpPair(100, -100, false);
    const auto bigSwingB = makeCpPair(300, -300, true);
    CHECK(assessStability(classify(init, bigSwingA, {}), classify(init, bigSwingB, {}), bigSwingA,
                          bigSwingB, {}, 4)
              .reason == StabilityReason::ScoreDrift);

    // Same Inaccuracy label (0.064 vs 0.089), but loss itself drifted > 0.02 so tolerance stays
    // off.
    const auto lossDriftA = makeCpPair(0, 70, false, false);
    const auto lossDriftB = makeCpPair(0, 98, true, false);
    CHECK(assessStability(classify(init, lossDriftA, {}), classify(init, lossDriftB, {}),
                          lossDriftA, lossDriftB, {}, 4)
              .reason == StabilityReason::ScoreDrift);
}
