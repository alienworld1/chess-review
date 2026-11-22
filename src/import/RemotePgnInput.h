#pragma once

#include "import/PgnInput.h"
#include <QByteArray>
#include <QObject>
#include <QStringView>
#include <QTimer>
#include <QUrl>
#include <optional>
#include <stop_token>
#include <variant>

class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;

namespace chess_review::input {
inline constexpr int maxGameLinkLength = 2048;

class RemoteGame {
  public:
    // Pure validation. Only the provider, game type and public ID survive parsing.
    static std::optional<RemoteGame> fromLink(QStringView link);
    QString providerName() const;
    QUrl publicUrl() const;

  private:
    enum class Provider { Lichess, ChessComLive, ChessComDaily };
    RemoteGame(Provider provider, QString id);
    bool isLichess() const { return provider_ == Provider::Lichess; }
    QString chessComType() const;

    Provider provider_;
    QString id_;
    friend class RemotePgnFetcher;
};

// One anonymous, bounded fetch at a time. Use from the controller's owning thread.
// The controller supplies the acquisition pool and keeps it alive through finished().
class RemotePgnFetcher final : public QObject {
    Q_OBJECT
  public:
    explicit RemotePgnFetcher(QThreadPool& pool, QNetworkAccessManager* network = nullptr);
    void fetch(RemoteGame game, std::stop_token stop);
    void abortRequest();
  signals:
    void finished(chess_review::input::InputResult result);

  private:
    using Step = std::variant<QUrl, InputResult>;
    static Step parseResponse(const RemoteGame& game, bool archive, QByteArray body);
    void request(const QUrl& url);
    void readResponse();
    void responseFinished();
    void advance(Step step);
    void finish(InputResult result);

    QThreadPool& pool_;
    QNetworkAccessManager* network_;
    QNetworkReply* reply_ = nullptr;
    QTimer deadline_;
    std::optional<RemoteGame> game_;
    std::stop_token stop_;
    QByteArray body_;
    qsizetype byteLimit_ = 0;
    bool archive_ = false;
    bool tooLarge_ = false;
    bool timedOut_ = false;
};
} // namespace chess_review::input
