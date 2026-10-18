#pragma once

#include "app/Diagnostic.h"

#include <QMainWindow>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;

namespace chess_review::diagnostics {
class Logger;
}

namespace chess_review::ui {

class StartupWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit StartupWindow(QWidget* parent = nullptr);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const app::Diagnostic& diagnostic);
    void setDiagnostics(diagnostics::Logger& logger);

  signals:
    void closeRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void showDiagnostics();
    QString safeReport() const;
    diagnostics::Logger* logger_ = nullptr;
    app::Diagnostic failure_;
    QLabel* summary_;
    QLabel* notice_;
    QPushButton* reveal_;
    QPushButton* copy_;
    bool loggingNoticeShown_ = false;
    QLabel* status_;
    QLabel* diagnostic_;
    QProgressBar* progress_;
};

} // namespace chess_review::ui
