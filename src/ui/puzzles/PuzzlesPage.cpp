#include "ui/puzzles/PuzzlesPage.h"
#include "ui/Theme.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>

namespace chess_review::ui {
namespace {
QLabel* label(QWidget* parent, const QString& text = {}) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
void fitWrappedLabel(QLabel& target, int widthHint = 0) {
    const int width = widthHint > 0 ? widthHint : target.width();
    if (target.text().isEmpty() || width <= 0) {
        if (target.minimumHeight() != 0) {
            target.setMinimumHeight(0);
        }
        return;
    }
    const int height = std::max(0, target.heightForWidth(width));
    if (target.minimumHeight() != height) {
        target.setMinimumHeight(height);
        target.updateGeometry();
        if (auto* parent = target.parentWidget()) {
            parent->updateGeometry();
        }
    }
}
class LabelHeightFilter final : public QObject {
  public:
    explicit LabelHeightFilter(QLabel& target, QObject* parent = nullptr)
        : QObject(parent), target_(target) {}

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == &target_ &&
            (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) {
            fitWrappedLabel(target_);
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QLabel& target_;
};
} // namespace
PuzzlesPage::PuzzlesPage(app::PuzzleController& controller,
                         app::PuzzleGenerationController& generation, QWidget* parent)
    : QWidget(parent), controller_(controller), generation_(generation) {
    setObjectName(QStringLiteral("puzzlesPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    generationNotice_ = label(this);
    generationNotice_->setObjectName(QStringLiteral("puzzleGenerationStatus"));
    setTextRole(*generationNotice_, "secondary");
    layout->addWidget(generationNotice_);
    auto* generationBar = new QWidget(this);
    auto* generationActions = new QHBoxLayout(generationBar);
    generationActions->setContentsMargins(0, 0, 0, 0);
    generationActions->setSpacing(8);
    cancel_ = new QPushButton(tr("Pause generation"), generationBar);
    setIcon(*cancel_, QStringLiteral("pause"));
    connect(cancel_, &QPushButton::clicked, &generation_, &app::PuzzleGenerationController::cancel);
    jobs_ = new QComboBox(generationBar);
    jobs_->setAccessibleName(tr("Interrupted puzzle generation"));
    resume_ = new QPushButton(tr("Resume generation"), generationBar);
    setIcon(*resume_, QStringLiteral("play"));
    connect(resume_, &QPushButton::clicked, this, [this] {
        const auto i = jobs_->currentIndex();
        if (i >= 0 && static_cast<std::size_t>(i) < jobIds_.size()) {
            generation_.resume(jobIds_[static_cast<std::size_t>(i)]);
        }
    });
    saved_ = new QPushButton(tr("Open saved puzzle"), generationBar);
    setIcon(*saved_, QStringLiteral("chevron-right"));
    connect(saved_, &QPushButton::clicked, this, [this] {
        if (generation_.state().lastSaved) {
            controller_.open(*generation_.state().lastSaved);
        }
    });
    generationActions->addWidget(cancel_);
    generationActions->addWidget(jobs_, 1);
    generationActions->addWidget(resume_);
    generationActions->addWidget(saved_);
    layout->addWidget(generationBar);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_, 1);
    auto* libraryPage = new QWidget(pages_);
    auto* libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->setSpacing(10);
    auto* headerRow = new QHBoxLayout;
    auto* title = label(libraryPage, tr("Your puzzles"));
    setTextRole(*title, "title");
    headerRow->addWidget(title);
    headerRow->addStretch();
    refresh_ = new QPushButton(libraryPage);
    refresh_->setObjectName(QStringLiteral("refreshPuzzles"));
    setQuietAction(*refresh_);
    setIcon(*refresh_, QStringLiteral("refresh"));
    headerRow->addWidget(refresh_);
    libraryLayout->addLayout(headerRow);
    libraryNotice_ =
        label(libraryPage, tr("Save positions from Analysis or generate puzzles from a completed "
                              "review. Choose any puzzle whenever you wish."));
    libraryNotice_->setObjectName(QStringLiteral("puzzleLibraryStatus"));
    setTextRole(*libraryNotice_, "secondary");
    libraryLayout->addWidget(libraryNotice_);
    library_ = new QTableWidget(0, 3, libraryPage);
    library_->setObjectName(QStringLiteral("puzzleLibrary"));
    library_->setAccessibleName(tr("Saved puzzle library"));
    library_->setHorizontalHeaderLabels({tr("Puzzle"), tr("Origin"), tr("Saved")});
    library_->setSelectionBehavior(QAbstractItemView::SelectRows);
    library_->setSelectionMode(QAbstractItemView::SingleSelection);
    library_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    library_->setShowGrid(false);
    library_->setAlternatingRowColors(true);
    library_->verticalHeader()->hide();
    library_->verticalHeader()->setDefaultSectionSize(36);
    library_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    library_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    library_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    libraryLayout->addWidget(library_, 1);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    open_ = new QPushButton(tr("Solve"), libraryPage);
    open_->setObjectName(QStringLiteral("openPuzzle"));
    setPrimaryAction(*open_);
    setIcon(*open_, QStringLiteral("play"));
    const auto openSelected = [this] {
        const int row = library_->currentRow();
        if (row >= 0 && static_cast<std::size_t>(row) < rows_.size()) {
            controller_.open(rows_[static_cast<std::size_t>(row)]);
        }
    };
    connect(open_, &QPushButton::clicked, this, openSelected);
    connect(library_, &QTableWidget::cellActivated, this,
            [openSelected](int, int) { openSelected(); });
    remove_ = new QPushButton(tr("Delete…"), libraryPage);
    setQuietAction(*remove_);
    setIcon(*remove_, QStringLiteral("trash"));
    connect(remove_, &QPushButton::clicked, this, &PuzzlesPage::removeSelected);
    previous_ = new QPushButton(tr("Previous"), libraryPage);
    next_ = new QPushButton(tr("Next"), libraryPage);
    setQuietAction(*previous_);
    setQuietAction(*next_);
    setIcon(*previous_, QStringLiteral("chevron-left"));
    setIcon(*next_, QStringLiteral("chevron-right"));
    connect(previous_, &QPushButton::clicked, this,
            [this] { controller_.refresh(controller_.state().library.offset - 50); });
    connect(next_, &QPushButton::clicked, this,
            [this] { controller_.refresh(controller_.state().library.offset + 50); });
    connect(refresh_, &QPushButton::clicked, this, [this] {
        if (!controller_.libraryFiltered() && !controller_.state().libraryError.isEmpty()) {
            controller_.refreshCurrent();
        } else {
            controller_.showAllPuzzles();
        }
    });
    actions->addWidget(open_);
    actions->addWidget(remove_);
    actions->addStretch();
    actions->addWidget(previous_);
    actions->addWidget(next_);
    libraryLayout->addLayout(actions);
    pages_->addWidget(libraryPage);
    auto* solvePage = new QWidget(pages_);
    auto* solveLayout = new QVBoxLayout(solvePage);
    solveLayout->setContentsMargins(0, 0, 0, 0);
    solveLayout->setSpacing(10);
    auto* solveTop = new QHBoxLayout;
    auto* back = new QPushButton(tr("Back to library"), solvePage);
    setQuietAction(*back);
    setIcon(*back, QStringLiteral("arrow-left"));
    connect(back, &QPushButton::clicked, &controller_, &app::PuzzleController::showLibrary);
    solveTop->addWidget(back);
    solveTop->addStretch();
    auto* flip = new QPushButton(tr("Flip board"), solvePage);
    setQuietAction(*flip);
    setIcon(*flip, QStringLiteral("flip"));
    connect(flip, &QPushButton::clicked, &controller_, &app::PuzzleController::flip);
    solveTop->addWidget(flip);
    solveLayout->addLayout(solveTop);
    splitter_ = new QSplitter(Qt::Horizontal, solvePage);
    splitter_->setChildrenCollapsible(false);
    board_ = new ChessBoardWidget(splitter_);
    board_->setObjectName(QStringLiteral("puzzleBoard"));
    board_->setMinimumSize(240, 240);
    auto* lesson = new QWidget(splitter_);
    setPanelSurface(*lesson);
    auto* lessonLayout = new QVBoxLayout(lesson);
    lessonLayout->setContentsMargins(18, 16, 18, 16);
    lessonLayout->setSpacing(12);
    message_ = label(lesson);
    message_->setObjectName(QStringLiteral("puzzleMessage"));
    message_->setAccessibleName(tr("Puzzle feedback"));
    setTextRole(*message_, "heading");
    message_->installEventFilter(new LabelHeightFilter(*message_, message_));
    lessonLayout->addWidget(message_);
    auto* hintBox = new QWidget(lesson);
    setSubtleSurface(*hintBox);
    auto* hintLayout = new QVBoxLayout(hintBox);
    hintLayout->setContentsMargins(12, 8, 12, 8);
    hints_ = label(hintBox);
    hints_->setObjectName(QStringLiteral("puzzleHints"));
    hints_->installEventFilter(new LabelHeightFilter(*hints_, hints_));
    hintLayout->addWidget(hints_);
    lessonLayout->addWidget(hintBox);
    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    hint_ = new QPushButton(tr("Hint"), lesson);
    retry_ = new QPushButton(tr("Retry"), lesson);
    reveal_ = new QPushButton(tr("Reveal"), lesson);
    hint_->setObjectName(QStringLiteral("hintPuzzle"));
    retry_->setObjectName(QStringLiteral("retryPuzzle"));
    reveal_->setObjectName(QStringLiteral("revealPuzzle"));
    setPrimaryAction(*hint_);
    setIcon(*hint_, QStringLiteral("hint"));
    setIcon(*retry_, QStringLiteral("retry"));
    setQuietAction(*reveal_);
    setIcon(*reveal_, QStringLiteral("eye"));
    connect(retry_, &QPushButton::clicked, &controller_, &app::PuzzleController::retry);
    connect(hint_, &QPushButton::clicked, &controller_, &app::PuzzleController::hint);
    connect(reveal_, &QPushButton::clicked, &controller_, &app::PuzzleController::reveal);
    controls->addWidget(hint_, 1);
    controls->addWidget(retry_);
    controls->addWidget(reveal_);
    lessonLayout->addLayout(controls);
    auto* lineControls = new QHBoxLayout;
    lineControls->setSpacing(8);
    stepBack_ = new QPushButton(tr("Previous move"), lesson);
    stepNext_ = new QPushButton(tr("Next move"), lesson);
    stepBack_->setObjectName(QStringLiteral("previousPuzzleMove"));
    stepNext_->setObjectName(QStringLiteral("nextPuzzleMove"));
    setIcon(*stepBack_, QStringLiteral("chevron-left"));
    setIcon(*stepNext_, QStringLiteral("chevron-right"));
    connect(stepBack_, &QPushButton::clicked, &controller_, &app::PuzzleController::previous);
    connect(stepNext_, &QPushButton::clicked, &controller_, &app::PuzzleController::next);
    lineControls->addWidget(stepBack_, 1);
    lineControls->addWidget(stepNext_, 1);
    lessonLayout->addLayout(lineControls);
    lessonLayout->addWidget(makeSeparator(lesson));
    analyze_ = new QPushButton(tr("Analyze"), lesson);
    analyze_->setObjectName(QStringLiteral("analyzePuzzle"));
    setQuietAction(*analyze_);
    setIcon(*analyze_, QStringLiteral("analyze"));
    connect(analyze_, &QPushButton::clicked, &controller_, &app::PuzzleController::openAnalysis);
    lessonLayout->addWidget(analyze_, 0, Qt::AlignLeft);
    lessonLayout->addStretch();
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    solveLayout->addWidget(splitter_, 1);
    pages_->addWidget(solvePage);
    connect(board_, &ChessBoardWidget::moveAttempted, &controller_,
            &app::PuzzleController::attemptMove);
    connect(board_, &ChessBoardWidget::squareSelected, &controller_,
            &app::PuzzleController::selectSquare);
    connect(board_, &ChessBoardWidget::selectionCleared, &controller_,
            &app::PuzzleController::clearSelection);
    connect(board_, &ChessBoardWidget::escapeRequested, &controller_,
            &app::PuzzleController::clearSelection);
    connect(board_, &ChessBoardWidget::previousRequested, &controller_,
            &app::PuzzleController::previous);
    connect(board_, &ChessBoardWidget::nextRequested, &controller_, &app::PuzzleController::next);
    connect(&controller_, &app::PuzzleController::moveFeedback, board_,
            &ChessBoardWidget::flashMove);
    connect(&controller_, &app::PuzzleController::changed, this, &PuzzlesPage::refresh);
    connect(&generation_, &app::PuzzleGenerationController::changed, this,
            &PuzzlesPage::refreshGeneration);
    connect(library_, &QTableWidget::itemSelectionChanged, this, [this] {
        open_->setEnabled(library_->currentRow() >= 0);
        remove_->setEnabled(library_->currentRow() >= 0);
    });
    refresh();
    refreshGeneration();
}
void PuzzlesPage::setBoardDisplayOptions(bool coordinates, bool animation) {
    board_->setDisplayOptions(coordinates, animation);
}
void PuzzlesPage::refresh() {
    const auto& state = controller_.state();
    pages_->setCurrentIndex(state.solving ? 1 : 0);
    board_->setState(controller_.board());
    message_->setText(state.error.isEmpty()
                          ? (state.loading ? tr("Opening puzzle…") : state.message)
                          : state.error);
    fitWrappedLabel(*message_);
    auto* hintBox = hints_->parentWidget();
    const bool wasVisible = hintBox->isVisible();
    hints_->setText(state.hints.join(QLatin1Char('\n')));
    hintBox->setVisible(!state.hints.empty());
    hints_->setVisible(!state.hints.empty());
    int hintWidth = 0;
    if (!state.hints.empty() && !wasVisible) {
        if (auto* lesson = hintBox->parentWidget(); lesson && lesson->width() > 0) {
            const auto lessonMargins =
                lesson->layout() ? lesson->layout()->contentsMargins() : QMargins{};
            const auto boxMargins =
                hintBox->layout() ? hintBox->layout()->contentsMargins() : QMargins{};
            hintWidth = lesson->width() - lessonMargins.left() - lessonMargins.right() -
                        boxMargins.left() - boxMargins.right() - 2;
        }
    }
    fitWrappedLabel(*hints_, hintWidth);
    retry_->setEnabled(state.selected.has_value() && !state.loading && !state.grading);
    hint_->setEnabled(controller_.canHint());
    reveal_->setEnabled(state.selected.has_value() && !state.loading && !state.revealed);
    analyze_->setEnabled(state.selected.has_value() && !state.loading);
    stepBack_->setEnabled(controller_.canStepBack());
    stepNext_->setEnabled(controller_.canStepForward());
    stepBack_->setVisible(state.revealed);
    stepNext_->setVisible(state.revealed);
    std::vector<persistence::PuzzleId> ids;
    for (const auto& row : state.library.puzzles) {
        ids.push_back(row.id);
    }
    if (ids != rows_) {
        const auto row = library_->currentRow();
        const auto previousId = row >= 0 && static_cast<std::size_t>(row) < rows_.size()
                                    ? std::optional{rows_[static_cast<std::size_t>(row)]}
                                    : std::nullopt;
        const QSignalBlocker block(library_);
        rows_ = std::move(ids);
        library_->setRowCount(static_cast<int>(rows_.size()));
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto& item = state.library.puzzles[i];
            const auto title = item.gameId ? tr("Puzzle %1 · Game position").arg(item.id)
                                           : tr("Puzzle %1").arg(item.id);
            library_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(title));
            library_->setItem(static_cast<int>(i), 1,
                              new QTableWidgetItem(item.origin == puzzles::Origin::Generated
                                                       ? tr("Generated")
                                                       : tr("Manual")));
            library_->setItem(static_cast<int>(i), 2,
                              new QTableWidgetItem(QLocale().toString(item.createdAt.toLocalTime(),
                                                                      QLocale::ShortFormat)));
        }
        if (!rows_.empty()) {
            const auto selected = previousId ? std::ranges::find(rows_, *previousId) : rows_.end();
            library_->selectRow(
                selected == rows_.end() ? 0 : static_cast<int>(selected - rows_.begin()));
        }
    }
    open_->setEnabled(library_->currentRow() >= 0);
    remove_->setEnabled(library_->currentRow() >= 0);
    const bool loading = controller_.libraryLoading();
    const bool ready = controller_.libraryReady();
    const bool filtered = controller_.libraryFiltered();
    previous_->setEnabled(ready && !loading && state.library.offset > 0);
    next_->setEnabled(ready && !loading && state.library.hasNext);
    refresh_->setEnabled(ready && !loading);
    refresh_->setText(filtered                        ? tr("Show all puzzles")
                      : !state.libraryError.isEmpty() ? tr("Retry loading")
                                                      : tr("Refresh"));
    libraryNotice_->setText(!ready    ? tr("Waiting for the puzzle library…")
                            : loading ? tr("Loading saved puzzles…")
                            : !state.libraryError.isEmpty() ? state.libraryError
                            : !state.error.isEmpty()        ? state.error
                            : rows_.empty() && filtered ? tr("No puzzles saved for this review.")
                            : rows_.empty()
                                ? tr("No puzzles saved yet. Generate puzzles from a completed "
                                     "review, or save a position from Analysis.")
                                : tr("Choose any puzzle whenever you wish. Solutions stay hidden "
                                     "until you solve or reveal them."));
    std::vector<persistence::PuzzleJobId> jobIds;
    for (const auto& job : state.interrupted) {
        jobIds.push_back(job.id);
    }
    if (jobIds != jobIds_) {
        jobIds_ = std::move(jobIds);
        jobs_->clear();
        for (const auto& job : state.interrupted) {
            jobs_->addItem(
                job.reviewId
                    ? tr("Review %1 · %2/%3 candidates")
                          .arg(*job.reviewId)
                          .arg(job.current)
                          .arg(job.total)
                    : tr("Manual save · %1/%2 candidates").arg(job.current).arg(job.total));
        }
    }
    refreshGeneration();
}
void PuzzlesPage::refreshGeneration() {
    const auto& state = generation_.state();
    generationNotice_->setText(state.error.isEmpty() ? state.message : state.error);
    generationNotice_->setVisible(!generationNotice_->text().isEmpty());
    cancel_->setVisible(state.active);
    cancel_->setEnabled(!state.cancelling);
    jobs_->setVisible(!jobIds_.empty());
    resume_->setVisible(!jobIds_.empty());
    resume_->setEnabled(!state.active);
    saved_->setVisible(state.lastSaved.has_value());
    cancel_->parentWidget()->setVisible(state.active || !jobIds_.empty() ||
                                        state.lastSaved.has_value());
}
void PuzzlesPage::removeSelected() {
    const int row = library_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return;
    }
    const auto id = rows_[static_cast<std::size_t>(row)];
    auto* dialog = new QMessageBox(QMessageBox::Question, tr("Delete puzzle"),
                                   tr("Delete this puzzle? Automatic generation will remember your "
                                      "choice. You can save the position manually again."),
                                   QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(dialog, &QMessageBox::finished, this, [this, id](int answer) {
        if (answer == QMessageBox::Yes) {
            controller_.remove(id);
        }
    });
    dialog->open();
}
void PuzzlesPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const auto outer = layout()->contentsMargins();
    const auto inner = splitter_->parentWidget()->layout()->contentsMargins();
    const int available = width() - outer.left() - outer.right() - inner.left() - inner.right();
    const int required = std::max(320, board_->minimumWidth()) +
                         splitter_->widget(1)->minimumSizeHint().width() + splitter_->handleWidth();
    splitter_->setOrientation(available < required ? Qt::Vertical : Qt::Horizontal);
}
} // namespace chess_review::ui
