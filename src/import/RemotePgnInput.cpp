#include "import/RemotePgnInput.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimeZone>
#include <chrono>
#include <exception>
#include <utility>

namespace chess_review::input {
namespace {
constexpr auto requestDeadline = std::chrono::seconds{30};
constexpr qsizetype maxPgnBytes = static_cast<qsizetype>(pgn::PgnLimits{}.maxGameBytes);
constexpr qsizetype maxChessComMetadataBytes = 256 * 1024;
constexpr qsizetype maxChessComArchiveBytes = 32 * 1024 * 1024;
constexpr qint64 replyBufferBytes = 64 * 1024;

InputResult failed(QString message) {
    InputResult result;
    result.error = std::move(message);
    return result;
}

bool isJson(const QByteArray& mediaType) {
    return mediaType == "application/json" || mediaType == "application/ld+json";
}
} // namespace

RemoteGame::RemoteGame(Provider provider, QString id) : provider_(provider), id_(std::move(id)) {}

std::optional<RemoteGame> RemoteGame::fromLink(QStringView link) {
    if (link.size() > maxGameLinkLength) {
        return std::nullopt;
    }
    link = link.trimmed();
    if (link.isEmpty() || link.contains(u'\\')) {
        return std::nullopt;
    }
    const bool hasWebScheme = link.startsWith(u"https://", Qt::CaseInsensitive) ||
                              link.startsWith(u"http://", Qt::CaseInsensitive);
    const auto url =
        QUrl(hasWebScheme ? link.toString() : QStringLiteral("https://") + link, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != u"https" && url.scheme() != u"http") ||
        url.authority(QUrl::FullyEncoded).contains(u'@') || url.port() != -1) {
        return std::nullopt;
    }
    const auto host = url.host();
    const auto path = url.path(QUrl::FullyEncoded);
    if (host == u"lichess.org" || host == u"www.lichess.org") {
        // Some Lichess section names have the same length as a public game ID.
        static const QRegularExpression gamePath(
            QStringLiteral("\\A(?:/game(?:/export)?/|/"
                           "(?!(?:analysis|features|moderate|password|practice|streamer|timeline|"
                           "training)(?:/|\\z)))"
                           "([A-Za-z0-9]{8})(?:/(?:white|black))?/?\\z"));
        const auto game = gamePath.match(path);
        if (game.hasMatch()) {
            return RemoteGame(Provider::Lichess, game.captured(1));
        }
    } else if (host == u"chess.com" || host == u"www.chess.com") {
        static const QRegularExpression gamePath(
            QStringLiteral("\\A/(?:[a-z]{2}(?:-[a-z]{2})?/)?(?:analysis/)?game/(live|daily)/"
                           "([1-9][0-9]{0,17})(?:/analysis)?/?\\z"));
        static const QRegularExpression legacyPath(
            QStringLiteral("\\A/(live|daily)/game/([1-9][0-9]{0,17})/?\\z"));
        auto game = gamePath.match(path);
        if (!game.hasMatch()) {
            game = legacyPath.match(path);
        }
        if (game.hasMatch()) {
            return RemoteGame(game.capturedView(1) == u"live" ? Provider::ChessComLive
                                                              : Provider::ChessComDaily,
                              game.captured(2));
        }
    }
    return std::nullopt;
}

QString RemoteGame::providerName() const {
    return isLichess() ? QStringLiteral("Lichess") : QStringLiteral("Chess.com");
}

QString RemoteGame::chessComType() const {
    return provider_ == Provider::ChessComDaily ? QStringLiteral("daily") : QStringLiteral("live");
}

QUrl RemoteGame::publicUrl() const {
    return isLichess()
               ? QUrl(QStringLiteral("https://lichess.org/%1").arg(id_))
               : QUrl(QStringLiteral("https://www.chess.com/game/%1/%2").arg(chessComType(), id_));
}

RemotePgnFetcher::RemotePgnFetcher(QThreadPool& pool, QNetworkAccessManager* network)
    : pool_(pool), network_(network) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        timedOut_ = true;
        abortRequest();
    });
}

void RemotePgnFetcher::fetch(RemoteGame game, std::stop_token stop) {
    Q_ASSERT(!game_ && !reply_);
    if (!network_) {
        network_ = new QNetworkAccessManager(this);
    }
    stop_ = stop;
    timedOut_ = false;
    archive_ = false;
    game_ = std::move(game);
    deadline_.start(requestDeadline);
    request(game_->isLichess()
                ? QUrl(QStringLiteral("https://lichess.org/game/export/%1?clocks=true&evals=false")
                           .arg(game_->id_))
                : QUrl(QStringLiteral("https://www.chess.com/callback/%1/game/%2")
                           .arg(game_->chessComType(), game_->id_)));
}

void RemotePgnFetcher::request(const QUrl& url) {
    tooLarge_ = false;
    byteLimit_ = game_->isLichess() ? maxPgnBytes
                 : archive_         ? maxChessComArchiveBytes
                                    : maxChessComMetadataBytes;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept",
                         game_->isLichess() ? "application/x-chess-pgn" : "application/json");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "chess-review/" CHESS_REVIEW_VERSION);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(replyBufferBytes);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        readResponse();
        if (tooLarge_) {
            abortRequest();
        }
    });
    connect(reply_, &QNetworkReply::finished, this, &RemotePgnFetcher::responseFinished);
}

void RemotePgnFetcher::readResponse() {
    if (reply_->bytesAvailable() > byteLimit_ - body_.size() ||
        reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong() > byteLimit_) {
        tooLarge_ = true;
        return;
    }
    body_ += reply_->readAll();
}

void RemotePgnFetcher::abortRequest() {
    if (reply_ && !reply_->isFinished()) {
        reply_->abort();
    }
}

void RemotePgnFetcher::responseFinished() {
    auto* reply = reply_;
    Q_ASSERT(reply);
    if (reply->error() == QNetworkReply::NoError) {
        readResponse();
    }
    disconnect(reply, nullptr, this, nullptr);
    reply_->deleteLater();
    reply_ = nullptr;
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto mediaType =
        reply->rawHeader("Content-Type").split(';').constFirst().trimmed().toLower();
    const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    const auto provider = game_->providerName();
    if (stop_.stop_requested() || timedOut_) {
        advance(InputResult{});
    } else if (tooLarge_) {
        finish(failed(tr("The %1 download exceeds the import size limit.").arg(provider)));
    } else if (status == 401 || status == 403 || status == 404 || status == 410 || status == 451) {
        finish(failed(tr("%1 did not make this game available. It may be private, missing, or "
                         "blocked by the provider.")
                          .arg(provider)));
    } else if (status == 429) {
        finish(failed(
            tr("%1 is limiting downloads. Wait before trying the link again.").arg(provider)));
    } else if (status >= 500 && status < 600) {
        finish(
            failed(tr("%1 is having trouble serving this game. Try again later.").arg(provider)));
    } else if (status >= 300 && status < 400) {
        finish(failed(
            tr("%1 redirected the download. Copy the current public game link or use Paste PGN.")
                .arg(provider)));
    } else if (reply->error() != QNetworkReply::NoError) {
        finish(failed(reply->error() == QNetworkReply::SslHandshakeFailedError
                          ? tr("Could not establish a secure connection to %1.").arg(provider)
                          : tr("Could not reach %1. Check your internet connection and try again.")
                                .arg(provider)));
    } else if (status != 200 || (!encoding.isEmpty() && encoding != "identity") ||
               (game_->isLichess() ? mediaType != "application/x-chess-pgn" : !isJson(mediaType))) {
        finish(failed(
            tr("%1 returned an unexpected download. Try exporting the game's PGN.").arg(provider)));
    } else {
        pool_.start([this, game = *game_, archive = archive_, body = std::exchange(body_, {}),
                     stop = stop_]() mutable {
            Step step;
            try {
                if (!stop.stop_requested()) {
                    step = parseResponse(game, archive, std::move(body));
                }
            } catch (const std::exception&) {
                step = failed(tr("The game download could not be processed."));
            }
            QMetaObject::invokeMethod(
                this, [this, step = std::move(step)]() mutable { advance(std::move(step)); },
                Qt::QueuedConnection);
        });
    }
}

RemotePgnFetcher::Step RemotePgnFetcher::parseResponse(const RemoteGame& game, bool archive,
                                                       QByteArray body) {
    const auto asPgn = [&game](const QByteArray& bytes) {
        InputResult result;
        result.bytes.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        result.bytesRead = result.bytes.size();
        result.source = {game.isLichess() ? QStringLiteral("lichess") : QStringLiteral("chess.com"),
                         game.publicUrl().toString(QUrl::FullyEncoded)};
        return result;
    };
    if (game.isLichess()) {
        return asPgn(body);
    }

    const auto document = QJsonDocument::fromJson(body);
    const auto invalid = [] { return failed(tr("Chess.com returned unrecognized game data.")); };
    if (!document.isObject()) {
        return invalid();
    }
    if (!archive) {
        const auto details = document.object().value("game").toObject();
        const auto finished = details.value("isFinished");
        if (QString::number(details.value("id").toInteger(-1)) != game.id_ || !finished.isBool()) {
            return invalid();
        }
        if (!finished.toBool()) {
            return failed(tr("This game is still in progress. Import its link after it ends."));
        }
        const auto player = details.value("pgnHeaders").toObject().value("White").toString();
        const auto end = details.value("endTime").toInteger(-1);
        static const QRegularExpression username(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
        // Bound the UTC archive date to a four-digit year before constructing its path.
        if (end < 0 || end > 253402300799 || !username.match(player).hasMatch()) {
            return invalid();
        }
        const auto month =
            QDateTime::fromSecsSinceEpoch(end, QTimeZone(QTimeZone::UTC)).toString("yyyy/MM");
        return QUrl(QStringLiteral("https://api.chess.com/pub/player/%1/games/%2")
                        .arg(player.toLower(), month));
    }

    const auto games = document.object().value("games");
    if (!games.isArray()) {
        return invalid();
    }
    const auto entries = games.toArray();
    for (const auto& entry : entries) {
        const auto record = entry.toObject();
        const auto candidate = RemoteGame::fromLink(record.value("url").toString());
        if (!candidate || candidate->provider_ != game.provider_ || candidate->id_ != game.id_) {
            continue;
        }
        const auto rules = record.value("rules").toString();
        if (rules.isEmpty()) {
            return invalid();
        }
        if (rules != u"chess") {
            return failed(tr("This Chess.com game uses a chess variant the app cannot import."));
        }
        const auto pgn = record.value("pgn").toString();
        if (pgn.isEmpty() || !pgn.isValidUtf16()) {
            return invalid();
        }
        if (pgn.size() > maxPgnBytes) {
            return failed(tr("The Chess.com PGN exceeds the import size limit."));
        }
        const auto bytes = pgn.toUtf8();
        if (bytes.size() > maxPgnBytes) {
            return failed(tr("The Chess.com PGN exceeds the import size limit."));
        }
        return asPgn(bytes);
    }
    return failed(tr("The game is missing from Chess.com's public archive. Recent games can take "
                     "time to appear. Try again later or use Paste PGN."));
}

void RemotePgnFetcher::advance(Step step) {
    if (stop_.stop_requested()) {
        InputResult result;
        result.cancelled = true;
        finish(std::move(result));
    } else if (timedOut_) {
        finish(failed(tr("The %1 download timed out. Check your connection and try again.")
                          .arg(game_->providerName())));
    } else if (const auto* archiveUrl = std::get_if<QUrl>(&step)) {
        archive_ = true;
        request(*archiveUrl);
    } else {
        finish(std::get<InputResult>(std::move(step)));
    }
}

void RemotePgnFetcher::finish(InputResult result) {
    deadline_.stop();
    game_.reset();
    body_.clear();
    emit finished(std::move(result));
}
} // namespace chess_review::input
