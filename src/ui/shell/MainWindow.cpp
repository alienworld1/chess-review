#include "ui/shell/MainWindow.h"
#include "ui/shell/StartupPanel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
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
    auto* quit = file->addAction(tr("&Quit"));
    quit->setObjectName(QStringLiteral("quitAction"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto* view = menuBar()->addMenu(tr("&View"));
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
void MainWindow::selectDestination() {
    // A previous signal subscriber may have synchronously redirected navigation.
    for (std::size_t i = 0; i < destinations_.size(); ++i) {
        auto* action = destinations_[i];
        if (action->data().value<app::Destination>() == navigation_.current()) {
            action->setChecked(true);
            pages_->setCurrentIndex(static_cast<int>(i));
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
    return {saveGeometry(), windowState().testFlag(Qt::WindowMaximized), navigation_.current()};
}
void MainWindow::showClosing() {
    if (closing_) {
        return;
    }
    closing_ = true;
    for (auto* action : destinations_) {
        action->setEnabled(false);
    }
    reset_->setEnabled(false);
    startup_->showClosing();
}
void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::fitToScreen);
}
void MainWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    if (!closing_) {
        emit closeRequested();
    }
}
} // namespace chess_review::ui
