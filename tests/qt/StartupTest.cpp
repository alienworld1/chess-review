#include "app/StartupCoordinator.h"
#include "ui/StartupWindow.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using chess_review::app::StartupCoordinator;
using chess_review::ui::StartupWindow;

class StartupTest final : public QObject {
    Q_OBJECT

  private slots:
    void initializesAndCloses();
    void failureShowsDiagnostic();
    void closeDuringStartup();
};

void StartupTest::initializesAndCloses() {
    QTemporaryDir directory;
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    QVERIFY(status);
    window.show();
    startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(status->text(), QStringLiteral("Ready"));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

void StartupTest::failureShowsDiagnostic() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("broken.sqlite3"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("invalid database") > 0);
    file.close();
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    const auto* diagnostic = window.findChild<QLabel*>(QStringLiteral("startupDiagnostic"));
    QVERIFY(status);
    QVERIFY(diagnostic);
    window.show();
    startup.start(path);
    QTRY_COMPARE(status->text(), QStringLiteral("Could not initialize local storage"));
    QVERIFY(diagnostic->text().contains(path));
    QCOMPARE(diagnostic->textFormat(), Qt::PlainText);
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

void StartupTest::closeDuringStartup() {
    QTemporaryDir directory;
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    window.show();
    startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    StartupTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "StartupTest.moc"
