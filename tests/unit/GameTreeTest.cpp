#include "chess/GameTree.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace chess_review::chess;

namespace {
template <typename T, typename E> T unwrap(std::variant<T, E> result) {
    if (const auto* failure = std::get_if<E>(&result)) {
        FAIL(failure->message);
    }
    return std::get<T>(std::move(result));
}
GameTree tree(GameTreeLimits limits = {}) { return unwrap(GameTree::initial(limits)); }
std::vector<Move> movesFor(Position position, std::initializer_list<std::string_view> moves) {
    std::vector<Move> result;
    for (const auto uci : moves) {
        const auto move = unwrap(position.parseUci(uci));
        unwrap(position.play(move));
        result.push_back(move);
    }
    return result;
}
NodeId add(GameTree& game, NodeId start, std::initializer_list<std::string_view> moves) {
    return unwrap(game.addLine(start, movesFor(unwrap(game.position(start)), moves)));
}
std::vector<GameTreeNode> snapshot(const GameTree& game) {
    std::vector<GameTreeNode> result{*game.node(game.rootId())};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto children = result[index].children;
        for (const auto id : children) {
            result.push_back(*game.node(id));
        }
    }
    return result;
}
template <typename T> GameTreeError rejects(GameTreeResult<T> result, GameTreeErrorCode code) {
    REQUIRE(std::holds_alternative<GameTreeError>(result));
    const auto failure = std::get<GameTreeError>(std::move(result));
    CHECK(failure.code == code);
    CHECK(failure.message.size() < 100);
    return failure;
}
void matchesReplay(const GameTree& game, NodeId end, std::initializer_list<std::string_view> moves,
                   Position replay) {
    for (const auto move : movesFor(replay, moves)) {
        unwrap(replay.play(move));
    }
    const auto resolved = unwrap(game.position(end));
    CHECK(resolved.fen() == replay.fen());
    CHECK(resolved.context() == replay.context());
}
} // namespace

TEST_CASE("Game tree preserves nested variations and promotes only siblings", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    const auto rootSnapshot = *game.node(root);
    CHECK(!(rootSnapshot.parent));
    CHECK(!(rootSnapshot.move));
    CHECK(!(rootSnapshot.san));
    const auto main = add(game, root, {"e2e4", "e7e5", "g1f3"});
    const auto line = unwrap(game.mainLine(root));
    REQUIRE(line.size() == 4);
    const auto e4 = line[1];
    const auto c5 = add(game, e4, {"c7c5"});
    const auto d5 = add(game, e4, {"d7d5"});
    const auto nc3 = add(game, c5, {"b1c3"});
    const auto nf3 = add(game, c5, {"g1f3"});
    const auto d4 = add(game, root, {"d2d4"});
    CHECK(unwrap(game.mainLine(root)) == line);
    unwrap(game.promote(nf3));
    CHECK(game.node(c5)->children == std::vector<NodeId>{nf3, nc3});
    CHECK(unwrap(game.mainLine(root)) == line);
    unwrap(game.promote(c5));
    CHECK(game.node(e4)->children == std::vector<NodeId>{c5, line[2], d5});
    CHECK(unwrap(game.mainLine(root)) == std::vector<NodeId>{root, e4, c5, nf3});
    unwrap(game.promote(c5));
    CHECK(game.node(root)->children == std::vector<NodeId>{e4, d4});
    CHECK(game.node(main)->parent == line[2]);
    CHECK(game.node(c5)->san == "c5");
    CHECK(rootSnapshot.children.empty());
    matchesReplay(game, nf3, {"e2e4", "c7c5", "g1f3"}, Position::initial());
}

TEST_CASE("Game tree reuses prefixes without changing annotations or order", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    const auto end = add(game, root, {"e2e4", "e7e5"});
    const auto e4 = unwrap(game.mainLine(root))[1];
    const NodeAnnotations notes{"before \xE2\x99\x9E", std::string{"after\0text", 10}, {1, 255, 1}};
    unwrap(game.setAnnotations(e4, notes));
    add(game, root, {"d2d4"});
    const auto before = snapshot(game);
    CHECK(add(game, root, {"e2e4", "e7e5"}) == end);
    CHECK(unwrap(game.addLine(e4, {})) == e4);
    CHECK(snapshot(game) == before);
    CHECK(game.node(e4)->annotations == notes);
    const auto newEnd = add(game, root, {"e2e4", "e7e5", "g1f3"});
    CHECK(game.node(newEnd)->parent == end);
    CHECK(game.node(e4)->annotations == notes);
    CHECK(game.node(root)->children.size() == 2);
}

TEST_CASE("Game tree deletion removes descendants and never reuses their IDs", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    const auto tail = add(game, root, {"e2e4", "e7e5", "g1f3"});
    const auto e4 = unwrap(game.mainLine(root))[1];
    const auto branch = add(game, e4, {"c7c5"});
    const auto d4 = add(game, root, {"d2d4"});
    unwrap(game.setAnnotations(tail, {"tail", "", {2, 2}}));
    unwrap(game.setAnnotations(branch, {"", "branch", {}}));
    unwrap(game.setAnnotations(root, {"root", "", {}}));
    const auto retained = *game.node(tail);
    unwrap(game.removeSubtree(e4));
    CHECK(game.size() == 2);
    CHECK(game.annotationBytes() == 4);
    CHECK(!(game.node(tail)));
    CHECK(!(game.node(branch)));
    CHECK(unwrap(game.mainLine(root)) == std::vector<NodeId>{root, d4});
    const auto replacement = add(game, root, {"e2e4"});
    CHECK(replacement.value > d4.value);
    CHECK(replacement != e4);
    CHECK(retained.annotations.commentBefore == "tail");
    CHECK(game.node(root)->children == std::vector<NodeId>{d4, replacement});
    rejects(game.removeSubtree(e4), GameTreeErrorCode::InvalidNode);
}

TEST_CASE("Game tree failed edits preserve complete state", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    const auto end = add(game, root, {"e2e4", "e7e5"});
    const auto before = snapshot(game);
    auto invalidLine = movesFor(Position::initial(), {"e2e4", "c7c5", "g1f3"});
    invalidLine.push_back(invalidLine.front());
    const auto failure =
        rejects(game.addLine(root, invalidLine), GameTreeErrorCode::PositionFailure);
    CHECK(failure.moveIndex == 3);
    REQUIRE(failure.positionError);
    CHECK(failure.positionError->code == ErrorCode::IllegalMove);
    CHECK(snapshot(game) == before);
    const auto valid = movesFor(Position::initial(), {"d2d4"}).front();
    const NodeId unknown{999'999};
    rejects(game.addMove(unknown, valid), GameTreeErrorCode::InvalidNode);
    rejects(game.addLine(unknown, {}), GameTreeErrorCode::InvalidNode);
    rejects(game.position(unknown), GameTreeErrorCode::InvalidNode);
    rejects(game.mainLine(unknown), GameTreeErrorCode::InvalidNode);
    rejects(game.promote(unknown), GameTreeErrorCode::InvalidNode);
    rejects(game.removeSubtree(unknown), GameTreeErrorCode::InvalidNode);
    rejects(game.setAnnotations(unknown, {}), GameTreeErrorCode::InvalidNode);
    rejects(game.promote(root), GameTreeErrorCode::RootOperation);
    rejects(game.removeSubtree(root), GameTreeErrorCode::RootOperation);
    CHECK(!(game.node(unknown)));
    CHECK(snapshot(game) == before);
    CHECK(add(game, root, {"d2d4"}).value == end.value + 1);
}

TEST_CASE("Game tree limits are exact and annotation budget is reclaimed", "[chess][unit]") {
    auto limits = GameTreeLimits{};
    limits.maxNodes = 0;
    rejects(GameTree::initial(limits), GameTreeErrorCode::InvalidLimits);
    rejects(GameTree::fromFen("bad", limits), GameTreeErrorCode::InvalidLimits);
    limits.maxNodes = 3;
    limits.maxDepth = 2;
    limits.maxCommentBytes = 4;
    limits.maxAnnotationBytes = 10;
    auto game = tree(limits);
    const auto root = game.rootId();
    const auto end = add(game, root, {"e2e4", "e7e5"});
    const auto before = snapshot(game);
    const auto exhausted = movesFor(Position::initial(), {"d2d4"});
    rejects(game.addLine(root, exhausted), GameTreeErrorCode::LimitExceeded);
    rejects(game.addLine(end, movesFor(unwrap(game.position(end)), {"g1f3"})),
            GameTreeErrorCode::LimitExceeded);
    CHECK(snapshot(game) == before);
    CHECK(add(game, root, {"e2e4", "e7e5"}) == end); // Reuse at a full budget.
    unwrap(game.setAnnotations(root, {"1234", "1234", {0, 255}}));
    CHECK(game.annotationBytes() == 10);
    const auto annotated = snapshot(game);
    rejects(game.setAnnotations(end, {"x", "", {}}), GameTreeErrorCode::LimitExceeded);
    rejects(game.setAnnotations(root, {"12345", "", {}}), GameTreeErrorCode::LimitExceeded);
    CHECK(snapshot(game) == annotated);
    unwrap(game.setAnnotations(root, {"1234", "", {}}));
    unwrap(game.setAnnotations(end, {"", "1234", {1, 1}}));
    CHECK(game.annotationBytes() == 10);
    unwrap(game.removeSubtree(unwrap(game.mainLine(root))[1]));
    CHECK(game.annotationBytes() == 4);
    const auto replacement = add(game, root, {"d2d4"});
    unwrap(game.setAnnotations(replacement, {"1234", "", {1, 1}}));
    CHECK(game.annotationBytes() == 10);
    limits.maxNodes = 20;
    limits.maxDepth = 1;
    auto shallow = tree(limits);
    const auto line = movesFor(Position::initial(), {"e2e4", "e7e5"});
    const auto failure =
        rejects(shallow.addLine(shallow.rootId(), line), GameTreeErrorCode::LimitExceeded);
    CHECK(failure.moveIndex == 1);
    CHECK(shallow.size() == 1);
    limits.maxDepth = 0;
    limits.maxAnnotationBytes = 0;
    auto rootOnly = tree(limits);
    unwrap(rootOnly.setAnnotations(rootOnly.rootId(), {}));
    rejects(rootOnly.addMove(rootOnly.rootId(), line.front()), GameTreeErrorCode::LimitExceeded);
    rejects(rootOnly.setAnnotations(rootOnly.rootId(), {"", "", {0}}),
            GameTreeErrorCode::LimitExceeded);
}

TEST_CASE("Game tree roots and branches retain exact position contexts", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    const auto first = add(game, root, {"g1f3", "g8f6", "b1c3", "b8c6"});
    const auto second = add(game, root, {"b1c3", "b8c6", "g1f3", "g8f6"});
    CHECK(first != second);
    const auto firstPosition = unwrap(game.position(first));
    const auto secondPosition = unwrap(game.position(second));
    CHECK(firstPosition.fen() == secondPosition.fen());
    CHECK(!(firstPosition.context() == secondPosition.context()));
    const auto fen = Position::initial().fen();
    auto fenGame = unwrap(GameTree::fromFen(fen));
    CHECK(unwrap(fenGame.position(fenGame.rootId())).context().provenance ==
          RootProvenance::FenAnalysisRoot);
    CHECK(!(unwrap(game.position(root)).context() ==
            unwrap(fenGame.position(fenGame.rootId())).context()));
    struct Fixture {
        std::string_view fen;
        std::initializer_list<std::string_view> moves;
    };
    const std::array<Fixture, 3> fixtures{{
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", {"e1g1", "e8c8"}},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", {"e5d6", "e8d7"}},
        {"4k3/P7/8/8/8/8/7p/4K3 w - - 0 1", {"a7a8n", "h2h1q"}},
    }};
    for (const auto& fixture : fixtures) {
        auto custom = unwrap(GameTree::fromFen(fixture.fen));
        const auto end = add(custom, custom.rootId(), fixture.moves);
        matchesReplay(custom, end, fixture.moves, unwrap(Position::fromFen(fixture.fen)));
    }
    rejects(GameTree::fromFen("malformed"), GameTreeErrorCode::PositionFailure);
    auto overflow = unwrap(GameTree::fromFen("4k3/8/8/8/8/8/8/4K3 b - - 0 4294967295"));
    const auto before = snapshot(overflow);
    const auto move = unwrap(unwrap(overflow.position(overflow.rootId())).parseUci("e8d8"));
    const auto failure =
        rejects(overflow.addMove(overflow.rootId(), move), GameTreeErrorCode::PositionFailure);
    REQUIRE(failure.positionError);
    CHECK(failure.positionError->code == ErrorCode::CounterOverflow);
    CHECK(snapshot(overflow) == before);
}

TEST_CASE("Game tree copies and moves own independent state", "[chess][unit]") {
    auto original = tree();
    const auto root = original.rootId();
    const auto end = add(original, root, {"e2e4", "e7e5"});
    unwrap(original.setAnnotations(end, {"original", "", {1}}));
    auto copy = original;
    CHECK(snapshot(copy) == snapshot(original));
    unwrap(copy.setAnnotations(end, {"copy", "", {2}}));
    unwrap(copy.removeSubtree(unwrap(copy.mainLine(root))[1]));
    CHECK(original.node(end)->annotations.commentBefore == "original");
    copy = original;
    CHECK(snapshot(copy) == snapshot(original));
    auto moved = std::move(copy);
    copy = tree();
    CHECK(snapshot(moved) == snapshot(original));
    auto assigned = tree();
    assigned = std::move(moved);
    CHECK(snapshot(assigned) == snapshot(original));
    matchesReplay(assigned, end, {"e2e4", "e7e5"}, Position::initial());
}

TEST_CASE("Deep and broad game trees use iterative traversal and deletion", "[chess][unit]") {
    auto limits = GameTreeLimits{};
    limits.maxDepth = 20'000;
    auto game = tree(limits);
    const auto cycle = movesFor(Position::initial(), {"g1f3", "g8f6", "f3g1", "f6g8"});
    std::vector<Move> moves;
    moves.reserve(limits.maxDepth);
    for (std::size_t index = 0; index < limits.maxDepth; ++index) {
        moves.push_back(cycle[index % cycle.size()]);
    }
    const auto end = unwrap(game.addLine(game.rootId(), moves));
    CHECK(game.size() == limits.maxDepth + 1);
    const auto line = unwrap(game.mainLine(game.rootId()));
    CHECK(line.size() == game.size());
    CHECK(line.back() == end);
    const auto resolved = unwrap(game.position(end));
    CHECK(resolved.context().moves == moves);
    {
        auto copy = game; // Destruction of a deep tree must also be iterative.
        unwrap(copy.setAnnotations(copy.rootId(), {"copy", "", {}}));
        CHECK(game.annotationBytes() == 0);
        CHECK(copy.size() == game.size());
    }
    unwrap(game.removeSubtree(line[1]));
    CHECK(game.size() == 1);
    for (const auto& move : Position::initial().legalMoves()) {
        const auto child = unwrap(game.addMove(game.rootId(), move));
        for (const auto& reply : unwrap(game.position(child)).legalMoves()) {
            unwrap(game.addMove(child, reply));
        }
    }
    CHECK(game.size() == 421);
    const auto children = game.node(game.rootId())->children;
    for (const auto child : children) {
        unwrap(game.removeSubtree(child));
    }
    CHECK(game.size() == 1);
}

TEST_CASE("Mixed tree edits preserve graph history and budget invariants", "[chess][unit]") {
    for (const auto seed : {7U, 41U, 97U, 601U}) {
        CAPTURE(seed);
        std::mt19937 random{seed};
        auto limits = GameTreeLimits{};
        limits.maxNodes = 48;
        limits.maxDepth = 12;
        limits.maxCommentBytes = 8;
        limits.maxAnnotationBytes = 64;
        auto game = tree(limits);
        std::uint64_t highestId = 0;
        std::set<NodeId> deleted;
        for (int step = 0; step < 200; ++step) {
            CAPTURE(step);
            const auto before = snapshot(game);
            const auto id = before[random() % before.size()].id;
            switch (random() % 5) {
            case 0:
            case 1: {
                const auto board = unwrap(game.position(id));
                const auto legal = board.legalMoves();
                if (legal.empty()) {
                    break;
                }
                const auto& move = legal[random() % legal.size()];
                const auto result = game.addMove(id, move);
                if (const auto* failure = std::get_if<GameTreeError>(&result)) {
                    CHECK(failure->code == GameTreeErrorCode::LimitExceeded);
                    CHECK(snapshot(game) == before);
                } else {
                    const auto added = std::get<NodeId>(result);
                    const auto old =
                        std::find_if(before.begin(), before.end(),
                                     [&](const auto& node) { return node.id == added; });
                    if (old == before.end()) {
                        CHECK(added.value > highestId);
                        highestId = added.value;
                    }
                }
                break;
            }
            case 2:
                if (id != game.rootId()) {
                    unwrap(game.promote(id));
                }
                break;
            case 3:
                if (id != game.rootId()) {
                    unwrap(game.removeSubtree(id));
                    for (const auto& node : before) {
                        if (!game.node(node.id)) {
                            deleted.insert(node.id);
                        }
                    }
                }
                break;
            case 4: {
                const NodeAnnotations notes{std::string(random() % 10, '\xFF'),
                                            std::string(random() % 10, '\0'),
                                            {0, 255, 0}};
                const auto status = game.setAnnotations(id, notes);
                if (const auto* failure = std::get_if<GameTreeError>(&status)) {
                    CHECK(failure->code == GameTreeErrorCode::LimitExceeded);
                    CHECK(snapshot(game) == before);
                } else {
                    CHECK(game.node(id)->annotations == notes);
                }
                break;
            }
            }
            std::vector<NodeId> queue{game.rootId()};
            std::set<NodeId> visited{game.rootId()};
            std::map<NodeId, Position> replay;
            replay.emplace(game.rootId(), Position::initial());
            std::size_t payload = 0;
            for (std::size_t index = 0; index < queue.size(); ++index) {
                REQUIRE(index < game.size());
                const auto node = game.node(queue[index]);
                REQUIRE(node);
                REQUIRE(node->id == queue[index]);
                CHECK(!deleted.contains(node->id));
                CHECK(node->annotations.commentBefore.size() <= limits.maxCommentBytes);
                CHECK(node->annotations.commentAfter.size() <= limits.maxCommentBytes);
                payload += node->annotations.commentBefore.size() +
                           node->annotations.commentAfter.size() + node->annotations.nags.size();
                CHECK(unwrap(game.position(node->id)).context() == replay.at(node->id).context());
                for (const auto childId : node->children) {
                    REQUIRE(visited.insert(childId).second); // No cycles or multiple parents.
                    const auto child = game.node(childId);
                    REQUIRE(child);
                    CHECK(child->parent == node->id);
                    REQUIRE(child->move);
                    CHECK(std::count_if(node->children.begin(), node->children.end(),
                                        [&](NodeId sibling) {
                                            const auto siblingNode = game.node(sibling);
                                            return siblingNode && siblingNode->move == child->move;
                                        }) == 1);
                    auto branch = replay.at(node->id);
                    CHECK(child->san == unwrap(branch.san(*child->move)));
                    unwrap(branch.play(*child->move));
                    CHECK(branch.context().moves.size() <= limits.maxDepth);
                    replay.emplace(childId, std::move(branch));
                    queue.push_back(childId);
                }
            }
            CHECK(visited.size() == game.size());
            CHECK(game.size() <= limits.maxNodes);
            CHECK(payload == game.annotationBytes());
            CHECK(payload <= limits.maxAnnotationBytes);
            const auto main = unwrap(game.mainLine(game.rootId()));
            REQUIRE(!main.empty());
            CHECK(main.front() == game.rootId());
            for (std::size_t index = 1; index < main.size(); ++index) {
                CHECK(game.node(main[index - 1])->children.front() == main[index]);
            }
            CHECK(game.node(main.back())->children.empty());
        }
    }
}

TEST_CASE("Tree transactions reject late overflow and node exhaustion atomically",
          "[chess][unit]") {
    auto game = unwrap(GameTree::fromFen("4k3/8/8/8/8/8/8/4K3 w - - 0 4294967295"));
    auto board = unwrap(game.position(game.rootId()));
    const auto white = unwrap(board.parseUci("e1d1"));
    unwrap(board.play(white));
    const auto black = unwrap(board.parseUci("e8d8"));
    const auto before = snapshot(game);
    const std::array line{white, black};
    const auto overflow =
        rejects(game.addLine(game.rootId(), line), GameTreeErrorCode::PositionFailure);
    CHECK(overflow.moveIndex == 1);
    REQUIRE(overflow.positionError);
    CHECK(overflow.positionError->code == ErrorCode::CounterOverflow);
    CHECK(snapshot(game) == before);
    CHECK(unwrap(game.addMove(game.rootId(), white)).value == 1);

    auto limits = GameTreeLimits{};
    limits.maxNodes = 3;
    auto bounded = tree(limits);
    const auto moves = movesFor(Position::initial(), {"e2e4", "e7e5", "g1f3"});
    const auto exhausted =
        rejects(bounded.addLine(bounded.rootId(), moves), GameTreeErrorCode::LimitExceeded);
    CHECK(exhausted.moveIndex == 2);
    CHECK(bounded.size() == 1);
    CHECK(unwrap(bounded.addMove(bounded.rootId(), moves.front())).value == 1);
    const auto existing = snapshot(bounded);
    const auto invalid = moves.front(); // White cannot move twice.
    const auto last = existing.back().id;
    rejects(bounded.addMove(last, invalid), GameTreeErrorCode::PositionFailure);
    rejects(bounded.position(NodeId{std::numeric_limits<std::uint64_t>::max()}),
            GameTreeErrorCode::InvalidNode);
    CHECK(snapshot(bounded) == existing);
}

TEST_CASE("Caller created malformed moves cannot enter or match a stored path", "[chess][unit]") {
    auto game = tree();
    const auto root = game.rootId();
    add(game, root, {"e2e4"});
    const auto before = snapshot(game);
    const auto ordinary = unwrap(Position::initial().parseUci("e2e4"));
    for (int representation = 0; representation < 256; ++representation) {
        CAPTURE(representation);
        auto malformed = ordinary;
        malformed.promotion = static_cast<PieceType>(representation);
        const auto failure =
            rejects(game.addMove(root, malformed), GameTreeErrorCode::PositionFailure);
        CHECK(failure.moveIndex == 0);
        REQUIRE(failure.positionError);
        CHECK(failure.positionError->code == ErrorCode::IllegalMove);
        CHECK(snapshot(game) == before);
    }
}

TEST_CASE("Bulk tree construction validates records, annotations and legal branches",
          "[chess][unit]") {
    const auto e4 = unwrap(Position::initial().parseSan("e4"));
    const auto d4 = unwrap(Position::initial().parseSan("d4"));
    auto afterE4 = Position::initial();
    unwrap(afterE4.play(e4));
    const auto e5 = unwrap(afterE4.parseSan("e5"));
    const std::vector<GameTreeInputNode> records{
        {0, std::nullopt, {"root", {}, {}}}, {0, e4, {}}, {0, d4, {}}, {1, e5, {{}, "reply", {1}}}};
    auto game = unwrap(GameTree::fromNodes(std::nullopt, records));
    CHECK(game.size() == 4);
    CHECK(game.node(NodeId{0})->children == std::vector<NodeId>{NodeId{1}, NodeId{2}});
    CHECK(game.node(NodeId{3})->san == "e5");
    CHECK(game.node(NodeId{3})->annotations.commentAfter == "reply");
    CHECK(unwrap(game.mainLine(game.rootId())) ==
          std::vector<NodeId>{NodeId{0}, NodeId{1}, NodeId{3}});
    CHECK(unwrap(game.addMove(NodeId{1}, unwrap(afterE4.parseSan("c5")))) == NodeId{4});
    auto invalid = records;
    invalid[2].parent = 2;
    rejects(GameTree::fromNodes(std::nullopt, invalid), GameTreeErrorCode::InvalidNode);
    invalid = records;
    invalid[0].move = e4;
    rejects(GameTree::fromNodes(std::nullopt, invalid), GameTreeErrorCode::InvalidNode);
    invalid = records;
    invalid[2].move = e4;
    rejects(GameTree::fromNodes(std::nullopt, invalid), GameTreeErrorCode::InvalidNode);
    invalid = records;
    invalid[3].move = e4;
    rejects(GameTree::fromNodes(std::nullopt, invalid), GameTreeErrorCode::PositionFailure);
    rejects(GameTree::fromNodes(std::nullopt, {}), GameTreeErrorCode::InvalidNode);
    auto limits = GameTreeLimits{};
    limits.maxNodes = 3;
    rejects(GameTree::fromNodes(std::nullopt, records, limits), GameTreeErrorCode::LimitExceeded);
    limits = {};
    limits.maxDepth = 1;
    rejects(GameTree::fromNodes(std::nullopt, records, limits), GameTreeErrorCode::LimitExceeded);
    limits = {};
    limits.maxAnnotationBytes = 8;
    rejects(GameTree::fromNodes(std::nullopt, records, limits), GameTreeErrorCode::LimitExceeded);
    std::stop_source stop;
    stop.request_stop();
    rejects(GameTree::fromNodes(std::nullopt, records, {}, stop.get_token()),
            GameTreeErrorCode::Cancelled);
    const std::array<GameTreeInputNode, 1> rootOnly{};
    const auto fen = "8/8/8/8/8/2k5/8/4K3 b - - 0 7";
    const auto fenTree = unwrap(GameTree::fromNodes(fen, rootOnly));
    CHECK(unwrap(fenTree.position(fenTree.rootId())).context() ==
          unwrap(Position::fromFen(fen)).context());
}
