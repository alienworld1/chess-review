#include "ui/shell/MainWindow.h"
#include "app/EngineAnalysisController.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/games/GamesPage.h"
#include "ui/games/PgnTextEdit.h"
#include "ui/settings/EngineSettingsPage.h"
#include "ui/shell/StartupPanel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
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

namespace chess_review::ui {
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
    auto* buttons = new QVBoxLayout(sidebar);
    buttons->setContentsMargins(0, 0, 12, 0);
    pages_ = new QStackedWidget(central);
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
                navigation_.navigate(destination);
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
void MainWindow::installEngineAnalysis(app::EngineAnalysisController& engine) {
    Q_ASSERT(analysis_);
    analysis_->installEngineAnalysis(engine);
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
        engine.setVisible(!closing_ && navigation_.current() == app::Destination::Analyze);
    });
    engine.setVisible(navigation_.current() == app::Destination::Analyze);
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
    connect(flip, &QAction::triggered, &controller, &app::AnalysisController::flip);
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
    pages_->insertWidget(pageIndex, games);
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
    fileButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    homeLayout->insertWidget(1, fileButton, 0, Qt::AlignLeft);
    auto* pasteButton = new QToolButton(home);
    pasteButton->setDefaultAction(pastePgn_);
    pasteButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    homeLayout->insertWidget(2, pasteButton, 0, Qt::AlignLeft);
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
    if (library_ && navigation_.current() == app::Destination::Games && !closing_) {
        library_->activate();
    }
    // A previous signal subscriber may have synchronously redirected navigation.
    for (std::size_t i = 0; i < destinations_.size(); ++i) {
        auto* action = destinations_[i];
        if (action->data().value<app::Destination>() == navigation_.current()) {
            action->setChecked(true);
            pages_->setCurrentIndex(static_cast<int>(i));
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
    if (analysis_) {
        analysis_->setPreferences(state.board);
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
    return {saveGeometry(), windowState().testFlag(Qt::WindowMaximized), navigation_.current(),
            analysis_ ? analysis_->preferences() : app::BoardPreferences{}};
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
            analysis_->requestDiscard([this] { emit closeRequested(); });
        } else {
            emit closeRequested();
        }
    }
}
} // namespace chess_review::ui
