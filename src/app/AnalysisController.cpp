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
    std::map<chess::NodeId, chess::NodeId> chosen;
};
struct AnalysisController::Snapshot {
    BoardViewState board;
    std::vector<HistoryEntry> history;
    std::vector<HistoryEntry> continuations;
    chess::NodeId current{0};
    std::optional<chess::NodeId> parent;
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
                const auto node = tree.node(session_->current);
                auto resolved = tree.position(session_->current);
                if (node && std::holds_alternative<chess::Position>(resolved)) {
                    const auto& position = std::get<chess::Position>(resolved);
                    snapshot.board.sideToMove = position.sideToMove();
                    snapshot.board.legalMoves = position.legalMoves();
                    for (int i = 0; i < 64; ++i) {
                        const auto square = *chess::Square::fromCoordinates(i % 8, i / 8);
                        const auto piece = position.pieceAt(square);
                        snapshot.board.pieces[static_cast<std::size_t>(i)] = piece;
                        if (piece && piece->type == chess::PieceType::King &&
                            piece->color == position.sideToMove() && position.inCheck()) {
                            snapshot.board.checkedKing = square;
                        }
                    }
                    for (const auto& move : snapshot.board.legalMoves) {
                        // En passant is also a capture although the destination is empty.
                        const auto piece = position.pieceAt(move.from);
                        if (position.pieceAt(move.to) ||
                            (piece && piece->type == chess::PieceType::Pawn &&
                             move.from.file() != move.to.file())) {
                            snapshot.board.captureTargets.push_back(move.to);
                        }
                    }
                    snapshot.board.lastMove = node->move;
                    snapshot.parent = node->parent;
                    snapshot.hasMoves = tree.size() > 1;
                    auto cursor = node;
                    while (cursor) {
                        snapshot.history.push_back(
                            {cursor->id, cursor->san ? QString::fromStdString(*cursor->san)
                                                     : tr("Initial position")});
                        cursor = cursor->parent ? tree.node(*cursor->parent) : std::nullopt;
                    }
                    std::reverse(snapshot.history.begin(), snapshot.history.end());
                    for (auto child : node->children) {
                        const auto continuation = tree.node(child);
                        snapshot.continuations.push_back(
                            {child, QString::fromStdString(*continuation->san)});
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
    if (snapshot.sessionReset) {
        annotations_.clear();
        annotationHistory_.clear();
    }
    const bool orientation = board_.blackAtBottom;
    board_ = std::move(snapshot.board);
    board_.blackAtBottom = orientation;
    board_.revision = request;
    current_ = snapshot.current;
    parent_ = snapshot.parent;
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
    submit(
        [fen](Session& session) {
            auto result = fen ? chess::GameTree::fromFen(*fen) : chess::GameTree::initial();
            if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.tree = std::move(std::get<chess::GameTree>(result));
            session.current = chess::NodeId{0};
            session.chosen.clear();
            return QString{};
        },
        true);
}
void AnalysisController::selectSquare(chess::Square square) {
    if (!board_.interactive || closing_) {
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
            session.chosen.insert_or_assign(from, session.current);
            return QString{};
        },
        false, move);
}
void AnalysisController::navigate(chess::NodeId node) {
    submit([node](Session& session) {
        if (!session.tree || !session.tree->node(node)) {
            return tr("Move is unavailable.");
        }
        const auto target = session.tree->node(node);
        if (target->parent) {
            session.chosen.insert_or_assign(*target->parent, node);
        }
        session.current = node;
        return QString{};
    });
}
void AnalysisController::previous() {
    if (parent_) {
        navigate(*parent_);
    }
}
void AnalysisController::beginning() { navigate(chess::NodeId{0}); }
void AnalysisController::next() {
    submit([](Session& session) {
        if (!session.tree) {
            return QString{};
        }
        const auto node = session.tree->node(session.current);
        if (node->children.empty()) {
            return QString{};
        }
        const auto found = session.chosen.find(session.current);
        session.current = found != session.chosen.end() ? found->second : node->children.front();
        return QString{};
    });
}
void AnalysisController::end() {
    submit([](Session& session) {
        if (!session.tree) {
            return QString{};
        }
        for (;;) {
            const auto node = session.tree->node(session.current);
            if (node->children.empty()) {
                break;
            }
            const auto found = session.chosen.find(session.current);
            session.current =
                found != session.chosen.end() ? found->second : node->children.front();
        }
        return QString{};
    });
}
void AnalysisController::flip() {
    if (closing_) {
        return;
    }
    board_.blackAtBottom = !board_.blackAtBottom;
    board_.selection.reset();
    emit changed();
}
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
