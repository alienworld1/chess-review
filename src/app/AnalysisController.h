#pragma once

#include "app/BoardState.h"
#include <QObject>
#include <QThreadPool>
#include <functional>
#include <map>
#include <memory>

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
    bool hasContent() const { return hasMoves_ || !annotations_.empty(); }
    bool canUndoAnnotation() const;
    bool canRedoAnnotation() const;
    void start();
    // Optional FEN is also useful to embedded consumers; validation stays in chess.
    void reset(const std::optional<std::string>& fen = std::nullopt);
    void selectSquare(chess::Square square);
    void clearSelection();
    void attemptMove(chess::Move move, std::uint64_t revision);
    void navigate(chess::NodeId node);
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
    void failed(const QString& message);
    void closed();

  private:
    struct Session;
    struct Snapshot;
    struct AnnotationHistory {
        std::vector<std::vector<BoardAnnotation>> undo;
        std::vector<std::vector<BoardAnnotation>> redo;
    };
    void submit(std::function<QString(Session&)> operation, bool resetSession = false,
                std::optional<chess::Move> animatedMove = std::nullopt);
    void publish(Snapshot snapshot, std::uint64_t request, std::optional<chess::Move> animatedMove);
    void replaceAnnotations(std::vector<BoardAnnotation> annotations);
    void finishClose();
    QThreadPool pool_;
    std::unique_ptr<Session> session_;
    BoardViewState board_;
    std::vector<HistoryEntry> history_;
    std::vector<HistoryEntry> continuations_;
    chess::NodeId current_{0};
    std::optional<chess::NodeId> parent_;
    std::map<chess::NodeId, std::vector<BoardAnnotation>> annotations_;
    std::map<chess::NodeId, AnnotationHistory> annotationHistory_;
    std::uint64_t request_ = 0;
    bool busy_ = false;
    bool closing_ = false;
    bool hasMoves_ = false;
};
} // namespace chess_review::app
