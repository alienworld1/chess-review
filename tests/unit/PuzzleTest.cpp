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
TEST_CASE("V2 verification accepts forks with shuffled tail MultiPV and differing losing replies",
          "[unit][puzzles]") {
    const auto makePass = [](const chess::Position& pos, std::uint64_t nodes, std::uint64_t id,
                             std::vector<std::pair<std::vector<chess::Move>, int>> lines) {
        review::PositionEvidence p;
        p.position = pos.context();
        p.complete = true;
        p.engineIdentity = "engine";
        p.sessionIdentity = "session";
        p.attemptIdentity = "pass-" + std::to_string(id);
        p.strength = {12, nodes, {}};
        int rank = 0;
        for (const auto& [pv, cp] : lines) {
            review::RawEvaluation e{
                pos.sideToMove(),
                review::RawScore{review::ScoreKind::Centipawns, cp, review::Bound::Exact},
                {}};
            p.candidates.push_back({++rank, e, pv, p.strength});
        }
        p.evaluation = p.candidates.front().evaluation;
        return p;
    };

    SECTION("Checking knight fork with shuffled MultiPV #3/#4 and differing losing replies") {
        // White: Kg1, Ne4, pawns a2, b2, h2. Black: Kg8, Qd7, Ng7, pawns a7, b7, f7, h7.
        const auto root = std::get<chess::Position>(
            chess::Position::fromFen("6k1/pp1q1pnp/8/8/4N3/8/PP5P/6K1 w - - 0 1"));
        const auto nf6 = std::get<chess::Move>(root.parseUci("e4f6"));
        auto afterFork = root;
        REQUIRE(std::holds_alternative<std::monostate>(afterFork.play(nf6)));
        const auto kh8 = std::get<chess::Move>(afterFork.parseUci("g8h8"));
        const auto kf8 = std::get<chess::Move>(afterFork.parseUci("g8f8"));

        auto afterKh8 = afterFork;
        REQUIRE(std::holds_alternative<std::monostate>(afterKh8.play(kh8)));
        const auto nxd7 = std::get<chess::Move>(afterKh8.parseUci("f6d7"));
        auto afterCapture = afterKh8;
        REQUIRE(std::holds_alternative<std::monostate>(afterCapture.play(nxd7)));
        const auto kg8 = std::get<chess::Move>(afterCapture.parseUci("h8g8"));
        auto quietLeaf = afterCapture;
        REQUIRE(std::holds_alternative<std::monostate>(quietLeaf.play(kg8)));
        const auto h3 = std::get<chess::Move>(quietLeaf.parseUci("h2h3"));

        auto d = definition(root);
        d.modelVersion = 2;

        // candidateGoal accepts a 1-move review PV with a checking knight fork.
        review::MoveEvidence reviewSample;
        reviewSample.playedMove = std::get<chess::Move>(root.parseUci("h2h3"));
        reviewSample.before = makePass(root, 10, 1, {{{nf6, kh8, nxd7, kg8, h3}, 600}});
        auto playedBoard = root;
        REQUIRE(std::holds_alternative<std::monostate>(playedBoard.play(reviewSample.playedMove)));
        reviewSample.after = makePass(
            playedBoard, 10, 2, {{{std::get<chess::Move>(playedBoard.parseUci("d7d1"))}, 200}});
        CHECK(puzzles::candidateGoal(root, nullptr, reviewSample, d.policy) ==
              puzzles::Goal::Material);

        const auto legalRoot = root.legalMoves();
        std::vector<chess::Move> others;
        for (const auto m : legalRoot) {
            if (m != nf6) {
                others.push_back(m);
            }
        }
        REQUIRE(others.size() >= 5);

        std::vector<review::PositionEvidence> passes;
        // Root passes with completely different 3rd/4th moves and upward eval drift (+600 -> +800).
        passes.push_back(makePass(root, 20, 10,
                                  {{{nf6, kh8, nxd7, kg8, h3}, 600},
                                   {{others[0]}, 0},
                                   {{others[1]}, -50},
                                   {{others[2]}, -100}}));
        passes.push_back(makePass(root, 40, 11,
                                  {{{nf6, kh8, nxd7, kg8, h3}, 800},
                                   {{others[0]}, 0},
                                   {{others[3]}, -60},
                                   {{others[4]}, -120}}));

        // Opponent node: Pass 1 prefers Kh8 (-620), Pass 2 prefers Kf8 (-750). Both decisively
        // lost.
        REQUIRE(afterFork.legalMoves().size() == 2);
        passes.push_back(makePass(afterFork, 20, 12, {{{kh8, nxd7}, -620}, {{kf8}, -630}}));
        passes.push_back(makePass(afterFork, 40, 13, {{{kf8}, -750}, {{kh8, nxd7}, -760}}));

        // Solver follow-up after 1...Kf8: 2. Nxd7 -> wins the queen.
        auto afterKf8 = afterFork;
        REQUIRE(std::holds_alternative<std::monostate>(afterKf8.play(kf8)));
        const auto nxd7Kf8 = std::get<chess::Move>(afterKf8.parseUci("f6d7"));
        const auto legalKf8 = afterKf8.legalMoves();
        std::vector<chess::Move> kf8Others;
        for (const auto m : legalKf8) {
            if (m != nxd7Kf8) {
                kf8Others.push_back(m);
            }
        }
        passes.push_back(makePass(afterKf8, 20, 14,
                                  {{{nxd7Kf8}, 700},
                                   {{kf8Others[0]}, 0},
                                   {{kf8Others[1]}, -50},
                                   {{kf8Others[2]}, -100}}));
        passes.push_back(makePass(afterKf8, 40, 15,
                                  {{{nxd7Kf8}, 780},
                                   {{kf8Others[0]}, 0},
                                   {{kf8Others[1]}, -50},
                                   {{kf8Others[2]}, -100}}));

        // Opponent node after 2. Nxd7: 2...Kg8
        auto afterNxd7Kf8 = afterKf8;
        REQUIRE(std::holds_alternative<std::monostate>(afterNxd7Kf8.play(nxd7Kf8)));
        const auto kg8FromF8 = std::get<chess::Move>(afterNxd7Kf8.parseUci("f8g8"));
        std::vector<chess::Move> oppOthers;
        for (const auto m : afterNxd7Kf8.legalMoves()) {
            if (m != kg8FromF8) {
                oppOthers.push_back(m);
            }
        }
        REQUIRE(oppOthers.size() >= 2);
        passes.push_back(
            makePass(afterNxd7Kf8, 20, 16,
                     {{{kg8FromF8}, -750}, {{oppOthers[0]}, -760}, {{oppOthers[1]}, -770}}));
        passes.push_back(
            makePass(afterNxd7Kf8, 40, 17,
                     {{{kg8FromF8}, -760}, {{oppOthers[0]}, -770}, {{oppOthers[1]}, -780}}));

        // Quiet leaf after 2...Kg8
        auto leaf = afterNxd7Kf8;
        REQUIRE(std::holds_alternative<std::monostate>(leaf.play(kg8FromF8)));
        const auto leafMoves = leaf.legalMoves();
        passes.push_back(makePass(leaf, 20, 18,
                                  {{{leafMoves[0]}, 760},
                                   {{leafMoves[1]}, 750},
                                   {{leafMoves[2]}, 740},
                                   {{leafMoves[3]}, 730}}));
        passes.push_back(makePass(leaf, 40, 19,
                                  {{{leafMoves[0]}, 770},
                                   {{leafMoves[1]}, 755},
                                   {{leafMoves[2]}, 745},
                                   {{leafMoves[3]}, 735}}));

        const auto verified = puzzles::verify(root, d, passes);
        REQUIRE(verified.outcome == puzzles::Outcome::Accepted);
        CHECK(verified.solution.goal == puzzles::Goal::Material);
        REQUIRE(!verified.solution.nodes.empty());
        CHECK(verified.solution.nodes.front().moves.front().move == nf6);
        CHECK(verified.solution.nodes.front().idea == review::RetryIdea::Fork);

        // gradeKnownMove works even when MultiPV #3/#4 differ between root passes.
        CHECK(puzzles::gradeKnownMove(root, nf6, d.policy, passes[0], passes[1]) ==
              puzzles::AlternativeGrade::Strong);
        CHECK(puzzles::gradeKnownMove(root, others[0], d.policy, passes[0], passes[1]) ==
              puzzles::AlternativeGrade::TryAgain);

        // Legacy modelVersion == 1 preserves strict all-4-candidate stability at (20, 40) nodes.
        auto legacy = d;
        legacy.modelVersion = 1;
        const auto legacyResult = puzzles::verify(root, legacy, passes);
        CHECK(legacyResult.outcome == puzzles::Outcome::Pending);
        REQUIRE(legacyResult.next);
        CHECK(legacyResult.next->nodes == 80);
    }

    SECTION("Non-checking knight fork qualifies without 16-ply quietEnd") {
        // White: Kg1, Nd3, pawns g2, h2. Black: Kg8, Qc6, Rg6, pawns a7, b7, g7, h7.
        // 1. Ne5 forks Qc6 and Rg6 (not check!).
        const auto root = std::get<chess::Position>(
            chess::Position::fromFen("6k1/pp4pp/2q3r1/8/8/3N4/6PP/6K1 w - - 0 1"));
        const auto ne5 = std::get<chess::Move>(root.parseUci("d3e5"));
        const auto afterNe5 = std::get<chess::Position>(
            chess::Position::fromFen("6k1/pp4pp/2q3r1/4N3/8/8/6PP/6K1 b - - 1 1"));
        const auto qc8 = std::get<chess::Move>(afterNe5.parseUci("c6c8"));
        review::MoveEvidence rev;
        rev.playedMove = std::get<chess::Move>(root.parseUci("h2h3"));
        rev.before = makePass(root, 10, 1, {{{ne5, qc8}, 250}});
        auto afterPlayed = root;
        REQUIRE(std::holds_alternative<std::monostate>(afterPlayed.play(rev.playedMove)));
        rev.after = makePass(afterPlayed, 10, 2,
                             {{{std::get<chess::Move>(afterPlayed.parseUci("g6e6"))}, 0}});
        auto d = definition(root);
        d.modelVersion = 2;
        CHECK(puzzles::candidateGoal(root, nullptr, rev, d.policy) == puzzles::Goal::Material);
    }

    SECTION("Opponent pass discovering a defensive save rejects the puzzle") {
        const auto root = std::get<chess::Position>(
            chess::Position::fromFen("6k1/pp1q1pnp/8/8/4N3/8/PP5P/6K1 w - - 0 1"));
        const auto nf6 = std::get<chess::Move>(root.parseUci("e4f6"));
        auto afterFork = root;
        REQUIRE(std::holds_alternative<std::monostate>(afterFork.play(nf6)));
        const auto kh8 = std::get<chess::Move>(afterFork.parseUci("g8h8"));
        const auto kf8 = std::get<chess::Move>(afterFork.parseUci("g8f8"));
        auto d = definition(root);
        d.modelVersion = 2;
        const auto rootOthers = root.legalMoves();
        std::vector<chess::Move> rOthers;
        for (const auto m : rootOthers) {
            if (m != nf6) {
                rOthers.push_back(m);
            }
        }
        std::vector<review::PositionEvidence> passes{
            makePass(root, 20, 1,
                     {{{nf6}, 600}, {{rOthers[0]}, 0}, {{rOthers[1]}, 0}, {{rOthers[2]}, 0}}),
            makePass(root, 40, 2,
                     {{{nf6}, 610}, {{rOthers[0]}, 0}, {{rOthers[1]}, 0}, {{rOthers[2]}, 0}}),
            makePass(afterFork, 20, 3, {{{kh8}, -600}, {{kf8}, -610}}),
            makePass(afterFork, 40, 4, {{{kf8}, 10}, {{kh8}, -610}}),
            makePass(afterFork, 80, 5, {{{kh8}, -600}, {{kf8}, -610}})};
        CHECK(puzzles::verify(root, d, passes).outcome == puzzles::Outcome::Insufficient);
    }
}
TEST_CASE(
    "Opera Game 16.Qb8+ mate and 10.Nxb5 sacrifice verify under v2 while nonsense is rejected",
    "[unit][puzzles]") {
    auto playLine = [](std::span<const char* const> ucis) {
        auto pos = chess::Position::initial();
        for (const auto* uci : ucis) {
            const auto move = std::get<chess::Move>(pos.parseUci(uci));
            REQUIRE(std::holds_alternative<std::monostate>(pos.play(move)));
        }
        return pos;
    };

    SECTION("Opera Game 16.Qb8+ forced mate with 1-legal-move reply and slower mate alternative") {
        constexpr std::array prefix{"e2e4", "e7e5", "g1f3", "d7d6", "d2d4", "c8g4", "d4e5", "g4f3",
                                    "d1f3", "d6e5", "f1c4", "g8f6", "f3b3", "d8e7", "b1c3", "c7c6",
                                    "c1g5", "b7b5", "c3b5", "c6b5", "c4b5", "b8d7", "e1c1", "a8d8",
                                    "d1d7", "d8d7", "h1d1", "e7e6", "b5d7", "f6d7"};
        const auto pos16 = playLine(prefix);
        const auto qb8 = std::get<chess::Move>(pos16.parseUci("b3b8"));
        auto pos16Reply = pos16;
        REQUIRE(std::holds_alternative<std::monostate>(pos16Reply.play(qb8)));
        REQUIRE(pos16Reply.legalMoves().size() == 1);
        const auto nxb8 = pos16Reply.legalMoves().front();
        auto pos17 = pos16Reply;
        REQUIRE(std::holds_alternative<std::monostate>(pos17.play(nxb8)));
        const auto rd8 = std::get<chess::Move>(pos17.parseUci("d1d8"));

        auto d = definition(pos16);
        d.modelVersion = 2;

        const auto makeMatePass = [](const chess::Position& pos, std::uint64_t nodes,
                                     std::uint64_t id, chess::Move best, int matePly,
                                     bool includeSlowerMate) {
            review::PositionEvidence p;
            p.position = pos.context();
            p.complete = true;
            p.engineIdentity = "engine";
            p.sessionIdentity = "session";
            p.attemptIdentity = "m-" + std::to_string(id);
            p.strength = {12, nodes, {}};
            const int width = static_cast<int>(std::min<std::size_t>(4, pos.legalMoves().size()));
            std::vector<chess::Move> roots{best};
            for (const auto m : pos.legalMoves()) {
                if (roots.size() == static_cast<std::size_t>(width)) {
                    break;
                }
                if (m != best) {
                    roots.push_back(m);
                }
            }
            int rank = 0;
            for (const auto m : roots) {
                ++rank;
                review::RawScore score{review::ScoreKind::Centipawns, 0, review::Bound::Exact};
                if (rank == 1) {
                    score = {review::ScoreKind::Mate, matePly, review::Bound::Exact};
                } else if (includeSlowerMate && rank == 2) {
                    score = {review::ScoreKind::Mate, matePly + 5, review::Bound::Exact};
                }
                p.candidates.push_back({rank, {pos.sideToMove(), score, {}}, {m}, p.strength});
            }
            p.evaluation = p.candidates.front().evaluation;
            return p;
        };

        std::vector<review::PositionEvidence> passes{
            makeMatePass(pos16, 20, 1, qb8, 2, true),
            makeMatePass(pos16, 40, 2, qb8, 2, true),
            makeMatePass(pos16Reply, 20, 3, nxb8, -1, false),
            makeMatePass(pos16Reply, 40, 4, nxb8, -1, false),
            makeMatePass(pos17, 20, 5, rd8, 1, false),
            makeMatePass(pos17, 40, 6, rd8, 1, false)};
        const auto res = puzzles::verify(pos16, d, passes);
        REQUIRE(res.outcome == puzzles::Outcome::Accepted);
        CHECK(res.solution.goal == puzzles::Goal::Mate);
        REQUIRE(res.solution.nodes.front().moves.size() == 1);
        CHECK(res.solution.nodes.front().moves.front().move == qb8);
        CHECK(res.solution.nodes.back().complete);
    }

    SECTION("Opera Game 10.Nxb5 tactical sacrifice verifies as Goal::Tactic") {
        constexpr std::array prefix{"e2e4", "e7e5", "g1f3", "d7d6", "d2d4", "c8g4",
                                    "d4e5", "g4f3", "d1f3", "d6e5", "f1c4", "g8f6",
                                    "f3b3", "d8e7", "b1c3", "c7c6", "c1g5", "b7b5"};
        const auto pos10 = playLine(prefix);
        const auto nxb5 = std::get<chess::Move>(pos10.parseUci("c3b5"));
        auto pos10Reply = pos10;
        REQUIRE(std::holds_alternative<std::monostate>(pos10Reply.play(nxb5)));
        const auto cxb5 = std::get<chess::Move>(pos10Reply.parseUci("c6b5"));
        auto pos11 = pos10Reply;
        REQUIRE(std::holds_alternative<std::monostate>(pos11.play(cxb5)));
        const auto bxb5 = std::get<chess::Move>(pos11.parseUci("c4b5"));
        auto pos11Reply = pos11;
        REQUIRE(std::holds_alternative<std::monostate>(pos11Reply.play(bxb5)));
        const auto nbd7 = std::get<chess::Move>(pos11Reply.parseUci("b8d7"));
        auto pos12 = pos11Reply;
        REQUIRE(std::holds_alternative<std::monostate>(pos12.play(nbd7)));
        const auto ooo = std::get<chess::Move>(pos12.parseUci("e1c1"));
        const auto rd1 = std::get<chess::Move>(pos12.parseUci("h1f1"));

        auto d = definition(pos10);
        d.modelVersion = 2;

        const auto makeCpPass = [](const chess::Position& pos, std::uint64_t nodes,
                                   std::uint64_t id, std::vector<chess::Move> pv, int bestCp,
                                   int secondCp) {
            review::PositionEvidence p;
            p.position = pos.context();
            p.complete = true;
            p.engineIdentity = "engine";
            p.sessionIdentity = "session";
            p.attemptIdentity = "t-" + std::to_string(id);
            p.strength = {12, nodes, {}};
            const int width = static_cast<int>(std::min<std::size_t>(4, pos.legalMoves().size()));
            std::vector<chess::Move> roots{pv.front()};
            for (const auto m : pos.legalMoves()) {
                if (roots.size() == static_cast<std::size_t>(width)) {
                    break;
                }
                if (m != pv.front()) {
                    roots.push_back(m);
                }
            }
            int rank = 0;
            for (const auto m : roots) {
                ++rank;
                const int cp = rank == 1 ? bestCp : secondCp - (rank - 2) * 30;
                p.candidates.push_back(
                    {rank,
                     {pos.sideToMove(),
                      review::RawScore{review::ScoreKind::Centipawns, cp, review::Bound::Exact},
                      {}},
                     rank == 1 ? pv : std::vector{m},
                     p.strength});
            }
            p.evaluation = p.candidates.front().evaluation;
            return p;
        };

        // Upward eval drift at root (+350 -> +500), non-forcing leaf at pos12 (two close moves).
        std::vector<review::PositionEvidence> passes{
            makeCpPass(pos10, 20, 1, {nxb5, cxb5, bxb5}, 350, 0),
            makeCpPass(pos10, 40, 2, {nxb5, cxb5, bxb5}, 500, 0),
            makeCpPass(pos10Reply, 20, 3, {cxb5, bxb5}, -350, -450),
            makeCpPass(pos10Reply, 40, 4, {cxb5, bxb5}, -480, -520),
            makeCpPass(pos11, 20, 5, {bxb5, nbd7, ooo}, 450, 50),
            makeCpPass(pos11, 40, 6, {bxb5, nbd7, ooo}, 490, 50),
            makeCpPass(pos11Reply, 20, 7, {nbd7, ooo}, -450, -700),
            makeCpPass(pos11Reply, 40, 8, {nbd7, ooo}, -490, -750)};
        // At pos12, ooo and rd1 are within alternativeGap -> tactic resolves cleanly.
        auto makeLeafPass = [&](std::uint64_t nodes, std::uint64_t id, int cp) {
            auto p = makeCpPass(pos12, nodes, id, {ooo}, cp, cp - 10);
            p.candidates[1].continuation = {rd1};
            return p;
        };
        passes.push_back(makeLeafPass(20, 9, 480));
        passes.push_back(makeLeafPass(40, 10, 500));

        const auto res = puzzles::verify(pos10, d, passes);
        REQUIRE(res.outcome == puzzles::Outcome::Accepted);
        CHECK(res.solution.goal == puzzles::Goal::Tactic);
        CHECK(res.solution.nodes.front().moves.front().move == nxb5);
    }

    SECTION("Balancing recapture is rejected by candidateGoal") {
        // 1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 4. Bxc6 dxc6 (d7c6 is a balancing recapture!)
        constexpr std::array opening{"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5c6"};
        const auto pos = playLine(opening);
        const auto dxc6 = std::get<chess::Move>(pos.parseUci("d7c6"));
        const auto bxc6 = std::get<chess::Move>(pos.parseUci("b7c6"));
        review::MoveEvidence ev;
        ev.playedMove = bxc6;
        ev.before.position = pos.context();
        ev.before.complete = true;
        ev.before.engineIdentity = "e";
        ev.before.sessionIdentity = "s";
        ev.before.attemptIdentity = "a";
        ev.before.strength = {10, 100, {}};
        ev.before.evaluation = review::RawEvaluation{
            pos.sideToMove(), review::RawScore{review::ScoreKind::Centipawns, 0}, {}};
        ev.before.candidates = std::vector<review::CandidateEvidence>{
            {1, ev.before.evaluation, {dxc6}, ev.before.strength}};
        auto afterPos = pos;
        REQUIRE(std::holds_alternative<std::monostate>(afterPos.play(bxc6)));
        ev.after.position = afterPos.context();
        ev.after.complete = true;
        ev.after.engineIdentity = "e";
        ev.after.sessionIdentity = "s";
        ev.after.attemptIdentity = "b";
        ev.after.strength = {10, 100, {}};
        ev.after.evaluation = review::RawEvaluation{
            afterPos.sideToMove(), review::RawScore{review::ScoreKind::Centipawns, 50}, {}};
        ev.after.candidates = std::vector<review::CandidateEvidence>{
            {1, ev.after.evaluation, {afterPos.legalMoves().front()}, ev.after.strength}};
        CHECK(!puzzles::candidateGoal(pos, nullptr, ev, puzzles::Policy{}));
    }
}
