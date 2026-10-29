#pragma once
#include "app/ReviewGameController.h"
#include <QWidget>
#include <utility>
#include <vector>
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QTableView;
class QPlainTextEdit;
class QSplitter;
class QFrame;
namespace chess_review::ui {
class ChessBoardWidget;
class MoveTimeline;
class ReviewRunsModel;
class ReviewMovesModel;
class ReviewPage final : public QWidget {
    Q_OBJECT
  public:
    explicit ReviewPage(app::ReviewGameController& controller, QWidget* parent = nullptr);
    // The explanation owns its scroll extent; it must not expand the outer
    // shell through height-for-width propagation when details are opened.
    bool hasHeightForWidth() const override { return false; }
    app::ReviewGameController& controller() { return controller_; }
    int splitShare() const;
    void setSplitShare(int share);
  signals:
    void backRequested();
    void settingsRequested();
    void analyzeRequested();

  protected:
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void update();
    void updateNavigation();
    void updateExploration();
    QComboBox* candidates_;
    QLabel* explorationStatus_;
    QPushButton* showLine_;
    QPushButton* tryAgain_;
    QPushButton* returnGame_;
    QPushButton* hint_;
    QPushButton* reveal_;
    QPushButton* analyze_;
    QPushButton* reloadExploration_;
    std::vector<std::pair<QPushButton*, QWidget*>> sections_;
    ChessBoardWidget* board_;
    MoveTimeline* timeline_;
    QSplitter* splitter_;
    QProgressBar* evaluation_;
    QLabel* evaluationText_;
    QLabel* previewLabel_;
    QLabel* headline_;
    QLabel* explanation_;
    QLabel* compactSummary_;
    QLabel* topPlayer_;
    QLabel* bottomPlayer_;
    QFrame* rail_;
    QWidget* setup_;
    QPushButton* setupToggle_;
    QPushButton* previousMoment_;
    QPushButton* nextMoment_;
    QPushButton* firstMove_;
    QPushButton* lastMove_;
    QPushButton* previousMove_;
    QPushButton* nextMove_;
    std::optional<review::ReviewId> shownRun_;
    int splitShare_ = 55;
    bool applyingSplit_ = false;
    app::ReviewGameController& controller_;
    QComboBox* side_;
    QComboBox* engine_;
    QComboBox* quality_;
    QLabel* game_;
    QLabel* status_;
    QLabel* error_;
    QLabel* reason_;
    QProgressBar* progress_;
    QPushButton* start_;
    QPushButton* cancel_;
    QPushButton* resume_;
    QPushButton* previous_;
    QPushButton* next_;
    QTableView* runs_;
    ReviewRunsModel* model_;
    QTableView* moves_;
    ReviewMovesModel* movesModel_;
    QPlainTextEdit* details_;
    QPlainTextEdit* summary_;
    QTableView* moments_;
    ReviewMovesModel* momentsModel_;
    QPushButton* previousMoments_;
    QPushButton* nextMoments_;
    QPushButton* refreshSummary_;
    QLabel* moveStatus_;
    QPushButton* previousMoves_;
    QPushButton* nextMoves_;
    QPushButton* retryMoves_;
    std::uint64_t profilesRevision_ = 0;
    persistence::GameId gameId_ = 0;
};
} // namespace chess_review::ui
