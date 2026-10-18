#include <QApplication>
#include <QMainWindow>
#include <QString>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("chess-review"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("chess-review"));
    window.resize(1000, 700);
    window.show();

    return application.exec();
}
