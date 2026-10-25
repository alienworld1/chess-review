#include "TestEnvironment.h"
#include "engine/private/Fingerprint.h"
#include "engine/private/SearchProtocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <algorithm>
#include <utility>
#include <variant>

using namespace chess_review::engine;
namespace {
ValidatedEngine discovery() {
    ValidatedEngine engine;
    engine.executableSha256 = QByteArray(32, 'a');
    engine.reportedName = QStringLiteral("Engine");
    for (const auto* declaration :
         {"option name Threads type spin default 04 min 1 max 8",
          "option name Hash type spin default 016 min 1 max 128",
          "option name MultiPV type spin default 1 min 1 max 8",
          "option name UCI_ShowWDL type check default false",
          "option name Network type string default a  b",
          "option name Style type combo default Normal var Normal var Aggressive",
          "option name Clear Hash type button"}) {
        auto parsed = parseOption(QString::fromLatin1(declaration));
        if (!std::holds_alternative<UciOption>(parsed) ||
            appendOption(engine.capabilities, std::get<UciOption>(parsed))) {
            qFatal("Invalid test declaration");
        }
    }
    return engine;
}
EngineFingerprint fingerprint(const ValidatedEngine& engine, int width = 1, bool wdl = false) {
    AnalysisRequest request;
    request.id = QUuid::createUuid();
    request.multiPv = width;
    request.wantWdl = wdl;
    auto prepared = std::get<PreparedSearch>(prepareSearch(request, engine.capabilities));
    auto value = makeFingerprint(engine, std::move(prepared.effectiveOptions));
    value.schemaVersion = 1;
    value.resourcePolicyVersion = 0;
    value.resources = ResourceVerification::ExternalResourcesUnverified;
    value.digest = fingerprintDigest(value);
    return value;
}
} // namespace
class EngineFingerprintTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void canonicalConfiguration() {
        auto engine = discovery();
        const auto first = fingerprint(engine);
        QCOMPARE(first.digest.toHex(),
                 QByteArray("5bad3e0d47febf31d68d35be94de23feb6fa122e814994e4b622b6d5116b2068"));
        QCOMPARE(first.options.size(), 6U);
        QCOMPARE(first.options[0].name, QStringLiteral("hash"));
        QCOMPARE(first.options[0].value, QStringLiteral("16"));
        QCOMPARE(first.options[2].value, QStringLiteral("a  b"));
        QCOMPARE(first.options[4].value, QStringLiteral("1"));
        QCOMPARE(first.resources, ResourceVerification::ExternalResourcesUnverified);
        std::reverse(engine.capabilities.options.begin(), engine.capabilities.options.end());
        for (auto& option : engine.capabilities.options) {
            option.name = option.name.toUpper();
        }
        engine.executablePath = QStringLiteral("/different/location");
        engine.lastValidatedAt = QDateTime::currentDateTimeUtc();
        QCOMPARE(fingerprint(engine), first);
        QVERIFY(fingerprint(engine, 2).digest != first.digest);
        QVERIFY(fingerprint(engine, 1, true).digest != first.digest);
        // The adapter resets Threads, so its advertised default is not the effective value.
        for (auto& option : engine.capabilities.options) {
            if (option.name == QStringLiteral("THREADS")) {
                option.defaultValue = QStringLiteral("8");
            }
        }
        QCOMPARE(fingerprint(engine), first);
    }
    void materialChanges() {
        const auto engine = discovery();
        const auto first = fingerprint(engine);
        auto changed = engine;
        changed.executableSha256[0] = 'b';
        QVERIFY(fingerprint(changed).digest != first.digest);
        changed = engine;
        changed.reportedName += u'!';
        QVERIFY(fingerprint(changed).digest != first.digest);
        changed = engine;
        changed.reportedAuthor = QStringLiteral("Author");
        QVERIFY(fingerprint(changed).digest != first.digest);
        changed = engine;
        for (auto& option : changed.capabilities.options) {
            if (option.name == QStringLiteral("Network")) {
                option.defaultValue = QStringLiteral("a b");
            }
        }
        QVERIFY(fingerprint(changed).digest != first.digest);
        auto versioned = first;
        ++versioned.schemaVersion;
        QVERIFY(fingerprintDigest(versioned) != first.digest);
        versioned = first;
        ++versioned.adapterPolicyVersion;
        QVERIFY(fingerprintDigest(versioned) != first.digest);
        versioned = first;
        versioned.options[0].type = OptionType::String;
        QVERIFY(fingerprintDigest(versioned) != first.digest);
    }
    void largeDiscoveryAdmission() {
        auto engine = discovery();
        for (int index = 0; index < 1000; ++index) {
            UciOption option;
            option.name = QString::number(index) + QString(380, u'X');
            option.defaultValue = QString(380, u'Y');
            QVERIFY(!appendOption(engine.capabilities, std::move(option)));
        }
        QVERIFY(encodeCapabilities(engine.capabilities).size() < 1024 * 1024);
        AnalysisRequest request;
        request.id = QUuid::createUuid();
        QElapsedTimer elapsed;
        elapsed.start();
        for (int index = 0; index < 256; ++index) {
            QVERIFY(!validateSearchRequest(request, engine.capabilities));
        }
        qInfo("256 admissions with 1,007 advertised options: %lld ms",
              static_cast<long long>(elapsed.elapsed()));
        const auto prepared = std::get<PreparedSearch>(prepareSearch(request, engine.capabilities));
        QCOMPARE(prepared.effectiveOptions.size(), 1006U);
        QCOMPARE(makeFingerprint(engine, prepared.effectiveOptions).digest.size(), 32);
    }
    void framedEncoding() {
        EngineFingerprint value;
        value.executableSha256 = QByteArray(32, 'a');
        value.reportedName = QStringLiteral("Engine");
        QCOMPARE(fingerprintDigest(value).toHex(),
                 QByteArray("4dbede299dcad7943f52475f946a8b60bf8922b96291c6f236e4c3dc6a58e874"));
        auto v2 = value;
        v2.schemaVersion = 2;
        QCOMPARE(fingerprintDigest(v2).toHex(),
                 QByteArray("e759c1ffaa53dca57636a3ed6441e8950519a91c5439b6b73b3e3e9f9acf3170"));
        auto other = value;
        value.reportedName = QStringLiteral("ab");
        value.reportedAuthor = QStringLiteral("c");
        other.reportedName = QStringLiteral("a");
        other.reportedAuthor = QStringLiteral("bc");
        QVERIFY(fingerprintDigest(value) != fingerprintDigest(other));
        other = value;
        other.reportedAuthor.reset();
        value.reportedAuthor = QString();
        QVERIFY(fingerprintDigest(value) != fingerprintDigest(other));
        other = value;
        other.reportedAuthor = QStringLiteral("");
        QCOMPARE(fingerprintDigest(value), fingerprintDigest(other));
        other = value;
        value.options = {{QStringLiteral("a"), OptionType::String, QStringLiteral("bc")}};
        other.options = {{QStringLiteral("ab"), OptionType::String, QStringLiteral("c")}};
        QVERIFY(fingerprintDigest(value) != fingerprintDigest(other));
    }
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    EngineFingerprintTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "EngineFingerprintTest.moc"
