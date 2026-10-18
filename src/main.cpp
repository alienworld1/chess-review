#include "app/StartupCoordinator.h"
#include "ui/StartupWindow.h"

#include <QApplication>
#include <QString>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chess-review"));
    QApplication::setQuitOnLastWindowClosed(false);

    chess_review::ui::StartupWindow window;
    chess_review::app::StartupCoordinator startup(window);
    QObject::connect(&startup, &chess_review::app::StartupCoordinator::finished, &application,
                     &QApplication::quit);
    window.show();
    startup.start(chess_review::app::StartupCoordinator::defaultDatabasePath());

    return application.exec();
}
