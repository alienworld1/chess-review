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
namespace chess_review::app {
class AnalysisController;
class ImportController;
class GameLibraryController;
} // namespace chess_review::app
namespace chess_review::diagnostics {
class Logger;
}
namespace chess_review::ui {
class StartupPanel;
class AnalysisPage;

// Pure geometry policy, also used to validate unavailable/removed monitor cases.
QRect fittedWindowGeometry(const QRect& desired, const QRect& available);

class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    // The application-owned navigation controller must outlive the window.
    explicit MainWindow(app::NavigationController& navigation, QWidget* parent = nullptr);
    // Both application-owned controllers must outlive this window.
    MainWindow(app::NavigationController& navigation, app::AnalysisController& analysis,
               QWidget* parent = nullptr);
    void installImports(app::ImportController& imports, app::GameLibraryController& library);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const app::Diagnostic& diagnostic);
    void setDiagnostics(diagnostics::Logger& logger);
    void showUiStateFailure(app::UiStateError error = app::UiStateError::Invalid);
    void restoreUiState(const app::UiState& state);
    app::UiState uiState() const;
    void resetLayout();
    void showClosing();
    // Call only after application services have closed; permits Qt's final window close.
    void completeShutdown();
  signals:
    void closeRequested();
    void invalidUiState();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void installAnalysis(app::AnalysisController& controller);
    void selectDestination();
    void resetWindowGeometry();
    void fitToScreen();
    void watchScreen(QScreen* screen);
    app::NavigationController& navigation_;
    AnalysisPage* analysis_ = nullptr;
    QAction* importFile_ = nullptr;
    QAction* pastePgn_ = nullptr;
    app::GameLibraryController* library_ = nullptr;
    StartupPanel* startup_;
    QStackedWidget* pages_;
    QLabel* preferencesNotice_;
    std::array<QAction*, 5> destinations_{};
    QAction* reset_;
    bool closing_ = false;
    bool shutdownComplete_ = false;
    bool stateNoticeShown_ = false;
    bool stateRestored_ = false;
    QMetaObject::Connection screenGeometryConnection_;
};
} // namespace chess_review::ui
