#pragma once
#include "app/Diagnostic.h"
#include "app/NavigationController.h"
#include "app/UiStateStore.h"
#include <QMainWindow>
#include <QRect>
#include <array>

class QAction;
class QLabel;
class QScreen;
class QStackedWidget;
namespace chess_review::diagnostics {
class Logger;
}
namespace chess_review::ui {
class StartupPanel;

// Pure geometry policy, also used to validate unavailable/removed monitor cases.
QRect fittedWindowGeometry(const QRect& desired, const QRect& available);

class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(app::NavigationController& navigation, QWidget* parent = nullptr);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const app::Diagnostic& diagnostic);
    void setDiagnostics(diagnostics::Logger& logger);
    void showUiStateFailure();
    void restoreUiState(const app::UiState& state);
    app::UiState uiState() const;
    void resetLayout();
    void showClosing();
  signals:
    void closeRequested();
    void invalidUiState();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void selectDestination(app::Destination destination);
    void fitToScreen();
    void watchScreen(QScreen* screen);
    app::NavigationController& navigation_;
    StartupPanel* startup_;
    QStackedWidget* pages_;
    QLabel* preferencesNotice_;
    std::array<QAction*, 5> destinations_{};
    QAction* reset_;
    bool closing_ = false;
    bool stateNoticeShown_ = false;
    bool stateRestored_ = false;
    QMetaObject::Connection screenGeometryConnection_;
};
} // namespace chess_review::ui
