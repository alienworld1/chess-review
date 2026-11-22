#include "ui/shell/MainWindow.h"
#include "app/EngineAnalysisController.h"
#include "app/ReviewWorkspaceController.h"
#include "ui/Theme.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/games/GamesPage.h"
#include "ui/games/PgnTextEdit.h"
#include "ui/home/HomePage.h"
#include "ui/puzzles/PuzzlesPage.h"
#include "ui/review/ReviewPage.h"
#include "ui/settings/EngineSettingsPage.h"
#include "ui/shell/StartupPanel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
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
// QWidgetItem normally asks the widget's layout for its height, which makes
// QStackedLayout include hidden pages with wrapped content.
class DestinationItem final : public QWidgetItem {
  public:
    explicit DestinationItem(QStackedWidget* pages) : QWidgetItem(pages) {}
    bool hasHeightForWidth() const override { return !isEmpty() && widget()->hasHeightForWidth(); }
    int heightForWidth(int width) const override {
        return hasHeightForWidth() ? widget()->heightForWidth(width) : -1;
    }
};

void showGameLinkDialog(QWidget& parent, app::ImportController& imports) {
    auto* dialog = new QDialog(&parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("importGameLinkDialog"));
    dialog->setWindowTitle(QObject::tr("Import a game link"));
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(
        QObject::tr("Paste the link to a completed public game from Chess.com or Lichess."),
        dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* label = new QLabel(QObject::tr("Game &link"), dialog);
    auto* link = new QLineEdit(dialog);
    link->setObjectName(QStringLiteral("gameLinkEditor"));
    link->setAccessibleName(QObject::tr("Game link"));
    link->setClearButtonEnabled(true);
    // Keep one extra character so overlong pasted input cannot become a valid truncated URL.
    link->setMaxLength(input::maxGameLinkLength + 1);
    link->setPlaceholderText(QStringLiteral("https://lichess.org/YO3ICtJi"));
    label->setBuddy(link);
    layout->addWidget(label);
    layout->addWidget(link);
    auto* error = new QLabel(dialog);
    error->setObjectName(QStringLiteral("gameLinkError"));
    error->setTextFormat(Qt::PlainText);
    error->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    QObject::connect(link, &QLineEdit::textEdited, error, &QWidget::hide);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto* submit = buttons->addButton(QObject::tr("Import game"), QDialogButtonBox::AcceptRole);
    submit->setObjectName(QStringLiteral("confirmGameLinkImport"));
    submit->setDefault(true);
    setPrimaryAction(*submit);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(submit, &QPushButton::clicked, dialog, [dialog, link, error, &imports] {
        const auto game = input::RemoteGame::fromLink(link->text());
        if (game && imports.importUrl(*game)) {
            dialog->accept();
            return;
        }
        error->setText(game ? QObject::tr("Import is currently unavailable. Try again shortly.")
                            : QObject::tr("Enter a public Chess.com or Lichess game link. Copy the "
                                          "link from the finished game's Share menu."));
        error->show();
        link->setFocus();
    });
    layout->addWidget(buttons);
    if (const auto game = input::RemoteGame::fromLink(QApplication::clipboard()->text())) {
        link->setText(game->publicUrl().toString(QUrl::FullyEncoded));
    } else if (imports.status() == app::ImportBatchStatus::Failed && !imports.files().empty() &&
               imports.files().front().remote) {
        link->setText(imports.files().front().name);
    }
    dialog->resize(440, dialog->sizeHint().height());
    dialog->open();
    link->setFocus();
    link->selectAll();
}
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
        if (values[i] == app::Destination::Home) {
            home_ = new HomePage(pages_);
            pages_->addWidget(home_);
            continue;
        }
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
    connect(home_, &HomePage::gamesRequested, destinations_[1], &QAction::trigger);
    connect(home_, &HomePage::settingsRequested, this, &MainWindow::openEngineSettings);
    connect(home_, &HomePage::reviewRequested, this, &MainWindow::showReview);
    connect(home_, &HomePage::recentGameRequested, this, [this](persistence::GameId id) {
        if (!closing_ && reviewController_) {
            reviewController_->selectGame(id);
            showReview();
        }
    });
    buttons->addStretch();
    row->addWidget(sidebar);
    row->addItem(new DestinationItem(pages_));
    row->setStretch(row->count() - 1, 1);
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
void MainWindow::installSettings(app::ReviewConfiguration& configuration) {
    if (auto* settings = pages_->findChild<EngineSettingsPage*>()) {
        settings->installConfiguration(configuration);
        const auto applyAnalysisLines = [this, &configuration] {
            const auto snapshot = configuration.snapshot();
            if (!snapshot || snapshot->ui.liveAnalysisLines == configuredAnalysisLines_) {
                return;
            }
            configuredAnalysisLines_ = snapshot->ui.liveAnalysisLines;
            if (configuredAnalysisLines_) {
                for (auto* view : {analysis_, linkedAnalysis_, puzzleAnalysis_}) {
                    if (view) {
                        view->setAnalysisMultiPv(*configuredAnalysisLines_);
                    }
                }
            }
        };
        connect(&configuration, &app::ReviewConfiguration::changed, this, applyAnalysisLines);
        applyAnalysisLines();
    }
}
void MainWindow::installPuzzles(app::PuzzleController& puzzles,
                                app::PuzzleGenerationController& generation) {
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
            &MainWindow::openEngineSettings);
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
        const auto update = [this, save, notice, openSaved, analysisPage, engine, &generation] {
            save->setEnabled(generation.canSavePosition() && (!engine || !engine->preview()) &&
                             !analysisPage->controller().busy() &&
                             analysisPage->controller().position());
            const auto& state = generation.state();
            const bool ownSave = state.savingPosition && puzzleSavePage_ == analysisPage;
            notice->setText(ownSave ? (state.error.isEmpty() ? state.message : state.error)
                                    : QString{});
            notice->setVisible(!notice->text().isEmpty());
            openSaved->setVisible(ownSave && state.lastSaved.has_value());
        };
        connect(&generation, &app::PuzzleGenerationController::changed, analysisPage, update);
        connect(&analysisPage->controller(), &app::AnalysisController::changed, analysisPage,
                update);
        if (engine) {
            connect(engine, &app::EngineAnalysisController::changed, analysisPage, update);
        }
        connect(save, &QPushButton::clicked, analysisPage,
                [this, analysisPage, engine, &generation] {
                    if (!generation.canSavePosition() || !analysisPage->controller().position()) {
                        return;
                    }
                    if (engine) {
                        engine->stop();
                    }
                    puzzleSavePage_ = analysisPage;
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
            const bool loading = !same || puzzles.reviewSummaryLoading();
            count->setVisible(ready);
            generate->setVisible(ready);
            library->setVisible(ready && !loading && saved.error.isEmpty() && saved.count > 0);
            count->setText(loading                  ? tr("Loading puzzles…")
                           : !saved.error.isEmpty() ? saved.error
                           : saved.more ? tr("More than %L1 saved puzzles").arg(saved.count)
                                        : tr("%n saved puzzle(s)", nullptr, saved.count));
            const bool resumable =
                same && saved.generation &&
                saved.generation->status != persistence::PuzzleJobStatus::Complete;
            generate->setText(!loading && !saved.error.isEmpty() ? tr("Retry loading puzzles")
                              : resumable                        ? tr("Resume puzzle generation")
                                                                 : tr("Generate Puzzles"));
            generate->setEnabled(ready && !loading && !generation.state().active &&
                                 (!saved.error.isEmpty() || !saved.generation || resumable));
        };
        connect(generate, &QPushButton::clicked, this, [this, &puzzles, &generation, update] {
            const auto& run = reviewController_->state().run;
            if (!run) {
                return;
            }
            const auto& saved = puzzles.reviewSummary();
            if (saved.id == run->id && !saved.error.isEmpty()) {
                puzzles.inspectReview(run->id, true);
                update();
            } else if (saved.id == run->id && saved.generation &&
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
    home_->installPuzzles(puzzles);
    connect(home_, &HomePage::puzzlesRequested, this, [this, &puzzles] {
        if (!closing_) {
            puzzles.showAllPuzzles();
            destinations_[2]->trigger();
        }
    });
    const auto updateSummaryVisibility = [this, &puzzles] {
        puzzles.setSummaryVisible(!closing_ && pages_->currentWidget() == home_);
    };
    connect(pages_, &QStackedWidget::currentChanged, this, updateSummaryVisibility);
    updateSummaryVisibility();
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
    linkedAnalysis_->installReviewNavigation();
    linkedAnalysis_->installEngineAnalysis(linkedExplorer.linkedEngine());
    shareAnalysisLineCount(*linkedAnalysis_);
    pages_->addWidget(linkedAnalysis_);
    const auto returnToOrigin = [this] {
        if (closing_ || pages_->currentWidget() != linkedAnalysis_) {
            return;
        }
        const bool restored = reviewController_->exploration().restoreReviewOrigin();
        showReview();
        reviewPage_->restoreAnalysisReadingState(restored);
    };
    connect(linkedAnalysis_, &AnalysisPage::backToReviewRequested, this, returnToOrigin);
    connect(linkedAnalysis_, &AnalysisPage::manageEnginesRequested, this,
            &MainWindow::openEngineSettings);
    connect(reviewPage_, &ReviewPage::analyzeRequested, this, [this] {
        auto& exploration = reviewController_->exploration();
        const auto requestedOrigin = exploration.analysisOrigin();
        if (!requestedOrigin) {
            return;
        }
        const auto epoch = reviewNavigationEpoch_;
        const auto open = [this, requestedOrigin, epoch] {
            if (closing_ || reviewLeavePending_ || pages_->currentWidget() != reviewPage_ ||
                epoch != reviewNavigationEpoch_) {
                return;
            }
            auto& currentExplorer = reviewController_->exploration();
            if (currentExplorer.analysisOrigin() != requestedOrigin) {
                return;
            }
            const bool resume = currentExplorer.sameAnalysisOrigin();
            if (!currentExplorer.openAnalysis() || pages_->currentWidget() != reviewPage_ ||
                epoch != reviewNavigationEpoch_) {
                return;
            }
            reviewPage_->rememberAnalysisReadingState();
            linkedAnalysis_->setReviewContext(reviewController_->state().game,
                                              reviewPage_->analysisDescription());
            if (analysis_ && !resume) {
                auto preferences = analysis_->preferences();
                preferences.blackAtBottom = requestedOrigin->blackAtBottom;
                linkedAnalysis_->setPreferences(preferences);
            }
            showContextPage(app::Destination::Analyze, linkedAnalysis_);
        };
        if (!exploration.sameAnalysisOrigin()) {
            linkedAnalysis_->requestDiscard(open);
        } else {
            open();
        }
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this] {
        ++reviewNavigationEpoch_;
        reviewController_->exploration().linkedEngine().setVisible(
            !closing_ && pages_->currentWidget() == linkedAnalysis_,
            app::EngineAnalysisController::HiddenSearch::Suspend);
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
    connect(reviewPage_, &ReviewPage::settingsRequested, this, &MainWindow::openEngineSettings);
    auto* scroll = qobject_cast<QScrollArea*>(centralWidget());
    auto* task = new QPushButton(scroll->widget());
    task->setObjectName(QStringLiteral("reviewTaskStatus"));
    task->setAccessibleName(tr("Background review status; return to review"));
    qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->insertWidget(2, task);
    task->hide();
    const auto sameReviewAsTask = [&review] {
        return review.state().run && review.taskState().run &&
               review.state().run->id == review.taskState().run->id;
    };
    connect(task, &QPushButton::clicked, this, [this, &review, returnToOrigin, sameReviewAsTask] {
        if (pages_->currentWidget() == linkedAnalysis_ && sameReviewAsTask()) {
            returnToOrigin();
        } else {
            review.workspace().returnToTask();
        }
    });
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
    const auto updateTask = [this, task, &review, sameReviewAsTask] {
        const auto& state = review.taskState();
        if (state.active && !reviewWasActive_) {
            reviewLeaveAllowed_ = false;
        }
        reviewWasActive_ = state.active;
        const bool viewingTask =
            sameReviewAsTask() && (pages_->currentWidget() == reviewPage_ ||
                                   (pages_->currentWidget() == linkedAnalysis_ && !state.active));
        task->setVisible(!viewingTask && pages_->currentWidget() != home_ &&
                         (state.active || state.run.has_value() || !state.error.isEmpty()));
        task->setText(state.active ? tr("Return to review in progress")
                                   : tr("Open your saved review"));
    };
    connect(&review, &app::ReviewGameController::changed, this, updateTask);
    connect(&review, &app::ReviewGameController::taskChanged, this, updateTask);
    connect(pages_, &QStackedWidget::currentChanged, this, [this, &review, updateTask] {
        review.workspace().setVisible(pages_->currentWidget() == reviewPage_ ||
                                      pages_->currentWidget() == linkedAnalysis_);
        updateTask();
    });
    home_->installReview(review);
}
void MainWindow::showReview() {
    if (closing_ || !reviewPage_) {
        return;
    }
    showContextPage(app::Destination::Games, reviewPage_);
}
void MainWindow::showContextPage(app::Destination destination, QWidget* page) {
    const QScopedValueRollback context(contextPage_, page);
    if (navigation_.current() == destination) {
        selectDestination();
    } else {
        navigation_.navigate(destination);
    }
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
    connect(analysis_, &AnalysisPage::manageEnginesRequested, this,
            &MainWindow::openEngineSettings);
    connect(&navigation_, &app::NavigationController::destinationChanged, this, [this, &engine] {
        // Earlier subscribers may have synchronously redirected this navigation.
        engine.setVisible(!closing_ && pages_->currentWidget() == analysis_);
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this, &engine] {
        engine.setVisible(!closing_ && pages_->currentWidget() == analysis_);
    });
    engine.setVisible(pages_->currentWidget() == analysis_);
}
void MainWindow::openEngineSettings() {
    if (!closing_) {
        if (auto* settings = pages_->findChild<EngineSettingsPage*>()) {
            settings->showEngines();
            if (backToReviewAnalysis_ && pages_->currentWidget() != settings) {
                backToReviewAnalysis_->setVisible(pages_->currentWidget() == linkedAnalysis_);
            }
        }
        navigation_.navigate(app::Destination::Settings);
    }
}
void MainWindow::installEngines(app::EngineSettingsController& engines) {
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("settingsPage"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(placeholder);
    const int index = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    auto* settings = new EngineSettingsPage(engines, pages_);
    pages_->insertWidget(index, settings);
    settings->setColorScheme(colorScheme_);
    connect(settings, &EngineSettingsPage::colorSchemeChanged, this,
            [this](Qt::ColorScheme scheme) {
                colorScheme_ = scheme;
                setThemeMode(scheme);
            });
    backToReviewAnalysis_ = new QPushButton(tr("Back to Analysis"), settings);
    backToReviewAnalysis_->setObjectName(QStringLiteral("backToReviewAnalysis"));
    backToReviewAnalysis_->hide();
    qobject_cast<QVBoxLayout*>(settings->layout())
        ->insertWidget(0, backToReviewAnalysis_, 0, Qt::AlignLeft);
    connect(backToReviewAnalysis_, &QPushButton::clicked, this, [this] {
        if (!closing_ && linkedAnalysis_) {
            showContextPage(app::Destination::Analyze, linkedAnalysis_);
        }
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this, settings] {
        if (pages_->currentWidget() != settings) {
            backToReviewAnalysis_->hide();
        }
    });
    auto* action = new QAction(tr("Engine Settings…"), this);
    action->setObjectName(QStringLiteral("engineSettingsAction"));
    findChild<QMenu*>(QStringLiteral("viewMenu"))->addAction(action);
    connect(action, &QAction::triggered, this, &MainWindow::openEngineSettings);
    home_->installEngines(engines);
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
    auto* file = findChild<QMenu*>(QStringLiteral("fileMenu"));
    auto* quit = findChild<QAction*>(QStringLiteral("quitAction"));
    auto* closeTab = new QAction(tr("Close Game Tab"), analysis_);
    closeTab->setObjectName(QStringLiteral("closeGameTabAction"));
    closeTab->setShortcut(QKeySequence::Close);
    file->insertAction(quit, closeTab);
    analysis_->installGameTabs(*closeTab);
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
            showContextPage(app::Destination::Analyze, nullptr);
        }
    });
    selectDestination();
}
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
    importLink_ = new QAction(tr("Import game link…"), this);
    importLink_->setObjectName(QStringLiteral("importGameLinkAction"));
    file->insertAction(before, importFile_);
    file->insertAction(before, pastePgn_);
    file->insertAction(before, importLink_);
    file->insertSeparator(before);
    auto* placeholder =
        pages_->findChild<QWidget*>(QStringLiteral("gamesPage"), Qt::FindDirectChildrenOnly);
    const int pageIndex = pages_->indexOf(placeholder);
    pages_->removeWidget(placeholder);
    delete placeholder;
    auto* games = new GamesPage(imports, library, *importFile_, *pastePgn_, *importLink_, pages_);
    games->setVerbose(verbose_);
    pages_->insertWidget(pageIndex, games);
    connect(pages_, &QStackedWidget::currentChanged, this, [this, games, &library] {
        if (!closing_ && (pages_->currentWidget() == games || pages_->currentWidget() == home_)) {
            library.activate(pages_->currentWidget() == games);
        } else {
            library.deactivate();
        }
    });
    connect(games, &GamesPage::openRequested, this, [this, &library](persistence::GameId id) {
        if (!closing_ && analysis_ && !library.opening()) {
            if (analysis_->controller().hasGameTab(id)) {
                analysis_->selectGameTab(id);
                showContextPage(app::Destination::Analyze, nullptr);
            } else {
                library.open(id);
            }
        }
    });
    connect(&library, &app::GameLibraryController::changed, this, [this, &library] {
        if (analysis_ && !closing_) {
            analysis_->setEnabled(!library.opening());
        }
    });
    home_->installImports(imports, *importFile_, *pastePgn_, *importLink_);
    home_->installLibrary(library);
    if (pages_->currentWidget() == home_) {
        library.activate(false);
    }
    auto* scroll = qobject_cast<QScrollArea*>(centralWidget());
    auto* notice = new QLabel(scroll->widget());
    notice->setObjectName(QStringLiteral("importNotice"));
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    notice->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    qobject_cast<QVBoxLayout*>(scroll->widget()->layout())->insertWidget(2, notice);
    notice->hide();
    const auto setNotice = [notice](const QString& message) {
        notice->setText(message);
        notice->setVisible(!message.isEmpty());
    };
    connect(&imports, &app::ImportController::rejected, notice, setNotice);
    connect(&library, &app::GameLibraryController::openFailed, this,
            [this, setNotice](const QString& message) {
                setNotice(message);
                if (!closing_) {
                    navigation_.navigate(app::Destination::Games);
                }
            });
    if (analysis_) {
        connect(&analysis_->controller(), &app::AnalysisController::failed, notice, setNotice);
        connect(&analysis_->controller(), &app::AnalysisController::gameOpened, notice,
                [setNotice] { setNotice({}); });
    }
    connect(&imports, &app::ImportController::submitted, this, [this, setNotice] {
        setNotice({});
        showContextPage(app::Destination::Games, nullptr);
    });
    connect(&imports, &app::ImportController::finished, this, [this, &imports, setNotice] {
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
            setNotice(tr("Some games have import issues. Open Games → Import report for details."));
        } else if (imports.files().front().remote && imports.duplicates()) {
            setNotice(tr("This game is already in your library."));
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
        importLink_->setEnabled(available);
        const auto reason = available ? tr("Import local UTF-8 PGN files")
                                      : tr("Import is unavailable while storage is loading, "
                                           "closing, or an import is running.");
        importFile_->setToolTip(reason);
        pastePgn_->setToolTip(available ? tr("Edit clipboard PGN before importing") : reason);
        importLink_->setToolTip(available ? tr("Import a public game from Chess.com or Lichess")
                                          : reason);
    };
    connect(&imports, &app::ImportController::changed, this, updateActions);
    updateActions();
    connect(importLink_, &QAction::triggered, this,
            [this, &imports] { showGameLinkDialog(*this, imports); });
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
    const auto destination = navigation_.current();
    auto* context =
        (destination == app::Destination::Games && contextPage_ == reviewPage_) ||
                (destination == app::Destination::Analyze && contextPage_ == linkedAnalysis_)
            ? contextPage_
            : nullptr;
    if (!closing_ && reviewPage_ && pages_->currentWidget() == reviewPage_ &&
        reviewController_->state().active && !reviewLeaveAllowed_ && !context) {
        if (reviewLeavePending_) {
            return;
        }
        reviewLeavePending_ = true;
        auto* dialog =
            new QMessageBox(QMessageBox::Question, tr("Leave active review?"),
                            tr("This game is still being reviewed."), QMessageBox::NoButton, this);
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
    // A previous signal subscriber may have synchronously redirected navigation.
    for (std::size_t i = 0; i < destinations_.size(); ++i) {
        auto* action = destinations_[i];
        if (action->data().value<app::Destination>() == destination) {
            action->setChecked(true);
            pages_->setCurrentWidget(context ? context : pages_->widget(static_cast<int>(i)));
            if (analysis_ && pages_->currentWidget() == analysis_ && !closing_) {
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
    colorScheme_ = state.colorScheme;
    setThemeMode(colorScheme_);
    if (auto* settings = pages_->findChild<EngineSettingsPage*>()) {
        settings->setColorScheme(colorScheme_);
    }
    reviewSplit_ = state.reviewSplit;
    if (reviewPage_) {
        reviewPage_->setSplitShare(reviewSplit_);
    }
    if (analysis_) {
        auto preferences = state.board;
        preferences.analysisMultiPv =
            configuredAnalysisLines_.value_or(preferences.analysisMultiPv);
        analysis_->setPreferences(preferences);
        for (auto* view : {linkedAnalysis_, puzzleAnalysis_}) {
            if (view) {
                view->setAnalysisMultiPv(preferences.analysisMultiPv);
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
            std::nullopt,          reviewPage_ ? reviewPage_->splitShare() : reviewSplit_,
            colorScheme_};
}
void MainWindow::showClosing() {
    if (closing_) {
        return;
    }
    closing_ = true;
    home_->setEnabled(false);
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
            analysis_->requestDiscardTabs([this] {
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
