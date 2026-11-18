#include "puzzles/Puzzle.h"
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace chess_review;
namespace {
chess::Position mateRoot() {
    return std::get<chess::Position>(
        chess::Position::fromFen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1"));
}
puzzles::Definition definition(const chess::Position& root) {
    puzzles::Definition d;
    d.position = root.context();
    d.origin = puzzles::Origin::Generated;
    d.policy.baselineNodes = 10;
    return d;
}
review::PositionEvidence matePass(const chess::Position& root, std::uint64_t nodes) {
    review::PositionEvidence pass;
    pass.position = root.context();
    pass.complete = true;
    pass.engineIdentity = "engine";
    pass.sessionIdentity = "session";
    pass.attemptIdentity = std::to_string(nodes);
    pass.strength = {12, nodes, {}};
    std::vector<chess::Move> roots{std::get<chess::Move>(root.parseUci("d1d8"))};
    for (const auto m : root.legalMoves()) {
        if (roots.size() == 4) {
            break;
        }
        if (m != roots.front()) {
            roots.push_back(m);
        }
    }
    int rank = 0;
    for (const auto move : roots) {
        review::RawEvaluation e{
            root.sideToMove(),
            review::RawScore{rank == 0 ? review::ScoreKind::Mate : review::ScoreKind::Centipawns,
                             rank == 0 ? 1 : 0, review::Bound::Exact},
            {}};
        pass.candidates.push_back({++rank, e, {move}, pass.strength});
    }
    pass.evaluation = pass.candidates.front().evaluation;
    return pass;
}
} // namespace
TEST_CASE("Puzzle verification requires two deeper independent attempts", "[unit][puzzles]") {
    const auto root = mateRoot();
    const auto d = definition(root);
    auto pending = puzzles::verify(root, d, {});
    REQUIRE(pending.next);
    CHECK(pending.next->nodes == 20);
    CHECK(pending.next->width == 4);
    std::vector passes{matePass(root, 20)};
    pending = puzzles::verify(root, d, passes);
    REQUIRE(pending.next);
    CHECK(pending.next->nodes == 40);
    passes.push_back(matePass(root, 40));
    const auto complete = puzzles::verify(root, d, passes);
    REQUIRE(complete.outcome == puzzles::Outcome::Accepted);
    CHECK(complete.solution.goal == puzzles::Goal::Mate);
    REQUIRE(complete.solution.nodes.size() == 2);
    CHECK(complete.solution.nodes[1].complete);
    passes[1].attemptIdentity = passes[0].attemptIdentity;
    CHECK(puzzles::verify(root, d, passes).outcome != puzzles::Outcome::Accepted);
}
TEST_CASE("Puzzle ambiguity and malformed mate companions fail closed", "[unit][puzzles]") {
    const auto root = mateRoot();
    const auto d = definition(root);
    for (int damage = 0; damage < 5; ++damage) {
        std::vector passes{matePass(root, 20), matePass(root, 40), matePass(root, 80)};
        for (auto& p : passes) {
            if (damage == 0) {
                p.candidates[1].evaluation.score->bound = review::Bound::Lower;
            }
            if (damage == 1) {
                p.candidates[1].continuation[0] = p.candidates[0].continuation[0];
            }
            if (damage == 2) {
                p.restricted = true;
            }
            if (damage == 3) {
                for (auto& c : p.candidates) {
                    c.evaluation = p.evaluation;
                }
            }
            if (damage == 4) {
                p.candidates.pop_back();
            }
        }
        CHECK(puzzles::verify(root, d, passes).outcome != puzzles::Outcome::Accepted);
    }
}
TEST_CASE("Puzzle budgets and exact history are enforced", "[unit][puzzles]") {
    const auto root = mateRoot();
    auto d = definition(root);
    d.policy.maximumBudgetUnits = 6;
    d.policy.rootMinimumNodes = 40;
    CHECK(puzzles::verify(root, d, {}).outcome == puzzles::Outcome::Insufficient);
    d = definition(root);
    d.position.halfmoveClock++;
    CHECK(puzzles::verify(root, d, {}).outcome == puzzles::Outcome::Rejected);
    d = definition(root);
    d.policy.baselineNodes = std::numeric_limits<std::uint64_t>::max();
    CHECK(puzzles::validPolicy(d.policy) == false);
}
TEST_CASE("Puzzle sequences verify every decision and require a supported endpoint",
          "[unit][puzzles]") {
    for (const bool black : {false, true}) {
        const auto root = std::get<chess::Position>(chess::Position::fromFen(
            black ? "5q2/8/8/8/8/5k2/8/7K b - - 0 1" : "7k/8/5K2/8/8/8/8/5Q2 w - - 0 1"));
        const std::vector<const char*> ucis =
            black ? std::vector<const char*>{"f8h8", "h1g1", "h8g7", "g1f1", "g7a1"}
                  : std::vector<const char*>{"f1h1", "h8g8", "h1g2", "g8f8", "g2a8"};
        auto board = root;
        std::vector<chess::Move> line;
        for (const auto uci : ucis) {
            const auto parsed = board.parseUci(uci);
            REQUIRE(std::holds_alternative<chess::Move>(parsed));
            line.push_back(std::get<chess::Move>(parsed));
            REQUIRE(std::holds_alternative<std::monostate>(board.play(line.back())));
        }
        REQUIRE(board.inCheck());
        REQUIRE(board.legalMoves().empty());
        auto d = definition(root);
        std::vector<review::PositionEvidence> passes;
        auto result = puzzles::verify(root, d, passes);
        while (result.outcome == puzzles::Outcome::Pending && result.next) {
            const auto need = *result.next;
            const auto step = need.position.context().moves.size();
            REQUIRE(step < line.size());
            review::PositionEvidence e;
            e.position = need.position.context();
            e.complete = true;
            e.engineIdentity = "engine";
            e.sessionIdentity = "session";
            e.attemptIdentity = std::to_string(passes.size());
            e.strength = {12, need.nodes, {}};
            const bool solver = step % 2 == 0;
            std::vector<chess::Move> roots{line[step]};
            for (const auto move : need.position.legalMoves()) {
                if (roots.size() == static_cast<std::size_t>(need.width)) {
                    break;
                }
                if (move != roots.front()) {
                    roots.push_back(move);
                }
            }
            int rank = 0;
            for (const auto move : roots) {
                const bool first = rank == 0;
                const review::RawEvaluation evaluation{
                    need.position.sideToMove(),
                    review::RawScore{first || !solver ? review::ScoreKind::Mate
                                                      : review::ScoreKind::Centipawns,
                                     first || !solver ? (solver ? 3 : -3) : 0},
                    {}};
                e.candidates.push_back(
                    {++rank, evaluation,
                     first ? std::vector<chess::Move>(
                                 line.begin() + static_cast<std::ptrdiff_t>(step), line.end())
                           : std::vector{move},
                     e.strength});
            }
            e.evaluation = e.candidates.front().evaluation;
            passes.push_back(e);
            result = puzzles::verify(root, d, passes);
            REQUIRE(passes.size() <= 10);
        }
        REQUIRE(result.outcome == puzzles::Outcome::Accepted);
        REQUIRE(result.solution.nodes.size() == 6);
        CHECK(result.solution.nodes.back().complete);
        CHECK(passes.size() == 10);
        // A PV does not certify its unsearched child decisions.
        CHECK(puzzles::verify(root, d, std::span(passes).first(2)).outcome ==
              puzzles::Outcome::Pending);
        d.policy.maximumPlayerMoves = 2;
        CHECK(puzzles::verify(root, d, passes).outcome == puzzles::Outcome::Insufficient);
    }
}
TEST_CASE("Automatic puzzles reject positional gaps while manual puzzles may practice them",
          "[unit][puzzles]") {
    auto root = chess::Position::initial();
    auto d = definition(root);
    auto pass = [&](std::uint64_t nodes) {
        auto p = matePass(mateRoot(), nodes);
        p.position = root.context();
        const auto moves = root.legalMoves();
        for (std::size_t i = 0; i < p.candidates.size(); ++i) {
            p.candidates[i].continuation = {moves[i]};
            p.candidates[i].evaluation.score =
                review::RawScore{review::ScoreKind::Centipawns, i == 0 ? 300 : 0};
        }
        p.evaluation = p.candidates.front().evaluation;
        return p;
    };
    const std::vector passes{pass(20), pass(40)};
    CHECK(puzzles::verify(root, d, passes).outcome == puzzles::Outcome::Rejected);
    d.origin = puzzles::Origin::Manual;
    const auto manual = puzzles::verify(root, d, passes);
    CHECK(manual.outcome == puzzles::Outcome::Pending);
    REQUIRE(manual.next);
    CHECK(manual.next->position.context().moves.size() == 1);
}
