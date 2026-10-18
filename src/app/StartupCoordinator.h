#pragma once

#include "persistence/Database.h"

#include <QObject>
#include <QString>

namespace chess_review::diagnostics {
class Logger;
}

namespace chess_review::ui {
class StartupWindow;
}

namespace chess_review::app {

class StartupCoordinator final : public QObject {
    Q_OBJECT

  public:
    explicit StartupCoordinator(ui::StartupWindow& window, QObject* parent = nullptr);
    // The application owns logger and keeps it alive until finished().
    StartupCoordinator(ui::StartupWindow& window, diagnostics::Logger& logger,
                       QObject* parent = nullptr);
    void start(const QString& databasePath);
    void shutdown();

  signals:
    void finished();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void connectWindow(ui::StartupWindow& window);
    void finish();
    diagnostics::Logger* logger_ = nullptr;
    persistence::Database database_;
    bool stopping_ = false;
    bool stopped_ = false;
};

} // namespace chess_review::app
