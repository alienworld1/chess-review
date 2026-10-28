#pragma once
#include "persistence/Database.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUuid>
#include <cstdint>
#include <optional>
namespace chess_review::app {
struct ReviewMovesState {
    std::optional<persistence::ReviewMovePage> page;
    int offset = 0;
    int selectedPosition = 0;
    bool loading = false;
    QString error;
};
// GUI-thread facade; database worker owns replay/derivation. The injected database
// outlives this controller. Close and drain pending requests before destruction.
class ReviewMovesController final : public QObject {
    Q_OBJECT
  public:
    explicit ReviewMovesController(persistence::Database& database);
    const ReviewMovesState& state() const { return state_; }
    void bind(const std::optional<persistence::ReviewSummary>& run);
    void showPage(int offset);
    void select(int positionIndex);
    void retry();
    void close();
    bool drained() const { return pending_.isNull(); }
  signals:
    void changed();
    void drainedChanged();

  private:
    void requestNext();
    persistence::Database& database_;
    ReviewMovesState state_;
    std::optional<persistence::ReviewSummary> run_;
    QUuid pending_;
    QTimer timer_;
    int backfillOffset_ = 0;
    int retryMs_ = 25;
    std::uint64_t generation_ = 0;
    std::uint64_t pendingGeneration_ = 0;
    bool pendingVisible_ = false;
    bool dirty_ = false;
    bool closing_ = false;
};
} // namespace chess_review::app
