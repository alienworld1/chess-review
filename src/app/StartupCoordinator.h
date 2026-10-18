#pragma once

#include "persistence/Database.h"

#include <QObject>
#include <QString>

namespace chess_review::ui {
class StartupWindow;
}

namespace chess_review::app {

class StartupCoordinator final : public QObject {
    Q_OBJECT

  public:
    explicit StartupCoordinator(ui::StartupWindow& window, QObject* parent = nullptr);
    void start(const QString& databasePath);
    void shutdown();
    static QString defaultDatabasePath();

  signals:
    void finished();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    persistence::Database database_;
    bool stopping_ = false;
    bool stopped_ = false;
};

} // namespace chess_review::app
