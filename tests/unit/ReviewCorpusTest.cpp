#include "pgn/Pgn.h"
#include "review/Intelligence.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <variant>

using namespace chess_review;
using namespace chess_review::review;
namespace {
struct ExpectedGame {
    std::size_t plies;
    std::size_t sacrificeRoot;
    int concession;
    chess::PieceType actor;
    const char* square;
};
const TacticalFact* find(const std::vector<TacticalFact>& facts, TacticalKind kind) {
    const auto it = std::find_if(facts.begin(), facts.end(),
                                 [=](const auto& fact) { return fact.kind == kind; });
    return it == facts.end() ? nullptr : &*it;
}
} // namespace

TEST_CASE("Historical game corpus retains concrete reasons and withholds unsupported claims",
          "[unit][review][corpus]") {
    std::ifstream input(REVIEW_INTELLIGENCE_CORPUS);
    REQUIRE(input.is_open());
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    pgn::PgnReader reader(text);
    const std::array games{
        ExpectedGame{33, 24, 2, chess::PieceType::Rook, "d7"},
        ExpectedGame{47, 40, 8, chess::PieceType::Queen, "d7"},
    };
    for (const auto& expected : games) {
        auto parsed = reader.next();
        REQUIRE(std::holds_alternative<pgn::PgnDocument>(parsed));
        const auto& document = std::get<pgn::PgnDocument>(parsed);
        INFO(document.tags.front().value);
        REQUIRE(document.warnings.empty());
        REQUIRE(document.originalMainline().size() == expected.plies);
        const auto moves = document.originalMainline();
        auto root = chess::Position::initial();
        for (std::size_t ply = 0; ply < moves.size(); ++ply) {
            CAPTURE(ply);
            const auto before = root.context();
            const auto continuation =
                moves.subspan(ply, std::min(tacticalLineLimit, moves.size() - ply));
            const auto facts = inspectTacticalLine(root, continuation, FactSource::Played);
            CHECK(root.context() == before);
            for (const auto& fact : facts) {
                CHECK(fact.modelVersion == motifModelVersion);
                CHECK(fact.source == FactSource::Played);
                REQUIRE(fact.linePlies > 0);
                REQUIRE(fact.line.size() <= continuation.size());
                CHECK(std::equal(fact.line.begin(), fact.line.end(), continuation.begin()));
                auto replay = root;
                for (const auto& move : fact.line) {
                    REQUIRE(std::holds_alternative<std::monostate>(replay.play(move)));
                }
                if (fact.kind == TacticalKind::Checkmate) {
                    CHECK(replay.inCheck());
                    CHECK(replay.legalMoves().empty());
                    CHECK(fact.beneficiary == chess::Color::White);
                }
            }
            if (ply == 0) {
                CHECK(!find(facts, TacticalKind::Sacrifice));
                CHECK(!find(facts, TacticalKind::Fork));
                CHECK(!find(facts, TacticalKind::Checkmate));
            }
            if (ply == expected.sacrificeRoot) {
                const auto* sacrifice = find(facts, TacticalKind::Sacrifice);
                REQUIRE(sacrifice);
                CHECK(sacrifice->material == expected.concession);
                CHECK(sacrifice->actor == expected.actor);
                CHECK(sacrifice->square.name() == expected.square);
                CHECK(sacrifice->beneficiary == chess::Color::White);
                REQUIRE(find(facts, TacticalKind::Checkmate));
                // The offered piece and its acceptance alone do not prove soundness:
                // legacy motif v3 withheld shallow sacrifices until recovery/mate, while
                // motif v4 recognizes the material sacrifice directly and leaves
                // soundness to numeric expected-score verification.
                const auto shallowV3 =
                    inspectTacticalLine(root, continuation.first(2), FactSource::Played, 3);
                CHECK(!find(shallowV3, TacticalKind::Sacrifice));
                CHECK(!find(shallowV3, TacticalKind::Checkmate));
                const auto shallowV4 =
                    inspectTacticalLine(root, continuation.first(2), FactSource::Played);
                CHECK(find(shallowV4, TacticalKind::Sacrifice));
                CHECK(!find(shallowV4, TacticalKind::Checkmate));
                // A historical brilliancy is not permission for a special label
                // without independent compatible numeric analysis evidence.
                MoveEvidence absent;
                absent.playedMove = moves[ply];
                const auto assessment = verifySpecial(root, absent, nullptr, {});
                CHECK(!assessment.label);
                CHECK(assessment.ruleId == "special.numeric_evidence_required.v4");
                CHECK(verifySpecial(root, absent, nullptr, {}, 3).ruleId ==
                      "special.numeric_evidence_required.v3");
            }
            REQUIRE(std::holds_alternative<std::monostate>(root.play(moves[ply])));
        }
        CHECK(root.inCheck());
        CHECK(root.legalMoves().empty());
    }
    CHECK(std::holds_alternative<pgn::EndOfInput>(reader.next()));
}
