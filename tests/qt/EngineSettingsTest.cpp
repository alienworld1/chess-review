#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "engine/EffectiveSearch.h"
#include "ui/shell/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
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
            auto* selectedPath =
                application.window.findChild<QPlainTextEdit*>(QStringLiteral("engineSelectedPath"));
            QVERIFY(selectedPath && selectedPath->isReadOnly());
            QCOMPARE(selectedPath->toPlainText(), controller.state().selectedPath);
            selectedPath->selectAll();
            controller.refresh();
            QTRY_VERIFY(!controller.state().listing);
            QCOMPARE(selectedPath->textCursor().selectedText(), controller.state().selectedPath);
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
            connect(&validator, &engine::EngineSupervisor::completed, &registrationObserver,
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
                &engine::EngineSupervisor::progress);
            application.startup.start(database);
            QTRY_COMPARE(application.startup.engines().state().profiles.size(), std::size_t(1));
            QCOMPARE(progress.size(), 0);
        }
    }
    void runtimeConnectionAndReadOnlyOptions() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        auto& supervisor = app::EngineControllerTestAccess::validator(controller);
        const auto script = engine_test::scenario(
            directory.filePath(QStringLiteral("runtime.json")),
            engine_test::handshake(QStringLiteral(
                "id name <b>Runtime</b>\nid author Tests\n"
                "option name Threads type spin default 1 min 1 max 4\n"
                "option name Style type combo default Quiet var Quiet var Aggressive\nuciok\n")));
        engine::ValidatorTestAccess::configure(supervisor, script);
        QSignalSpy runtimeEvents(&supervisor, &engine::EngineSupervisor::runtimeChanged);
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        controller.registerSelected();
        QTRY_COMPARE(controller.state().profiles.size(), std::size_t(1));
        const auto saved = controller.state().profiles.front();
        QCOMPARE(runtimeEvents.size(), 0);
        auto* table = application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
        auto* connectButton =
            application.window.findChild<QPushButton*>(QStringLiteral("connectEngine"));
        auto* disconnectButton =
            application.window.findChild<QPushButton*>(QStringLiteral("disconnectEngine"));
        auto* options =
            application.window.findChild<QComboBox*>(QStringLiteral("engineRuntimeOptions"));
        auto* details = application.window.findChild<QPlainTextEdit*>(
            QStringLiteral("engineRuntimeOptionDetails"));
        auto* status = application.window.findChild<QLabel*>(QStringLiteral("engineRuntimeStatus"));
        QVERIFY(connectButton && disconnectButton && options && details && status);
        QVERIFY(!connectButton->isEnabled());
        table->selectRow(0);
        QVERIFY(connectButton->isEnabled());
        application.navigation.navigate(app::Destination::Settings);
        application.window.show();
        connectButton->click();
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        const auto ready = controller.state().runtime;
        QVERIFY(status->text().contains(QStringLiteral("<b>Runtime</b>")));
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QCOMPARE(options->count(), 2);
        QVERIFY(details->isReadOnly());
        QVERIFY(details->toPlainText().contains(QStringLiteral("Maximum: 4")));
        options->setCurrentIndex(1);
        QVERIFY(details->toPlainText().contains(QStringLiteral("Aggressive")));
        QVERIFY(disconnectButton->isEnabled());
        QVERIFY(!connectButton->isEnabled());
        const auto selectedPath = controller.state().selectedPath;
        controller.selectExecutable(QStringLiteral("/cannot-select-while-connected"));
        controller.registerSelected();
        QCOMPARE(controller.state().selectedPath, selectedPath);
        QVERIFY(controller.state().registration != app::EngineRegistrationState::Validating);
        auto stale = ready;
        stale.session = QUuid::createUuid();
        stale.phase = engine::RuntimePhase::Failed;
        emit supervisor.runtimeChanged(stale);
        stale = ready;
        stale.phase = engine::RuntimePhase::Handshaking;
        emit supervisor.runtimeChanged(stale);
        QCOMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        application.navigation.navigate(app::Destination::Analyze);
        application.navigation.navigate(app::Destination::Settings);
        QCOMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        table->clearSelection();
        QVERIFY(disconnectButton->isEnabled());
        QCOMPARE(controller.state().connectedDisplayName, saved.displayName);
        application.window.resize(1100, 720);
        const auto screenshots = qEnvironmentVariable("CHESS_REVIEW_ENGINE_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(
                application.window.grab().save(screenshots + QStringLiteral("/runtime-1100.png")));
            application.window.resize(1360, 850);
            QVERIFY(
                application.window.grab().save(screenshots + QStringLiteral("/runtime-1360.png")));
        }
        disconnectButton->click();
        emit supervisor.runtimeChanged(ready); // Queued success cannot undo cancellation.
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Disconnected);
        controller.refresh();
        QTRY_VERIFY(!controller.state().listing);
        QCOMPARE(controller.state().profiles.front(), saved);
        QVERIFY(!disconnectButton->isEnabled());
        table->selectRow(0);
        connectButton->click();
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        application.startup.shutdown();
        QCOMPARE(controller.state().runtime.phase, engine::RuntimePhase::Disconnecting);
        QVERIFY(!controller.state().runtime.discovery);
        QTRY_COMPARE(application.startup.state(), app::Lifecycle::Stopped);
        QVERIFY(!engine::runtimeActive(controller.state().runtime.phase));
    }
    void savedThreadsReachTestSearchAndFingerprint() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        auto& supervisor = app::EngineControllerTestAccess::validator(controller);
        const auto declarations = QStringLiteral(
            "id name Engine\noption name Threads type spin default 1 min 1 max 16\nuciok\n");
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(directory.filePath(QStringLiteral("register.json")),
                                  engine_test::handshake(declarations)),
            1000);
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        controller.registerSelected();
        QTRY_COMPARE(controller.state().profiles.size(), 1U);
        auto* table = application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
        table->selectRow(0);
        application.navigation.navigate(app::Destination::Settings);
        application.window.show();
        application.window.findChild<QPushButton*>(QStringLiteral("editEngineOptions"))->click();
        auto* value = application.window.findChild<QLineEdit*>(QStringLiteral("engineOptionValue"));
        QVERIFY(value);
        QCOMPARE(value->text(), QStringLiteral("1"));
        value->setText(QStringLiteral("2"));
        QTest::keyPress(value, Qt::Key_Return);
        QTRY_COMPARE(
            controller.state().profiles.front().optionOverrides.value(QStringLiteral("threads")),
            QStringLiteral("2"));
        const auto ready = engine_test::step(QStringLiteral("isready"),
                                             {engine_test::write(QStringLiteral("readyok\n"))});
        const QJsonArray steps{
            engine_test::step(QStringLiteral("uci"), {engine_test::write(declarations)}),
            engine_test::step(QStringLiteral("setoption name Threads value 2"), {}),
            ready,
            engine_test::step(QStringLiteral("ucinewgame"), {}),
            ready,
            engine_test::step(QStringLiteral("position startpos"), {}),
            engine_test::step(QStringLiteral("go depth 4"),
                              {engine_test::write(QStringLiteral(
                                  "info depth 4 score cp 10 pv e2e4\nbestmove e2e4\n"))}),
            ready,
            engine_test::step(QStringLiteral("quit"),
                              {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(directory.filePath(QStringLiteral("test.json")), steps), 1000);
        application.window.findChild<QPushButton*>(QStringLiteral("connectEngine"))->click();
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        QSignalSpy finished(&supervisor, &engine::EngineSupervisor::analysisFinished);
        application.window.findChild<QPushButton*>(QStringLiteral("testEngine"))->click();
        QTRY_COMPARE(finished.size(), 1);
        auto* status = application.window.findChild<QLabel*>(QStringLiteral("engineTestMessage"));
        QTRY_VERIFY(status->text().contains(QStringLiteral("Test passed")));
        const auto result = qvariant_cast<engine::AnalysisResult>(finished.front().front());
        QVERIFY(result.evidence.provenance);
        const auto& profile = controller.state().profiles.front();
        QCOMPARE(result.evidence.provenance->fingerprint,
                 std::get<engine::EngineFingerprint>(
                     engine::quickReviewFingerprint(profile.engine, profile.optionOverrides)));
    }
    void editAndRepairTomlFromSettings() {
        QTemporaryDir directory;
        const auto filename = directory.filePath(QStringLiteral("config.toml"));
        Application application;
        auto& configuration = application.startup.reviewConfiguration();
        application.window.installSettings(configuration);
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")),
                                  std::nullopt, filename);
        QTRY_COMPARE(configuration.state(), app::ConfigurationState::Valid);
        application.navigation.navigate(app::Destination::Settings);
        auto* sections =
            application.window.findChild<QTabWidget*>(QStringLiteral("settingsSections"));
        QCOMPARE(sections->count(), 3);
        sections->setCurrentIndex(1);
        auto* quality = sections->findChild<QComboBox*>(QStringLiteral("defaultReviewQuality"));
        auto* apply = sections->findChild<QPushButton*>(QStringLiteral("saveReviewPreferences"));
        QVERIFY(quality && apply);
        application.window.resize(1100, 720);
        application.window.show();
        QTRY_VERIFY(application.window.rect().contains(
            QRect(apply->mapTo(&application.window, QPoint{}), apply->size())));
        quality->setCurrentIndex(0);
        QVERIFY(apply->isEnabled());
        apply->click();
        QTRY_COMPARE(configuration.snapshot()->values.quality, review::Quality::Fast);
        auto* bothSides = sections->findChild<QCheckBox*>(QStringLiteral("reviewBothSides"));
        auto* threshold = sections->findChild<QLineEdit*>(QStringLiteral("inaccuracyLoss"));
        auto* classification =
            sections->findChild<QComboBox*>(QStringLiteral("classificationPreset"));
        QVERIFY(bothSides && threshold && classification);
        QCOMPARE(classification->currentIndex(), 0);
        QTest::keyPress(classification, Qt::Key_Down);
        QCOMPARE(classification->currentIndex(), 1);
        QCOMPARE(threshold->text(), QStringLiteral("0.04"));
        apply->click();
        QTRY_COMPARE(configuration.snapshot()->values.classification,
                     (review::ClassificationConfig{0.04, 0.08, 0.16}));
        QCOMPARE(classification->currentIndex(), 1);
        bothSides->setChecked(true);
        threshold->setText(QStringLiteral("0.15"));
        QVERIFY(!apply->isEnabled());
        threshold->setText(QStringLiteral("0.06123456789012345"));
        QCOMPARE(classification->currentIndex(), -1);
        QVERIFY(apply->isEnabled());
        apply->click();
        QTRY_VERIFY(configuration.snapshot()->ui.reviewBothSides);
        QCOMPARE(configuration.snapshot()->values.classification.inaccuracyLoss,
                 0.06123456789012345);
        const auto disk =
            std::get<std::shared_ptr<const config::ValidatedConfiguration>>(config::load(filename));
        QCOMPARE(disk->values.classification, configuration.snapshot()->values.classification);
        QVERIFY(disk->ui.reviewBothSides);
        auto* workers = sections->findChild<QSpinBox*>(QStringLiteral("analysisWorkerCount"));
        auto* cpu = sections->findChild<QSpinBox*>(QStringLiteral("analysisCpuThreads"));
        QVERIFY(workers && cpu);
        workers->setValue(2);
        cpu->setValue(1);
        QVERIFY(!apply->isEnabled());
        cpu->setValue(4);
        QVERIFY(apply->isEnabled());
        apply->click();
        QTRY_COMPARE(configuration.snapshot()->enginePool, (engine::EnginePoolLimits{2, 4}));
        const auto resources =
            std::get<std::shared_ptr<const config::ValidatedConfiguration>>(config::load(filename));
        QCOMPARE(resources->enginePool, (engine::EnginePoolLimits{2, 4}));
        auto& engineControl = application.startup.engines();
        auto& supervisor = app::EngineControllerTestAccess::validator(engineControl);
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(directory.filePath(QStringLiteral("pool.json")),
                                  engine_test::handshake()),
            1000);
        QTRY_VERIFY(engineControl.state().ready && !engineControl.state().listing);
        engineControl.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(engineControl.state().registration, app::EngineRegistrationState::Selected);
        engineControl.registerSelected();
        QTRY_COMPARE(engineControl.state().profiles.size(), std::size_t{1});
        QTRY_VERIFY(!engineControl.state().listing);
        QVERIFY(engineControl.connectEngine(engineControl.state().profiles.front().id));
        QTRY_COMPARE(engineControl.state().runtime.phase, engine::RuntimePhase::Ready);
        QCOMPARE(engineControl.state().runtime.workers, 2);
        engineControl.disconnectEngine();
        QTRY_COMPARE(engineControl.state().runtime.phase, engine::RuntimePhase::Disconnected);
        sections->setCurrentIndex(2);
        auto* editor = sections->findChild<QPlainTextEdit*>(QStringLiteral("advancedToml"));
        auto* save = sections->findChild<QPushButton*>(QStringLiteral("saveToml"));
        auto* validate = sections->findChild<QPushButton*>(QStringLiteral("validateToml"));
        auto* report = sections->findChild<QPlainTextEdit*>(QStringLiteral("tomlDiagnostics"));
        editor->selectAll();
        editor->insertPlainText(QStringLiteral("[review]\nquality=\"unexpected\"\n"));
        validate->click();
        QTRY_VERIFY(report->toPlainText().contains(QStringLiteral("review.quality")));
        QVERIFY(editor->document()->isModified());
        QCOMPARE(configuration.snapshot()->values.quality, review::Quality::Fast);
        editor->selectAll();
        editor->insertPlainText(
            QStringLiteral("# Keep this note\n[review]\nquality=\"thorough\"\n"));
        save->click();
        QTRY_COMPARE(configuration.snapshot()->values.quality, review::Quality::Thorough);
        QVERIFY(!editor->document()->isModified());
        QFile file(filename);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().startsWith("# Keep this note"));
        const auto screenshots = qEnvironmentVariable("CHESS_REVIEW_ENGINE_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            auto* appearance =
                application.window.findChild<QComboBox*>(QStringLiteral("appearanceMode"));
            QVERIFY(appearance);
            const auto restoreAppearance =
                qScopeGuard([appearance, initial = appearance->currentIndex()] {
                    appearance->setCurrentIndex(initial);
                });
            for (const bool dark : {false, true}) {
                const auto scheme = dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
                appearance->setCurrentIndex(appearance->findData(static_cast<int>(scheme)));
                QVERIFY(appearance->currentIndex() >= 0);
                for (int index = 0; index < sections->count(); ++index) {
                    sections->setCurrentIndex(index);
                    QCoreApplication::processEvents();
                    QVERIFY(application.window.grab().save(
                        screenshots +
                        QStringLiteral("/settings-tab-%1-%2.png")
                            .arg(index)
                            .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
                }
            }
        }
    }
    void appearanceOverridesTheSystemPaletteAndRestores() {
        Application application;
        auto* appearance =
            application.window.findChild<QComboBox*>(QStringLiteral("appearanceMode"));
        QVERIFY(appearance);
        const auto baseline = qApp->palette();
        const auto restore = qScopeGuard([&] {
            appearance->setCurrentIndex(0);
            qApp->setPalette(baseline);
        });
        auto dark = baseline;
        dark.setColor(QPalette::Window, QColor("#202124"));
        qApp->setPalette(dark);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#191f2a"));
        app::UiState state;
        state.colorScheme = Qt::ColorScheme::Light;
        application.window.restoreUiState(state);
        QCOMPARE(appearance->currentText(), QStringLiteral("Light"));
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#f4f6f8"));
        dark.setColor(QPalette::Window, QColor("#202125"));
        qApp->setPalette(dark);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#f4f6f8"));
        appearance->setCurrentIndex(2);
        QCOMPARE(application.window.uiState().colorScheme, Qt::ColorScheme::Dark);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#191f2a"));
        appearance->setCurrentIndex(1);
        appearance->setCurrentIndex(0);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#191f2a"));
    }
    void connectUsesSelectedRowInsteadOfKeyboardFocus() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        auto& supervisor = app::EngineControllerTestAccess::validator(controller);
        engine::ValidatorTestAccess::configure(
            supervisor, engine_test::scenario(directory.filePath(QStringLiteral("engine.json")),
                                              engine_test::handshake()));
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        controller.selectExecutable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        controller.registerSelected();
        QTRY_COMPARE(controller.state().profiles.size(), std::size_t(1));
        const auto saved = controller.state().profiles.front();
        auto other = saved;
        other.id = saved.id + 1;
        other.displayName = QStringLiteral("Unselected engine");
        other.engine.executablePath = directory.filePath(QStringLiteral("not-selected"));
        app::EngineControllerTestAccess::replaceProfiles(controller, {saved, other});
        auto* table = application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
        auto* connectButton =
            application.window.findChild<QPushButton*>(QStringLiteral("connectEngine"));
        table->selectRow(0);
        // Ctrl+navigation can move focus without selecting that row.
        table->selectionModel()->setCurrentIndex(table->model()->index(1, 0),
                                                 QItemSelectionModel::NoUpdate);
        QCOMPARE(table->selectionModel()->selectedRows().front().row(), 0);
        QCOMPARE(table->currentRow(), 1);
        connectButton->click();
        QCOMPARE(controller.state().runtime.profileId, saved.id);
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        auto* details = application.window.findChild<QPlainTextEdit*>(
            QStringLiteral("engineRuntimeOptionDetails"));
        QVERIFY(details->toPlainText().contains(saved.engine.executablePath));
        app::EngineControllerTestAccess::replaceProfiles(controller, {});
        QCOMPARE(controller.state().runtime.profileId, saved.id);
        QCOMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        QVERIFY(details->toPlainText().contains(saved.engine.executablePath));
        auto* disconnectButton =
            application.window.findChild<QPushButton*>(QStringLiteral("disconnectEngine"));
        QVERIFY(disconnectButton->isEnabled());
        QVERIFY(!connectButton->isEnabled());
    }
    void invisibleEngineTextIsVisibleAndFullDetailsAreAvailable() {
        QTemporaryDir directory;
        Application application;
        auto& controller = application.startup.engines();
        const auto name = QString(300, u'x') + QStringLiteral("\u202E.exe\U000e0001\U0001f600");
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
        QVERIFY(details->toPlainText().contains(QStringLiteral("\\U000e0001")));
        QVERIFY(!details->toPlainText().contains(QStringLiteral("\U000e0001")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("\U0001f600")));
        application.navigation.navigate(app::Destination::Settings);
        application.window.resize(1100, 720);
        application.window.show();
        controller.connectEngine(controller.state().profiles.front().id);
        QTRY_COMPARE(controller.state().runtime.phase, engine::RuntimePhase::Ready);
        // Resize after first-show geometry has settled (offscreen screens may be smaller).
        application.window.resize(1100, 720);
        QCOMPARE(application.window.width(), 1100);
        const auto* status =
            application.window.findChild<QLabel*>(QStringLiteral("engineRuntimeStatus"));
        QVERIFY(status);
        QVERIFY(status->mapTo(&application.window, QPoint{}).x() + status->width() <=
                application.window.width());
        auto* liveDetails = application.window.findChild<QPlainTextEdit*>(
            QStringLiteral("engineRuntimeOptionDetails"));
        QVERIFY(liveDetails->toPlainText().contains(QStringLiteral("\\U000e0001")));
        QVERIFY(application.window.findChild<QComboBox*>(QStringLiteral("engineRuntimeOptions"))
                    ->isHidden());
        const auto screenshots = qEnvironmentVariable("CHESS_REVIEW_ENGINE_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(
                application.window.grab().save(screenshots + QStringLiteral("/hostile-1100.png")));
        }
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
        connect(&validator, &engine::EngineSupervisor::progress, this,
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
    void detectsAndLocatesMissingEngine() {
        QTemporaryDir directory;
        const auto detectDir = directory.filePath(QStringLiteral("detect"));
        const auto movedDir = directory.filePath(QStringLiteral("moved"));
        QVERIFY(QDir().mkpath(detectDir));
        QVERIFY(QDir().mkpath(movedDir));
        const auto originalPath = QDir(detectDir).filePath(QStringLiteral("stockfish"));
        const auto movedPath = QDir(movedDir).filePath(QStringLiteral("stockfish"));
        QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), originalPath));
        QVERIFY(QFile::setPermissions(originalPath, QFileDevice::ReadOwner |
                                                        QFileDevice::WriteOwner |
                                                        QFileDevice::ExeOwner));
        // Non-executable file must be ignored by detection.
        QFile nonExec(QDir(detectDir).filePath(QStringLiteral("lc0")));
        QVERIFY(nonExec.open(QIODevice::WriteOnly));
        nonExec.close();
        QVERIFY(nonExec.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));

        Application application;
        auto& controller = application.startup.engines();
        auto& supervisor = app::EngineControllerTestAccess::validator(controller);
        engine::ValidatorTestAccess::configureDetection(supervisor, {detectDir}, false);
        engine::ValidatorTestAccess::configure(
            supervisor, engine_test::scenario(directory.filePath(QStringLiteral("handshake.json")),
                                              engine_test::handshake()));
        application.startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_VERIFY(controller.state().ready);
        application.navigation.navigate(app::Destination::Settings);
        application.window.show();

        auto* detectButton =
            application.window.findChild<QPushButton*>(QStringLiteral("detectEngines"));
        auto* detectedCombo =
            application.window.findChild<QComboBox*>(QStringLiteral("detectedEngines"));
        auto* selectDetected =
            application.window.findChild<QPushButton*>(QStringLiteral("selectDetectedEngine"));
        QVERIFY(detectButton && detectedCombo && selectDetected);
        detectButton->click();
        QTRY_COMPARE(detectedCombo->count(), 1);
        QVERIFY(detectedCombo->currentText().contains(QStringLiteral("stockfish")));
        selectDetected->click();
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        QCOMPARE(controller.state().selectedPath, QFileInfo(originalPath).canonicalFilePath());
        // Selection must not silently validate or register the binary.
        QTest::qWait(60);
        QVERIFY(controller.state().profiles.empty());

        auto* add = application.window.findChild<QPushButton*>(QStringLiteral("registerEngine"));
        add->click();
        QTRY_COMPARE(controller.state().profiles.size(), std::size_t(1));
        const auto savedId = controller.state().profiles.front().id;

        // Move the binary on disk to simulate an externally moved/missing engine.
        QVERIFY(QFile::rename(originalPath, movedPath));
        controller.refresh();
        QTRY_VERIFY(controller.state().missingProfiles.contains(savedId));

        auto* table = application.window.findChild<QTableWidget*>(QStringLiteral("engineProfiles"));
        QTRY_VERIFY(table->item(0, 0)->text().contains(QStringLiteral("(Missing)")));
        table->selectRow(0);
        auto* details =
            application.window.findChild<QPlainTextEdit*>(QStringLiteral("engineProfileDetails"));
        QVERIFY(
            details->toPlainText().contains(QStringLiteral("Executable missing at saved path")));
        auto* locate = application.window.findChild<QPushButton*>(QStringLiteral("locateEngine"));
        QVERIFY(locate && locate->isVisible() && locate->isEnabled());
        auto* homeEngine =
            application.window.findChild<QLabel*>(QStringLiteral("homeEngineStatus"));
        QVERIFY(homeEngine && homeEngine->text().contains(QStringLiteral("Executable missing")));

        // A different binary cannot silently replace the missing profile.
        const auto differentPath = QDir(movedDir).filePath(QStringLiteral("other-stockfish"));
        QVERIFY(QFile::copy(movedPath, differentPath));
        QFile differentFile(differentPath);
        QVERIFY(differentFile.open(QIODevice::Append));
        QCOMPARE(differentFile.write("diff"), qint64(4));
        differentFile.close();
        QVERIFY(QFile::setPermissions(differentPath, QFileDevice::ReadOwner |
                                                         QFileDevice::WriteOwner |
                                                         QFileDevice::ExeOwner));
        controller.relocateProfile(savedId, differentPath);
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        QCOMPARE(controller.state().relocatingProfileId, std::optional{savedId});
        QVERIFY(add->text().contains(QStringLiteral("locate")));
        add->click();
        QTRY_VERIFY(controller.state().message.contains(QStringLiteral("different engine binary")));
        QCOMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        QVERIFY(!controller.state().relocatingProfileId);

        // Selecting the actual moved binary preserves profile ID and clears missing status.
        controller.relocateProfile(savedId, movedPath);
        QTRY_COMPARE(controller.state().registration, app::EngineRegistrationState::Selected);
        QCOMPARE(controller.state().relocatingProfileId, std::optional{savedId});
        add->click();
        QTRY_VERIFY(controller.state().registration == app::EngineRegistrationState::Idle &&
                    !controller.state().listing);
        QVERIFY(!controller.state().missingProfiles.contains(savedId));
        QCOMPARE(controller.state().profiles.size(), std::size_t(1));
        QCOMPARE(controller.state().profiles.front().id, savedId);
        QCOMPARE(controller.state().profiles.front().engine.executablePath,
                 QFileInfo(movedPath).canonicalFilePath());
        QTRY_VERIFY(!table->item(0, 0)->text().contains(QStringLiteral("(Missing)")));
        QVERIFY(!locate->isVisible());
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
