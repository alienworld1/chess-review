#pragma once

#include "app/BoardState.h"
#include "persistence/GameRepository.h"
#include <QObject>
#include <QThreadPool>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chess_review::app {
// GUI-thread facade. Only its serial worker accesses Session and its chess values.
// close() is asynchronous; await closed() before destruction.
class AnalysisController final : public QObject {
    Q_OBJECT
  public:
    explicit AnalysisController(QObject* parent = nullptr);
    ~AnalysisController() override;
    const BoardViewState& board() const { return board_; }
    const std::vector<HistoryEntry>& history() const { return history_; }
    const std::vector<HistoryEntry>& continuations() const { return continuations_; }
    chess::NodeId currentNode() const { return current_; }
    bool busy() const { return busy_; }
    const QString& error() const { return error_; }
    const std::shared_ptr<const NavigationTree>& navigationTree() const { return navigationTree_; }
    const std::vector<chess::NodeId>& chosenLine() const { return chosenLine_; }
    chess::NodeId requestedNode() const { return requested_; }
    NavigationTarget target(chess::NodeId node) const { return {sessionGeneration_, node}; }
    bool canNavigate() const { return navigationTree_ && !closing_ && !resetting_; }
    bool canPrevious() const;
    bool canNext() const;
    bool hasContent() const { return hasMoves_ || !annotations_.empty(); }
    bool canUndoAnnotation() const;
    bool canRedoAnnotation() const;
    const std::shared_ptr<const chess::Position>& position() const { return position_; }
    void start();
    void openGame(const persistence::StoredGame& game);
    std::optional<persistence::GameId> gameId() const { return gameId_; }
    const QString& whitePlayer() const { return whitePlayer_; }
    const QString& blackPlayer() const { return blackPlayer_; }
    const QString& gameDescription() const { return gameDescription_; }
    const QString& positionNotes() const { return positionNotes_; }
    // Optional FEN is also useful to embedded consumers; validation stays in chess.
    void openLine(std::shared_ptr<const chess::Position> position, std::vector<chess::Move> moves,
                  int step);
    void openPosition(std::shared_ptr<const chess::Position> position);
    void reset(const std::optional<std::string>& fen = std::nullopt);
    void selectSquare(chess::Square square);
    void clearSelection();
    void attemptMove(chess::Move move, std::uint64_t revision);
    void navigate(chess::NodeId node);
    void navigate(NavigationTarget target);
    void setOrientation(bool blackAtBottom);
    void previous();
    void next();
    void beginning();
    void end();
    void flip();
    void toggleAnnotation(BoardAnnotation annotation);
    void clearAnnotations();
    void undoAnnotation();
    void redoAnnotation();
    void close();
  signals:
    void changed();
    void moveAccepted(chess_review::chess::Move move);
    void navigationAnimationRequested(chess_review::chess::Move move);
    void failed(const QString& message);
    void closed();
    void gameOpened();

  private:
    struct Session;
    struct Snapshot;
    struct ResetRequest {
        std::optional<std::string> fen;
        std::optional<persistence::StoredGame> game;
        std::shared_ptr<const chess::Position> position = {};
        std::vector<chess::Move> continuation = {};
        int step = 0;
    };
    struct AnnotationHistory {
        std::vector<std::vector<BoardAnnotation>> undo;
        std::vector<std::vector<BoardAnnotation>> redo;
    };
    void submit(std::function<QString(Session&)> operation, bool resetSession = false,
                std::optional<chess::Move> animatedMove = std::nullopt);
    void publish(Snapshot snapshot, std::uint64_t request, std::optional<chess::Move> animatedMove);
    void startReset(ResetRequest reset);
    void requestReset(ResetRequest reset);
    void dispatchNavigation();
    void rememberPath(chess::NodeId node);
    void updateChosenLine();
    void replaceAnnotations(std::vector<BoardAnnotation> annotations);
    void finishClose();
    QThreadPool pool_;
    std::unique_ptr<Session> session_;
    BoardViewState board_;
    std::shared_ptr<const chess::Position> position_;
    std::vector<HistoryEntry> history_;
    std::vector<HistoryEntry> continuations_;
    std::shared_ptr<const NavigationTree> navigationTree_;
    std::vector<chess::NodeId> chosenLine_;
    std::map<chess::NodeId, chess::NodeId> chosen_;
    chess::NodeId requested_{0};
    std::optional<chess::NodeId> pendingNavigation_;
    std::optional<chess::NodeId> pendingMoveChoice_;
    std::optional<ResetRequest> pendingReset_;
    std::uint64_t sessionGeneration_ = 0;
    bool resetting_ = false;
    bool navigating_ = false;
    chess::NodeId current_{0};
    std::map<chess::NodeId, std::vector<BoardAnnotation>> annotations_;
    std::map<chess::NodeId, AnnotationHistory> annotationHistory_;
    std::uint64_t request_ = 0;
    bool busy_ = false;
    bool closing_ = false;
    bool hasMoves_ = false;
    std::optional<persistence::GameId> gameId_;
    QString whitePlayer_;
    QString blackPlayer_;
    QString gameDescription_;
    QString positionNotes_;
    QString error_;
};
} // namespace chess_review::app
