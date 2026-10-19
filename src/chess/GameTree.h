#pragma once

#include "chess/Position.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chess_review::chess {

// IDs belong to one tree (or its independent copies), not to a process-wide registry.
struct NodeId {
    explicit NodeId(std::uint64_t rawValue) : value(rawValue) {}
    std::uint64_t value;
    auto operator<=>(const NodeId&) const = default;
};

struct GameTreeLimits {
    std::size_t maxNodes{100'000}; // Includes the root.
    std::size_t maxDepth{16'384};  // Root depth is zero.
    std::size_t maxCommentBytes{64 * 1024};
    std::size_t maxAnnotationBytes{16 * 1024 * 1024};
};

struct NodeAnnotations {
    // Opaque bytes intended for UTF-8. No interpretation or normalization.
    std::string commentBefore;
    std::string commentAfter;
    std::vector<std::uint8_t> nags;
    bool operator==(const NodeAnnotations&) const = default;
};

struct GameTreeNode {
    NodeId id;
    std::optional<NodeId> parent;
    std::optional<Move> move;
    std::optional<std::string> san;
    std::vector<NodeId> children; // First child is the main continuation.
    NodeAnnotations annotations;
    bool operator==(const GameTreeNode&) const = default;
};

enum class GameTreeErrorCode {
    InvalidLimits,
    InvalidNode,
    RootOperation,
    PositionFailure,
    LimitExceeded,
    IdExhausted
};

struct GameTreeError {
    GameTreeErrorCode code;
    std::string message;                  // Bounded static diagnostic, without user content.
    std::optional<std::size_t> moveIndex; // Zero-based index for addLine failures.
    std::optional<Error> positionError;
};
template <typename T> using GameTreeResult = std::variant<T, GameTreeError>;
using GameTreeStatus = GameTreeResult<std::monostate>;

// Independent values with exclusive ownership; synchronize concurrent access externally.
// Moved-from trees support destruction and assignment only. No cursor, I/O, or Qt.
class GameTree {
  public:
    [[nodiscard]] static GameTreeResult<GameTree> initial(GameTreeLimits limits = {});
    [[nodiscard]] static GameTreeResult<GameTree> fromFen(std::string_view fen,
                                                          GameTreeLimits limits = {});
    GameTree(const GameTree&) = default;
    GameTree& operator=(const GameTree& other);
    GameTree(GameTree&&) noexcept = default;
    GameTree& operator=(GameTree&&) noexcept = default;
    ~GameTree() = default;

    [[nodiscard]] NodeId rootId() const { return NodeId{0}; }
    [[nodiscard]] std::size_t size() const { return nodes_.size(); }
    [[nodiscard]] std::size_t annotationBytes() const { return annotationBytes_; }
    [[nodiscard]] GameTreeLimits limits() const { return limits_; }
    // Snapshots own their data and remain valid after any edit or tree destruction.
    [[nodiscard]] std::optional<GameTreeNode> node(NodeId id) const;
    // Includes start, then repeatedly follows the first child.
    [[nodiscard]] GameTreeResult<std::vector<NodeId>> mainLine(NodeId start) const;
    [[nodiscard]] GameTreeResult<Position> position(NodeId id) const;

    // Edits are atomic on errors and allocation failure. Existing nodes are reused unchanged.
    [[nodiscard]] GameTreeResult<NodeId> addMove(NodeId parent, const Move& move);
    // Empty input returns start; all returned IDs refer to the final node of the line.
    [[nodiscard]] GameTreeResult<NodeId> addLine(NodeId start, std::span<const Move> moves);
    [[nodiscard]] GameTreeStatus promote(NodeId id);
    [[nodiscard]] GameTreeStatus removeSubtree(NodeId id);
    [[nodiscard]] GameTreeStatus setAnnotations(NodeId id, const NodeAnnotations& annotations);

  private:
    struct StoredNode {
        GameTreeNode data;
        std::size_t depth;
    };
    GameTree(Position root, GameTreeLimits limits);
    void swap(GameTree& other) noexcept;

    Position root_;
    GameTreeLimits limits_;
    std::map<NodeId, StoredNode> nodes_;
    std::optional<std::uint64_t> nextId_{1};
    std::size_t annotationBytes_{};
};

} // namespace chess_review::chess
