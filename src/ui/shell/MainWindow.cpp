#include "ui/shell/MainWindow.h"
#include "app/EngineAnalysisController.h"
#include "app/ReviewWorkspaceController.h"
#include "ui/Theme.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/games/GamesPage.h"
#include "ui/games/PgnTextEdit.h"
#include "ui/puzzles/PuzzlesPage.h"
#include "ui/review/ReviewPage.h"
#include "ui/settings/EngineSettingsPage.h"
#include "ui/shell/StartupPanel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWindow>
#include <algorithm>
#include <map>
#include <set>

namespace chess_review::ui {
namespace {
// Hidden workspaces must not force the active page beyond the scroll viewport.
// Each active page keeps its own real minimum; the shell still scrolls when needed.
class DestinationStack final : public QStackedWidget {
  public:
    explicit DestinationStack(QWidget* parent) : QStackedWidget(parent) {
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override {
        const auto* page = currentWidget();
        return page ? page->sizeHint() : QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override {
        const auto* page = currentWidget();
        return page ? page->minimumSizeHint().expandedTo(page->minimumSize())
                    : QStackedWidget::minimumSizeHint();
    }
    bool hasHeightForWidth() const override {
        const auto* page = currentWidget();
        return page && page->hasHeightForWidth();
    }
    int heightForWidth(int width) const override {
        const auto* page = currentWidget();
        return page ? page->heightForWidth(width) : -1;
    }
};
} // namespace
QRect fittedWindowGeometry(const QRect& desired, const QRect& available) {
    if (available.isEmpty()) {
        return desired;
    }
    const QSize size = desired.size().boundedTo(available.size()).expandedTo(QSize(1, 1));
    const int x = std::clamp(desired.x(), available.left(), available.right() - size.width() + 1);
    const int y = std::clamp(desired.y(), available.top(), available.bottom() - size.height() + 1);
    return {QPoint(x, y), size};
}
MainWindow::MainWindow(app::NavigationController& navigation, QWidget* parent)
    : QMainWindow(parent), navigation_(navigation) {
    installTheme();
    setWindowTitle(QStringLiteral("chess-review"));
    auto* file = menuBar()->addMenu(tr("&File"));
    file->setObjectName(QStringLiteral("fileMenu"));
    auto* quit = file->addAction(tr("&Quit"));
    quit->setObjectName(QStringLiteral("quitAction"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto* view = menuBar()->addMenu(tr("&View"));
    view->setObjectName(QStringLiteral("viewMenu"));
    auto* central = new QWidget(this);
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(12, 12, 12, 12);
    startup_ = new StartupPanel(central);
    connect(startup_, &StartupPanel::closeRequested, this, &QWidget::close);
    column->addWidget(startup_);
    preferencesNotice_ = new QLabel(central);
    preferencesNotice_->setObjectName(QStringLiteral("uiStateNotice"));
    preferencesNotice_->setTextFormat(Qt::PlainText);
    preferencesNotice_->setWordWrap(true);
    preferencesNotice_->hide();
    column->addWidget(preferencesNotice_);
    auto* row = new QHBoxLayout;
    auto* sidebar = new QWidget(central);
    sidebar->setAccessibleName(tr("Destinations"));
    sidebar->setObjectName(QStringLiteral("navigationRail"));
    auto* buttons = new QVBoxLayout(sidebar);
    buttons->setContentsMargins(0, 0, 12, 0);
    pages_ = new DestinationStack(central);
    pages_->setObjectName(QStringLiteral("destinationPages"));
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    const std::array labels{tr("&Home"), tr("&Games"), tr("&Puzzles"), tr("&Analyze"),
                            tr("&Settings")};
    const std::array values{app::Destination::Home, app::Destination::Games,
                            app::Destination::Puzzles, app::Destination::Analyze,
                            app::Destination::Settings};
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto* action = new QAction(labels[i], this);
        action->setObjectName(app::destinationId(values[i]) + QStringLiteral("Action"));
        action->setCheckable(true);
        action->setData(QVariant::fromValue(values[i]));
        group->addAction(action);
        view->addAction(action);
        destinations_[i] = action;
        connect(action, &QAction::triggered, this, [this, destination = values[i]] {
            if (!closing_) {
                if (reviewController_) {
                    reviewController_->workspace().suppressRestoration();
                }
                if (navigation_.current() == destination) {
                    selectDestination();
                } else {
                    navigation_.navigate(destination);
                }
            }
        });
        auto* button = new QToolButton(sidebar);
        button->setObjectName(app::destinationId(values[i]) + QStringLiteral("Button"));
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        buttons->addWidget(button);
        auto* page = new QWidget(pages_);
        page->setObjectName(app::destinationId(values[i]) + QStringLiteral("Page"));
        auto* layout = new QVBoxLayout(page);
        auto* heading = new QLabel(QString(labels[i]).remove(QLatin1Char('&')), page);
        heading->setTextFormat(Qt::PlainText);
        setTextRole(*heading, "title");
        layout->addWidget(heading);
        layout->addStretch();
        pages_->addWidget(page);
    }
    buttons->addStretch();
    row->addWidget(sidebar);
    row->addWidget(pages_, 1);
    column->addLayout(row, 1);
    // Keep every command reachable when enlarged fonts/errors exceed a small work area.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("shellScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(central);
    setCentralWidget(scroll);
    view->addSeparator();
    reset_ = view->addAction(tr("Reset &Layout"));
    reset_->setObjectName(QStringLiteral("resetLayoutAction"));
    connect(reset_, &QAction::triggered, this, &MainWindow::resetLayout);
    auto* help = menuBar()->addMenu(tr("&Help"));
    auto* diagnostics = help->addAction(tr("&Diagnostics…"));
    diagnostics->setObjectName(QStringLiteral("diagnosticsAction"));
    connect(diagnostics, &QAction::triggered, startup_, &StartupPanel::showDiagnostics);
    auto* artwork = help->addAction(tr("Chess Piece Artwork License…"));
    connect(artwork, &QAction::triggered, this, [this] {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Chess piece artwork license"));
        auto* layout = new QVBoxLayout(dialog);
        auto* text = new QPlainTextEdit(dialog);
        text->setReadOnly(true);
        text->setAccessibleName(tr("Piece artwork license and provenance"));
        QFile license(QStringLiteral(":/pieces/LICENSE.txt"));
        QFile provenance(QStringLiteral(":/pieces/provenance.json"));
        if (license.open(QIODevice::ReadOnly) && provenance.open(QIODevice::ReadOnly)) {
            text->setPlainText(QString::fromUtf8(license.readAll()) + QLatin1Char('\n') +
                               QString::fromUtf8(provenance.readAll()));
        } else {
            text->setPlainText(
                tr("Cburnett pieces by Colin M. L. Burnett and contributors. "
                   "See assets/pieces/cburnett/LICENSE.txt in the source distribution."));
        }
        layout->addWidget(text);
        auto* licenseButtons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(licenseButtons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(licenseButtons);
        dialog->resize(640, 480);
        dialog->show();
    });
    auto* about = help->addAction(tr("&About chess-review…"));
    about->setMenuRole(QAction::AboutRole);
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, tr("About chess-review"),
            tr("chess-review %1\nQt %2\nLocal chess review")
                .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    });
    connect(&navigation_, &app::NavigationController::destinationChanged, this,
            &MainWindow::selectDestination);
    selectDestination();
    resetWindowGeometry();
    // Screen removal/changes are handled through Qt's current window screen.
    connect(qApp, &QGuiApplication::screenRemoved, this,
            [this] { QTimer::singleShot(0, this, &MainWindow::fitToScreen); });
}
MainWindow::MainWindow(app::NavigationController& navigation, app::AnalysisController& analysis,
                       QWidget* parent)
    : MainWindow(navigation, parent) {
    installAnalysis(analysis);
}
void MainWindow::setVerbose(bool verbose) {
    verbose_ = verbose;
    if (reviewPage_) {
        reviewPage_->setVerbose(verbose);
    }
    if (auto* settings = pages_->findChild<EngineSettingsPage*>()) {
        settings->setVerbose(verbose);
    }
    if (auto* games = pages_->findChild<GamesPage*>()) {
        games->setVerbose(verbose);
    }
}
void MainWindow::installPuzzles(app::PuzzleController& puzzles,
                                app::PuzzleGenerationController& generation,
                                app::ReviewConfiguration& configuration) {
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("puzzlesPage"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(placeholder);
    const int index = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    auto* page = new PuzzlesPage(puzzles, generation, pages_);
    if (analysis_) {
        page->setPreferences(analysis_->preferences());
    }
    pages_->insertWidget(index, page);
    puzzleAnalysis_ = new AnalysisPage(puzzles.linkedAnalysis(), pages_);
    puzzleAnalysis_->setObjectName(QStringLiteral("puzzleAnalysisPage"));
    puzzleAnalysis_->installEngineAnalysis(puzzles.linkedEngine());
    shareAnalysisLineCount(*puzzleAnalysis_);
    pages_->addWidget(puzzleAnalysis_);
    auto* back = new QPushButton(tr("Back to puzzle"), puzzleAnalysis_);
    back->setObjectName(QStringLiteral("backToPuzzle"));
    puzzleAnalysis_->findChild<QHBoxLayout*>(QStringLiteral("analysisCommands"))->addWidget(back);
    connect(back, &QPushButton::clicked, this,
            [this] { navigation_.navigate(app::Destination::Puzzles); });
    connect(&navigation_, &app::NavigationController::destinationChanged, this,
            [this] { ++puzzleNavigationEpoch_; });
    connect(&puzzles, &app::PuzzleController::analysisRequested, this, [this, &puzzles] {
        const auto epoch = puzzleNavigationEpoch_;
        puzzleAnalysis_->requestDiscard([this, &puzzles, epoch] {
            if (closing_ || epoch != puzzleNavigationEpoch_ || !puzzles.enterAnalysis()) {
                return;
            }
            if (analysis_) {
                puzzleAnalysis_->setPreferences(analysis_->preferences());
            }
            navigation_.navigate(app::Destination::Analyze);
            pages_->setCurrentWidget(puzzleAnalysis_);
        });
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this, &puzzles, page] {
        puzzles.linkedEngine().setVisible(!closing_ && pages_->currentWidget() == puzzleAnalysis_);
        if (!closing_ && pages_->currentWidget() == page) {
            puzzles.refreshCurrent();
        }
    });
    connect(puzzleAnalysis_, &AnalysisPage::manageEnginesRequested, this,
            [this] { navigation_.navigate(app::Destination::Settings); });
    const auto addSave = [this, &generation](AnalysisPage* analysisPage,
                                             app::EngineAnalysisController* engine) {
        if (!analysisPage) {
            return;
        }
        auto* commands = analysisPage->findChild<QHBoxLayout*>(QStringLiteral("analysisCommands"));
        auto* save = new QPushButton(tr("Save Position as Puzzle"), analysisPage);
        save->setObjectName(QStringLiteral("savePositionAsPuzzle"));
        commands->addWidget(save);
        auto* notice = new QLabel(analysisPage);
        notice->setObjectName(QStringLiteral("manualPuzzleStatus"));
        notice->setTextFormat(Qt::PlainText);
        notice->setWordWrap(true);
        qobject_cast<QVBoxLayout*>(analysisPage->layout())->insertWidget(1, notice);
        auto* openSaved = new QPushButton(tr("Open saved puzzle"), analysisPage);
        openSaved->setObjectName(QStringLiteral("openManualPuzzle"));
        commands->addWidget(openSaved);
        const auto update = [save, notice, openSaved, analysisPage, engine, &generation] {
            save->setEnabled(generation.canSavePosition() && (!engine || !engine->preview()) &&
                             !analysisPage->controller().busy() &&
                             analysisPage->controller().position());
            const auto& state = generation.state();
            notice->setText(state.error.isEmpty() ? state.message : state.error);
            notice->setVisible(!notice->text().isEmpty());
            openSaved->setVisible(state.lastSaved.has_value());
        };
        connect(&generation, &app::PuzzleGenerationController::changed, analysisPage, update);
        connect(&analysisPage->controller(), &app::AnalysisController::changed, analysisPage,
                update);
        if (engine) {
            connect(engine, &app::EngineAnalysisController::changed, analysisPage, update);
        }
        connect(save, &QPushButton::clicked, analysisPage, [analysisPage, engine, &generation] {
            if (engine) {
                engine->stop();
            }
            generation.savePosition(analysisPage->controller().position(),
                                    analysisPage->controller().gameId());
        });
        connect(openSaved, &QPushButton::clicked, this, [this, &generation] {
            if (generation.state().lastSaved) {
                navigation_.navigate(app::Destination::Puzzles);
            }
        });
        update();
    };
    addSave(analysis_, liveEngine_);
    if (reviewController_) {
        addSave(linkedAnalysis_, &reviewController_->exploration().linkedEngine());
    }
    addSave(puzzleAnalysis_, &puzzles.linkedEngine());
    for (auto* openSaved : findChildren<QPushButton*>(QStringLiteral("openManualPuzzle"))) {
        connect(openSaved, &QPushButton::clicked, &puzzles, [&puzzles, &generation] {
            if (generation.state().lastSaved) {
                puzzles.open(*generation.state().lastSaved);
            }
        });
    }
    if (auto* settings = pages_->findChild<EngineSettingsPage*>()) {
        auto* automatic =
            new QCheckBox(tr("Automatically generate puzzles after completed reviews"), settings);
        automatic->setObjectName(QStringLiteral("automaticPuzzles"));
        auto* error = new QLabel(settings);
        error->setTextFormat(Qt::PlainText);
        error->setWordWrap(true);
        auto* layout = qobject_cast<QVBoxLayout*>(settings->layout());
        layout->insertWidget(1, automatic);
        layout->insertWidget(2, error);
        connect(automatic, &QCheckBox::toggled, &configuration,
                &app::ReviewConfiguration::setAutomaticPuzzles);
        const auto update = [automatic, error, &configuration] {
            const QSignalBlocker block(automatic);
            automatic->setEnabled(configuration.canSavePuzzles());
            if (const auto c = configuration.snapshot()) {
                automatic->setChecked(c->puzzles.automatic);
            }
            error->setText(configuration.puzzleSettingsError());
            error->setVisible(!error->text().isEmpty());
        };
        connect(&configuration, &app::ReviewConfiguration::changed, settings, update);
        update();
    }
    if (reviewController_ && reviewPage_) {
        auto* summary = reviewPage_->findChild<QWidget*>(QStringLiteral("reviewOverviewPanel"));
        auto* layout = qobject_cast<QVBoxLayout*>(summary->layout());
        auto* count = new QLabel(summary);
        count->setObjectName(QStringLiteral("reviewPuzzleSummary"));
        count->setTextFormat(Qt::PlainText);
        count->setWordWrap(true);
        auto* generate = new QPushButton(tr("Generate Puzzles"), summary);
        generate->setObjectName(QStringLiteral("generateReviewPuzzles"));
        auto* library = new QPushButton(tr("Open puzzles from this review"), summary);
        layout->addWidget(count);
        layout->addWidget(generate);
        layout->addWidget(library);
        const auto update = [this, count, generate, library, &puzzles, &generation] {
            const auto& run = reviewController_->state().run;
            if (run) {
                puzzles.inspectReview(run->id);
            }
            const auto& saved = puzzles.reviewSummary();
            const bool ready = run && run->status == review::JobStatus::Complete;
            const bool same = run && saved.id == run->id;
            count->setVisible(ready);
            generate->setVisible(ready);
            library->setVisible(ready && same && saved.count > 0);
            count->setText(same ? tr("%1%2 saved puzzle(s)")
                                      .arg(saved.count)
                                      .arg(saved.more ? QStringLiteral("+") : QString{})
                                : tr("Loading puzzles…"));
            const bool resumable =
                same && saved.generation &&
                saved.generation->status != persistence::PuzzleJobStatus::Complete;
            generate->setText(resumable ? tr("Resume puzzle generation") : tr("Generate Puzzles"));
            generate->setEnabled(ready && !generation.state().active &&
                                 (!same || !saved.generation || resumable));
        };
        connect(generate, &QPushButton::clicked, this, [this, &puzzles, &generation] {
            const auto& run = reviewController_->state().run;
            if (!run) {
                return;
            }
            const auto& saved = puzzles.reviewSummary();
            if (saved.id == run->id && saved.generation &&
                saved.generation->status != persistence::PuzzleJobStatus::Complete) {
                generation.resume(saved.generation->id);
            } else {
                generation.generate(run->id);
            }
        });
        connect(library, &QPushButton::clicked, this, [this, &puzzles] {
            if (const auto& run = reviewController_->state().run) {
                puzzles.refresh(0, run->id);
            }
            navigation_.navigate(app::Destination::Puzzles);
        });
        connect(&puzzles, &app::PuzzleController::reviewSummaryChanged, reviewPage_, update);
        connect(reviewController_, &app::ReviewGameController::changed, reviewPage_, update);
        connect(&generation, &app::PuzzleGenerationController::changed, reviewPage_, update);
        connect(&generation, &app::PuzzleGenerationController::libraryChanged, &puzzles,
                [this, &puzzles] {
                    if (const auto& run = reviewController_->state().run) {
                        puzzles.inspectReview(run->id, true);
                    }
                });
        update();
    }
    auto* home =
        pages_->findChild<QWidget*>(QStringLiteral("homePage"), Qt::FindDirectChildrenOnly);
    auto* shortcut = new QPushButton(tr("Open puzzle library"), home);
    qobject_cast<QVBoxLayout*>(home->layout())->insertWidget(1, shortcut);
    connect(shortcut, &QPushButton::clicked, this,
            [this] { navigation_.navigate(app::Destination::Puzzles); });
    selectDestination();
}
void MainWindow::installReview(app::ReviewGameController& review) {
    reviewController_ = &review;
    reviewPage_ = new ReviewPage(review, pages_);
    reviewPage_->setVerbose(verbose_);
    pages_->addWidget(reviewPage_);
    reviewPage_->setSplitShare(reviewSplit_);
    auto& linkedExplorer = review.exploration();
    linkedAnalysis_ = new AnalysisPage(linkedExplorer.linkedAnalysis(), pages_);
    linkedAnalysis_->setObjectName(QStringLiteral("reviewLinkedAnalysisPage"));
    linkedAnalysis_->installEngineAnalysis(linkedExplorer.linkedEngine());
    shareAnalysisLineCount(*linkedAnalysis_);
    pages_->addWidget(linkedAnalysis_);
    auto* linkedCommands =
        linkedAnalysis_->findChild<QHBoxLayout*>(QStringLiteral("analysisCommands"));
    auto* backToReview = new QPushButton(tr("Back to Review"), linkedAnalysis_);
    backToReview->setObjectName(QStringLiteral("backToReview"));
    linkedCommands->addWidget(backToReview);
    connect(backToReview, &QPushButton::clicked, this, [this] {
        reviewController_->exploration().restoreReviewOrigin();
        showReview();
    });
    connect(linkedAnalysis_, &AnalysisPage::manageEnginesRequested, this,
            [this] { navigation_.navigate(app::Destination::Settings); });
    connect(reviewPage_, &ReviewPage::analyzeRequested, this, [this] {
        auto& exploration = reviewController_->exploration();
        const auto requestedOrigin = exploration.analysisOrigin();
        if (!requestedOrigin) {
            return;
        }
        const auto open = [this, requestedOrigin] {
            if (closing_) {
                return;
            }
            auto& currentExplorer = reviewController_->exploration();
            if (currentExplorer.analysisOrigin() != requestedOrigin) {
                return;
            }
            const bool resume = currentExplorer.sameAnalysisOrigin();
            const auto origin = currentExplorer.analysisOrigin();
            currentExplorer.openAnalysis();
            if (analysis_ && !resume && origin) {
                auto preferences = analysis_->preferences();
                preferences.blackAtBottom = origin->blackAtBottom;
                linkedAnalysis_->setPreferences(preferences);
            }
            linkedAnalysisPending_ = true;
            if (navigation_.current() == app::Destination::Analyze) {
                selectDestination();
            } else {
                navigation_.navigate(app::Destination::Analyze);
            }
        };
        if (!exploration.sameAnalysisOrigin()) {
            linkedAnalysis_->requestDiscard(open);
        } else {
            open();
        }
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this] {
        if (reviewController_) {
            reviewController_->exploration().linkedEngine().setVisible(
                !closing_ && pages_->currentWidget() == linkedAnalysis_);
        }
    });
    auto* games = pages_->findChild<GamesPage*>();
    Q_ASSERT(games);
    connect(games, &GamesPage::reviewRequested, this, [this, &review](persistence::GameId id) {
        if (closing_) {
            return;
        }
        review.selectGame(id);
        showReview();
    });
    connect(reviewPage_, &ReviewPage::backRequested, this, [this] {
        navigation_.navigate(app::Destination::Games);
        selectDestination();
    });
    connect(reviewPage_, &ReviewPage::settingsRequested, this,
            [this] { navigation_.navigate(app::Destination::Settings); });
    auto* scroll = qobject_cast<QScrollArea*>(centralWidget());
    auto* task = new QPushButton(scroll->widget());
    task->setObjectName(QStringLiteral("reviewTaskStatus"));
    task->setAccessibleName(tr("Background review status; return to review"));
    qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->insertWidget(2, task);
    task->hide();
    connect(task, &QPushButton::clicked, &review.workspace(),
            &app::ReviewWorkspaceController::returnToTask);
    connect(&review.workspace(), &app::ReviewWorkspaceController::showRequested, this,
            &MainWindow::showReview);
    connect(&review.workspace(), &app::ReviewWorkspaceController::restoreFailed, this, [this] {
        if (closing_) {
            return;
        }
        const auto& state = reviewController_->state();
        preferencesNotice_->setText(state.error.isEmpty() ? state.notice : state.error);
        preferencesNotice_->setVisible(!preferencesNotice_->text().isEmpty());
        navigation_.navigate(app::Destination::Games);
        selectDestination();
    });
    const auto updateTask = [this, task, &review] {
        const auto& state = review.taskState();
        if (state.active && !reviewWasActive_) {
            reviewLeaveAllowed_ = false;
        }
        reviewWasActive_ = state.active;
        const bool viewingTask = pages_->currentWidget() == reviewPage_ && review.state().run &&
                                 state.run && review.state().run->id == state.run->id;
        const bool onHome = pages_->currentWidget() == pages_->widget(0);
        task->setVisible(!viewingTask && (state.active || (!onHome && state.run.has_value()) ||
                                          !state.error.isEmpty()));
        task->setText(state.active ? tr("Review in progress — Return to review")
                                   : tr("Open your saved review"));
    };
    connect(&review, &app::ReviewGameController::changed, this, updateTask);
    connect(&review, &app::ReviewGameController::taskChanged, this, updateTask);
    connect(pages_, &QStackedWidget::currentChanged, this, [this, &review, updateTask] {
        review.workspace().setVisible(pages_->currentWidget() == reviewPage_ ||
                                      pages_->currentWidget() == linkedAnalysis_);
        updateTask();
    });
    auto* home = pages_->widget(0);
    auto* resumeReview = new QPushButton(tr("Resume review"), home);
    resumeReview->setObjectName(QStringLiteral("homeResumeReview"));
    resumeReview->hide();
    qobject_cast<QVBoxLayout*>(home->layout())->insertWidget(3, resumeReview, 0, Qt::AlignLeft);
    connect(resumeReview, &QPushButton::clicked, this, &MainWindow::showReview);
    connect(&review, &app::ReviewGameController::changed, resumeReview,
            [&review, resumeReview] { resumeReview->setVisible(review.state().run.has_value()); });
    auto* recovery = new QWidget(home);
    recovery->setObjectName(QStringLiteral("reviewRecovery"));
    auto* recoveryLayout = new QVBoxLayout(recovery);
    auto* recoveryNotice = new QLabel(tr("You have unfinished reviews."), recovery);
    recoveryNotice->setWordWrap(true);
    recoveryLayout->addWidget(recoveryNotice);
    auto* showRecovery = new QPushButton(tr("Unfinished reviews"), recovery);
    showRecovery->setObjectName(QStringLiteral("showInterruptedReviews"));
    showRecovery->setCheckable(true);
    recoveryLayout->addWidget(showRecovery);
    auto* recoveryDetails = new QWidget(recovery);
    recoveryDetails->setObjectName(QStringLiteral("interruptedReviewList"));
    auto* detailsLayout = new QVBoxLayout(recoveryDetails);
    auto* recoveryRows = new QListWidget(recoveryDetails);
    recoveryRows->setObjectName(QStringLiteral("interruptedReviewRows"));
    recoveryRows->setAccessibleName(tr("Interrupted reviews"));
    recoveryRows->setSelectionMode(QAbstractItemView::SingleSelection);
    recoveryRows->setMinimumHeight(100);
    recoveryRows->setMaximumHeight(240);
    detailsLayout->addWidget(recoveryRows);
    auto* openRecovery = new QPushButton(tr("Open selected review"), recoveryDetails);
    openRecovery->setObjectName(QStringLiteral("openInterruptedReview"));
    openRecovery->setEnabled(false);
    detailsLayout->addWidget(openRecovery);
    const auto openSelected = [this, recoveryRows, &review] {
        const auto* item = recoveryRows->currentItem();
        if (closing_ || !item || !item->data(Qt::UserRole + 2).toBool()) {
            return;
        }
        review.workspace().selectGame(item->data(Qt::UserRole + 1).toLongLong(),
                                      item->data(Qt::UserRole).toLongLong());
        showReview();
    };
    connect(openRecovery, &QPushButton::clicked, this, openSelected);
    connect(recoveryRows, &QListWidget::itemActivated, this,
            [openSelected](QListWidgetItem*) { openSelected(); });
    connect(recoveryRows, &QListWidget::currentRowChanged, this, [recoveryRows, openRecovery] {
        const auto* item = recoveryRows->currentItem();
        openRecovery->setEnabled(item && item->data(Qt::UserRole + 2).toBool());
    });
    recoveryLayout->addWidget(recoveryDetails);
    recoveryDetails->hide();
    connect(showRecovery, &QPushButton::toggled, recoveryDetails, &QWidget::setVisible);
    connect(showRecovery, &QPushButton::toggled, this, [showRecovery](bool shown) {
        showRecovery->setText(shown ? tr("Hide interrupted reviews") : tr("Unfinished reviews"));
    });
    auto* recoveryError = new QLabel(recovery);
    recoveryError->setWordWrap(true);
    recoveryError->setTextFormat(Qt::PlainText);
    recoveryLayout->addWidget(recoveryError);
    auto* controls = new QHBoxLayout;
    auto* earlier = new QPushButton(tr("Previous interrupted reviews"), recovery);
    auto* later = new QPushButton(tr("Next interrupted reviews"), recovery);
    auto* refresh = new QPushButton(tr("Refresh interrupted reviews"), recovery);
    controls->addWidget(earlier);
    controls->addWidget(later);
    controls->addWidget(refresh);
    detailsLayout->addLayout(controls);
    qobject_cast<QVBoxLayout*>(home->layout())->insertWidget(home->layout()->count() - 1, recovery);
    const auto updateRecovery = [this, recovery, recoveryRows, recoveryError, earlier, later,
                                 refresh, openRecovery, &review] {
        auto& workspace = review.workspace();
        const auto* selected = recoveryRows->currentItem();
        const auto previous = selected ? selected->data(Qt::UserRole) : QVariant{};
        const QSignalBlocker blocker(recoveryRows);
        // Retain the items/focus during the initial busy notification.
        if (!workspace.recoveryBusy()) {
            recoveryRows->clear();
            for (const auto& run : workspace.interrupted()) {
                QString title = tr("Saved game");
                if (library_) {
                    const auto& gameRows = library_->rows();
                    const auto game = std::find_if(
                        gameRows.begin(), gameRows.end(),
                        [&run](const auto& candidate) { return candidate.id == run.gameId; });
                    if (game != gameRows.end()) {
                        title = tr("%1 vs %2")
                                    .arg(game->white.isEmpty() ? tr("White") : game->white,
                                         game->black.isEmpty() ? tr("Black") : game->black);
                    }
                }
                auto* item = new QListWidgetItem(
                    run.metadataError
                        ? tr("%1\nSaved review could not be read.").arg(title)
                        : tr("%1 — %2\n%3")
                              .arg(title,
                                   run.selectedSide == chess::Color::White ? tr("White")
                                                                           : tr("Black"),
                                   QLocale().toString(run.createdAt.toLocalTime(),
                                                      QLocale::ShortFormat)),
                    recoveryRows);
                item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(run.id)));
                item->setData(Qt::UserRole + 1,
                              QVariant::fromValue(static_cast<qint64>(run.gameId)));
                item->setData(Qt::UserRole + 2, run.id > 0 && run.gameId > 0);
                item->setToolTip(
                    run.metadataError
                        ? QObject::tr("The saved data is preserved. Open for details, or use "
                                      "Games to inspect another run or start a new review.")
                        : item->text());
                if (item->data(Qt::UserRole) == previous) {
                    recoveryRows->setCurrentItem(item);
                }
            }
            if (!recoveryRows->currentItem() && recoveryRows->count() > 0) {
                recoveryRows->setCurrentRow(0);
            }
        }
        const auto* current = recoveryRows->currentItem();
        openRecovery->setEnabled(current && current->data(Qt::UserRole + 2).toBool());
        recovery->setVisible(!workspace.interrupted().empty() ||
                             !workspace.recoveryError().isEmpty());
        recoveryError->setText(workspace.recoveryError());
        earlier->setEnabled(!workspace.recoveryBusy() && workspace.recoveryPage() > 0);
        later->setEnabled(!workspace.recoveryBusy() && workspace.recoveryHasNext());
        refresh->setEnabled(!workspace.recoveryBusy());
    };
    connect(&review.workspace(), &app::ReviewWorkspaceController::recoveryChanged, this,
            updateRecovery);
    if (library_) {
        connect(library_, &app::GameLibraryController::changed, this, updateRecovery);
    }
    connect(earlier, &QPushButton::clicked, this, [&review] {
        auto& w = review.workspace();
        w.refreshRecovery(w.recoveryPage() - 1);
    });
    connect(later, &QPushButton::clicked, this, [&review] {
        auto& w = review.workspace();
        w.refreshRecovery(w.recoveryPage() + 1);
    });
    connect(refresh, &QPushButton::clicked, this,
            [&review] { review.workspace().refreshRecovery(); });
    recovery->hide();
}
void MainWindow::showReview() {
    if (closing_ || !reviewPage_) {
        return;
    }
    navigation_.navigate(app::Destination::Games);
    pages_->setCurrentWidget(reviewPage_);
}
void MainWindow::shareAnalysisLineCount(AnalysisPage& page) {
    if (analysis_) {
        page.setAnalysisMultiPv(analysis_->preferences().analysisMultiPv);
    }
    connect(&page, &AnalysisPage::analysisMultiPvChanged, this, [this](int count) {
        for (auto* view : {analysis_, linkedAnalysis_, puzzleAnalysis_}) {
            if (view) {
                view->setAnalysisMultiPv(count);
            }
        }
    });
}
void MainWindow::installEngineAnalysis(app::EngineAnalysisController& engine) {
    liveEngine_ = &engine;
    Q_ASSERT(analysis_);
    analysis_->installEngineAnalysis(engine);
    shareAnalysisLineCount(*analysis_);
    if (auto* duplicate =
            analysis_->findChild<QPushButton*>(QStringLiteral("analysisEngineSettings"))) {
        delete duplicate; // The live panel now owns the visible Manage Engines affordance.
    }
    connect(analysis_, &AnalysisPage::manageEnginesRequested, this, [this] {
        if (!closing_) {
            navigation_.navigate(app::Destination::Settings);
        }
    });
    connect(&navigation_, &app::NavigationController::destinationChanged, this, [this, &engine] {
        // Earlier subscribers may have synchronously redirected this navigation.
        engine.setVisible(!closing_ && pages_->currentWidget() == analysis_);
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this, &engine] {
        engine.setVisible(!closing_ && pages_->currentWidget() == analysis_);
    });
    engine.setVisible(pages_->currentWidget() == analysis_);
}
void MainWindow::installEngines(app::EngineSettingsController& engines) {
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("settingsPage"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(placeholder);
    const int index = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    pages_->insertWidget(index, new EngineSettingsPage(engines, pages_));
    auto* action = new QAction(tr("Engine Settings…"), this);
    action->setObjectName(QStringLiteral("engineSettingsAction"));
    findChild<QMenu*>(QStringLiteral("viewMenu"))->addAction(action);
    connect(action, &QAction::triggered, this, [this] {
        if (!closing_) {
            navigation_.navigate(app::Destination::Settings);
        }
    });
    if (analysis_) {
        auto* button = new QPushButton(tr("Engine Settings…"), analysis_);
        button->setObjectName(QStringLiteral("analysisEngineSettings"));
        auto* commands = analysis_->findChild<QHBoxLayout*>(QStringLiteral("analysisCommands"));
        Q_ASSERT(commands);
        commands->addWidget(button);
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
    }
    selectDestination();
}
void MainWindow::installAnalysis(app::AnalysisController& controller) {
    Q_ASSERT(!analysis_);
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("analyzePage"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(placeholder);
    const int pageIndex = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    analysis_ = new AnalysisPage(controller, pages_);
    pages_->insertWidget(pageIndex, analysis_);
    auto* flip = new QAction(tr("Flip Board"), this);
    flip->setObjectName(QStringLiteral("flipBoardAction"));
    flip->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    connect(flip, &QAction::triggered, this, [this, &controller] {
        if (puzzleAnalysis_ && pages_->currentWidget() == puzzleAnalysis_) {
            puzzleAnalysis_->controller().flip();
        } else if (linkedAnalysis_ && pages_->currentWidget() == linkedAnalysis_) {
            linkedAnalysis_->controller().flip();
        } else {
            controller.flip();
        }
    });
    // Reuse the existing View menu without introducing a second command surface.
    auto* view = findChild<QMenu*>(QStringLiteral("viewMenu"));
    Q_ASSERT(view);
    view->addAction(flip);
    connect(&navigation_, &app::NavigationController::destinationChanged, flip, [this, flip] {
        flip->setEnabled(!closing_ && navigation_.current() == app::Destination::Analyze);
    });
    flip->setEnabled(navigation_.current() == app::Destination::Analyze);
    connect(&controller, &app::AnalysisController::gameOpened, this, [this] {
        if (!closing_) {
            navigation_.navigate(app::Destination::Analyze);
        }
    });
    selectDestination();
}
namespace {
class ImportDropButton final : public QToolButton {
  public:
    ImportDropButton(app::ImportController& imports, QWidget* parent)
        : QToolButton(parent), imports_(imports) {
        setAcceptDrops(true);
    }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (!imports_.available() || !event->possibleActions().testFlag(Qt::CopyAction) ||
            !event->mimeData()->hasUrls()) {
            return;
        }
        if (!app::ImportController::acceptsFiles(event->mimeData()->urls())) {
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
    void dropEvent(QDropEvent* event) override {
        if (imports_.available() && event->possibleActions().testFlag(Qt::CopyAction) &&
            event->mimeData()->hasUrls() && imports_.importFiles(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
    }

  private:
    app::ImportController& imports_;
};
} // namespace
void MainWindow::installImports(app::ImportController& imports,
                                app::GameLibraryController& library) {
    Q_ASSERT(!library_);
    library_ = &library;
    auto* file = findChild<QMenu*>(QStringLiteral("fileMenu"));
    auto* before = findChild<QAction*>(QStringLiteral("quitAction"));
    importFile_ = new QAction(tr("&Import PGN…"), this);
    importFile_->setObjectName(QStringLiteral("importPgnAction"));
    importFile_->setShortcut(QKeySequence::Open);
    pastePgn_ = new QAction(tr("Paste PGN…"), this);
    pastePgn_->setObjectName(QStringLiteral("pastePgnAction"));
    pastePgn_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    file->insertAction(before, importFile_);
    file->insertAction(before, pastePgn_);
    file->insertSeparator(before);
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("gamesPage"), Qt::FindDirectChildrenOnly);
    const int pageIndex = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    auto* games = new GamesPage(imports, library, *importFile_, *pastePgn_, pages_);
    games->setVerbose(verbose_);
    pages_->insertWidget(pageIndex, games);
    connect(pages_, &QStackedWidget::currentChanged, this, [this, games, &library] {
        if (!closing_ &&
            (pages_->currentWidget() == games || pages_->currentWidget() == pages_->widget(0))) {
            library.activate(pages_->currentWidget() == games);
        } else {
            library.deactivate();
        }
    });
    connect(games, &GamesPage::openRequested, this, [this, &library](persistence::GameId id) {
        if (!closing_ && analysis_ && !library.opening()) {
            analysis_->requestDiscard([this, &library, id] {
                if (!closing_) {
                    library.open(id);
                }
            });
        }
    });
    connect(&library, &app::GameLibraryController::changed, this, [this, &library] {
        if (analysis_ && !closing_) {
            analysis_->setEnabled(!library.opening());
        }
    });
    auto* home =
        pages_->findChild<QWidget*>(QStringLiteral("homePage"), Qt::FindDirectChildrenOnly);
    auto* homeLayout = qobject_cast<QVBoxLayout*>(home->layout());
    auto* fileButton = new ImportDropButton(imports, home);
    fileButton->setObjectName(QStringLiteral("homeImportButton"));
    fileButton->setDefaultAction(importFile_);
    fileButton->setText(tr("Review a game"));
    connect(importFile_, &QAction::changed, fileButton,
            [fileButton] { fileButton->setText(tr("Review a game")); });
    setPrimaryAction(*fileButton);
    fileButton->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* pasteButton = new QToolButton(home);
    pasteButton->setDefaultAction(pastePgn_);
    pasteButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* welcome = new QLabel(tr("Import a PGN file or paste a game to get started."), home);
    welcome->setWordWrap(true);
    setTextRole(*welcome, "secondary");
    homeLayout->insertWidget(1, welcome);
    auto* importActions = new QWidget(home);
    auto* importButtons = new QHBoxLayout(importActions);
    importButtons->setContentsMargins(0, 0, 0, 0);
    importButtons->addWidget(fileButton);
    importButtons->addWidget(pasteButton);
    importButtons->addStretch();
    homeLayout->insertWidget(2, importActions);
    auto* homeHeading = home->findChild<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    if (homeHeading) {
        homeHeading->setText(tr("Understand your game"));
    }
    auto* recentHeading = new QLabel(tr("Recent games"), home);
    setTextRole(*recentHeading, "heading");
    homeLayout->insertWidget(3, recentHeading);
    auto* recent = new QListWidget(home);
    recent->setObjectName(QStringLiteral("homeRecentGames"));
    recent->setAccessibleName(tr("Recent games"));
    recent->setMaximumHeight(240);
    homeLayout->insertWidget(4, recent);
    auto* openRecent = new QPushButton(tr("Review selected game"), home);
    openRecent->setEnabled(false);
    homeLayout->insertWidget(5, openRecent, 0, Qt::AlignLeft);
    const auto reviewRecent = [this, recent] {
        const auto* item = recent->currentItem();
        if (!closing_ && reviewController_ && item) {
            reviewController_->selectGame(item->data(Qt::UserRole).toLongLong());
            showReview();
        }
    };
    connect(openRecent, &QPushButton::clicked, this, reviewRecent);
    connect(recent, &QListWidget::itemActivated, this,
            [reviewRecent](QListWidgetItem*) { reviewRecent(); });
    connect(recent, &QListWidget::currentRowChanged, openRecent,
            [openRecent](int row) { openRecent->setEnabled(row >= 0); });
    connect(&library, &app::GameLibraryController::changed, recent,
            [this, &library, home, recent, recentHeading, openRecent] {
                if (library.busy() || pages_->currentWidget() != home) {
                    return;
                }
                const auto* selected = recent->currentItem();
                const auto id = selected ? selected->data(Qt::UserRole) : QVariant{};
                const QSignalBlocker blocker(recent);
                recent->clear();
                for (const auto& game : library.rows()) {
                    if (recent->count() >= 5) {
                        break;
                    }
                    auto* item = new QListWidgetItem;
                    // Force plain text: PGN names are untrusted.
                    item->setText(tr("%1 vs %2\n%3    %4")
                                      .arg(game.white.isEmpty() ? tr("White") : game.white,
                                           game.black.isEmpty() ? tr("Black") : game.black,
                                           game.result == QLatin1String("*")
                                               ? tr("Result not recorded")
                                               : game.result,
                                           game.playedAt));
                    item->setSizeHint(QSize(0, recent->fontMetrics().height() * 2 + 16));
                    item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(game.id));
                    recent->addItem(item);
                    if (item->data(Qt::UserRole) == id) {
                        recent->setCurrentItem(item);
                    }
                }
                if (!recent->currentItem() && recent->count() > 0) {
                    recent->setCurrentRow(0);
                }
                const bool populated = recent->count() > 0;
                recent->setVisible(populated);
                recent->setFixedHeight(
                    std::min(260, recent->count() * (recent->fontMetrics().height() * 2 + 16) + 2));
                recentHeading->setVisible(populated);
                openRecent->setVisible(populated);
                openRecent->setEnabled(recent->currentRow() >= 0);
            });
    recentHeading->hide();
    recent->hide();
    openRecent->hide();
    if (pages_->currentWidget() == home) {
        library.activate(false);
    }
    auto* scroll = qobject_cast<QScrollArea*>(centralWidget());
    auto* notice = new QLabel(scroll->widget());
    notice->setObjectName(QStringLiteral("importNotice"));
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    notice->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->insertWidget(2, notice);
    connect(&imports, &app::ImportController::rejected, notice, &QLabel::setText);
    connect(&library, &app::GameLibraryController::openFailed, this,
            [this, notice](const QString& message) {
                notice->setText(message);
                if (!closing_) {
                    navigation_.navigate(app::Destination::Games);
                }
            });
    if (analysis_) {
        connect(&analysis_->controller(), &app::AnalysisController::failed, notice,
                &QLabel::setText);
        connect(&analysis_->controller(), &app::AnalysisController::gameOpened, notice,
                &QLabel::clear);
    }
    connect(&imports, &app::ImportController::submitted, this, [this, notice] {
        notice->clear();
        navigation_.navigate(app::Destination::Games);
    });
    connect(&imports, &app::ImportController::finished, this, [this, &imports, notice] {
        if (closing_ || !reviewController_ ||
            imports.status() != app::ImportBatchStatus::Completed ||
            navigation_.current() != app::Destination::Games ||
            pages_->currentWidget() == reviewPage_) {
            return;
        }
        std::set<persistence::GameId> imported;
        std::map<persistence::GameId, QString> titles;
        for (const auto& importedFile : imports.files()) {
            for (const auto& item : importedFile.items) {
                if (const auto* added = std::get_if<persistence::Inserted>(&item.outcome)) {
                    imported.insert(added->id);
                    titles[added->id] = item.title;
                } else if (const auto* duplicate =
                               std::get_if<persistence::Duplicate>(&item.outcome)) {
                    imported.insert(duplicate->id);
                    titles[duplicate->id] = item.title;
                }
            }
        }
        if (imported.empty()) {
            return;
        }
        if (imports.failures() || imports.warnings()) {
            notice->setText(
                tr("Some games have import issues. Open Games → Import report for details."));
        }
        if (imported.size() == 1) {
            reviewController_->selectGame(*imported.begin());
            showReview();
            return;
        }
        auto* dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("importGameChooser"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Choose a game to review"));
        auto* layout = new QVBoxLayout(dialog);
        auto* list = new QListWidget(dialog);
        list->setObjectName(QStringLiteral("importGameChoices"));
        list->setAccessibleName(tr("Imported games"));
        for (const auto id : imported) {
            auto* item =
                new QListWidgetItem(titles[id].isEmpty() ? tr("Imported game") : titles[id], list);
            item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(id));
        }
        list->setCurrentRow(0);
        layout->addWidget(list);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
        auto* open = buttons->addButton(tr("Review game"), QDialogButtonBox::AcceptRole);
        open->setObjectName(QStringLiteral("reviewImportedGame"));
        setPrimaryAction(*open);
        layout->addWidget(buttons);
        const auto choose = [this, list, dialog] {
            if (!closing_ && reviewController_ && list->currentItem()) {
                reviewController_->selectGame(list->currentItem()->data(Qt::UserRole).toLongLong());
                showReview();
                dialog->accept();
            }
        };
        connect(buttons, &QDialogButtonBox::accepted, dialog, choose);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(list, &QListWidget::itemActivated, dialog,
                [choose](QListWidgetItem*) { choose(); });
        dialog->resize(480, 380);
        dialog->open();
    });
    const auto updateActions = [this, &imports] {
        const bool available = imports.available() && !closing_;
        importFile_->setEnabled(available);
        pastePgn_->setEnabled(available);
        const auto reason = available ? tr("Import local UTF-8 PGN files")
                                      : tr("Import is unavailable while storage is loading, "
                                           "closing, or an import is running.");
        importFile_->setToolTip(reason);
        pastePgn_->setToolTip(available ? tr("Edit clipboard PGN before importing") : reason);
    };
    connect(&imports, &app::ImportController::changed, this, updateActions);
    updateActions();
    connect(importFile_, &QAction::triggered, this, [this, &imports] {
        auto* picker = new QFileDialog(this, tr("Import PGN files"));
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setObjectName(QStringLiteral("importFileDialog"));
        picker->setFileMode(QFileDialog::ExistingFiles);
        picker->setNameFilter(tr("PGN files (*.pgn *.pgN *.pGn *.pGN *.Pgn *.PgN *.PGn *.PGN)"));
        picker->setWindowModality(Qt::WindowModal);
        connect(picker, &QFileDialog::accepted, this,
                [picker, &imports] { imports.importFiles(picker->selectedUrls()); });
        picker->open();
    });
    connect(pastePgn_, &QAction::triggered, this, [this, &imports] {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("pastePgnDialog"));
        dialog->setWindowTitle(tr("Paste PGN"));
        dialog->setWindowModality(Qt::WindowModal);
        auto* layout = new QVBoxLayout(dialog);
        auto* editor = new PgnTextEdit(dialog);
        editor->setObjectName(QStringLiteral("pastePgnEditor"));
        editor->setAccessibleName(tr("PGN text to import"));
        // Clipboard access is explicit and happens only on this user action.
        const auto text = QApplication::clipboard()->text();
        auto* error = new QLabel(dialog);
        error->setObjectName(QStringLiteral("pastePgnError"));
        error->setTextFormat(Qt::PlainText);
        error->setWordWrap(true);
        connect(editor, &PgnTextEdit::inputRejected, error, &QLabel::setText);
        connect(editor, &QPlainTextEdit::textChanged, error, &QLabel::clear);
        auto* previewInfo = new QLabel(dialog);
        previewInfo->setObjectName(QStringLiteral("pastePgnPreviewInfo"));
        previewInfo->setTextFormat(Qt::PlainText);
        previewInfo->setWordWrap(true);
        previewInfo->setText(tr("Large PGN: read-only preview of the beginning. Import full text "
                                "imports all supplied text. To edit it, use a UTF-8 PGN file."));
        previewInfo->hide();
        layout->addWidget(previewInfo);
        layout->addWidget(editor, 1);
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
        auto* submit = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
        submit->setObjectName(QStringLiteral("confirmPasteImport"));
        connect(editor, &PgnTextEdit::previewChanged, dialog, [previewInfo, submit](bool preview) {
            previewInfo->setVisible(preview);
            submit->setText(preview ? tr("Import full text") : tr("Import"));
        });
        editor->setPgnText(text);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(submit, &QPushButton::clicked, dialog, [dialog, editor, error, &imports] {
            if (imports.importText(editor->pgnText())) {
                dialog->accept();
            } else {
                error->setText(
                    tr("Import could not start. Check text size and storage availability."));
            }
        });
        layout->addWidget(buttons);
        dialog->resize(640, 420);
        dialog->open();
        editor->setFocus();
    });
    qApp->installEventFilter(this);
    selectDestination();
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (pastePgn_ && event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        auto* focus = QApplication::focusWidget();
        if (key->key() == Qt::Key_V &&
            key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && focus &&
            (qobject_cast<QLineEdit*>(focus) || qobject_cast<QTextEdit*>(focus) ||
             qobject_cast<QPlainTextEdit*>(focus))) {
            event->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::selectDestination() {
    if (!closing_ && reviewPage_ && pages_->currentWidget() == reviewPage_ &&
        reviewController_->state().active && !reviewLeaveAllowed_) {
        if (reviewLeavePending_) {
            return;
        }
        reviewLeavePending_ = true;
        auto* dialog =
            new QMessageBox(QMessageBox::Question, tr("Leave active review?"),
                            tr("The quick scan is still running."), QMessageBox::NoButton, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("leaveReviewDialog"));
        auto* background = dialog->addButton(tr("Continue in background"), QMessageBox::AcceptRole);
        auto* stay = dialog->addButton(tr("Stay"), QMessageBox::RejectRole);
        auto* cancel = dialog->addButton(tr("Cancel task"), QMessageBox::DestructiveRole);
        dialog->setDefaultButton(stay);
        const auto origin = reviewController_->taskToken();
        connect(dialog, &QDialog::finished, this, [this, dialog, background, cancel, origin] {
            reviewLeavePending_ = false;
            if (closing_) {
                return;
            }
            if (dialog->clickedButton() == background || dialog->clickedButton() == cancel) {
                reviewLeaveAllowed_ = true;
                if (dialog->clickedButton() == cancel && origin == reviewController_->taskToken()) {
                    reviewController_->cancel();
                }
                selectDestination();
            } else {
                reviewLeaveAllowed_ = true;
                showReview();
                reviewLeaveAllowed_ = false;
            }
        });
        dialog->open();
        return;
    }
    if (navigation_.current() != app::Destination::Analyze) {
        linkedAnalysisPending_ = false;
    }
    // A previous signal subscriber may have synchronously redirected navigation.
    for (std::size_t i = 0; i < destinations_.size(); ++i) {
        auto* action = destinations_[i];
        if (action->data().value<app::Destination>() == navigation_.current()) {
            action->setChecked(true);
            pages_->setCurrentIndex(static_cast<int>(i));
            if (navigation_.current() == app::Destination::Analyze && linkedAnalysisPending_) {
                linkedAnalysisPending_ = false;
                pages_->setCurrentWidget(linkedAnalysis_);
            }
            if (analysis_ && navigation_.current() == app::Destination::Analyze && !closing_) {
                analysis_->controller().start();
            }
            return;
        }
    }
    Q_ASSERT_X(false, "MainWindow", "Navigation destination has no page");
}
void MainWindow::showProgress(const QString& message) {
    if (!closing_) {
        startup_->showProgress(message);
    }
}
void MainWindow::showReady() {
    if (!closing_) {
        startup_->showReady();
    }
}
void MainWindow::showFailure(const app::Diagnostic& diagnostic) {
    if (!closing_) {
        startup_->showFailure(diagnostic);
    }
}
void MainWindow::setDiagnostics(diagnostics::Logger& logger) { startup_->setDiagnostics(logger); }
void MainWindow::showUiStateFailure(app::UiStateError error) {
    if (stateNoticeShown_) {
        return;
    }
    stateNoticeShown_ = true;
    QString reason;
    switch (error) {
    case app::UiStateError::Read:
        reason =
            tr("Window preferences could not be read. Check access to the configuration folder.");
        break;
    case app::UiStateError::Invalid:
        reason = tr("Window preferences are invalid or unavailable. Use View → Reset Layout or "
                    "check the configuration file.");
        break;
    case app::UiStateError::Write:
        reason = tr("Window preferences could not be saved. Check free space and access to the "
                    "configuration folder.");
        break;
    case app::UiStateError::Busy:
        reason = tr(
            "Window preferences were not saved because another settings writer or lock is present. "
            "Close other instances and check the configuration folder before relaunching.");
        break;
    }
    if (reason.isEmpty()) {
        reason = tr("Window preferences are unavailable.");
    }
    preferencesNotice_->setText(reason + tr(" The app remains usable."));
    preferencesNotice_->show();
}
void MainWindow::watchScreen(QScreen* screen) {
    disconnect(screenGeometryConnection_);
    if (screen) {
        screenGeometryConnection_ =
            connect(screen, &QScreen::availableGeometryChanged, this, [this] { fitToScreen(); });
    }
}
void MainWindow::fitToScreen() {
    if (closing_ || isMaximized() || isMinimized()) {
        return;
    }
    auto* target = screen();
    if (!target) {
        target = QGuiApplication::primaryScreen();
    }
    if (target) {
        // Leave room for the native window frame so its title bar remains reachable.
        const auto margins = windowHandle() ? windowHandle()->frameMargins() : QMargins();
        const auto available = target->availableGeometry().marginsRemoved(margins);
        setGeometry(fittedWindowGeometry(geometry(), available));
    }
}
void MainWindow::resetWindowGeometry() {
    if (closing_) {
        return;
    }
    setWindowState(Qt::WindowNoState);
    if (auto* target = screen()) {
        const auto available = target->availableGeometry();
        QRect desired(QPoint(), QSize(1360, 850).boundedTo(available.size()));
        desired.moveCenter(available.center());
        setGeometry(fittedWindowGeometry(desired, available));
    } else {
        resize(1360, 850);
    }
    fitToScreen();
}
void MainWindow::resetLayout() {
    reviewSplit_ = 55;
    if (reviewPage_) {
        reviewPage_->setSplitShare(55);
    }
    if (closing_) {
        return;
    }
    resetWindowGeometry();
    navigation_.navigate(app::Destination::Home);
}
void MainWindow::restoreUiState(const app::UiState& state) {
    if (closing_ || stateRestored_) {
        return;
    }
    resetWindowGeometry();
    reviewSplit_ = state.reviewSplit;
    if (reviewPage_) {
        reviewPage_->setSplitShare(reviewSplit_);
    }
    if (analysis_) {
        analysis_->setPreferences(state.board);
        for (auto* view : {linkedAnalysis_, puzzleAnalysis_}) {
            if (view) {
                view->setAnalysisMultiPv(state.board.analysisMultiPv);
            }
        }
    }
    if (!state.geometry.isEmpty() && !restoreGeometry(state.geometry)) {
        showUiStateFailure();
        emit invalidUiState();
        resetWindowGeometry();
    }
    setWindowState(state.maximized ? Qt::WindowMaximized : Qt::WindowNoState);
    navigation_.navigate(state.destination);
    fitToScreen();
    stateRestored_ = true;
    // Creating the native handle makes screen tracking available before first show.
    static_cast<void>(winId());
    watchScreen(screen());
    connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* target) {
        watchScreen(target);
        fitToScreen();
    });
}
app::UiState MainWindow::uiState() const {
    return {saveGeometry(),        windowState().testFlag(Qt::WindowMaximized),
            navigation_.current(), analysis_ ? analysis_->preferences() : app::BoardPreferences{},
            std::nullopt,          reviewPage_ ? reviewPage_->splitShare() : reviewSplit_};
}
void MainWindow::showClosing() {
    if (closing_) {
        return;
    }
    closing_ = true;
    if (analysis_) {
        analysis_->deactivate();
    }
    for (auto* action : destinations_) {
        action->setEnabled(false);
    }
    reset_->setEnabled(false);
    startup_->showClosing();
}
void MainWindow::completeShutdown() {
    shutdownComplete_ = true;
    close();
}
void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::fitToScreen);
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (shutdownComplete_) {
        QMainWindow::closeEvent(event);
        return;
    }
    event->ignore();
    if (!closing_) {
        if (analysis_) {
            analysis_->requestDiscard([this] {
                if (linkedAnalysis_) {
                    linkedAnalysis_->requestDiscard([this] {
                        if (puzzleAnalysis_) {
                            puzzleAnalysis_->requestDiscard([this] { emit closeRequested(); });
                        } else {
                            emit closeRequested();
                        }
                    });
                } else {
                    emit closeRequested();
                }
            });
        } else {
            emit closeRequested();
        }
    }
}
void MainWindow::showConfigurationFailure(const QString& safeDetails) {
    startup_->showConfigurationFailure(safeDetails);
}
} // namespace chess_review::ui
