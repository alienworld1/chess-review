#include "chess/GameTree.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>

namespace chess_review::chess {
namespace {
GameTreeError error(GameTreeErrorCode code, const char* message,
                    std::optional<std::size_t> index = std::nullopt) {
    return {code, message, index, std::nullopt};
}
GameTreeError positionFailure(const Error& cause, std::optional<std::size_t> index = std::nullopt) {
    return {GameTreeErrorCode::PositionFailure, "Position operation failed.", index, cause};
}
std::size_t payloadBytes(const NodeAnnotations& annotations) {
    // Only called on stored annotations, whose total was checked before admission.
    return annotations.commentBefore.size() + annotations.commentAfter.size() +
           annotations.nags.size();
}
} // namespace

GameTree::GameTree(Position root, GameTreeLimits limits) : root_(std::move(root)), limits_(limits) {
    nodes_.emplace(
        rootId(),
        StoredNode{GameTreeNode{rootId(), std::nullopt, std::nullopt, std::nullopt, {}, {}}, 0});
}

GameTreeResult<GameTree> GameTree::initial(GameTreeLimits limits) {
    if (limits.maxNodes == 0) {
        return error(GameTreeErrorCode::InvalidLimits, "Node budget must include the root.");
    }
    return GameTree{Position::initial(), limits};
}

GameTreeResult<GameTree> GameTree::fromFen(std::string_view fen, GameTreeLimits limits) {
    if (limits.maxNodes == 0) {
        return error(GameTreeErrorCode::InvalidLimits, "Node budget must include the root.");
    }
    auto root = Position::fromFen(fen);
    if (const auto* failure = std::get_if<Error>(&root)) {
        return positionFailure(*failure);
    }
    return GameTree{std::get<Position>(std::move(root)), limits};
}

void GameTree::swap(GameTree& other) noexcept {
    using std::swap;
    swap(root_, other.root_);
    swap(limits_, other.limits_);
    nodes_.swap(other.nodes_);
    swap(nextId_, other.nextId_);
    swap(annotationBytes_, other.annotationBytes_);
}

GameTree& GameTree::operator=(const GameTree& other) {
    if (this != &other) {
        GameTree copy{other};
        swap(copy);
    }
    return *this;
}

std::optional<GameTreeNode> GameTree::node(NodeId id) const {
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return std::nullopt;
    }
    return found->second.data;
}

GameTreeResult<std::vector<NodeId>> GameTree::mainLine(NodeId start) const {
    auto found = nodes_.find(start);
    if (found == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    std::vector<NodeId> line;
    while (true) {
        line.push_back(found->first);
        if (found->second.data.children.empty()) {
            return line;
        }
        found = nodes_.find(found->second.data.children.front());
    }
}

GameTreeResult<Position> GameTree::position(NodeId id) const {
    auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    std::vector<Move> path;
    path.reserve(found->second.depth);
    while (found->second.data.parent) {
        path.push_back(*found->second.data.move);
        found = nodes_.find(*found->second.data.parent);
    }
    auto result = root_;
    for (auto move = path.rbegin(); move != path.rend(); ++move) {
        const auto status = result.play(*move);
        if (const auto* failure = std::get_if<Error>(&status)) {
            return positionFailure(*failure);
        }
    }
    return result;
}

GameTreeResult<NodeId> GameTree::addMove(NodeId parent, const Move& move) {
    return addLine(parent, std::span<const Move>{&move, 1});
}

GameTreeResult<NodeId> GameTree::addLine(NodeId start, std::span<const Move> moves) {
    auto first = nodes_.find(start);
    if (first == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    // Existing links already encode validated legal moves and exact histories. Reusing
    // them needs no board reconstruction, SAN regeneration, or allocation.
    std::size_t prefix = 0;
    while (prefix < moves.size()) {
        const auto& children = first->second.data.children;
        const auto existing = std::find_if(children.begin(), children.end(), [&](NodeId child) {
            return nodes_.at(child).data.move == moves[prefix];
        });
        if (existing == children.end()) {
            break;
        }
        first = nodes_.find(*existing);
        ++prefix;
    }
    if (prefix == moves.size()) {
        return first->first;
    }
    auto resolved = position(first->first);
    if (const auto* failure = std::get_if<GameTreeError>(&resolved)) {
        return *failure;
    }
    auto board = std::get<Position>(std::move(resolved));
    std::map<NodeId, StoredNode> staged;
    auto nextId = nextId_;
    auto current = first->first;
    auto depth = first->second.depth;
    StoredNode* attachment = nullptr;
    std::vector<NodeId> attachmentChildren;
    for (std::size_t index = prefix; index < moves.size(); ++index) {
        const auto& move = moves[index];
        auto notation = board.san(move);
        if (const auto* failure = std::get_if<Error>(&notation)) {
            return positionFailure(*failure, index);
        }
        const auto status = board.play(move);
        if (const auto* failure = std::get_if<Error>(&status)) {
            return positionFailure(*failure, index);
        }
        auto& parent = staged.empty() ? nodes_.at(current) : staged.at(current);
        if (depth >= limits_.maxDepth) {
            return error(GameTreeErrorCode::LimitExceeded, "Depth budget exceeded.", index);
        }
        if (staged.size() >= limits_.maxNodes - nodes_.size()) {
            return error(GameTreeErrorCode::LimitExceeded, "Node budget exceeded.", index);
        }
        if (!nextId) {
            return error(GameTreeErrorCode::IdExhausted, "Node IDs exhausted.", index);
        }
        const NodeId id{*nextId};
        nextId = *nextId == std::numeric_limits<std::uint64_t>::max()
                     ? std::nullopt
                     : std::optional<std::uint64_t>{*nextId + 1};
        staged.emplace(
            id,
            StoredNode{
                GameTreeNode{id, current, move, std::get<std::string>(std::move(notation)), {}, {}},
                depth + 1});
        if (!attachment) {
            attachmentChildren = parent.data.children;
            attachmentChildren.push_back(id);
            attachment = &parent;
        } else {
            parent.data.children.push_back(id);
        }
        current = id;
        ++depth;
    }
    // Same allocators and nonthrowing NodeId ordering: merge transfers nodes without allocating.
    // No live links or counters are changed until all fallible preparation has succeeded.
    nodes_.merge(staged);
    if (attachment) {
        attachment->data.children.swap(attachmentChildren);
    }
    nextId_ = nextId;
    return current;
}

GameTreeStatus GameTree::promote(NodeId id) {
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    if (!found->second.data.parent) {
        return error(GameTreeErrorCode::RootOperation, "Cannot promote the root.");
    }
    auto& siblings = nodes_.at(*found->second.data.parent).data.children;
    const auto child = std::find(siblings.begin(), siblings.end(), id);
    std::rotate(siblings.begin(), child, child + 1);
    return std::monostate{};
}

GameTreeStatus GameTree::removeSubtree(NodeId id) {
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    if (!found->second.data.parent) {
        return error(GameTreeErrorCode::RootOperation, "Cannot delete the root.");
    }
    std::vector<NodeId> removed{id};
    for (std::size_t index = 0; index < removed.size(); ++index) {
        const auto& children = nodes_.at(removed[index]).data.children;
        removed.insert(removed.end(), children.begin(), children.end());
    }
    auto& siblings = nodes_.at(*found->second.data.parent).data.children;
    std::erase(siblings, id);
    for (const auto removedId : removed) {
        const auto entry = nodes_.find(removedId);
        annotationBytes_ -= payloadBytes(entry->second.data.annotations);
        nodes_.erase(entry);
    }
    return std::monostate{};
}

GameTreeStatus GameTree::setAnnotations(NodeId id, const NodeAnnotations& annotations) {
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return error(GameTreeErrorCode::InvalidNode, "Node does not exist.");
    }
    if (annotations.commentBefore.size() > limits_.maxCommentBytes ||
        annotations.commentAfter.size() > limits_.maxCommentBytes) {
        return error(GameTreeErrorCode::LimitExceeded, "Comment budget exceeded.");
    }
    const auto retained = annotationBytes_ - payloadBytes(found->second.data.annotations);
    auto available = limits_.maxAnnotationBytes - retained;
    for (const auto bytes : {annotations.commentBefore.size(), annotations.commentAfter.size(),
                             annotations.nags.size()}) {
        if (bytes > available) {
            return error(GameTreeErrorCode::LimitExceeded, "Annotation budget exceeded.");
        }
        available -= bytes;
    }
    auto replacement = annotations;
    using std::swap;
    swap(found->second.data.annotations, replacement);
    annotationBytes_ = limits_.maxAnnotationBytes - available;
    return std::monostate{};
}

} // namespace chess_review::chess
