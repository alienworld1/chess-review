#pragma once

#include "app/Diagnostic.h"

#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;

namespace chess_review::diagnostics {
class Logger;
}

namespace chess_review::ui {

class StartupPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit StartupPanel(QWidget* parent = nullptr);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const app::Diagnostic& diagnostic);
    void showConfigurationFailure(const QString& safeDetails);
    void setDiagnostics(diagnostics::Logger& logger);

    void showDiagnostics();
    void showClosing();

  signals:
    void closeRequested();

  private:
    QString safeReport() const;
    diagnostics::Logger* logger_ = nullptr; // Non-owning; cleared by destroyed().
    app::Diagnostic failure_;
    QPushButton* exit_;
    QLabel* summary_;
    QLabel* notice_;
    QLabel* configurationNotice_;
    QString configurationReport_;
    QPushButton* reveal_;
    QPushButton* copy_;
    bool loggingNoticeShown_ = false;
    QLabel* status_;
    QPlainTextEdit* diagnostic_;
    QProgressBar* progress_;
};

} // namespace chess_review::ui
