#include "chess/GameTree.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Isolated test executable: fail each ordinary allocation in turn, including allocations
// inside standard containers and Position. Never install this override in the application.
namespace {
thread_local std::optional<std::size_t> allocationsBeforeFailure;
}

void* operator new(std::size_t size) {
    if (allocationsBeforeFailure) {
        if (*allocationsBeforeFailure == 0) {
            throw std::bad_alloc{};
        }
        --*allocationsBeforeFailure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

using namespace chess_review::chess;

namespace {
struct FailAfter {
    explicit FailAfter(std::size_t count) { allocationsBeforeFailure = count; }
    ~FailAfter() { allocationsBeforeFailure.reset(); }
    FailAfter(const FailAfter&) = delete;
    FailAfter& operator=(const FailAfter&) = delete;
};
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
