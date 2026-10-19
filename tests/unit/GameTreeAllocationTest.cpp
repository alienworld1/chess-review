#include "chess/GameTree.h"
#include "pgn/Pgn.h"
#include "support/AllocationFailure.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace chess_review::chess;
using chess_review::test_support::FailAfter;

namespace {
template <typename T, typename E> T unwrap(std::variant<T, E> result) {
    REQUIRE(std::holds_alternative<T>(result));
    return std::get<T>(std::move(result));
}
std::vector<GameTreeNode> snapshot(const GameTree& game) {
    std::vector<GameTreeNode> nodes{*game.node(game.rootId())};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto children = nodes[index].children;
        for (const auto child : children) {
            nodes.push_back(*game.node(child));
        }
    }
    return nodes;
}
template <typename Operation> void checkAtomic(const GameTree& original, Operation operation) {
    const auto before = snapshot(original);
    const auto context = unwrap(original.position(original.rootId())).context();
    bool reachedSuccess = false;
    for (std::size_t count = 0; count < 1024; ++count) {
        auto candidate = original;
        bool failed = false;
        bool succeeded = false;
        try {
            const FailAfter fail{count};
            succeeded = operation(candidate);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        if (!failed) {
            CHECK(succeeded);
            reachedSuccess = true;
            break;
        }
        CHECK(snapshot(candidate) == before);
        CHECK(candidate.annotationBytes() == original.annotationBytes());
        CHECK(unwrap(candidate.position(candidate.rootId())).context() == context);
        // A failed transaction must not consume IDs, even if some nodes were staged.
        auto control = original;
        const auto fresh = unwrap(Position::initial().parseUci("d2d4"));
        CHECK(unwrap(candidate.addMove(candidate.rootId(), fresh)) ==
              unwrap(control.addMove(control.rootId(), fresh)));
    }
    REQUIRE(reachedSuccess);
}
} // namespace

TEST_CASE("Game tree edits remain atomic at every allocation failure", "[chess][unit]") {
    auto game = unwrap(GameTree::initial());
    auto board = Position::initial();
    std::vector<Move> line;
    for (const auto uci : {"e2e4", "e7e5", "g1f3", "b8c6"}) {
        const auto move = unwrap(board.parseUci(uci));
        unwrap(board.play(move));
        line.push_back(move);
    }
    const auto root = game.rootId();
    const auto e4 = unwrap(game.addMove(root, line.front()));
    const NodeAnnotations notes{std::string(200, 'a'), std::string(300, 'b'), {1, 2, 255}};
    unwrap(game.setAnnotations(e4, notes));
    checkAtomic(game, [&](GameTree& candidate) {
        return std::holds_alternative<NodeId>(candidate.addLine(root, line));
    });
    checkAtomic(game, [&](GameTree& candidate) {
        return std::holds_alternative<std::monostate>(candidate.setAnnotations(e4, notes));
    });
    unwrap(game.addLine(root, line));
    checkAtomic(game, [&](GameTree& candidate) {
        return std::holds_alternative<std::monostate>(candidate.removeSubtree(e4));
    });
    auto replacement = unwrap(GameTree::fromFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
    unwrap(replacement.setAnnotations(replacement.rootId(), notes));
    checkAtomic(game, [&](GameTree& candidate) {
        candidate = replacement;
        return true;
    });
}

TEST_CASE("Reusing existing game tree paths needs no allocation", "[chess][unit]") {
    auto game = unwrap(GameTree::initial());
    auto board = Position::initial();
    std::vector<Move> moves;
    for (const auto uci : {"e2e4", "e7e5", "g1f3", "b8c6"}) {
        const auto move = unwrap(board.parseUci(uci));
        unwrap(board.play(move));
        moves.push_back(move);
    }
    const auto end = unwrap(game.addLine(game.rootId(), moves));
    const auto parent = *game.node(end)->parent;
    const auto before = snapshot(game);
    for (const bool wholeLine : {false, true}) {
        bool allocationFailed = false;
        bool reused = false;
        try {
            const FailAfter fail{0};
            const auto result =
                wholeLine ? game.addLine(game.rootId(), moves) : game.addMove(parent, moves.back());
            const auto* id = std::get_if<NodeId>(&result);
            reused = id && *id == end;
        } catch (const std::bad_alloc&) {
            allocationFailed = true;
        }
        CHECK(!allocationFailed);
        CHECK(reused);
        CHECK(snapshot(game) == before);
    }
}

TEST_CASE("Bulk tree construction handles every ordinary allocation failure", "[chess][unit]") {
    const auto e4 = unwrap(Position::initial().parseSan("e4"));
    auto board = Position::initial();
    unwrap(board.play(e4));
    const auto e5 = unwrap(board.parseSan("e5"));
    const auto c5 = unwrap(board.parseSan("c5"));
    const std::vector<GameTreeInputNode> records{
        {0, std::nullopt, {std::string(200, 'r'), {}, {1}}},
        {0, e4, {{}, std::string(300, 'a'), {1, 2}}},
        {1, e5, {}},
        {1, c5, {std::string(200, 'v'), {}, {255}}}};
    bool reachedSuccess = false;
    for (std::size_t count = 0; count < 2048; ++count) {
        bool failed = false;
        bool succeeded = false;
        try {
            const FailAfter fail{count};
            auto result = GameTree::fromNodes(std::nullopt, records);
            const auto* built = std::get_if<GameTree>(&result);
            succeeded = built && built->size() == 4 && built->annotationBytes() == 704;
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        if (!failed) {
            REQUIRE(succeeded);
            reachedSuccess = true;
            break;
        }
    }
    REQUIRE(reachedSuccess);
}

TEST_CASE("PGN allocation failures preserve previously published documents", "[pgn][unit]") {
    using namespace chess_review::pgn;
    const std::string source = "[Event \"first\"] 1.e4 * [Event {" + std::string(200, 'g') +
                               "} \"second\"] [Event \"second\"] {opening} 1{" +
                               std::string(200, 'n') + "}.e4 {" + std::string(200, 'a') +
                               "} (1.e4 {" + std::string(200, 'b') + "} c5 $255) e5 2.Nf3 *";
    bool reachedSuccess = false;
    for (std::size_t count = 0; count < 2048; ++count) {
        PgnReader reader(source);
        auto firstOutcome = reader.next();
        REQUIRE(std::holds_alternative<PgnDocument>(firstOutcome));
        const auto& first = std::get<PgnDocument>(firstOutcome);
        const auto before = snapshot(first.tree);
        const auto sourceBefore = first.originalText;
        const auto played =
            std::vector<Move>(first.originalMainline().begin(), first.originalMainline().end());
        bool failed = false;
        bool succeeded = false;
        try {
            const FailAfter fail{count};
            auto outcome = reader.next();
            const auto* document = std::get_if<PgnDocument>(&outcome);
            succeeded = document && document->tree.size() == 5 &&
                        document->originalMainline().size() == 3 && document->warnings.size() == 2;
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        CHECK(snapshot(first.tree) == before);
        CHECK(first.originalText == sourceBefore);
        CHECK(std::vector<Move>(first.originalMainline().begin(), first.originalMainline().end()) ==
              played);
        CHECK(first.tags.front().value == "first");
        if (!failed) {
            REQUIRE(succeeded);
            reachedSuccess = true;
            break;
        }
        // The reader is destroyed here after an exceptional failure, as its API requires.
    }
    REQUIRE(reachedSuccess);
}
