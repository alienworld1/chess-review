#pragma once

#include "persistence/Database.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <vector>

namespace chess_review::app {
class GameLibraryController final : public QObject {
    Q_OBJECT
  public:
    explicit GameLibraryController(persistence::Database& database, QObject* parent = nullptr);
    void setReady(bool ready);
    void activate();
    void refresh(bool newest = false);
    void importedGame();
    void previous();
    void next();
    const std::vector<persistence::GameSummary>& rows() const { return rows_; }
    bool busy() const { return !pending_.isNull(); }
    bool ready() const { return ready_; }
    bool hasNext() const { return hasNext_; }
    std::int64_t page() const { return offset_ / 50; }
    const QString& error() const { return error_; }
  signals:
    void changed();

  private:
    void requestPage();
    persistence::Database& database_;
    QTimer refreshTimer_;
    persistence::RequestId pending_;
    std::vector<persistence::GameSummary> rows_;
    QString error_;
    std::int64_t offset_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t requestedRevision_ = 0;
    bool ready_ = false;
    bool active_ = false;
    bool hasNext_ = false;
};
} // namespace chess_review::app
