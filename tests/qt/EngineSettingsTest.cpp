#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "ui/shell/MainWindow.h"
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
using namespace chess_review;
namespace {
class Application final {
  public:
    Application() : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
    }
    ~Application() {
        QEventLoop loop;
        bool stopped = false;
        QObject::connect(&startup, &app::StartupCoordinator::finished, &loop, [&] {
            stopped = true;
            loop.quit();
        });
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        startup.shutdown();
        if (startup.state() != app::Lifecycle::Stopped) {
            loop.exec();
        } else {
            stopped = true;
        }
        if (!stopped) {
            qFatal("Engine application fixture did not close");
        }
        window.completeShutdown();
    }
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window;
};
} // namespace
class EngineSettingsTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void settingsWithoutAnalysisController() {
        Application application;
        ui::MainWindow shell(application.navigation);
        shell.installEngines(application.startup.engines());
        QVERIFY(shell.findChild<QTableWidget*>(QStringLiteral("engineProfiles")));
        shell.completeShutdown();
    }
    void explicitRegistrationAndReopen() {
        QTemporaryDir directory;
        const auto database = directory.filePath(QStringLiteral("library.sqlite3"));
        const auto scenario = engine_test::scenario(
            directory.filePath(QStringLiteral("engine.json")),
            engine_test::handshake(QStringLiteral("id name <b>Engine</b>\nuciok\n")));
        {
            Application application;
            auto& controller = application.startup.engines();
            engine::ValidatorTestAccess::configure(
                app::EngineControllerTestAccess::validator(controller), scenario);
            application.startup.start(database);
            QTRY_VERIFY(controller.state().ready);
            application.navigation.navigate(app::Destination::Settings);
            application.window.show();
            controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
            QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
            // Selecting and inspecting a path must not start the handshake or create a profile.
            QTest::qWait(80);
            QCOMPARE(controller.state().profiles.size(), std::size_t(0));
            QCOMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
            auto* add =
                application.window.findChild<QPushButton*>(QStringLiteral("registerEngine"));
            QVERIFY(add && add->isEnabled());
            int savingUpdates = 0;
            bool replayed = false;
            QObject registrationObserver;
            connect(
                &controller, &app::EngineSettingsController::changed, &registrationObserver, [&] {
                    if (controller.state().registration == app::EngineRegistrationState::Saving) {
                        ++savingUpdates;
                    }
                });
            auto& validator = app::EngineControllerTestAccess::validator(controller);
            connect(&validator, &engine::ExecutableValidator::completed, &registrationObserver,
                    [&](QUuid request, const engine::ValidationResult& result) {
                        if (!replayed && std::holds_alternative<engine::ValidatedEngine>(result)) {
                            replayed = true;
                            emit validator.completed(request, result);
                        }
                    });
            add->click();
            QTRY_COMPARE(controller.state().profiles.size(), std::size_t(1));
            QCOMPARE(savingUpdates, 1);
            QCOMPARE(controller.state().profiles[0].displayName, QStringLiteral("<b>Engine</b>"));
            auto* table =
                application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
            QCOMPARE(table->item(0, 0)->text(), QStringLiteral("<b>Engine</b>"));
            table->selectRow(0);
            auto* details = application.window.findChild<QPlainTextEdit*>(
                QStringLiteral("engineProfileDetails"));
            QVERIFY(details);
            QVERIFY(details->toPlainText().contains(QStringLiteral("<b>Engine</b>")));
            QVERIFY(details->toPlainText().contains(
                controller.state().profiles[0].engine.executablePath));
            QSignalSpy tableChanges(table->model(), &QAbstractItemModel::dataChanged);
            controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
            QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
            QCOMPARE(table->currentRow(), 0);
            QVERIFY(!table->selectedItems().isEmpty());
            QCOMPARE(tableChanges.size(), 0);
            controller.registerSelected();
            QTRY_VERIFY(controller.state().message.contains(QStringLiteral("already registered")));
            QCOMPARE(controller.state().profiles.size(), std::size_t(1));
            application.window.resize(1100, 720);
            const auto screenshots = qEnvironmentVariable("CHESS_REVIEW_ENGINE_SCREENSHOTS");
            if (!screenshots.isEmpty()) {
                QVERIFY(application.window.grab().save(screenshots +
                                                       QStringLiteral("/settings-1100.png")));
                application.window.resize(1360, 850);
                QVERIFY(application.window.grab().save(screenshots +
                                                       QStringLiteral("/settings-1360.png")));
            }
        }
        {
            Application application;
            // No scenario is configured: any unintended launch would fail the fixture executable.
            QSignalSpy progress(
                &app::EngineControllerTestAccess::validator(application.startup.engines()),
                &engine::ExecutableValidator::progress);
            application.startup.start(database);
            QTRY_COMPARE(application.startup.engines().state().profiles.size(), std::size_t(1));
            QCOMPARE(progress.size(), 0);
        }
    }
    void invisibleEngineTextIsVisibleAndFullDetailsAreAvailable() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        const auto name = QString(300, u'x') + QStringLiteral("\u202E.exe");
        engine::ValidatorTestAccess::configure(
            app::EngineControllerTestAccess::validator(controller),
            engine_test::scenario(directory.filePath(QStringLiteral("engine.json")),
                                  engine_test::handshake(QStringLiteral("id name ") + name +
                                                         QStringLiteral("\nuciok\n"))));
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        controller.registerSelected();
        QTRY_COMPARE(controller.state().profiles.size(), std::size_t(1));
        QCOMPARE(controller.state().profiles[0].engine.reportedName, name);
        auto* table = application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
        table->selectRow(0);
        auto* details =
            application.window.findChild<QPlainTextEdit*>(QStringLiteral("engineProfileDetails"));
        QVERIFY(details->toPlainText().contains(QString(300, u'x')));
        QVERIFY(details->toPlainText().contains(QStringLiteral("\\u202e.exe")));
        QVERIFY(!details->toPlainText().contains(QChar(0x202e)));
    }
    void cancelBeforeQueuedSuccess() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        engine::ValidatorTestAccess::configure(
            app::EngineControllerTestAccess::validator(controller),
            engine_test::scenario(directory.filePath(QStringLiteral("success.json")),
                                  engine_test::handshake()));
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        connect(&controller, &app::EngineSettingsController::changed, this, [&] {
            if (controller.state().message == QStringLiteral("Checking executable is unchanged…")) {
                controller.cancel();
            }
        });
        auto& validator = app::EngineControllerTestAccess::validator(controller);
        connect(&validator, &engine::ExecutableValidator::progress, this,
                [&](QUuid request, const QString& message) {
                    if (message == QStringLiteral("Checking executable is unchanged…")) {
                        emit validator.progress(request, QStringLiteral("stale progress"));
                        QCOMPARE(controller.state().message,
                                 QStringLiteral("Cancelling and closing engine…"));
                    }
                });
        controller.registerSelected();
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        QVERIFY(controller.state().profiles.empty());
        QVERIFY(controller.state().message.contains(QStringLiteral("cancelled")));
    }
    void cancelAndShutdown() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        const auto script = engine_test::scenario(
            directory.filePath(QStringLiteral("hang.json")),
            {engine_test::step(QStringLiteral("uci"),
                               {QJsonObject{{QStringLiteral("type"), QStringLiteral("hang")}}})});
        engine::ValidatorTestAccess::configure(
            app::EngineControllerTestAccess::validator(controller), script, 10000);
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        controller.registerSelected();
        auto* cancel =
            application.window.findChild<QPushButton*>(QStringLiteral("cancelEngineRegistration"));
        QVERIFY(cancel->isEnabled());
        cancel->click();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state().registration,
                                  app::EngineRegistrationState::Selected, 5000);
        QVERIFY(controller.state().profiles.empty());
        controller.registerSelected();
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Validating);
        application.startup.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(application.startup.state(), app::Lifecycle::Stopped, 5000);
    }
};
int main(int argc, char* argv[]) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    EngineSettingsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "EngineSettingsTest.moc"
