#include "app/AppPaths.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "diagnostics/Logger.h"
#include "ui/shell/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QString>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chess-review"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationVersion(QStringLiteral(CHESS_REVIEW_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Local chess review"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption debugOption(
        QStringLiteral("debug-logging"),
        QStringLiteral("Enable additional redacted logging for this run."));
    parser.addOption(debugOption);
    parser.process(application);
    const auto paths = chess_review::app::AppPaths::resolve();

    chess_review::diagnostics::Logger logger(paths.logDirectory(), parser.isSet(debugOption));
    if (!logger.installQtHandler()) {
        logger.record(chess_review::diagnostics::Event::QtCaptureUnavailable, {},
                      chess_review::diagnostics::Severity::Warning);
    }
    chess_review::app::NavigationController navigation;
    chess_review::app::StartupCoordinator startup(logger);
    chess_review::ui::MainWindow window(navigation, startup.analysis());
    window.installEngines(startup.engines());
    window.installImports(startup.localImports(), startup.library());
    window.setDiagnostics(logger);
    startup.requireQuitConfirmation();
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::quitRequested, &window,
                     [&window] { window.close(); });
    bool windowInitialized = false;
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::initialUiState, &window,
                     [&](const chess_review::app::UiState& state) {
                         window.restoreUiState(state);
                         windowInitialized = true;
                         window.show();
                     });
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::progress, &window,
                     &chess_review::ui::MainWindow::showProgress);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::ready, &window,
                     &chess_review::ui::MainWindow::showReady);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::failed, &window,
                     &chess_review::ui::MainWindow::showFailure);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::uiStateFailed, &window,
                     &chess_review::ui::MainWindow::showUiStateFailure);
    QObject::connect(&window, &chess_review::ui::MainWindow::invalidUiState, &startup, [&] {
        chess_review::diagnostics::Context context;
        context.errorCode = static_cast<int>(chess_review::app::UiStateError::Invalid);
        logger.record(chess_review::diagnostics::Event::UiStateFailed, context,
                      chess_review::diagnostics::Severity::Warning);
    });
    QObject::connect(&window, &chess_review::ui::MainWindow::closeRequested, &startup,
                     &chess_review::app::StartupCoordinator::shutdown);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::stopping, &window, [&] {
        if (windowInitialized) {
            startup.setFinalUiState(window.uiState());
        }
        window.showClosing();
    });
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::finished, &window,
                     &chess_review::ui::MainWindow::completeShutdown);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::finished, &application,
                     &QApplication::quit);
    startup.start(paths.databasePath(), paths.uiStateFilePath());

    return application.exec();
}
