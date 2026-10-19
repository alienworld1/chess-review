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

GameTreeResult<GameTree> GameTree::fromNodes(std::optional<std::string_view> rootFen,
                                             std::span<const GameTreeInputNode> input,
                                             GameTreeLimits limits, std::stop_token stop) {
    if (stop.stop_requested()) {
        return error(GameTreeErrorCode::Cancelled, "Tree construction cancelled.");
    }
    if (input.empty() || input.front().move || input.front().parent != 0) {
        return error(GameTreeErrorCode::InvalidNode, "Invalid root record.");
    }
    auto created = rootFen ? fromFen(*rootFen, limits) : initial(limits);
    if (std::holds_alternative<GameTreeError>(created)) {
        return created;
    }
    auto tree = std::get<GameTree>(std::move(created));
    if (input.size() > limits.maxNodes ||
        input.size() >= std::numeric_limits<std::uint64_t>::max()) {
        return error(GameTreeErrorCode::LimitExceeded, "Node budget exceeded.");
    }
    auto status = tree.setAnnotations(tree.rootId(), input.front().annotations);
    if (const auto* failure = std::get_if<GameTreeError>(&status)) {
        return *failure;
    }
    for (std::size_t index = 1; index < input.size(); ++index) {
        if (stop.stop_requested()) {
            return error(GameTreeErrorCode::Cancelled, "Tree construction cancelled.");
        }
        const auto& record = input[index];
        if (record.parent >= index || !record.move) {
            return error(GameTreeErrorCode::InvalidNode, "Invalid parent or move record.", index);
        }
        const NodeId parentId{static_cast<std::uint64_t>(record.parent)};
        auto& parent = tree.nodes_.at(parentId);
        if (parent.depth >= limits.maxDepth) {
            return error(GameTreeErrorCode::LimitExceeded, "Depth budget exceeded.", index);
        }
        const NodeId id{static_cast<std::uint64_t>(index)};
        tree.nodes_.emplace(
            id, StoredNode{GameTreeNode{id, parentId, record.move, std::nullopt, {}, {}},
                           parent.depth + 1});
        parent.data.children.push_back(id);
        status = tree.setAnnotations(id, record.annotations);
        if (const auto* failure = std::get_if<GameTreeError>(&status)) {
            return *failure;
        }
    }
    auto board = tree.root_;
    struct Frame {
        NodeId id;
        std::size_t child{};
    };
    std::vector<Frame> stack{{tree.rootId(), 0}};
    while (!stack.empty()) {
        if (stop.stop_requested()) {
            return error(GameTreeErrorCode::Cancelled, "Tree construction cancelled.");
        }
        auto& frame = stack.back();
        auto& node = tree.nodes_.at(frame.id).data;
        if (frame.child == node.children.size()) {
            if (node.parent) {
                (void)board.undo();
            }
            stack.pop_back();
            continue;
        }
        const auto childId = node.children[frame.child++];
        auto& child = tree.nodes_.at(childId).data;
        // Earlier siblings have already passed legality checks, bounding this search
        // by the number of legal moves rather than the number of untrusted records.
        for (std::size_t i = 0; i + 1 < frame.child; ++i) {
            if (tree.nodes_.at(node.children[i]).data.move == child.move) {
                return error(GameTreeErrorCode::InvalidNode, "Duplicate sibling move.",
                             static_cast<std::size_t>(childId.value));
            }
        }
        auto notation = board.san(*child.move);
        if (const auto* failure = std::get_if<Error>(&notation)) {
            return positionFailure(*failure, static_cast<std::size_t>(childId.value));
        }
        const auto played = board.play(*child.move);
        if (const auto* failure = std::get_if<Error>(&played)) {
            return positionFailure(*failure, static_cast<std::size_t>(childId.value));
        }
        child.san = std::get<std::string>(std::move(notation));
        stack.push_back({childId, 0});
    }
    tree.nextId_ = static_cast<std::uint64_t>(input.size());
    return tree;
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
