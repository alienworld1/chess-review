#include "app/AnalysisController.h"
#include "app/GamePgn.h"
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <exception>
#include <iterator>
#include <utility>

namespace chess_review::app {
namespace {
constexpr std::size_t maximumGameTabs = 8;
}
BoardViewState makeAnalysisBoard(const chess::Position& position,
                                 std::optional<chess::Move> lastMove, bool allowMoves) {
    BoardViewState board;
    board.sideToMove = position.sideToMove();
    auto legal = position.legalMoves();
    const bool inCheck = position.inCheck();
    const bool noMoves = legal.empty();
    const auto side = position.sideToMove() == chess::Color::White
                          ? AnalysisController::tr("White")
                          : AnalysisController::tr("Black");
    if (noMoves && inCheck) {
        board.status = BoardStatus::Checkmate;
        board.positionDescription = AnalysisController::tr("Checkmate — %1 wins")
                                        .arg(position.sideToMove() == chess::Color::White
                                                 ? AnalysisController::tr("Black")
                                                 : AnalysisController::tr("White"));
    } else if (noMoves) {
        board.status = BoardStatus::Stalemate;
        board.positionDescription = AnalysisController::tr("Stalemate — draw");
    } else if (inCheck) {
        board.status = BoardStatus::Check;
        board.positionDescription = AnalysisController::tr("%1 to move — check").arg(side);
    } else {
        board.positionDescription = AnalysisController::tr("%1 to move").arg(side);
    }
    for (int i = 0; i < 64; ++i) {
        const auto square = *chess::Square::fromCoordinates(i % 8, i / 8);
        const auto piece = position.pieceAt(square);
        board.pieces[static_cast<std::size_t>(i)] = piece;
        if (piece && piece->type == chess::PieceType::King &&
            piece->color == position.sideToMove() && inCheck) {
            board.checkedKing = square;
        }
    }
    if (allowMoves) {
        board.legalMoves = std::move(legal);
    }
    for (const auto& move : board.legalMoves) {
        // En passant is also a capture although the destination is empty.
        const auto piece = position.pieceAt(move.from);
        if (position.pieceAt(move.to) || (piece && piece->type == chess::PieceType::Pawn &&
                                          move.from.file() != move.to.file())) {
            board.captureMoves.push_back(move);
        }
    }
    board.lastMove = lastMove;
    board.interactive = allowMoves;
    return board;
}
struct AnalysisController::Session {
    persistence::GameId tab = 0;
    std::optional<chess::GameTree> tree;
    chess::NodeId current{0};
    std::shared_ptr<const NavigationTree> navigation;
    bool treeDirty = false;
    std::shared_ptr<const pgn::PgnDocument> source;
    QString sourceFile;
    std::optional<persistence::GameId> gameId;
    QString white;
    QString black;
    QString description;
};
struct AnalysisController::Snapshot {
    BoardViewState board;
    std::shared_ptr<const chess::Position> position;
    std::shared_ptr<const NavigationTree> navigation;
    std::vector<HistoryEntry> history;
    std::vector<HistoryEntry> continuations;
    chess::NodeId current{0};
    bool treeDirty = false;
    AnalysisNotes editableNotes;
    std::vector<BoardAnnotation> originalAnnotations;
    bool sessionReset = false;
    persistence::GameId tab = 0;
    bool previousTabClosed = false;
    QString error;
    QString notes;
    std::optional<persistence::GameId> gameId;
    QString white;
    QString black;
    QString description;
};
AnalysisController::AnalysisController(QObject* parent)
    : QObject(parent), session_(std::make_unique<Session>()) {
    pool_.setMaxThreadCount(1);
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
                                std::optional<chess::Move> animatedMove, bool changesPosition,
                                bool announceGame) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_ || busy_) {
        return;
    }
    busy_ = true;
    changingPosition_ = changesPosition;
    board_.interactive = false;
    board_.selection.reset();
    const auto request = ++request_;
    pool_.start([this, operation = std::move(operation), request, resetSession, animatedMove,
                 announceGame] {
        Snapshot snapshot;
        const auto previousTab = session_->tab;
        try {
            snapshot.error = operation(*session_);
            snapshot.sessionReset = resetSession && snapshot.error.isEmpty();
            if (session_->tree) {
                snapshot.gameId = session_->gameId;
                snapshot.white = session_->white;
                snapshot.black = session_->black;
                snapshot.description = session_->description;
                auto& tree = *session_->tree;
                snapshot.current = session_->current;
                if (!session_->navigation || session_->navigation->nodes.size() != tree.size()) {
                    session_->navigation = makeNavigationTree(tree);
                }
                snapshot.navigation = session_->navigation;
                const auto node = tree.node(session_->current);
                auto resolved = tree.position(session_->current);
                if (node && std::holds_alternative<chess::Position>(resolved)) {
                    const auto& position = std::get<chess::Position>(resolved);
                    snapshot.position = std::make_shared<const chess::Position>(position);
                    snapshot.board = makeAnalysisBoard(position, node->move, true);
                    snapshot.treeDirty = session_->treeDirty;
                    snapshot.editableNotes = {analysisComment(node->annotations.commentBefore,
                                                              &snapshot.originalAnnotations),
                                              analysisComment(node->annotations.commentAfter,
                                                              &snapshot.originalAnnotations),
                                              node->annotations.nags};
                    snapshot.notes = session_->navigation->nodes.at(session_->current).notes;
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
                } else {
                    snapshot.error = tr("The analysis position could not be resolved.");
                }
            }
        } catch (const std::exception&) {
            // An edit may have completed before snapshot allocation failed. Preserve
            // the discard guard even when presenting the result must fail closed.
            snapshot.treeDirty = session_->treeDirty;
            snapshot.current = session_->current;
            snapshot.error = tr("Analysis could not complete. The position was not displayed.");
            snapshot.board.interactive = false;
            snapshot.position.reset();
        }
        snapshot.tab = session_->tab;
        snapshot.previousTabClosed =
            previousTab != snapshot.tab && !suspendedSessions_.contains(previousTab);
        const bool opened = announceGame && snapshot.sessionReset && snapshot.gameId.has_value();
        QMetaObject::invokeMethod(
            this,
            [this, snapshot = std::move(snapshot), request, animatedMove, opened]() mutable {
                publish(std::move(snapshot), request, animatedMove, opened);
            },
            Qt::QueuedConnection);
    });
    // Admission is complete before exposing callbacks that may request shutdown.
    emit changed();
}
void AnalysisController::publish(Snapshot snapshot, std::uint64_t request,
                                 std::optional<chess::Move> animatedMove, bool opened) {
    if (closing_ || request != request_) {
        return;
    }
    error_ = snapshot.error;
    if (exporting_) {
        exporting_ = false;
        if (snapshot.error.isEmpty()) {
            exportedAnnotations_ = annotations_;
            QTimer::singleShot(0, this, &AnalysisController::pgnExported);
        }
    }
    const bool wasNavigating = navigating_;
    const bool changedPosition = changingPosition_ || snapshot.current != current_;
    changingPosition_ = false;
    busy_ = false;
    navigating_ = false;
    resetting_ = false;
    switchingTabs_ = false;
    const bool tabChanged = snapshot.tab != activeTab_;
    if (tabChanged || snapshot.sessionReset) {
        if (tabChanged && !snapshot.previousTabClosed) {
            const bool dirty = hasContent();
            suspendedViews_.insert_or_assign(
                activeTab_, TabView{tabTitle(), dirty, board_.blackAtBottom, std::move(chosen_),
                                    std::move(annotations_), std::move(exportedAnnotations_),
                                    std::move(annotationHistory_)});
        }
        chosen_.clear();
        navigationTree_.reset();
        chosenLine_.clear();
        pendingNavigation_.reset();
        annotations_.clear();
        exportedAnnotations_.clear();
        annotationHistory_.clear();
        if (tabChanged) {
            restoreTabView(snapshot);
        }
    }
    if (pendingReset_) {
        treeDirty_ = snapshot.treeDirty;
        auto reset = std::move(*pendingReset_);
        pendingReset_.reset();
        startReset(std::move(reset));
        return;
    }
    if (snapshot.navigation) {
        if (navigationTree_ && snapshot.navigation->nodes.size() < navigationTree_->nodes.size()) {
            const auto& nodes = snapshot.navigation->nodes;
            std::erase_if(chosen_, [&nodes](const auto& edge) {
                return !nodes.contains(edge.first) || !nodes.contains(edge.second);
            });
            std::erase_if(annotations_,
                          [&nodes](const auto& item) { return !nodes.contains(item.first); });
            std::erase_if(annotationHistory_,
                          [&nodes](const auto& item) { return !nodes.contains(item.first); });
        }
        navigationTree_ = std::move(snapshot.navigation);
    }
    treeDirty_ = snapshot.treeDirty;
    if (!snapshot.error.isEmpty() ||
        (pendingNavigation_ &&
         (!navigationTree_ || !navigationTree_->nodes.contains(*pendingNavigation_)))) {
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
        if (changedPosition && !tabChanged) {
            rememberPath(requested_);
        }
        updateChosenLine();
    }
    std::optional<chess::Move> navigationMove;
    if (wasNavigating && !snapshot.sessionReset && snapshot.error.isEmpty() && navigationTree_) {
        const auto& destination = navigationTree_->nodes.at(snapshot.current);
        if (destination.parent == current_) {
            navigationMove = snapshot.board.lastMove;
        } else if (const auto previous = navigationTree_->nodes.find(current_);
                   previous != navigationTree_->nodes.end() &&
                   previous->second.parent == snapshot.current && board_.lastMove) {
            const auto& move = *board_.lastMove;
            navigationMove = chess::Move{move.to, move.from, std::nullopt};
        }
    }
    const bool orientation = board_.blackAtBottom;
    gameId_ = snapshot.gameId;
    whitePlayer_ = std::move(snapshot.white);
    blackPlayer_ = std::move(snapshot.black);
    gameDescription_ = std::move(snapshot.description);
    positionNotes_ = std::move(snapshot.notes);
    notes_ = std::move(snapshot.editableNotes);
    originalAnnotations_ = std::move(snapshot.originalAnnotations);
    position_ = std::move(snapshot.position);
    const auto oldRevision = board_.revision;
    board_ = std::move(snapshot.board);
    board_.blackAtBottom = orientation;
    board_.revision = changedPosition ? request : oldRevision;
    current_ = snapshot.current;
    treeDirty_ = snapshot.treeDirty;
    history_ = std::move(snapshot.history);
    continuations_ = std::move(snapshot.continuations);
    if (const auto found = annotations_.find(current_); found != annotations_.end()) {
        board_.annotations = found->second;
    } else {
        board_.annotations = originalAnnotations_;
    }
    // Context-owned queued delivery survives reentrant changed() handlers safely:
    // deletion cancels the callback; close or another request invalidates it.
    QMetaObject::invokeMethod(
        this,
        [this, request, error = std::move(snapshot.error), animatedMove, navigationMove, opened] {
            if (closing_ || request != request_) {
                return;
            }
            if (!error.isEmpty()) {
                emit failed(error);
            } else if (opened) {
                emit gameOpened();
            } else if (animatedMove) {
                emit moveAccepted(*animatedMove);
            } else if (navigationMove) {
                emit navigationAnimationRequested(*navigationMove);
            }
        },
        Qt::QueuedConnection);
    if (deferredGame_) {
        QTimer::singleShot(0, this, [this] {
            if (!closing_ && !busy_ && deferredGame_) {
                openGameTab(*std::exchange(deferredGame_, std::nullopt));
            }
        });
    }
    emit changed();
}
QString AnalysisController::tabTitle() const {
    if (activeTab_ == 0) {
        return tr("Scratch");
    }
    const auto white = whitePlayer_.simplified();
    const auto black = blackPlayer_.simplified();
    return tr("%1 vs %2")
        .arg(white.isEmpty() ? tr("White") : white, black.isEmpty() ? tr("Black") : black);
}
std::vector<AnalysisTab> AnalysisController::tabs() const {
    std::vector<AnalysisTab> result;
    result.reserve(gameTabs_.size() + 1);
    const auto append = [this, &result](persistence::GameId id) {
        if (id == activeTab_) {
            result.push_back({id, tabTitle(), hasContent()});
        } else {
            const auto& view = suspendedViews_.at(id);
            result.push_back({id, view.title, view.dirty});
        }
    };
    append(0);
    for (const auto id : gameTabs_) {
        append(id);
    }
    return result;
}
void AnalysisController::restoreTabView(const Snapshot& snapshot) {
    if (snapshot.previousTabClosed) {
        std::erase(gameTabs_, activeTab_);
    }
    activeTab_ = snapshot.tab;
    if (auto cached = suspendedViews_.extract(activeTab_); !cached.empty()) {
        auto& view = cached.mapped();
        board_.blackAtBottom = view.blackAtBottom;
        chosen_ = std::move(view.chosen);
        annotations_ = std::move(view.annotations);
        exportedAnnotations_ = std::move(view.exportedAnnotations);
        annotationHistory_ = std::move(view.annotationHistory);
    }
    if (activeTab_ > 0 && !hasGameTab(activeTab_)) {
        gameTabs_.push_back(activeTab_);
    }
}
bool AnalysisController::hasGameTab(persistence::GameId id) const {
    return std::ranges::find(gameTabs_, id) != gameTabs_.end();
}
void AnalysisController::openGameTab(const persistence::StoredGame& game) {
    if (closing_) {
        return;
    }
    const auto id = game.summary.id;
    if (!game.document || id <= 0) {
        emit failed(tr("The stored game is unavailable."));
        return;
    }
    if (busy_) {
        deferredGame_ = game;
        return;
    }
    if (activeTab_ == id) {
        emit gameOpened();
        return;
    }
    if (gameTabs_.size() >= maximumGameTabs && !hasGameTab(id)) {
        emit failed(tr("Close a game tab before opening another. Up to %1 can be open.")
                        .arg(maximumGameTabs));
        return;
    }
    ResetRequest reset{std::nullopt, game};
    reset.tab = id;
    requestReset(std::move(reset));
}
void AnalysisController::selectTab(persistence::GameId id) {
    if (busy_ || closing_ || activeTab_ == id || (id != 0 && !hasGameTab(id))) {
        return;
    }
    ResetRequest reset{std::nullopt, std::nullopt};
    reset.tab = id;
    requestReset(std::move(reset));
}
void AnalysisController::closeActiveGameTab() {
    if (busy_ || closing_ || activeTab_ == 0) {
        return;
    }
    const auto current = std::ranges::find(gameTabs_, activeTab_);
    Q_ASSERT(current != gameTabs_.end());
    ResetRequest reset{std::nullopt, std::nullopt};
    reset.tab = current != gameTabs_.begin() ? *std::prev(current) : 0;
    reset.closePreviousTab = true;
    requestReset(std::move(reset));
}
void AnalysisController::openGame(const persistence::StoredGame& game) {
    if (!game.document || game.summary.id <= 0) {
        emit failed(tr("The stored game is unavailable."));
        return;
    }
    requestReset({std::nullopt, game});
}
void AnalysisController::openLine(std::shared_ptr<const chess::Position> position,
                                  std::vector<chess::Move> moves, int step) {
    if (position && step >= 0 && step <= static_cast<int>(moves.size())) {
        requestReset({std::nullopt, std::nullopt, std::move(position), std::move(moves), step});
    }
}
void AnalysisController::openPosition(std::shared_ptr<const chess::Position> position) {
    if (position) {
        requestReset({std::nullopt, std::nullopt, std::move(position)});
    }
}
void AnalysisController::reset(const std::optional<std::string>& fen) {
    requestReset({fen, std::nullopt});
}
void AnalysisController::requestReset(ResetRequest reset) {
    if (closing_) {
        return;
    }
    if (activeTab_ > 0 && !reset.tab) {
        emit failed(tr("Open the scratch tab to set a new analysis position."));
        return;
    }
    ++sessionGeneration_;
    pendingNavigation_.reset();
    pendingMoveChoice_.reset();
    requested_ = current_;
    resetting_ = true;
    board_.interactive = false;
    board_.selection.reset();
    if (busy_) {
        pendingReset_ = std::move(reset);
        emit changed();
        return;
    }
    startReset(std::move(reset));
}
void AnalysisController::startReset(ResetRequest reset) {
    resetting_ = true;
    switchingTabs_ = reset.tab.has_value();
    const bool announceGame = reset.game.has_value();
    submit(
        [this, reset = std::move(reset)](Session& session) {
            if (reset.tab) {
                const auto target = suspendedSessions_.find(*reset.tab);
                if (target != suspendedSessions_.end()) {
                    if (reset.closePreviousTab) {
                        session = std::move(*target->second);
                        suspendedSessions_.erase(target);
                    } else {
                        auto stored = suspendedSessions_.extract(target);
                        std::swap(session, *stored.mapped());
                        stored.key() = stored.mapped()->tab;
                        suspendedSessions_.insert(std::move(stored));
                    }
                    return QString{};
                }
                if (!reset.game) {
                    return tr("This analysis tab is unavailable.");
                }
            }
            // Build the replacement fully on the worker before retiring the old session.
            Session replacement;
            replacement.tab = reset.tab.value_or(session.tab);
            if (reset.game) {
                const auto& game = *reset.game;
                replacement.tree = game.document->tree;
                replacement.source = game.document;
                if (game.source.type == QStringLiteral("file")) {
                    replacement.sourceFile = QUrl(game.source.uri).toLocalFile();
                }
                replacement.gameId = game.summary.id;
                const auto playerName = [](const QString& value) {
                    auto name = value.left(256);
                    if (name.size() < value.size()) {
                        if (name.back().isHighSurrogate()) {
                            name.chop(1);
                        }
                        name += QChar(0x2026);
                    }
                    return name;
                };
                replacement.white = playerName(game.summary.white);
                replacement.black = playerName(game.summary.black);
                replacement.description =
                    tr("%1 — %2 | Result: %3")
                        .arg(replacement.white, replacement.black, game.summary.result);
            } else if (reset.position) {
                const auto context = reset.position->context();
                std::vector<chess::GameTreeInputNode> nodes{{0, std::nullopt, {}}};
                for (const auto& move : context.moves) {
                    nodes.push_back({nodes.size() - 1, move, {}});
                }
                for (const auto& move : reset.continuation) {
                    nodes.push_back({nodes.size() - 1, move, {}});
                }
                auto result = chess::GameTree::fromNodes(
                    context.provenance == chess::RootProvenance::InitialPosition
                        ? std::nullopt
                        : std::optional<std::string_view>{context.rootFen},
                    nodes);
                if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                    return QString::fromStdString(error->message);
                }
                replacement.tree = std::move(std::get<chess::GameTree>(result));
                replacement.current =
                    chess::NodeId{context.moves.size() + static_cast<std::size_t>(reset.step)};
            } else {
                auto result =
                    reset.fen ? chess::GameTree::fromFen(*reset.fen) : chess::GameTree::initial();
                if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                    return QString::fromStdString(
                        error->positionError ? error->positionError->message : error->message);
                }
                replacement.tree = std::move(std::get<chess::GameTree>(result));
                replacement.treeDirty = reset.fen.has_value();
            }
            replacement.navigation = makeNavigationTree(*replacement.tree);
            if (reset.tab) {
                if (!session.tree) {
                    session.tree = std::get<chess::GameTree>(chess::GameTree::initial());
                }
                auto previous = std::make_unique<Session>();
                const auto slot = suspendedSessions_.try_emplace(session.tab, std::move(previous));
                Q_ASSERT(slot.second);
                *slot.first->second = std::move(session);
            }
            session = std::move(replacement);
            return QString{};
        },
        true, std::nullopt, true, announceGame);
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
            const auto before = session.tree->size();
            auto result = session.tree->addMove(from, move);
            if (auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.treeDirty |= session.tree->size() != before;
            session.current = std::get<chess::NodeId>(result);
            return QString{};
        },
        false, move);
}
void AnalysisController::addEngineLine(std::vector<chess::Move> moves, std::uint64_t revision) {
    if (!board_.interactive || revision != board_.revision || moves.empty()) {
        return;
    }
    submit([moves = std::move(moves)](Session& session) {
        const auto before = session.tree->size();
        auto result = session.tree->addLine(session.current, moves);
        if (const auto* error = std::get_if<chess::GameTreeError>(&result)) {
            return QString::fromStdString(error->message);
        }
        session.treeDirty |= before != session.tree->size();
        session.current = std::get<chess::NodeId>(result);
        return QString{};
    });
}

bool AnalysisController::canEdit(NavigationTarget target) const {
    return !busy_ && board_.interactive && canNavigate() && target.session == sessionGeneration_ &&
           navigationTree_->nodes.contains(target.node);
}
bool AnalysisController::canPromote(NavigationTarget target) const {
    if (!canDelete(target)) {
        return false;
    }
    const auto parent = *navigationTree_->nodes.at(target.node).parent;
    return navigationTree_->nodes.at(parent).children.front() != target.node;
}
bool AnalysisController::canDelete(NavigationTarget target) const {
    return canEdit(target) && navigationTree_->nodes.at(target.node).parent.has_value();
}
void AnalysisController::setNotes(NavigationTarget target, const AnalysisNotes& notes) {
    if (!canEdit(target) || target.node != current_ || notes == notes_) {
        return;
    }
    if (notes.before.contains(QChar(0)) || notes.after.contains(QChar(0)) ||
        (!canDelete(target) && !notes.nags.empty())) {
        emit failed(tr("Invalid PGN annotation."));
        return;
    }
    submit(
        [notes, id = target.node](Session& session) {
            const auto original = session.tree->node(id)->annotations;
            auto updated = original;
            const auto update = [](std::string& field, const QString& replacement) {
                std::vector<BoardAnnotation> drawings;
                if (analysisComment(field, &drawings) != replacement) {
                    field = analysisCommentWithDrawings(replacement, drawings);
                }
            };
            update(updated.commentBefore, notes.before);
            update(updated.commentAfter, notes.after);
            updated.nags = notes.nags;
            const auto result = session.tree->setAnnotations(id, updated);
            if (const auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.treeDirty = true;
            session.navigation.reset();
            return QString{};
        },
        false, std::nullopt, false);
}
void AnalysisController::promoteVariation(NavigationTarget target) {
    if (!canPromote(target)) {
        return;
    }
    // Forget the previously preferred sibling so forward navigation follows the promoted line.
    const auto parent = *navigationTree_->nodes.at(target.node).parent;
    chosen_.insert_or_assign(parent, target.node);
    submit(
        [id = target.node](Session& session) {
            const auto result = session.tree->promote(id);
            if (const auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            session.treeDirty = true;
            session.navigation.reset();
            return QString{};
        },
        false, std::nullopt, false);
}
void AnalysisController::deleteVariation(NavigationTarget target) {
    if (!canDelete(target)) {
        return;
    }
    const auto parent = *navigationTree_->nodes.at(target.node).parent;
    const bool containsCurrent = std::ranges::any_of(
        history_, [target](const auto& move) { return move.id == target.node; });
    submit(
        [id = target.node, parent, containsCurrent](Session& session) {
            const auto result = session.tree->removeSubtree(id);
            if (const auto* error = std::get_if<chess::GameTreeError>(&result)) {
                return QString::fromStdString(error->message);
            }
            if (containsCurrent) {
                session.current = parent;
            }
            session.treeDirty = true;
            session.navigation.reset();
            return QString{};
        },
        false, std::nullopt, containsCurrent);
}
void AnalysisController::exportPgn(const QString& filename) {
    if (filename.isEmpty() || !canEdit(target(current_))) {
        return;
    }
    auto drawings = annotations_;
    // While an export holds the serialized tree the board is read-only, so these
    // remain the exact annotations written even if a navigation request queues.
    exporting_ = true;
    submit(
        [filename, drawings = std::move(drawings)](Session& session) {
            if (!session.sourceFile.isEmpty() &&
                QFileInfo(filename) == QFileInfo(session.sourceFile)) {
                return tr("Choose a different file to keep the source PGN intact.");
            }
            auto error = writeAnalysisPgn(filename, *session.tree, session.source.get(), drawings);
            if (error.isEmpty()) {
                session.treeDirty = false;
            }
            return error;
        },
        false, std::nullopt, false);
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
    if (annotations == originalAnnotations_) {
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
    if (board_.annotations == originalAnnotations_) {
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
    if (board_.annotations == originalAnnotations_) {
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
    deferredGame_.reset();
    ++sessionGeneration_;
    ++request_;
    board_.interactive = false;
    pool_.start([this] {
        suspendedSessions_.clear();
        session_.reset();
    });
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
