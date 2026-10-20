#include "persistence/private/GameCodec.h"
#include "persistence/private/StorageSession.h"
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {
GameError storageError() {
    return {GameErrorCode::Storage, QStringLiteral("Game storage operation failed.")};
}
GameError invalid() {
    return {GameErrorCode::InvalidSnapshot, QStringLiteral("Stored game is invalid or too large.")};
}
QString tag(const pgn::PgnDocument& document, const char* name) {
    for (const auto& entry : document.tags) {
        if (entry.name == name) {
            return QString::fromUtf8(entry.value);
        }
    }
    return QStringLiteral("");
}
QString resultText(pgn::GameResult result) {
    switch (result) {
    case pgn::GameResult::Unknown:
        return QStringLiteral("*");
    case pgn::GameResult::WhiteWin:
        return QStringLiteral("1-0");
    case pgn::GameResult::BlackWin:
        return QStringLiteral("0-1");
    case pgn::GameResult::Draw:
        return QStringLiteral("1/2-1/2");
    }
    return {};
}
GameSummary summary(QSqlQuery& query) {
    return {query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(),   query.value(4).toString(), query.value(5).toString(),
            query.value(6).toString()};
}
// Failure to roll back makes this session unusable for further product operations.
class Transaction {
  public:
    Transaction(QSqlDatabase& database, bool& usable) : database_(database), usable_(usable) {}
    ~Transaction() {
        if (active_ && !database_.rollback()) {
            usable_ = false;
        }
    }
    bool begin() {
        QSqlQuery query(database_);
        active_ = query.exec(QStringLiteral("BEGIN IMMEDIATE"));
        return active_;
    }
    bool commit() {
        if (!database_.commit()) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    QSqlDatabase& database_;
    bool& usable_;
    bool active_ = false;
};
} // namespace

InsertGameResult StorageSession::insertGame(const pgn::PgnDocument& document,
                                            const GameSource& source) {
    if (!usable_) {
        return storageError();
    }
    if (source.type.size() > 128 || source.uri.size() > 64 * 1024 ||
        source.type.contains(QChar::Null) || source.uri.contains(QChar::Null) ||
        !source.type.isValidUtf16() || !source.uri.isValidUtf16()) {
        return invalid();
    }
    auto encoded = encodeGame(document);
    if (const auto* error = std::get_if<GameError>(&encoded)) {
        return *error;
    }
    const auto& game = std::get<EncodedGame>(encoded);
    Transaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storageError();
    }
    QSqlQuery query(database_);
    if (!query.prepare(QStringLiteral("SELECT id, substr(identity,1,1048577), identity_version "
                                      "FROM games WHERE content_hash=?"))) {
        return storageError();
    }
    query.addBindValue(game.hash);
    if (!query.exec()) {
        return storageError();
    }
    if (query.next()) {
        const auto id = query.value(0).toLongLong();
        if (query.value(1).toByteArray() != game.identity || query.value(2).toInt() != 1) {
            return invalid();
        }
        query.finish();
        const auto stored = loadGame(id);
        if (const auto* error = std::get_if<GameError>(&stored)) {
            return *error;
        }
        if (!transaction.commit()) {
            return storageError();
        }
        return Duplicate{id};
    }
    if (query.lastError().isValid()) {
        return storageError();
    }
    query.finish();
    if (!query.prepare(QStringLiteral(
            "INSERT INTO "
            "games(identity_version,content_hash,identity,original_pgn,snapshot,format_version,"
            "white_name,black_name,result,played_at,time_control,source_type,source_uri,imported_"
            "at) VALUES(1,?,?,?,?,1,?,?,?,?,?,?,?,?)"))) {
        return storageError();
    }
    query.addBindValue(game.hash);
    query.addBindValue(game.identity);
    query.addBindValue(game.original);
    query.addBindValue(game.snapshot);
    query.addBindValue(tag(document, "White"));
    query.addBindValue(tag(document, "Black"));
    query.addBindValue(resultText(document.result));
    query.addBindValue(tag(document, "Date"));
    query.addBindValue(tag(document, "TimeControl"));
    query.addBindValue(source.type.isNull() ? QStringLiteral("") : source.type);
    query.addBindValue(source.uri.isNull() ? QStringLiteral("") : source.uri);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec() || query.numRowsAffected() != 1) {
        return storageError();
    }
    const auto id = query.lastInsertId().toLongLong();
    query.finish();
    if (id <= 0 || !transaction.commit()) {
        return storageError();
    }
    return Inserted{id};
}

LoadGameResult StorageSession::loadGame(GameId id) {
    if (!usable_) {
        return storageError();
    }
    QSqlQuery query(database_);
    // substr bounds materialization even for externally corrupted rows. Length/type checks
    // distinguish truncation from legitimate content before decoding.
    if (!query.prepare(QStringLiteral(
            "SELECT id, substr(white_name,1,65537), substr(black_name,1,65537), "
            "substr(result,1,16), "
            "substr(played_at,1,65537), substr(time_control,1,65537), substr(imported_at,1,65), "
            "substr(source_type,1,129), substr(source_uri,1,65537), "
            "substr(original_pgn,1,8388609), substr(snapshot,1,67108865), "
            "substr(identity,1,1048577), substr(content_hash,1,33), identity_version, "
            "format_version, "
            "typeof(original_pgn), typeof(snapshot), typeof(identity), typeof(content_hash), "
            "(id>0 AND typeof(identity_version)='integer' AND typeof(format_version)='integer' "
            "AND typeof(white_name)='text' AND length(CAST(white_name AS BLOB))<=65536 "
            "AND typeof(black_name)='text' AND length(CAST(black_name AS BLOB))<=65536 "
            "AND typeof(result)='text' AND length(CAST(result AS BLOB))<=16 "
            "AND typeof(played_at)='text' AND length(CAST(played_at AS BLOB))<=65536 "
            "AND typeof(time_control)='text' AND length(CAST(time_control AS BLOB))<=65536 "
            "AND typeof(imported_at)='text' AND length(CAST(imported_at AS BLOB))<=64 "
            "AND typeof(source_type)='text' AND length(source_type)<=128 "
            "AND typeof(source_uri)='text' AND length(source_uri)<=65536) "
            "FROM games WHERE id=?"))) {
        return storageError();
    }
    query.addBindValue(static_cast<qlonglong>(id));
    if (!query.exec()) {
        return storageError();
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            return storageError();
        }
        return GameError{GameErrorCode::Missing, QStringLiteral("Game not found.")};
    }
    if (!query.value(19).toBool()) {
        return invalid();
    }
    for (int column : {1, 2, 4, 5, 8}) {
        if (query.value(column).toString().size() > 65536) {
            return invalid();
        }
    }
    if (query.value(7).toString().size() > 128 || query.value(6).toString().size() > 64 ||
        !QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs).isValid()) {
        return invalid();
    }
    if (query.value(13) != QVariant(1) || query.value(14) != QVariant(1)) {
        return GameError{GameErrorCode::UnsupportedVersion,
                         QStringLiteral("Unsupported stored game version.")};
    }
    for (int column : {15, 16, 17, 18}) {
        if (query.value(column).toString() != QStringLiteral("blob")) {
            return invalid();
        }
    }
    auto result = decodeGame(query.value(9).toByteArray(), query.value(10).toByteArray());
    auto* loaded = std::get_if<StoredGame>(&result);
    if (!loaded) {
        return result;
    }
    const auto key = gameIdentity(*loaded->document);
    if (key.key != query.value(11).toByteArray() || key.hash != query.value(12).toByteArray()) {
        return invalid();
    }
    loaded->summary = summary(query);
    loaded->source = {query.value(7).toString(), query.value(8).toString()};
    if (loaded->source.type.contains(QChar::Null) || loaded->source.uri.contains(QChar::Null) ||
        !loaded->source.type.isValidUtf16() || !loaded->source.uri.isValidUtf16()) {
        return invalid();
    }
    if (loaded->summary.white != tag(*loaded->document, "White") ||
        loaded->summary.black != tag(*loaded->document, "Black") ||
        loaded->summary.result != resultText(loaded->document->result) ||
        loaded->summary.playedAt != tag(*loaded->document, "Date") ||
        loaded->summary.timeControl != tag(*loaded->document, "TimeControl")) {
        return invalid();
    }
    return result;
}

ListGamesResult StorageSession::listGames(std::int64_t offset, int limit) {
    if (!usable_) {
        return storageError();
    }
    if (offset < 0 || limit < 1 || limit > 200) {
        return GameError{GameErrorCode::InvalidSnapshot,
                         QStringLiteral("Invalid game page bounds.")};
    }
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT "
            "id,substr(white_name,1,65537),substr(black_name,1,65537),substr(result,1,16),substr("
            "played_at,1,65537),substr(time_control,1,65537),substr(imported_at,1,65) FROM games "
            "ORDER BY imported_at DESC,id DESC LIMIT ? OFFSET ?"))) {
        return storageError();
    }
    query.addBindValue(limit);
    query.addBindValue(static_cast<qlonglong>(offset));
    if (!query.exec()) {
        return storageError();
    }
    std::vector<GameSummary> games;
    while (query.next()) {
        for (int column : {1, 2, 4, 5}) {
            if (query.value(column).toString().size() > 65536) {
                return invalid();
            }
        }
        games.push_back(summary(query));
    }
    if (query.lastError().isValid()) {
        return storageError();
    }
    return games;
}
} // namespace chess_review::persistence::detail
