#include "app/AppPaths.h"
#include "app/StartupCoordinator.h"
#include "diagnostics/Logger.h"
#include "ui/StartupWindow.h"
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
    logger.installQtHandler();
    chess_review::ui::StartupWindow window;
    chess_review::app::StartupCoordinator startup(window, logger);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::finished, &application,
                     &QApplication::quit);
    window.show();
    startup.start(paths.databasePath());

    return application.exec();
}
