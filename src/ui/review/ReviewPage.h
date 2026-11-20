#pragma once
#include "app/ReviewGameController.h"
#include <QWidget>
#include <utility>
#include <vector>
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QTableView;
class QPlainTextEdit;
class QSplitter;
class QFrame;
class QAction;
class QTableWidget;
class QScrollBar;
class QScrollArea;
class QKeyEvent;
namespace chess_review::ui {
class ChessBoardWidget;
class MoveTimeline;
class ReviewRunsModel;
class ReviewMovesModel;
class ReviewComparisonModel;
class EvaluationBarWidget;
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
    void setVerbose(bool verbose);
  signals:
    void backRequested();
    void settingsRequested();
    void analyzeRequested();

  protected:
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void update();
    void updateNavigation();
    void updateExploration();
    void fitRetryFeedback();
    void updateSummary();
    void captureRetryReadingState();
    void restoreRetryReadingState();
    void restoreRetryFocus();
    struct RetryReadingState {
        persistence::GameId game;
        review::ReviewId run;
        int position;
        bool details;
        std::vector<bool> sections;
        // These fixed widgets are owned by this page or its containing scroll area.
        std::vector<std::pair<QScrollBar*, int>> scrollBars;
    };
    std::optional<RetryReadingState> retryReadingState_;
    std::uint64_t readingRestoreGeneration_ = 0;
    bool retryBoardFocused_ = false;
    bool retryFocusPending_ = false;
    QScrollArea* explanationScroll_;
    std::uint64_t retryFocusGeneration_ = 0;
    bool verbose_ = false;
    bool guiding_ = false;
    int lastPosition_ = 0;
    QWidget* lesson_;
    QWidget* summaryPanel_;
    QWidget* chessDetails_;
    QPushButton* chessDetailsToggle_;
    QPushButton* startGuided_;
    QLabel* accuracyValue_;
    QLabel* momentsValue_;
    QLabel* coverageValue_;
    QLabel* summaryState_;
    QLabel* resultValue_;
    QLabel* summaryOpening_;
    QLabel* classification_;
    QLabel* availability_;
    QLabel* alternative_;
    QLabel* impact_;
    QTableWidget* classificationCounts_;
    QTableWidget* inspectorFields_;
    QTableWidget* overviewFields_;
    QAction* inspectorAction_ = nullptr;
    QComboBox* candidates_;
    QWidget* comparison_;
    QLabel* comparisonStatus_;
    QTableView* comparisonRows_;
    ReviewComparisonModel* comparisonModel_;
    QLabel* explorationStatus_ = nullptr;
    QPushButton* showLine_;
    QPushButton* tryAgain_;
    QPushButton* returnGame_;
    QPushButton* hint_;
    QPushButton* reveal_;
    QPushButton* analyze_;
    QPushButton* reloadExploration_;
    std::vector<std::pair<QAction*, QWidget*>> sections_;
    ChessBoardWidget* board_;
    MoveTimeline* timeline_;
    QSplitter* splitter_;
    EvaluationBarWidget* evaluation_;
    QLabel* previewLabel_;
    QLabel* headline_;
    QLabel* explanation_;
    QLabel* topPlayer_;
    QLabel* bottomPlayer_;
    QFrame* rail_;
    QWidget* setup_;
    QAction* setupToggle_;
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
    QCheckBox* bookEnabled_;
    std::optional<bool> bookOverride_;
    std::optional<review::Quality> qualityOverride_;
    std::optional<std::int64_t> engineOverride_;
    QLabel* game_;
    QLabel* headerOpening_;
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
