#pragma once
#include "app/Diagnostic.h"
#include "app/NavigationController.h"
#include "app/UiStateStore.h"
#include <QMainWindow>
#include <QRect>
#include <array>
#include <cstdint>

class QAction;
class QLabel;
class QPushButton;
class QScreen;
class QStackedWidget;
namespace chess_review::app {
class AnalysisController;
class EngineSettingsController;
class EngineAnalysisController;
class ReviewGameController;
class ImportController;
class GameLibraryController;
class PuzzleController;
class PuzzleGenerationController;
class ReviewConfiguration;
} // namespace chess_review::app
namespace chess_review::diagnostics {
class Logger;
}
namespace chess_review::ui {
class StartupPanel;
class HomePage;
class AnalysisPage;
class ReviewPage;

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
    void installReview(app::ReviewGameController& review);
    void installPuzzles(app::PuzzleController& puzzles,
                        app::PuzzleGenerationController& generation);
    void installSettings(app::ReviewConfiguration& configuration);
    void installEngineAnalysis(app::EngineAnalysisController& engine);
    void installEngines(app::EngineSettingsController& engines);
    void installImports(app::ImportController& imports, app::GameLibraryController& library);
    void setVerbose(bool verbose);
    void showProgress(const QString& message);
    void showReady();
    void showFailure(const app::Diagnostic& diagnostic);
    void showConfigurationFailure(const QString& safeDetails);
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
    void boardDisplayOptionsChanged(bool coordinates, bool animation);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void shareBoardDisplayOptions(AnalysisPage& page);
    void setBoardDisplayOptions(bool coordinates, bool animation);
    void shareAnalysisLineCount(AnalysisPage& page);
    void openEngineSettings();
    void installAnalysis(app::AnalysisController& controller);
    void selectDestination();
    void showContextPage(app::Destination destination, QWidget* page);
    void showReview();
    void resetWindowGeometry();
    void fitToScreen();
    void watchScreen(QScreen* screen);
    app::NavigationController& navigation_;
    Qt::ColorScheme colorScheme_ = Qt::ColorScheme::Unknown;
    int reviewSplit_ = 55;
    ReviewPage* reviewPage_ = nullptr;
    app::ReviewGameController* reviewController_ = nullptr;
    bool reviewLeaveAllowed_ = false;
    bool reviewLeavePending_ = false;
    bool reviewWasActive_ = false;
    AnalysisPage* analysis_ = nullptr;
    AnalysisPage* linkedAnalysis_ = nullptr;
    AnalysisPage* puzzleAnalysis_ = nullptr;
    AnalysisPage* puzzleSavePage_ = nullptr;
    QPushButton* backToReviewAnalysis_ = nullptr;
    std::uint64_t puzzleNavigationEpoch_ = 0;
    app::EngineAnalysisController* liveEngine_ = nullptr;
    QWidget* contextPage_ = nullptr;
    std::uint64_t reviewNavigationEpoch_ = 0;
    bool verbose_ = false;
    std::optional<int> configuredAnalysisLines_;
    QAction* importFile_ = nullptr;
    QAction* pastePgn_ = nullptr;
    QAction* importLink_ = nullptr;
    app::GameLibraryController* library_ = nullptr;
    StartupPanel* startup_;
    HomePage* home_ = nullptr;
    QStackedWidget* pages_;
    QLabel* preferencesNotice_;
    std::array<QAction*, 5> destinations_{};
    QAction* reset_;
    QAction* boardCoordinates_;
    QAction* boardAnimation_;
    bool closing_ = false;
    bool shutdownComplete_ = false;
    bool stateNoticeShown_ = false;
    bool stateRestored_ = false;
    QMetaObject::Connection screenGeometryConnection_;
};
} // namespace chess_review::ui
