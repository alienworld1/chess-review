#pragma once

#include <QMainWindow>
#include <QString>

class QLabel;
class QProgressBar;

namespace chess_review::ui {

class StartupWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit StartupWindow(QWidget* parent = nullptr);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const QString& diagnostic);

  signals:
    void closeRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    QLabel* status_;
    QLabel* diagnostic_;
    QProgressBar* progress_;
};

} // namespace chess_review::ui
