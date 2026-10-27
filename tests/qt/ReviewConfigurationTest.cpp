#include "app/ReviewConfiguration.h"
#include "TestEnvironment.h"
#include "analysis/ReviewEvidenceAdapter.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "config/Configuration.h"
#include "engine/AnalysisCache.h"
#include "engine/private/Fingerprint.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace chess_review;
namespace {
std::shared_ptr<const config::ValidatedConfiguration> valid(std::string_view text) {
    auto result = config::parse(text);
    if (!std::holds_alternative<std::shared_ptr<const config::ValidatedConfiguration>>(result)) {
        return {};
    }
    return std::get<std::shared_ptr<const config::ValidatedConfiguration>>(result);
}
void write(const QString& path, const QByteArray& text) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(text), text.size());
}
engine::AnalysisResult analysisResult() {
    engine::AnalysisResult result;
    result.outcome = engine::AnalysisOutcome::Completed;
    auto& snapshot = result.evidence;
    snapshot.session = QUuid::createUuid();
    snapshot.request = QUuid::createUuid();
    snapshot.limit = engine::NodeLimit{100};
    snapshot.searchStarted = true;
    snapshot.achievedWidth = 1;
    snapshot.commonDepth = 12;
    snapshot.statistics.depth = 12;
    snapshot.statistics.nodes = 100;
    const auto position = chess::Position::initial();
    const auto played = std::get<chess::Move>(position.parseUci("e2e4"));
    snapshot.bestMove = played;
    engine::AnalysisLine line;
    line.score =
        engine::AnalysisScore{engine::ScoreType::Centipawns, 300, engine::ScoreBound::Exact};
    line.pv = {played};
    line.statistics = snapshot.statistics;
    snapshot.lines = {line};
    auto provenance = std::make_shared<engine::AnalysisProvenance>();
    provenance->position = position.context();
    provenance->createdAt = QDateTime::currentDateTimeUtc();
    auto& fingerprint = provenance->fingerprint;
    fingerprint.schemaVersion = 1;
    fingerprint.executableSha256 = QByteArray(32, 'x');
    fingerprint.reportedName = QStringLiteral("Synthetic engine");
    fingerprint.digest = engine::fingerprintDigest(fingerprint);
    snapshot.provenance = provenance;
    return result;
}
} // namespace
class ReviewConfigurationTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void hostileConfigIsRejectedWithoutCrashing() {
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--hostile-config-probe")});
        QVERIFY(process.waitForStarted(1000));
        const bool finished = process.waitForFinished(5000);
        if (!finished) {
            process.kill();
            process.waitForFinished(1000);
        }
        QVERIFY2(finished, "Hostile configuration must not hang parsing or destruction");
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
    }
    void unreadableAncestorIsNotMissingConfiguration() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto folder = directory.filePath("hidden");
        QVERIFY(QDir().mkdir(folder));
        const auto filename = folder + QStringLiteral("/config.toml");
        write(filename, "[review]\nquality='fast'");
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner);
        });
        QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner));
        QFile probe(filename);
        if (probe.open(QIODevice::ReadOnly)) {
            QSKIP("Current identity bypasses directory search permissions");
        }
        const auto result = config::load(filename);
        QVERIFY(std::holds_alternative<config::Diagnostics>(result));
#else
        QSKIP("Unix directory search permission regression");
#endif
    }
    void specialFilesAreRejectedWithoutBlocking() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto filename = directory.filePath("config.toml");
        const auto link = directory.filePath("link.toml");
        const auto fifo = directory.filePath("fifo.toml");
        write(filename, "version=1");
        QVERIFY(QFile::link(filename, link));
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(link)));
        QVERIFY(QFile::remove(filename));
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(link)));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--reject-config-file"), fifo});
        QVERIFY(process.waitForStarted(1000));
        const bool finished = process.waitForFinished(2000);
        if (!finished) {
            process.kill();
            process.waitForFinished(1000);
        }
        QVERIFY2(finished, "FIFO configuration must never wait for a writer");
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        write(filename, "version=1");
        QVERIFY(std::holds_alternative<config::Diagnostics>(
            config::load(filename + "/child/config.toml")));
#else
        QSKIP("Unix file descriptor boundary regression");
#endif
    }
    void repeatedErrorsDoNotLeakFileDescriptors() {
#ifdef Q_OS_LINUX
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto filename = directory.filePath("config.toml");
        const auto count = [] {
            return QDir(QStringLiteral("/proc/self/fd"))
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .size();
        };
        if (!QDir(QStringLiteral("/proc/self/fd")).isReadable()) {
            QSKIP("Descriptor accounting is unavailable");
        }
        const auto before = count();
        for (int i = 0; i < 200; ++i) {
            write(filename, i % 2 == 0 ? QByteArray("version=") : QByteArray(65537, '#'));
            QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(filename)));
        }
        QCOMPARE(count(), before);
#else
        QSKIP("Linux descriptor accounting regression");
#endif
    }
    void manyErrorsKeepTheNoticeCompact() {
        app::NavigationController navigation;
        ui::MainWindow window(navigation);
        QString details;
        for (int i = 0; i < 32; ++i) {
            details +=
                QStringLiteral("configuration: Unknown configuration key (line %1, column 1)\n")
                    .arg(i + 1);
        }
        window.showConfigurationFailure(details);
        const auto* notice = window.findChild<QLabel*>("configurationNotice");
        QVERIFY(notice);
        QVERIFY(notice->text().size() < 700);
        QVERIFY(notice->text().contains("Diagnostics"));
        QVERIFY(notice->text().contains("line 1"));
    }
    void exampleMatchesDefaults() {
        const auto result = config::load(QString::fromUtf8(CHESS_REVIEW_CONFIG_EXAMPLE));
        QVERIFY(
            std::holds_alternative<std::shared_ptr<const config::ValidatedConfiguration>>(result));
        QCOMPARE(std::get<std::shared_ptr<const config::ValidatedConfiguration>>(result)->values,
                 review::Configuration{});
    }
    void defaultsAndOverrides() {
        const auto defaults = valid("");
        QVERIFY(defaults);
        QCOMPARE(defaults->values, review::Configuration{});
        const auto changed =
            valid("version=1\n[review]\nquality='fast'\npreferWdl=false\n[analysis.fast]"
                  "\nquickScanNodes=1000\n[expectedScore]\nfallbackScaleCp=200\n");
        QVERIFY(changed);
        QCOMPARE(changed->values.quality, review::Quality::Fast);
        QCOMPARE(changed->values.budgets[0].quickScanNodes, 1000ULL);
        QCOMPARE(changed->values.budgets[0].verificationNodes, 2000000ULL);
        QVERIFY(!changed->values.conversion.preferWdl);
        QCOMPARE(changed->values.conversion.fallbackScaleCp, 200);
    }
    void invalidConfiguration_data() {
        QTest::addColumn<QByteArray>("contents");
        for (const auto* input : {"version=2",
                                  "version=0",
                                  "version=1.0",
                                  "privateSecret='x'",
                                  "[analysis]\nfast=1",
                                  "[review]\nquality='custom'",
                                  "[review]\npreferWdl='true'",
                                  "[expectedScore]\nfallbackScaleCp=nan",
                                  "[expectedScore]\nfallbackScaleCp=inf",
                                  "[expectedScore]\nfallbackScaleCp=0",
                                  "[expectedScore]\nfallbackScaleCp=10001",
                                  "[classification]\ninaccuracyLoss=0",
                                  "[classification]\nblunderLoss=1",
                                  "[classification]\nmistakeLoss=0.3",
                                  "[analysis.fast]\nquickScanNodes=-1",
                                  "[analysis.fast]\nquickScanNodes=1000000000001",
                                  "[analysis.fast]\nquickScanNodes=500001",
                                  "[analysis.fast]\nquickScanNodes=true",
                                  "[analysis.fast]\nunknown=2",
                                  "key=",
                                  "version=1\nversion=1"}) {
            QTest::newRow(input) << QByteArray(input);
        }
        QTest::newRow("oversized") << QByteArray(65537, '#');
    }
    void invalidConfiguration() {
        QFETCH(QByteArray, contents);
        const auto result = config::parse(
            std::string_view(contents.constData(), static_cast<std::size_t>(contents.size())));
        QVERIFY(std::holds_alternative<config::Diagnostics>(result));
        const auto errors = std::get<config::Diagnostics>(result);
        QVERIFY(!errors.empty());
        QVERIFY(errors.size() <= 32);
        for (const auto& error : errors) {
            QVERIFY(error.field.find("privateSecret") == std::string::npos);
            QVERIFY(error.message.find("privateSecret") == std::string::npos);
        }
    }
    void diagnosticLocationsAndBound() {
        const auto result = config::parse("[review]\nquality=3");
        const auto& errors = std::get<config::Diagnostics>(result);
        QCOMPARE(errors.front().field, std::string("review.quality"));
        QCOMPARE(errors.front().line, std::optional<std::uint32_t>(2));
        std::string text;
        for (int i = 0; i < 100; ++i) {
            text += "key" + std::to_string(i) + "=0\n";
        }
        QCOMPARE(std::get<config::Diagnostics>(config::parse(text)).size(), 32U);
    }
    void canonicalGoldenVectors() {
        const auto parsed = valid("");
        QVERIFY(parsed);
        QCOMPARE(parsed->identities.classification.toHex(),
                 QByteArray("08ff820351ea33c4027ace720ad8f1c26b12df7eaaada02520143f8a4eb6a36d"));
        QCOMPARE(parsed->identities.conversion.toHex(),
                 QByteArray("5136a1ae0435b81fc247490dd783b0a2949ef6c1030f17f407e12c26f5d48162"));
        QCOMPARE(parsed->identities.analysis.toHex(),
                 QByteArray("cfade89abac9aa3dd679a8bbc3847e84c6bfcfe816674bddfd59459afac4da7c"));
    }
    void identitiesSeparateSemantics() {
        const auto a = valid("");
        const auto b = valid("# "
                             "comment\nversion=1\n[classification]\nblunderLoss=0."
                             "20\ninaccuracyLoss=0.05\nmistakeLoss=0.10\n[review]\nquality='"
                             "standard'\npreferWdl=true\n[expectedScore]\nfallbackScaleCp=300.0");
        QVERIFY(a);
        QVERIFY(b);
        QCOMPARE(a->identities.classification, b->identities.classification);
        QCOMPARE(a->identities.conversion, b->identities.conversion);
        QCOMPARE(a->identities.analysis, b->identities.analysis);
        const auto c = valid("[classification]\nblunderLoss=0.25");
        QVERIFY(c);
        QVERIFY(a->identities.classification != c->identities.classification);
        QCOMPARE(a->identities.conversion, c->identities.conversion);
        QCOMPARE(a->identities.analysis, c->identities.analysis);
        const auto d = valid("[analysis.fast]\nquickScanNodes=1000");
        QVERIFY(d);
        QVERIFY(a->identities.analysis != d->identities.analysis);
        QCOMPARE(a->identities.conversion, d->identities.conversion);
        QCOMPARE(a->identities.classification, d->identities.classification);
        const auto e = valid("[expectedScore]\nfallbackScaleCp=200");
        QVERIFY(e);
        QVERIFY(a->identities.conversion != e->identities.conversion);
        QCOMPARE(a->identities.analysis, e->identities.analysis);
    }
    void files() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto result = config::load(directory.filePath("missing/sub/config.toml"));
        QVERIFY(
            std::holds_alternative<std::shared_ptr<const config::ValidatedConfiguration>>(result));
        QVERIFY(!QFile::exists(directory.filePath("missing")));
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(directory.path())));
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(QString())));
        const auto filename = directory.filePath("config.toml");
        write(filename, "[review]\nquality='thorough'");
        result = config::load(filename);
        QVERIFY(
            std::holds_alternative<std::shared_ptr<const config::ValidatedConfiguration>>(result));
        QCOMPARE(
            std::get<std::shared_ptr<const config::ValidatedConfiguration>>(result)->values.quality,
            review::Quality::Thorough);
        write(filename, QByteArray(65537, '#'));
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(filename)));
        write(filename, "invalid=");
        QVERIFY(std::holds_alternative<config::Diagnostics>(config::load(filename)));
    }
    void loaderLifecycle() {
        app::ReviewConfiguration loader;
        QSignalSpy changed(&loader, &app::ReviewConfiguration::changed);
        QSignalSpy closed(&loader, &app::ReviewConfiguration::closed);
        QCOMPARE(loader.state(), app::ConfigurationState::Loading);
        QVERIFY(!loader.snapshot());
        loader.load(std::nullopt);
        QCOMPARE(changed.count(), 0); // Always queued, including defaults.
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(loader.state(), app::ConfigurationState::Valid);
        const auto snapshot = loader.snapshot();
        QVERIFY(snapshot);
        loader.load(QStringLiteral("ignored"));
        loader.close();
        loader.close();
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(snapshot->values, review::Configuration{});
    }
    void closeWhileLoading() {
        app::ReviewConfiguration loader;
        QSignalSpy changed(&loader, &app::ReviewConfiguration::changed);
        QSignalSpy closed(&loader, &app::ReviewConfiguration::closed);
        loader.load(std::nullopt);
        loader.close();
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(changed.count(), 0);
    }
    void invalidConfigurationKeepsAppReady() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto filename = directory.filePath("config.toml");
        write(filename, "[review]\nquality='privateSecret'");
        app::NavigationController navigation;
        app::StartupCoordinator startup;
        ui::MainWindow window(navigation, startup.analysis());
        window.installEngines(startup.engines());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        QSignalSpy ready(&startup, &app::StartupCoordinator::ready);
        QSignalSpy finished(&startup, &app::StartupCoordinator::finished);
        auto& configuration = startup.reviewConfiguration();
        connect(&configuration, &app::ReviewConfiguration::changed, &window, [&] {
            if (configuration.state() == app::ConfigurationState::Invalid) {
                window.showConfigurationFailure(configuration.safeReport());
            }
        });
        startup.start(directory.filePath("library.sqlite3"), std::nullopt, filename);
        QVERIFY(!startup.engines().state().ready);
        QTRY_COMPARE(ready.count(), 1);
        QTRY_COMPARE(configuration.state(), app::ConfigurationState::Invalid);
        QVERIFY(!configuration.snapshot());
        QCOMPARE(startup.state(), app::Lifecycle::Ready);
        const auto* notice = window.findChild<QLabel*>("configurationNotice");
        QVERIFY(notice);
        QVERIFY(!notice->isHidden());
        QVERIFY(notice->text().contains("review.quality"));
        QVERIFY(!notice->text().contains("privateSecret"));
        QVERIFY(!configuration.safeReport().contains(directory.path()));
        QVERIFY(startup.library().ready());
        QVERIFY(startup.localImports().available());
        QSignalSpy imported(&startup.localImports(), &app::ImportController::finished);
        QVERIFY(
            startup.localImports().importText(QStringLiteral("[Event \"Test\"]\n\n1. e4 e5 *")));
        QTRY_COMPARE(imported.count(), 1);
        QCOMPARE(startup.localImports().inserted(), 1U);
        startup.analysis().start();
        QTRY_VERIFY(!startup.analysis().busy());
        const auto position = startup.analysis().position();
        QVERIFY(position);
        const auto played = std::get<chess::Move>(position->parseUci("e2e4"));
        QSignalSpy moved(&startup.analysis(), &app::AnalysisController::moveAccepted);
        startup.analysis().attemptMove(played, startup.analysis().board().revision);
        QTRY_COMPARE(moved.count(), 1);
        auto* diagnostics = window.findChild<QAction*>("diagnosticsAction");
        QVERIFY(diagnostics);
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        diagnostics->trigger();
        auto* copy = window.findChild<QPushButton*>("copyDiagnosticsSnapshot");
        QVERIFY(copy);
        copy->click();
        const auto report = QApplication::clipboard()->text();
        QVERIFY(report.contains("review.quality"));
        QVERIFY(!report.contains("privateSecret"));
        QVERIFY(!report.contains(directory.path()));
        startup.shutdown();
        QTRY_COMPARE(finished.count(), 1);
    }
    void adapterWitnessNeverBypassesHistoryOrBounds() {
        auto raw = analysisResult();
        auto root = chess::Position::initial();
        const auto direct = analysis::reviewEvidence(raw);
        const auto witnessed = analysis::reviewEvidence(raw, &root);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(witnessed));
        QCOMPARE(std::get<analysis::ReviewEvidenceRecord>(witnessed).evidence.position,
                 std::get<analysis::ReviewEvidenceRecord>(direct).evidence.position);
        QVERIFY(std::holds_alternative<std::monostate>(
            root.play(std::get<chess::Move>(root.parseUci("e2e4")))));
        const auto wrongWitness = analysis::reviewEvidence(raw, &root);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(wrongWitness));
        QCOMPARE(std::get<analysis::ReviewEvidenceRecord>(wrongWitness).evidence.position,
                 raw.evidence.provenance->position);
        root = chess::Position::initial();
        for (int i = 0; i < 3277; ++i) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                QVERIFY(std::holds_alternative<std::monostate>(
                    root.play(std::get<chess::Move>(root.parseUci(uci)))));
            }
        }
        auto provenance = std::make_shared<engine::AnalysisProvenance>(*raw.evidence.provenance);
        provenance->position = root.context();
        raw.evidence.provenance = provenance;
        // The board exactly matches this oversized context; independent structural limits still
        // reject it.
        QVERIFY(std::holds_alternative<analysis::EvidenceAdapterIssue>(
            analysis::reviewEvidence(raw, &root)));
    }
    void adapterUsesScoreSetDepth() {
        auto raw = analysisResult();
        raw.evidence.statistics.depth = 13;
        const auto adapted = analysis::reviewEvidence(raw);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(adapted));
        const auto& evidence = std::get<analysis::ReviewEvidenceRecord>(adapted).evidence;
        QVERIFY(evidence.complete);
        QCOMPARE(evidence.strength.depth, raw.evidence.commonDepth);
        QCOMPARE(evidence.strength.depth, std::optional<int>(12));
    }
    void adapterTerminalPreservesRawClaim() {
        auto raw = analysisResult();
        auto provenance = std::make_shared<engine::AnalysisProvenance>(*raw.evidence.provenance);
        const auto position =
            std::get<chess::Position>(chess::Position::fromFen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"));
        provenance->position = position.context();
        raw.evidence.provenance = provenance;
        raw.evidence.bestMove.reset();
        raw.evidence.achievedWidth = 0;
        raw.evidence.commonDepth.reset();
        raw.evidence.lines[0].pv.clear();
        raw.evidence.lines[0].score =
            engine::AnalysisScore{engine::ScoreType::Mate, 0, engine::ScoreBound::Exact};
        raw.evidence.lines[0].wdl = engine::Wdl{0, 0, 1000};
        const auto adapted = analysis::reviewEvidence(raw);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(adapted));
        const auto& evidence = std::get<analysis::ReviewEvidenceRecord>(adapted).evidence;
        QCOMPARE(evidence.terminal,
                 std::optional<review::TerminalOutcome>(review::TerminalOutcome::Checkmate));
        QVERIFY(evidence.complete);
        QVERIFY(evidence.candidates.empty());
        QVERIFY(evidence.evaluation.score);
        QCOMPARE(evidence.evaluation.score->kind, review::ScoreKind::Mate);
        QCOMPARE(evidence.evaluation.score->value, 0);
        QVERIFY(evidence.evaluation.wdl);
        QCOMPARE(evidence.evaluation.wdl->losses, 1000);
    }
    void adapterPreservesProvenanceAndRejectsInsufficientEvidence() {
        auto raw = analysisResult();
        QVERIFY(!engine::validateCacheResult(raw));
        auto adapted = analysis::reviewEvidence(raw);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(adapted));
        const auto record = std::get<analysis::ReviewEvidenceRecord>(adapted);
        QVERIFY(record.evidence.complete);
        QCOMPARE(record.provenance, raw.evidence.provenance);
        QCOMPARE(record.evidence.position, raw.evidence.provenance->position);
        QCOMPARE(record.evidence.attemptIdentity,
                 raw.evidence.request.toString(QUuid::WithoutBraces).toStdString());
        QCOMPARE(record.evidence.candidates.size(), 1U);
        QCOMPARE(record.evidence.evaluation.score->value, 300);
        QCOMPARE(record.evidence.strength.nodes, std::optional<std::uint64_t>(100));
        raw.evidence.statistics.nodes = 99;
        adapted = analysis::reviewEvidence(raw);
        QVERIFY(std::holds_alternative<analysis::ReviewEvidenceRecord>(adapted));
        QVERIFY(!std::get<analysis::ReviewEvidenceRecord>(adapted).evidence.complete);
        raw.outcome = engine::AnalysisOutcome::Stopped;
        QVERIFY(
            std::holds_alternative<analysis::EvidenceAdapterIssue>(analysis::reviewEvidence(raw)));
        raw.outcome = engine::AnalysisOutcome::Completed;
        raw.evidence.lines[0].pv.clear();
        QVERIFY(
            std::holds_alternative<analysis::EvidenceAdapterIssue>(analysis::reviewEvidence(raw)));
        // The retained immutable provenance survives source destruction.
        raw = {};
        QVERIFY(!record.provenance->fingerprint.digest.isEmpty());
    }
};
int main(int argc, char** argv) {
    if (argc == 3 && QByteArray(argv[1]) == "--reject-config-file") {
        return std::holds_alternative<config::Diagnostics>(
                   config::load(QString::fromLocal8Bit(argv[2])))
                   ? 0
                   : 2;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--hostile-config-probe") {
        std::string dotted;
        for (int i = 0; i < 30000; ++i) {
            dotted += "a.";
        }
        dotted += "a=1";
        if (!std::holds_alternative<config::Diagnostics>(config::parse(dotted))) {
            return 2;
        }
        const std::string arrays = "unknown=" + std::string(200, '[') + "0" + std::string(200, ']');
        if (!std::holds_alternative<config::Diagnostics>(config::parse(arrays))) {
            return 3;
        }
        std::string tables = "unknown=";
        for (int i = 0; i < 200; ++i) {
            tables += "{a=";
        }
        tables += "0" + std::string(200, '}');
        return std::holds_alternative<config::Diagnostics>(config::parse(tables)) ? 0 : 4;
    }
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    ReviewConfigurationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReviewConfigurationTest.moc"
