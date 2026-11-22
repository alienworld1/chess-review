#include "TestEnvironment.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "ui/Theme.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <cstring>
#include <utility>

using namespace chess_review;
namespace {
const auto lichessLink = QStringLiteral("https://lichess.org/Ab12Cd34");
const auto chessComLink = QStringLiteral("https://www.chess.com/game/live/169227053782");
const QByteArray pgnBytes =
    "[White \"Mira\"]\n[Black \"Arun\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0\n";
const QByteArray pgnMediaType = "application/x-chess-pgn; charset=utf-8";
const QByteArray jsonMediaType = "application/json; charset=utf-8";

// Delivers selected responses on demand. Requests never leave the process.
class FixtureReply final : public QNetworkReply {
  public:
    FixtureReply(const QNetworkRequest& request, QNetworkAccessManager* parent)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(ReadOnly | Unbuffered);
    }
    void respond(int status, const QByteArray& mediaType, QByteArray body = {}) {
        if (status) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        } else {
            setError(HostNotFoundError, QStringLiteral("sensitive native diagnostic"));
        }
        setRawHeader("Content-Type", mediaType);
        if (status == 302) {
            setRawHeader("Location", "http://127.0.0.1/private");
            setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                         QUrl(QStringLiteral("http://127.0.0.1/private")));
        }
        data_ = std::move(body);
        emit metaDataChanged();
        if (!data_.isEmpty()) {
            emit readyRead();
        }
        if (!isFinished()) {
            setFinished(true);
            emit finished();
        }
    }
    void abort() override {
        setError(OperationCanceledError, QStringLiteral("Cancelled"));
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return data_.size() - offset_ + QNetworkReply::bytesAvailable();
    }

  protected:
    qint64 readData(char* destination, qint64 maxBytes) override {
        const auto count = std::min(maxBytes, static_cast<qint64>(data_.size()) - offset_);
        if (count == 0) {
            return isFinished() ? -1 : 0;
        }
        std::memcpy(destination, data_.constData() + offset_, static_cast<std::size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray data_;
    qint64 offset_ = 0;
};

class FixtureNetwork final : public QNetworkAccessManager {
  public:
    QList<QNetworkRequest> requests;
    FixtureReply* reply = nullptr; // Owned by the manager and cleared on completion.

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override {
        if (operation != GetOperation || outgoingData) {
            qFatal("Expected an anonymous GET");
        }
        requests.append(request);
        auto* response = new FixtureReply(request, this);
        reply = response;
        connect(response, &QNetworkReply::finished, this, [this, response] {
            if (reply == response) {
                reply = nullptr;
            }
        });
        return response;
    }
};

struct Fixture {
    QTemporaryDir directory;
    app::StartupCoordinator startup;
    bool closed = false;

    explicit Fixture(QNetworkAccessManager* network = nullptr) : startup(nullptr, network) {
        if (!directory.isValid()) {
            qFatal("Temporary directory unavailable");
        }
        QObject::connect(&startup, &app::StartupCoordinator::finished, &startup,
                         [this] { closed = true; });
        startup.start(directory.filePath(QStringLiteral("remote.sqlite3")));
    }
    ~Fixture() {
        startup.shutdown();
        if (!QTest::qWaitFor([this] { return closed; }, 10'000)) {
            qFatal("Remote import fixture did not shut down");
        }
    }
    app::ImportController& imports() { return startup.localImports(); }
};

QByteArray chessMetadata(qint64 id, bool finished = true,
                         const QString& white = QStringLiteral("Sample_User")) {
    const QJsonObject game{{"id", id},
                           {"isFinished", finished},
                           {"endTime", 1739051923},
                           {"pgnHeaders", QJsonObject{{"White", white}}}};
    return QJsonDocument(QJsonObject{{"game", game}}).toJson(QJsonDocument::Compact);
}

QJsonObject archiveRecord(const QString& url, const QByteArray& bytes,
                          const QString& rules = QStringLiteral("chess")) {
    return {{"url", url}, {"rules", rules}, {"pgn", QString::fromUtf8(bytes)}};
}

QByteArray chessArchive(const QJsonArray& games) {
    return QJsonDocument(QJsonObject{{"games", games}}).toJson(QJsonDocument::Compact);
}
} // namespace

class RemoteImportTest final : public QObject {
    Q_OBJECT
  private slots:
    void linkPolicy();
    void desktopJourney();
    void chessComArchive_data();
    void chessComArchive();
    void chessComRejections();
    void badDownloads_data();
    void badDownloads();
    void cancellationAndShutdown();
    void liveProviders_data();
    void liveProviders();
};

void RemoteImportTest::linkPolicy() {
    const auto lichess = input::RemoteGame::fromLink(
        u"  HTTP://WWW.LICHESS.ORG/Ab12Cd34/black?tracking=example#42  ");
    QVERIFY(lichess);
    QCOMPARE(lichess->publicUrl(), QUrl(lichessLink));
    QVERIFY(input::RemoteGame::fromLink(u"lichess.org/game/Ab12Cd34"));
    QVERIFY(input::RemoteGame::fromLink(u"lichess.org/game/export/Ab12Cd34?x=https://example.org"));
    const auto chessCom = input::RemoteGame::fromLink(
        u"https://chess.com/pt-br/analysis/game/live/169227053782/analysis?token=text#details");
    QVERIFY(chessCom);
    QCOMPARE(chessCom->publicUrl(), QUrl(chessComLink));
    QVERIFY(input::RemoteGame::fromLink(u"https://chess.com/daily/game/768394935"));

    for (const auto* link :
         {u"https://lichess.org.evil.test/Ab12Cd34", u"https://evil.test@lichess.org/Ab12Cd34",
          u"https://lichess.org:443/Ab12Cd34", u"https://lichess.org@evil.test/Ab12Cd34",
          u"https://lichess.org/Ab12Cd34xxxx", u"https://lichess.org/analysis/",
          u"https://lichess.org/training", u"https://lichess.org/study/Ab12Cd34",
          u"https://lichess.org/%2fAb12Cd34", u"https://lichess.org/../Ab12Cd34",
          u"https://lichess.org\\Ab12Cd34", u"ftp://lichess.org/Ab12Cd34", u"file:///Ab12Cd34",
          u"https://127.0.0.1/Ab12Cd34", u"https://chess.com/analysis",
          u"https://chess.com/game/bot/12",
          u"https://api.chess.com/pub/player/someone/games/2024/07",
          u"https://chess.com/game/daily/-12", u"https://chess.com/game/live/001"}) {
        QVERIFY2(!input::RemoteGame::fromLink(link), qPrintable(QStringView(link).toString()));
    }
    QVERIFY(!input::RemoteGame::fromLink(QString(input::maxGameLinkLength + 1, u'x')));
}

void RemoteImportTest::desktopJourney() {
    FixtureNetwork network;
    Fixture fixture(&network);
    app::NavigationController navigation;
    ui::MainWindow window(navigation, fixture.startup.analysis());
    window.installImports(fixture.imports(), fixture.startup.library());
    window.installReview(fixture.startup.reviewGame());
    window.resize(1100, 720);
    window.show();
    QTRY_VERIFY(fixture.imports().available());
    window.showReady();
    auto* action = window.findChild<QAction*>(QStringLiteral("importGameLinkAction"));
    auto* home = window.findChild<QWidget*>(QStringLiteral("homePage"));
    QVERIFY(action && home);
    const auto buttons = home->findChildren<QToolButton*>();
    const auto button = std::ranges::find(buttons, action, &QToolButton::defaultAction);
    QVERIFY(button != buttons.end());
    QVERIFY((*button)->isVisible());
    QVERIFY(network.requests.isEmpty());

    QApplication::clipboard()->setText(QStringLiteral("https://lichess.org/Ab12Cd34#20"));
    QTest::mouseClick(*button, Qt::LeftButton);
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("importGameLinkDialog"));
    QVERIFY(dialog && dialog->isVisible());
    auto* editor = dialog->findChild<QLineEdit*>(QStringLiteral("gameLinkEditor"));
    auto* error = dialog->findChild<QLabel*>(QStringLiteral("gameLinkError"));
    QVERIFY(editor && error);
    QCOMPARE(editor->text(), lichessLink);
    QCOMPARE(editor->selectedText(), editor->text());
    QVERIFY(network.requests.isEmpty());
    const auto renderDirectory = qEnvironmentVariable("CHESS_REVIEW_REMOTE_IMPORT_RENDER_DIR");
    if (!renderDirectory.isEmpty()) {
        QVERIFY(
            dialog->grab().save(QDir(renderDirectory).filePath(QStringLiteral("link-dialog.png"))));
    }
    editor->setText(QStringLiteral("https://example.test/Ab12Cd34"));
    QTest::keyPress(editor, Qt::Key_Return);
    QTest::keyRelease(editor, Qt::Key_Return);
    QVERIFY(error->isVisible());
    QVERIFY(network.requests.isEmpty());
    editor->setText(lichessLink);
    QTest::keyPress(editor, Qt::Key_Return);
    QCOMPARE(navigation.current(), app::Destination::Games);
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(!action->isEnabled());
    auto* status = window.findChild<QLabel*>(QStringLiteral("importStatus"));
    QVERIFY(status);
    QTRY_VERIFY(status->text().contains(u"Downloading"));

    const auto request = network.requests.front();
    QCOMPARE(
        request.url(),
        QUrl(QStringLiteral("https://lichess.org/game/export/Ab12Cd34?clocks=true&evals=false")));
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
             int(QNetworkRequest::ManualRedirectPolicy));
    for (const auto attribute : {QNetworkRequest::AuthenticationReuseAttribute,
                                 QNetworkRequest::CookieLoadControlAttribute,
                                 QNetworkRequest::CookieSaveControlAttribute}) {
        QCOMPARE(request.attribute(attribute).toInt(), int(QNetworkRequest::Manual));
    }
    network.reply->respond(200, pgnMediaType, pgnBytes);
    QTRY_COMPARE(fixture.imports().inserted(), 1U);
    const auto* review = window.findChild<QWidget*>(QStringLiteral("reviewPage"));
    QTRY_VERIFY(review && review->isVisible());
    QVERIFY(action->isEnabled());
    const auto id =
        std::get<persistence::Inserted>(fixture.imports().files().front().items.front().outcome).id;
    QSignalSpy loaded(&fixture.startup.gameRepository(), &persistence::Database::gameLoaded);
    const auto loadRequest = QUuid::createUuid();
    fixture.startup.gameRepository().loadGame(loadRequest, id);
    const auto loadedGame = [&] {
        return std::ranges::find_if(loaded, [loadRequest](const auto& signal) {
            return signal.front().toUuid() == loadRequest;
        });
    };
    QTRY_VERIFY(loadedGame() != loaded.end());
    const auto result = loadedGame()->at(1).value<persistence::LoadGameResult>();
    const auto* game = std::get_if<persistence::StoredGame>(&result);
    QVERIFY(game);
    QCOMPARE(game->source.type, QStringLiteral("lichess"));
    QCOMPARE(game->source.uri, lichessLink);
    QCOMPARE(game->document->originalMainline().size(), 2U);

    QVERIFY(fixture.imports().importUrl(*input::RemoteGame::fromLink(lichessLink)));
    network.reply->respond(200, pgnMediaType, pgnBytes);
    QTRY_VERIFY(!fixture.imports().busy());
    QCOMPARE(fixture.imports().duplicates(), 1U);
    QCOMPARE(
        std::get<persistence::Duplicate>(fixture.imports().files().front().items.front().outcome)
            .id,
        id);
    QApplication::clipboard()->clear();
    QVERIFY(fixture.imports().importUrl(*input::RemoteGame::fromLink(lichessLink)));
    QVERIFY(!review->isVisible());
    network.reply->respond(429, {});
    QTRY_VERIFY(status->isVisible() && status->text().contains(u"limiting downloads"));
    const auto* report = window.findChild<QPushButton*>(QStringLiteral("importReportButton"));
    QVERIFY(report && report->isVisible());
    if (!renderDirectory.isEmpty()) {
        QTRY_VERIFY(!fixture.startup.library().busy());
        QVERIFY(
            window.grab().save(QDir(renderDirectory).filePath(QStringLiteral("link-error.png"))));
    }
    action->trigger();
    auto* retry = window.findChild<QDialog*>(QStringLiteral("importGameLinkDialog"));
    QVERIFY(retry && retry->isVisible());
    QCOMPARE(retry->findChild<QLineEdit*>(QStringLiteral("gameLinkEditor"))->text(), lichessLink);
    QTest::keyPress(retry, Qt::Key_Escape);
    QCOMPARE(network.requests.size(), 3);

    QVERIFY(fixture.imports().importUrl(*input::RemoteGame::fromLink(lichessLink)));
    auto* cancel = window.findChild<QPushButton*>(QStringLiteral("cancelImport"));
    QVERIFY(cancel && cancel->isVisible());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Cancelled);
    QVERIFY(!network.reply);
}

void RemoteImportTest::chessComArchive_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<qint64>("id");
    QTest::newRow("live") << QStringLiteral("live") << 169227053782LL;
    QTest::newRow("daily") << QStringLiteral("daily") << 768394935LL;
}

void RemoteImportTest::chessComArchive() {
    QFETCH(QString, type);
    QFETCH(qint64, id);
    FixtureNetwork network;
    Fixture fixture(&network);
    QTRY_VERIFY(fixture.imports().available());
    const auto link = QStringLiteral("https://www.chess.com/game/%1/%2").arg(type).arg(id);
    const auto game = input::RemoteGame::fromLink(link);
    QVERIFY(game && fixture.imports().importUrl(*game));
    QCOMPARE(network.requests.front().url(),
             QUrl(QStringLiteral("https://www.chess.com/callback/%1/game/%2").arg(type).arg(id)));
    network.reply->respond(200, jsonMediaType, chessMetadata(id));
    QTRY_COMPARE(network.requests.size(), 2);
    QCOMPARE(network.requests.back().url(),
             QUrl(QStringLiteral("https://api.chess.com/pub/player/sample_user/games/2025/02")));
    const auto otherType = type == u"daily" ? QStringLiteral("live") : QStringLiteral("daily");
    network.reply->respond(
        200, jsonMediaType,
        chessArchive({archiveRecord(
                          QStringLiteral("https://www.chess.com/game/%1/%2").arg(otherType).arg(id),
                          "1. d4 d5 1-0"),
                      archiveRecord(link, pgnBytes)}));
    QTRY_VERIFY(!fixture.imports().busy());
    QCOMPARE(fixture.imports().inserted(), 1U);
    const auto savedId =
        std::get<persistence::Inserted>(fixture.imports().files().front().items.front().outcome).id;
    QSignalSpy loaded(&fixture.startup.gameRepository(), &persistence::Database::gameLoaded);
    fixture.startup.gameRepository().loadGame(QUuid::createUuid(), savedId);
    QTRY_COMPARE(loaded.size(), 1);
    const auto result = loaded.last().at(1).value<persistence::LoadGameResult>();
    const auto* saved = std::get_if<persistence::StoredGame>(&result);
    QVERIFY(saved);
    QCOMPARE(saved->summary.white, QStringLiteral("Mira"));
    QCOMPARE(saved->source.type, QStringLiteral("chess.com"));
    QCOMPARE(saved->source.uri, link);
}

void RemoteImportTest::chessComRejections() {
    FixtureNetwork network;
    Fixture fixture(&network);
    QTRY_VERIFY(fixture.imports().available());
    const auto game = input::RemoteGame::fromLink(chessComLink);
    QVERIFY(game);
    constexpr qint64 id = 169227053782LL;
    for (const auto& reply : {chessMetadata(id + 1), chessMetadata(id, false),
                              chessMetadata(id, true, QStringLiteral("../../unexpected"))}) {
        QVERIFY(fixture.imports().importUrl(*game));
        const auto count = network.requests.size();
        network.reply->respond(200, jsonMediaType, reply);
        QTRY_VERIFY(!fixture.imports().busy());
        QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Failed);
        QCOMPARE(network.requests.size(), count);
        QCOMPARE(fixture.imports().inserted(), 0U);
    }
    for (const auto& reply :
         {chessArchive({archiveRecord(lichessLink, pgnBytes)}),
          chessArchive({archiveRecord(chessComLink, pgnBytes, QStringLiteral("chess960"))})}) {
        QVERIFY(fixture.imports().importUrl(*game));
        const auto count = network.requests.size();
        network.reply->respond(200, jsonMediaType, chessMetadata(id));
        QTRY_COMPARE(network.requests.size(), count + 1);
        network.reply->respond(200, jsonMediaType, reply);
        QTRY_VERIFY(!fixture.imports().busy());
        QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Failed);
        QCOMPARE(fixture.imports().inserted(), 0U);
    }
}

void RemoteImportTest::badDownloads_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("mediaType");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("message");
    QTest::newRow("offline") << 0 << QByteArray{} << QByteArray{} << QStringLiteral("connection");
    QTest::newRow("private") << 403 << QByteArray{} << QByteArray{} << QStringLiteral("private");
    QTest::newRow("missing") << 404 << QByteArray{} << QByteArray{} << QStringLiteral("missing");
    QTest::newRow("rate-limited") << 429 << QByteArray{} << QByteArray{}
                                  << QStringLiteral("limiting");
    QTest::newRow("server-error") << 503 << QByteArray{} << QByteArray{} << QStringLiteral("later");
    QTest::newRow("redirect") << 302 << QByteArray{} << QByteArray{}
                              << QStringLiteral("redirected");
    QTest::newRow("html") << 200 << QByteArray("text/html") << QByteArray("<html>Add cookie</html>")
                          << QStringLiteral("unexpected");
    QTest::newRow("oversize") << 200 << pgnMediaType << QByteArray(8 * 1024 * 1024 + 1, ' ')
                              << QStringLiteral("size limit");
    QTest::newRow("empty") << 200 << pgnMediaType << QByteArray{}
                           << QStringLiteral("one completed game");
    QTest::newRow("ongoing") << 200 << pgnMediaType << QByteArray("1. e4 *")
                             << QStringLiteral("one completed game");
    QTest::newRow("truncated") << 200 << pgnMediaType << QByteArray("[Result \"1-0\"]\n\n1. e4 e5")
                               << QStringLiteral("termination marker");
    QTest::newRow("multiple-games")
        << 200 << pgnMediaType << pgnBytes + pgnBytes << QStringLiteral("one completed game");
}

void RemoteImportTest::badDownloads() {
    QFETCH(int, status);
    QFETCH(QByteArray, mediaType);
    QFETCH(QByteArray, body);
    QFETCH(QString, message);
    FixtureNetwork network;
    Fixture fixture(&network);
    QTRY_VERIFY(fixture.imports().available());
    const auto game = input::RemoteGame::fromLink(lichessLink);
    QVERIFY(game && fixture.imports().importUrl(*game));
    network.reply->respond(status, mediaType, body);
    QTRY_VERIFY(!fixture.imports().busy());
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Failed);
    QCOMPARE(fixture.imports().inserted(), 0U);
    const auto& report = fixture.imports().files().front();
    QCOMPARE(report.state, app::ImportFileState::Failed);
    if (!report.message.isEmpty()) {
        QVERIFY2(report.message.contains(message), qPrintable(report.message));
    } else {
        QVERIFY(!report.items.empty());
        const auto* failure = std::get_if<pgn::PgnFailure>(&report.items.front().outcome);
        QVERIFY(failure);
        QVERIFY2(QString::fromStdString(failure->error.message).contains(message),
                 failure->error.message.c_str());
    }
}

void RemoteImportTest::cancellationAndShutdown() {
    FixtureNetwork network;
    Fixture fixture(&network);
    QTRY_VERIFY(fixture.imports().available());
    const auto chessCom = input::RemoteGame::fromLink(chessComLink);
    QVERIFY(chessCom && fixture.imports().importUrl(*chessCom));
    QVERIFY(!fixture.imports().importText(QStringLiteral("1. d4 *")));
    network.reply->respond(200, jsonMediaType, chessMetadata(169227053782LL));
    fixture.imports().cancel();
    QTRY_VERIFY(!fixture.imports().busy());
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Cancelled);
    QCOMPARE(fixture.imports().inserted(), 0U);

    const auto lichess = input::RemoteGame::fromLink(lichessLink);
    QVERIFY(lichess && fixture.imports().importUrl(*lichess));
    QVERIFY(network.reply && !network.reply->isFinished());
    fixture.startup.shutdown();
    QTRY_VERIFY(fixture.closed);
    QCOMPARE(network.requests.size(), 2);
    QCOMPARE(fixture.imports().status(), app::ImportBatchStatus::Cancelled);
    QCOMPARE(fixture.imports().inserted(), 0U);
}

void RemoteImportTest::liveProviders_data() {
    QTest::addColumn<QString>("link");
    QTest::newRow("lichess") << QStringLiteral("https://lichess.org/YO3ICtJi");
    QTest::newRow("chess-com-live") << chessComLink;
    QTest::newRow("chess-com-daily")
        << QStringLiteral("https://www.chess.com/game/daily/768394935");
}

void RemoteImportTest::liveProviders() {
    if (!qEnvironmentVariableIsSet("CHESS_REVIEW_LIVE_IMPORT")) {
        QSKIP("Set CHESS_REVIEW_LIVE_IMPORT to check real provider availability");
    }
    QFETCH(QString, link);
    Fixture fixture;
    QTRY_VERIFY(fixture.imports().available());
    const auto game = input::RemoteGame::fromLink(link);
    QVERIFY(game && fixture.imports().importUrl(*game));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.imports().busy(), 35'000);
    const auto& report = fixture.imports().files().front();
    if (!report.items.empty()) {
        if (const auto* failure = std::get_if<pgn::PgnFailure>(&report.items.front().outcome)) {
            QFAIL(failure->error.message.c_str());
        }
    }
    QVERIFY2(fixture.imports().inserted() == 1, qPrintable(report.message));
}

int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_REMOTE_IMPORT_DARK")) {
        auto palette = application.palette();
        palette.setColor(QPalette::Window, QColor("#191f2a"));
        application.setPalette(palette);
        ui::installTheme();
    }
    RemoteImportTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "RemoteImportTest.moc"
