#pragma once

#include "persistence/GameRepository.h"
#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace chess_review::diagnostics {
class Logger;
}

namespace chess_review::persistence {

// Values are stable diagnostic identifiers; do not renumber.
enum class DatabaseErrorCode {
    Path = 0,
    Locked = 1,
    Open = 2,
    Configuration = 3,
    Schema = 4,
    Backup = 5,
    Migration = 6
};

struct DatabaseError {
    DatabaseErrorCode code{};
    QString operation;
    QString detail;
    QString nativeCode;
    QString databasePath;
    QString backupPath;
    std::optional<int> migrationVersion;
};

// Call on the owning (application) thread. One initialization per instance.
// The owner must call close() and await closed() before destroying this service.
// SQL connections and all filesystem/database work belong to the storage thread.
class Database final : public QObject {
    Q_OBJECT

  public:
    explicit Database(QObject* parent = nullptr);
    // The injected logger must outlive closed(); a default instance has no logging.
    explicit Database(diagnostics::Logger& logger, QObject* parent = nullptr);
    ~Database() override;

    void initialize(const QString& databasePath);
    void close();
    // Repository calls are asynchronous, on this object's owning thread. IDs are caller-owned.
    // At most 16 accepted repository requests may be pending; otherwise returns Busy.
    void insertGame(RequestId request, std::shared_ptr<const pgn::PgnDocument> document,
                    GameSource source = {});
    void loadGame(RequestId request, GameId id);
    void listGames(RequestId request, std::int64_t offset = 0, int limit = 50);

  signals:
    void progress(const QString& message);
    void ready(int schemaVersion);
    void failed(const chess_review::persistence::DatabaseError& error);
    void closed();
    void gameInserted(chess_review::persistence::RequestId request,
                      const chess_review::persistence::InsertGameResult& result);
    void gameLoaded(chess_review::persistence::RequestId request,
                    const chess_review::persistence::LoadGameResult& result);
    void gamesListed(chess_review::persistence::RequestId request,
                     const chess_review::persistence::ListGamesResult& result);

  private:
    void finishClose();
    std::optional<GameError> acceptRequest();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chess_review::persistence

Q_DECLARE_METATYPE(chess_review::persistence::DatabaseError)
