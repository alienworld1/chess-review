#pragma once

#include "persistence/GameRepository.h"
#include <QWidget>

class QAction;
class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace chess_review::app {
class EngineSettingsController;
class GameLibraryController;
class ImportController;
class PuzzleController;
class ReviewGameController;
} // namespace chess_review::app

namespace chess_review::ui {
class HomePage final : public QWidget {
    Q_OBJECT
  public:
    explicit HomePage(QWidget* parent = nullptr);
    void installLibrary(app::GameLibraryController& library);
    void installImports(app::ImportController& imports, QAction& file, QAction& paste,
                        QAction& link);
    void installReview(app::ReviewGameController& review);
    void installPuzzles(app::PuzzleController& puzzles);
    void installEngines(app::EngineSettingsController& engines);

  signals:
    void gamesRequested();
    void puzzlesRequested();
    void settingsRequested();
    void reviewRequested();
    void recentGameRequested(persistence::GameId id);

  private:
    void installRecovery(app::ReviewGameController& review);
    void updateRecent(app::GameLibraryController& library);
    QLabel* emptyArt_;
    QLabel* entryHint_;
    QVBoxLayout* entry_;
    QWidget* resume_;
    QLabel* resumeDetail_;
    QPushButton* resumeAction_;
    QVBoxLayout* recovery_;
    QWidget* recentSection_;
    QListWidget* recent_;
    QLabel* recentStatus_;
    QPushButton* openRecent_;
    QPushButton* retryRecent_;
    QWidget* puzzleSection_;
    QLabel* puzzleDetail_;
    QWidget* engineSection_;
    QLabel* engineDetail_;
    app::GameLibraryController* library_ = nullptr;
};
} // namespace chess_review::ui
