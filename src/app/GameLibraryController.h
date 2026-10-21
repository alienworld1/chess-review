#pragma once

#include "persistence/Database.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <vector>

namespace chess_review::app {
// GUI-thread presentation facade. The injected Database must outlive this controller.
class GameLibraryController final : public QObject {
    Q_OBJECT
  public:
    explicit GameLibraryController(persistence::Database& database, QObject* parent = nullptr);
    void setReady(bool ready);
    void activate();
    void open(persistence::GameId id);
    bool opening() const { return !pendingOpen_.isNull(); }
    void refresh(bool newest = false);
    void importedGame();
    void previous();
    void next();
    const std::vector<persistence::GameSummary>& rows() const { return rows_; }
    bool busy() const { return !pending_.isNull(); }
    bool ready() const { return ready_; }
    bool hasNext() const { return hasNext_; }
    std::int64_t page() const { return offset_ / 50; }
    const QString& error() const { return openError_.isEmpty() ? error_ : openError_; }
  signals:
    void changed();
    void openFailed(const QString& message);
    void opened(const chess_review::persistence::StoredGame& game);

  private:
    void requestPage();
    persistence::Database& database_;
    QTimer refreshTimer_;
    persistence::RequestId pending_;
    persistence::RequestId pendingOpen_;
    std::vector<persistence::GameSummary> rows_;
    QString error_;
    QString openError_;
    std::uint64_t openGeneration_ = 0;
    std::uint64_t requestedOpenGeneration_ = 0;
    std::int64_t offset_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t requestedRevision_ = 0;
    bool ready_ = false;
    bool active_ = false;
    bool hasNext_ = false;
};
} // namespace chess_review::app
