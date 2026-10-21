#include "app/AnalysisController.h"
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <exception>

namespace chess_review::app {
struct AnalysisController::Session {
    std::optional<chess::GameTree> tree;
    chess::NodeId current{0};
    std::shared_ptr<const NavigationTree> navigation;
};
struct AnalysisController::Snapshot {
    BoardViewState board;
    std::shared_ptr<const NavigationTree> navigation;
    std::vector<HistoryEntry> history;
    std::vector<HistoryEntry> continuations;
    chess::NodeId current{0};
    bool hasMoves = false;
    bool sessionReset = false;
    QString error;
};
AnalysisController::AnalysisController(QObject* parent)
    : QObject(parent), session_(std::make_unique<Session>()) {
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(0);
}
AnalysisController::~AnalysisController() {
    Q_ASSERT_X(pool_.waitForDone(0), "AnalysisController", "Await closed() before destruction");
}
void AnalysisController::start() {
    if (history_.empty() && !busy_) {
        reset();
    }
}
void AnalysisController::submit(std::function<QString(Session&)> operation, bool resetSession,
                                std::optional<chess::Move> animatedMove) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_ || busy_) {
        return;
    }
    busy_ = true;
    board_.interactive = false;
    board_.selection.reset();
    const auto request = ++request_;
    pool_.start([this, operation = std::move(operation), request, resetSession, animatedMove] {
        Snapshot snapshot;
        try {
            snapshot.error = operation(*session_);
            snapshot.sessionReset = resetSession && snapshot.error.isEmpty();
            if (session_->tree) {
                auto& tree = *session_->tree;
                snapshot.current = session_->current;
                // Scratch edits only append/reuse nodes; reset explicitly clears this cache.
                if (!session_->navigation || session_->navigation->nodes.size() != tree.size()) {
                    session_->navigation = makeNavigationTree(tree);
                }
                snapshot.navigation = session_->navigation;
                const auto node = tree.node(session_->current);
                auto resolved = tree.position(session_->current);
                if (node && std::holds_alternative<chess::Position>(resolved)) {
                    const auto& position = std::get<chess::Position>(resolved);
                    snapshot.board.sideToMove = position.sideToMove();
                    snapshot.board.legalMoves = position.legalMoves();
                    const bool inCheck = position.inCheck();
                    const bool noMoves = snapshot.board.legalMoves.empty();
                    const auto side =
                        position.sideToMove() == chess::Color::White ? tr("White") : tr("Black");
                    if (noMoves && inCheck) {
                        snapshot.board.status = BoardStatus::Checkmate;
                        snapshot.board.positionDescription =
                            tr("Checkmate — %1 wins")
                                .arg(position.sideToMove() == chess::Color::White ? tr("Black")
                                                                                  : tr("White"));
                    } else if (noMoves) {
                        snapshot.board.status = BoardStatus::Stalemate;
                        snapshot.board.positionDescription = tr("Stalemate — draw");
                    } else if (inCheck) {
                        snapshot.board.status = BoardStatus::Check;
                        snapshot.board.positionDescription = tr("%1 to move — check").arg(side);
                    } else {
                        snapshot.board.positionDescription = tr("%1 to move").arg(side);
                    }
                    for (int i = 0; i < 64; ++i) {
                        const auto square = *chess::Square::fromCoordinates(i % 8, i / 8);
                        const auto piece = position.pieceAt(square);
                        snapshot.board.pieces[static_cast<std::size_t>(i)] = piece;
                        if (piece && piece->type == chess::PieceType::King &&
                            piece->color == position.sideToMove() && inCheck) {
                            snapshot.board.checkedKing = square;
                        }
                    }
                    for (const auto& move : snapshot.board.legalMoves) {
                        // En passant is also a capture although the destination is empty.
                        const auto piece = position.pieceAt(move.from);
                        if (position.pieceAt(move.to) ||
                            (piece && piece->type == chess::PieceType::Pawn &&
                             move.from.file() != move.to.file())) {
                            snapshot.board.captureMoves.push_back(move);
                        }
                    }
                    snapshot.board.lastMove = node->move;
                    snapshot.hasMoves = tree.size() > 1;
                    auto cursor = node;
                    while (cursor) {
                        snapshot.history.push_back(
                            {cursor->id, session_->navigation->nodes.at(cursor->id).label});
                        cursor = cursor->parent ? tree.node(*cursor->parent) : std::nullopt;
                    }
                    std::reverse(snapshot.history.begin(), snapshot.history.end());
                    for (const auto child : node->children) {
                        snapshot.continuations.push_back(
                            {child, session_->navigation->nodes.at(child).label});
                    }
                    snapshot.board.interactive = true;
                } else {
                    snapshot.error = tr("The analysis position could not be resolved.");
                }
            }
        } catch (const std::exception&) {
            // An edit may have completed before snapshot allocation failed. Preserve
            // the discard guard even when presenting the result must fail closed.
            snapshot.hasMoves = session_->tree && session_->tree->size() > 1;
            snapshot.current = session_->current;
            snapshot.error = tr("Analysis could not complete. The position was not displayed.");
            snapshot.board.interactive = false;
        }
        QMetaObject::invokeMethod(
            this,
            [this, snapshot = std::move(snapshot), request, animatedMove]() mutable {
                publish(std::move(snapshot), request, animatedMove);
            },
            Qt::QueuedConnection);
    });
    // Admission is complete before exposing callbacks that may request shutdown.
    emit changed();
}
void AnalysisController::publish(Snapshot snapshot, std::uint64_t request,
                                 std::optional<chess::Move> animatedMove) {
    if (closing_ || request != request_) {
        return;
    }
    busy_ = false;
    navigating_ = false;
    resetting_ = false;
    if (snapshot.sessionReset) {
        chosen_.clear();
        navigationTree_.reset();
        chosenLine_.clear();
        pendingNavigation_.reset();
        annotations_.clear();
        annotationHistory_.clear();
    }
    if (pendingReset_) {
        hasMoves_ = snapshot.hasMoves;
        auto fen = std::move(pendingReset_->fen);
        pendingReset_.reset();
        startReset(std::move(fen));
        return;
    }
    if (snapshot.navigation) {
        navigationTree_ = std::move(snapshot.navigation);
    }
    hasMoves_ = snapshot.hasMoves;
    if (!snapshot.error.isEmpty()) {
        pendingNavigation_.reset();
    }
    if (animatedMove && snapshot.error.isEmpty() && navigationTree_ && pendingMoveChoice_) {
        // Ancestor navigation preserves choices. Only the move's own edge is new;
        // an explicit choice of that edge during validation has already won.
        chosen_.insert_or_assign(*pendingMoveChoice_, snapshot.current);
        updateChosenLine();
    }
    pendingMoveChoice_.reset();
    if (pendingNavigation_ && *pendingNavigation_ != snapshot.current) {
        // A newer intent owns the display. Only topology from this result is retained.
        dispatchNavigation();
        return;
    }
    if (pendingNavigation_) {
        animatedMove.reset();
    }
    pendingNavigation_.reset();
    if (navigationTree_ && !navigationTree_->nodes.contains(snapshot.current)) {
        navigationTree_.reset();
        chosenLine_.clear();
        chosen_.clear();
    }
    requested_ = snapshot.current;
    if (navigationTree_) {
        rememberPath(requested_);
        updateChosenLine();
    }
    const bool orientation = board_.blackAtBottom;
    board_ = std::move(snapshot.board);
    board_.blackAtBottom = orientation;
    board_.revision = request;
    current_ = snapshot.current;
    hasMoves_ = snapshot.hasMoves;
    history_ = std::move(snapshot.history);
    continuations_ = std::move(snapshot.continuations);
    if (const auto found = annotations_.find(current_); found != annotations_.end()) {
        board_.annotations = found->second;
    }
    // Context-owned queued delivery survives reentrant changed() handlers safely:
    // deletion cancels the callback; close or another request invalidates it.
    QMetaObject::invokeMethod(
        this,
        [this, request, error = std::move(snapshot.error), animatedMove] {
            if (closing_ || request != request_) {
                return;
            }
            if (!error.isEmpty()) {
                emit failed(error);
            } else if (animatedMove) {
                emit moveAccepted(*animatedMove);
            }
        },
        Qt::QueuedConnection);
    emit changed();
}
void AnalysisController::reset(const std::optional<std::string>& fen) {
    if (closing_) {
        return;
    }
    ++sessionGeneration_;
    pendingNavigation_.reset();
    pendingMoveChoice_.reset();
    requested_ = current_;
    if (busy_) {
        pendingReset_ = ResetRequest{fen};
        resetting_ = true;
        emit changed();
        return;
    }
    startReset(fen);
}
void AnalysisController::startReset(std::optional<std::string> fen) {
    resetting_ = true;
    submit(
        [fen = std::move(fen)](Session& session) {
            auto result = fen ? chess::GameTree::fromFen(*fen) : chess::GameTree::initial();
            if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.tree = std::move(std::get<chess::GameTree>(result));
            session.current = chess::NodeId{0};
            session.navigation.reset();
            return QString{};
        },
        true);
}
void AnalysisController::selectSquare(chess::Square square) {
    if (!board_.interactive || closing_ || board_.legalMoves.empty()) {
        return;
    }
    const auto piece = board_.pieces[static_cast<std::size_t>(square.index())];
    board_.selection =
        piece && piece->color == board_.sideToMove ? std::optional{square} : std::nullopt;
    emit changed();
}
void AnalysisController::clearSelection() {
    board_.selection.reset();
    emit changed();
}
void AnalysisController::attemptMove(chess::Move move, std::uint64_t revision) {
    if (!board_.interactive || revision != board_.revision) {
        return;
    }
    pendingMoveChoice_ = current_;
    submit(
        [move](Session& session) {
            if (!session.tree) {
                return tr("Analysis is unavailable.");
            }
            const auto from = session.current;
            auto result = session.tree->addMove(from, move);
            if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.current = std::get<chess::NodeId>(result);
            return QString{};
        },
        false, move);
}
void AnalysisController::rememberPath(chess::NodeId node) {
    while (navigationTree_->nodes.at(node).parent) {
        const auto parent = *navigationTree_->nodes.at(node).parent;
        if (pendingMoveChoice_ == parent) {
            pendingMoveChoice_.reset();
        }
        chosen_.insert_or_assign(parent, node);
        node = parent;
    }
}
void AnalysisController::updateChosenLine() {
    std::vector<chess::NodeId> line;
    auto cursor = chess::NodeId{0};
    for (;;) {
        line.push_back(cursor);
        const auto& children = navigationTree_->nodes.at(cursor).children;
        if (children.empty()) {
            break;
        }
        const auto found = chosen_.find(cursor);
        cursor = found == chosen_.end() ? children.front() : found->second;
    }
    chosenLine_ = std::move(line);
}
void AnalysisController::navigate(chess::NodeId node) { navigate(target(node)); }
void AnalysisController::navigate(NavigationTarget target) {
    if (!canNavigate() || target.session != sessionGeneration_ ||
        !navigationTree_->nodes.contains(target.node)) {
        return;
    }
    if (target.node == requested_ && (!busy_ || navigating_ || pendingNavigation_)) {
        return;
    }
    requested_ = target.node;
    rememberPath(requested_);
    updateChosenLine();
    pendingNavigation_ = requested_;
    if (!busy_) {
        dispatchNavigation();
    } else {
        emit changed();
    }
}
void AnalysisController::dispatchNavigation() {
    const auto node = *pendingNavigation_;
    pendingNavigation_.reset();
    navigating_ = true;
    submit([node](Session& session) {
        if (!session.tree || !session.tree->node(node)) {
            return tr("Move is unavailable.");
        }
        session.current = node;
        return QString{};
    });
}
bool AnalysisController::canPrevious() const {
    return canNavigate() && navigationTree_->nodes.at(requested_).parent.has_value();
}
bool AnalysisController::canNext() const {
    return canNavigate() && !navigationTree_->nodes.at(requested_).children.empty();
}
void AnalysisController::previous() {
    if (canPrevious()) {
        navigate(*navigationTree_->nodes.at(requested_).parent);
    }
}
void AnalysisController::beginning() {
    if (canNavigate()) {
        navigate(chess::NodeId{0});
    }
}
void AnalysisController::next() {
    if (canNext()) {
        const auto& children = navigationTree_->nodes.at(requested_).children;
        const auto chosen = chosen_.find(requested_);
        navigate(chosen == chosen_.end() ? children.front() : chosen->second);
    }
}
void AnalysisController::end() {
    if (canNavigate() && !chosenLine_.empty()) {
        navigate(chosenLine_.back());
    }
}
void AnalysisController::setOrientation(bool blackAtBottom) {
    if (closing_ || board_.blackAtBottom == blackAtBottom) {
        return;
    }
    board_.blackAtBottom = blackAtBottom;
    board_.selection.reset();
    emit changed();
}
void AnalysisController::flip() { setOrientation(!board_.blackAtBottom); }
void AnalysisController::replaceAnnotations(std::vector<BoardAnnotation> annotations) {
    auto& history = annotationHistory_[current_];
    history.undo.push_back(board_.annotations);
    if (history.undo.size() > 64) {
        history.undo.erase(history.undo.begin());
    }
    history.redo.clear();
    if (annotations.empty()) {
        annotations_.erase(current_);
    } else {
        annotations_[current_] = annotations;
    }
    board_.annotations = std::move(annotations);
    emit changed();
}
void AnalysisController::toggleAnnotation(BoardAnnotation annotation) {
    if (!board_.interactive || closing_ || annotation.color < 0 || annotation.color > 3) {
        return;
    }
    auto annotations = board_.annotations;
    const auto found = std::find(annotations.begin(), annotations.end(), annotation);
    if (found != annotations.end()) {
        annotations.erase(found);
    } else if (annotations.size() < 256) {
        annotations.push_back(annotation);
    } else {
        emit failed(tr("Clear some annotations before adding more."));
        return;
    }
    replaceAnnotations(std::move(annotations));
}
void AnalysisController::clearAnnotations() {
    if (board_.interactive && !closing_ && !board_.annotations.empty()) {
        replaceAnnotations({});
    }
}
bool AnalysisController::canUndoAnnotation() const {
    const auto found = annotationHistory_.find(current_);
    return found != annotationHistory_.end() && !found->second.undo.empty();
}
bool AnalysisController::canRedoAnnotation() const {
    const auto found = annotationHistory_.find(current_);
    return found != annotationHistory_.end() && !found->second.redo.empty();
}
void AnalysisController::undoAnnotation() {
    if (!board_.interactive || closing_ || !canUndoAnnotation()) {
        return;
    }
    auto& history = annotationHistory_[current_];
    history.redo.push_back(board_.annotations);
    board_.annotations = std::move(history.undo.back());
    history.undo.pop_back();
    if (board_.annotations.empty()) {
        annotations_.erase(current_);
    } else {
        annotations_[current_] = board_.annotations;
    }
    emit changed();
}
void AnalysisController::redoAnnotation() {
    if (!board_.interactive || closing_ || !canRedoAnnotation()) {
        return;
    }
    auto& history = annotationHistory_[current_];
    history.undo.push_back(board_.annotations);
    board_.annotations = std::move(history.redo.back());
    history.redo.pop_back();
    if (board_.annotations.empty()) {
        annotations_.erase(current_);
    } else {
        annotations_[current_] = board_.annotations;
    }
    emit changed();
}
void AnalysisController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    pendingNavigation_.reset();
    pendingMoveChoice_.reset();
    pendingReset_.reset();
    ++sessionGeneration_;
    ++request_;
    board_.interactive = false;
    QTimer::singleShot(0, this, &AnalysisController::finishClose);
    emit changed();
}
void AnalysisController::finishClose() {
    if (!pool_.waitForDone(0)) {
        QTimer::singleShot(5, this, &AnalysisController::finishClose);
        return;
    }
    emit closed();
}
} // namespace chess_review::app
