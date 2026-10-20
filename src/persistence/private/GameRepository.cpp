#include "persistence/private/GameCodec.h"
#include "persistence/private/GameRecords.h"
#include "persistence/private/StorageSession.h"
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {
// SQL byte projections below are deliberate caps, not silently tunable defaults.
static_assert(pgn::PgnLimits{}.maxGameBytes == 8 * 1024 * 1024);
static_assert(pgn::PgnLimits{}.maxTagValueBytes == 65536);
static_assert(maxSnapshotBytes == 64 * 1024 * 1024 && maxIdentityBytes == 1024 * 1024);
GameError storageError() {
    return {GameErrorCode::Storage, QStringLiteral("Game storage operation failed.")};
}
GameError invalid() {
    return {GameErrorCode::InvalidSnapshot, QStringLiteral("Stored game is invalid or too large.")};
}
// The SQL projection is a byte-bounded BLOB. Decode only after validating the exact
// bytes, so SQLite's text NUL handling and Qt's replacement decoding cannot hide damage.
std::optional<QString> readText(QSqlQuery& query, int column, qsizetype maxBytes) {
    const auto bytes = query.value(column).toByteArray();
    if (bytes.size() > maxBytes || bytes.contains('\0') || !bytes.isValidUtf8()) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    return decoder(bytes);
}
std::optional<GameSummary> readSummary(QSqlQuery& query) {
    const auto white = readText(query, 1, 65536);
    const auto black = readText(query, 2, 65536);
    const auto result = readText(query, 3, 16);
    const auto playedAt = readText(query, 4, 65536);
    const auto timeControl = readText(query, 5, 65536);
    const auto importedAt = readText(query, 6, 64);
    if (!white || !black || !result || !playedAt || !timeControl || !importedAt ||
        (*result != QStringLiteral("*") && *result != QStringLiteral("1-0") &&
         *result != QStringLiteral("0-1") && *result != QStringLiteral("1/2-1/2")) ||
        !importedAt->endsWith(u'Z') ||
        !QDateTime::fromString(*importedAt, Qt::ISODateWithMs).isValid()) {
        return std::nullopt;
    }
    return GameSummary{
        query.value(0).toLongLong(), *white, *black, *result, *playedAt, *timeControl, *importedAt};
}
void clipSummary(GameSummary& summary) {
    for (auto* value : {&summary.white, &summary.black, &summary.playedAt, &summary.timeControl}) {
        if (value->size() > 256) {
            qsizetype length = 256;
            if (value->at(length - 1).isHighSurrogate()) {
                --length;
            }
            // A fresh small allocation releases the large tag's retained capacity too.
            *value = QString(value->constData(), length);
            summary.metadataTruncated = true;
        }
    }
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

QByteArray utf8TextBytes(const QString& value) {
    return value.isEmpty() ? QByteArray("") : value.toUtf8();
}

bool StorageSession::refreshGameValidationCache() {
    // Called within BEGIN IMMEDIATE: other writers cannot change the snapshot between
    // reading these counters and using a cached validation. Never compare across connections.
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("PRAGMA main.data_version")) || !query.next()) {
        return false;
    }
    bool validVersion = false;
    const auto dataVersion = query.value(0).toLongLong(&validVersion);
    query.finish();
    if (!query.exec(QStringLiteral("SELECT total_changes()")) || !query.next()) {
        return false;
    }
    bool validChanges = false;
    const auto changes = query.value(0).toLongLong(&validChanges);
    if (!validVersion || !validChanges || dataVersion < 0 || changes < 0) {
        return false;
    }
    const std::pair<std::int64_t, std::int64_t> stamp{dataVersion, changes};
    if (validationStamp_ != stamp) {
        validatedGames_.clear();
        validationStamp_ = stamp;
    }
    return true;
}

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
    const auto metadata = gameMetadata(document);
    Transaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storageError();
    }
    QSqlQuery query(database_);
    if (!query.prepare(QStringLiteral("SELECT id, substr(CAST(identity AS BLOB),1,1048577), "
                                      "identity_version FROM games WHERE content_hash=?"))) {
        return storageError();
    }
    query.addBindValue(game.hash);
    if (!query.exec()) {
        return storageError();
    }
    if (query.next()) {
        const auto id = query.value(0).toLongLong();
        if (query.value(1).toByteArray() != game.identity || query.value(2).toLongLong() != 2) {
            return invalid();
        }
        query.finish();
        if (!refreshGameValidationCache()) {
            return storageError();
        }
        const bool cached = validatedGames_.contains(id);
        if (!cached) {
            const auto stored = loadGame(id);
            if (const auto* error = std::get_if<GameError>(&stored)) {
                return *error;
            }
        }
        if (!transaction.commit()) {
            return storageError();
        }
        if (!cached) {
            if (validatedGames_.size() >= 256) {
                validatedGames_.clear();
            }
            validatedGames_.insert(id);
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
            "at) VALUES(2,?,?,?,?,2,CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS "
            "TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT))"))) {
        return storageError();
    }
    query.addBindValue(game.hash);
    query.addBindValue(game.identity);
    query.addBindValue(game.original);
    query.addBindValue(game.snapshot);
    query.addBindValue(utf8TextBytes(metadata.white));
    query.addBindValue(utf8TextBytes(metadata.black));
    query.addBindValue(utf8TextBytes(metadata.result));
    query.addBindValue(utf8TextBytes(metadata.playedAt));
    query.addBindValue(utf8TextBytes(metadata.timeControl));
    query.addBindValue(utf8TextBytes(source.type));
    query.addBindValue(utf8TextBytes(source.uri));
    query.addBindValue(utf8TextBytes(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
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
    return readGame(database_, id);
}
LoadGameResult readGame(QSqlDatabase& database, GameId id) {
    QSqlQuery query(database);
    // substr bounds materialization even for externally corrupted rows. Length/type checks
    // distinguish truncation from legitimate content before decoding.
    if (!query.prepare(QStringLiteral(
            "SELECT id, substr(CAST(white_name AS BLOB),1,65537), substr(CAST(black_name AS "
            "BLOB),1,65537), substr(CAST(result AS BLOB),1,17), substr(CAST(played_at AS "
            "BLOB),1,65537), substr(CAST(time_control AS BLOB),1,65537), substr(CAST(imported_at "
            "AS BLOB),1,65), substr(CAST(source_type AS BLOB),1,513), substr(CAST(source_uri AS "
            "BLOB),1,262145), substr(CAST(original_pgn AS BLOB),1,8388609), substr(CAST(snapshot "
            "AS BLOB),1,67108865), substr(CAST(identity AS BLOB),1,1048577), "
            "substr(CAST(content_hash AS BLOB),1,33), identity_version, format_version, "
            "typeof(original_pgn), typeof(snapshot), typeof(identity), typeof(content_hash), (id>0 "
            "AND typeof(identity_version)='integer' AND typeof(format_version)='integer' AND "
            "typeof(white_name)='text' AND typeof(black_name)='text' AND typeof(result)='text' AND "
            "typeof(played_at)='text' AND typeof(time_control)='text' AND "
            "typeof(imported_at)='text' AND typeof(source_type)='text' AND "
            "typeof(source_uri)='text') FROM games WHERE id=?"))) {
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
    const auto metadata = readSummary(query);
    const auto sourceType = readText(query, 7, 512);
    const auto sourceUri = readText(query, 8, 262144);
    if (!metadata || !sourceType || !sourceUri || sourceType->size() > 128 ||
        sourceUri->size() > 65536) {
        return invalid();
    }
    const auto identityVersion = query.value(13).toLongLong();
    const auto formatVersion = query.value(14).toLongLong();
    if ((formatVersion != 1 && formatVersion != 2) || identityVersion != formatVersion) {
        return GameError{GameErrorCode::UnsupportedVersion,
                         QStringLiteral("Unsupported stored game version.")};
    }
    for (int column : {15, 16, 17, 18}) {
        if (query.value(column).toString() != QStringLiteral("blob")) {
            return invalid();
        }
    }
    auto result = decodeGame(query.value(9).toByteArray(), query.value(10).toByteArray(),
                             static_cast<int>(formatVersion));
    auto* loaded = std::get_if<StoredGame>(&result);
    if (!loaded) {
        return result;
    }
    const auto key = gameIdentity(*loaded->document, static_cast<int>(formatVersion));
    if (key.key != query.value(11).toByteArray() || key.hash != query.value(12).toByteArray()) {
        return invalid();
    }
    loaded->summary = *metadata;
    loaded->source = {*sourceType, *sourceUri};
    const auto expected = gameMetadata(*loaded->document, static_cast<int>(formatVersion));
    if (loaded->summary.white != expected.white || loaded->summary.black != expected.black ||
        loaded->summary.result != expected.result ||
        loaded->summary.playedAt != expected.playedAt ||
        loaded->summary.timeControl != expected.timeControl) {
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
            "SELECT id,substr(CAST(white_name AS BLOB),1,65537),substr(CAST(black_name AS "
            "BLOB),1,65537),substr(CAST(result AS BLOB),1,17),substr(CAST(played_at AS "
            "BLOB),1,65537),substr(CAST(time_control AS BLOB),1,65537),substr(CAST(imported_at AS "
            "BLOB),1,65) ,(id>0 AND typeof(white_name)='text' AND typeof(black_name)='text' AND "
            "typeof(result)='text' AND typeof(played_at)='text' AND typeof(time_control)='text' "
            "AND typeof(imported_at)='text') FROM games ORDER BY imported_at DESC,id DESC LIMIT ? "
            "OFFSET ?"))) {
        return storageError();
    }
    query.addBindValue(limit);
    query.addBindValue(static_cast<qlonglong>(offset));
    if (!query.exec()) {
        return storageError();
    }
    std::vector<GameSummary> games;
    while (query.next()) {
        auto metadata = readSummary(query);
        if (!query.value(7).toBool() || !metadata) {
            GameSummary damaged;
            damaged.id = query.value(0).toLongLong();
            damaged.metadataError = invalid();
            games.push_back(std::move(damaged));
            continue;
        }
        clipSummary(*metadata);
        games.push_back(std::move(*metadata));
    }
    if (query.lastError().isValid()) {
        return storageError();
    }
    return games;
}
} // namespace chess_review::persistence::detail
