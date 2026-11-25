#include "diagnostics/Logger.h"
#include "openings/PackagedOpenings.h"
#include "persistence/private/GameCodec.h"
#include "persistence/private/GameRecords.h"
#include "persistence/private/StorageSession.h"
#include "review/Review.h"
#include <QDateTime>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <exception>

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
void clipText(QString& value, qsizetype length, bool& truncated) {
    if (value.size() > length) {
        if (value.at(length - 1).isHighSurrogate()) {
            --length;
        }
        // A fresh small allocation releases the large tag's retained capacity too.
        value = QString(value.constData(), length);
        truncated = true;
    }
}
void clipSummary(GameSummary& summary, qsizetype length) {
    for (auto* value : {&summary.white, &summary.black, &summary.playedAt, &summary.timeControl}) {
        clipText(*value, length, summary.metadataTruncated);
    }
}

QString playerPattern(const QString& term) {
    // SQLite GLOB matches UTF-8 code points but has no Unicode case-insensitive mode.
    QString pattern = QStringLiteral("*");
    for (const auto value : term.toUcs4()) {
        const auto codePoint = static_cast<char32_t>(value);
        const auto lower = QChar::toLower(codePoint);
        const auto upper = QChar::toUpper(codePoint);
        if (lower != upper) {
            pattern += u'[';
            pattern += QString::fromUcs4(&lower, 1);
            pattern += QString::fromUcs4(&upper, 1);
            pattern += QString::fromUcs4(&codePoint, 1);
            pattern += u']';
        } else {
            const bool special = codePoint == U'*' || codePoint == U'?' || codePoint == U'[';
            if (special) {
                pattern += u'[';
            }
            pattern += QString::fromUcs4(&codePoint, 1);
            if (special) {
                pattern += u']';
            }
        }
    }
    return pattern + u'*';
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

std::optional<OpeningResolution>
StorageSession::recognizeOpening(const pgn::PgnDocument& document,
                                 const openings::BookConfig* book) {
    try {
        if (!openingIndexLoaded_) {
            openingIndexLoaded_ = true;
            openingIndex_ = openings::loadPackagedOpenings();
            if (!openingIndex_) {
                if (logger_) {
                    logger_->record(diagnostics::Event::OpeningDatasetUnavailable, {},
                                    diagnostics::Severity::Warning);
                } else {
                    qWarning("The bundled opening dataset is unavailable or invalid.");
                }
            }
        }
        if (!openingIndex_) {
            return std::nullopt;
        }
        auto root = document.tree.position(document.tree.rootId());
        if (auto* position = std::get_if<chess::Position>(&root)) {
            const auto moves = document.originalMainline();
            const auto plies =
                book && book->enabled
                    ? std::optional(openingIndex_->bookPrefix(*position, moves, *book))
                    : std::nullopt;
            return OpeningResolution{QString::fromLatin1(openings::datasetVersion),
                                     openingIndex_->recognize(std::move(*position), moves), plies};
        }
    } catch (const std::exception&) {
        qWarning("Opening recognition could not complete.");
    }
    return std::nullopt;
}

void StorageSession::readOpeningCache(std::span<GameSummary> games) {
    if (!openingCacheUsable_ || games.empty()) {
        return;
    }
    QStringList placeholders;
    for (const auto& game : games) {
        if (!game.metadataError) {
            placeholders.push_back(QStringLiteral("?"));
        }
    }
    if (placeholders.isEmpty()) {
        return;
    }
    const auto version = QString::fromLatin1(openings::datasetVersion);
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    const auto read = [&] {
        static_assert(openings::maxOpeningNameBytes == 256);
        if (!query.prepare(
                QStringLiteral("SELECT game_id, substr(CAST(eco AS BLOB),1,4), "
                               "substr(CAST(name AS BLOB),1,257), typeof(eco), typeof(name) "
                               "FROM game_openings WHERE dataset_version=? AND policy_version=? "
                               "AND game_id IN (%1)")
                    .arg(placeholders.join(u',')))) {
            return false;
        }
        query.addBindValue(version);
        query.addBindValue(openings::recognitionPolicyVersion);
        for (const auto& game : games) {
            if (!game.metadataError) {
                query.addBindValue(static_cast<qlonglong>(game.id));
            }
        }
        if (!query.exec()) {
            return false;
        }
        while (query.next()) {
            const auto id = query.value(0).toLongLong();
            const auto game = std::ranges::find(games, id, &GameSummary::id);
            if (game == games.end()) {
                continue;
            }
            const auto ecoType = query.value(3).toString();
            const auto nameType = query.value(4).toString();
            if (ecoType == QStringLiteral("null") && nameType == QStringLiteral("null")) {
                game->opening = OpeningResolution{version, std::nullopt};
            } else if (ecoType == QStringLiteral("text") && nameType == QStringLiteral("text")) {
                const auto eco = query.value(1).toByteArray().toStdString();
                const auto bytes = query.value(2).toByteArray();
                const auto name = bytes.toStdString();
                if (bytes.isValidUtf8() && openings::validOpening(eco, name)) {
                    game->opening = OpeningResolution{version, openings::Opening{eco, name}};
                }
            }
        }
        return !query.lastError().isValid();
    };
    if (!read()) {
        for (auto& game : games) {
            game.opening.reset();
        }
        openingCacheUsable_ = false;
        qWarning("The game opening cache could not be read.");
    }
}

bool StorageSession::writeOpeningCache(GameId id, const OpeningResolution& opening) {
    if (!openingCacheUsable_) {
        return false;
    }
    QSqlQuery query(database_);
    const auto write = [&] {
        if (!query.prepare(QStringLiteral(
                "INSERT INTO game_openings(game_id,dataset_version,policy_version,eco,name) "
                "VALUES(?,?,?,CAST(? AS TEXT),CAST(? AS TEXT)) "
                "ON CONFLICT(game_id) DO UPDATE SET dataset_version=excluded.dataset_version,"
                "policy_version=excluded.policy_version,eco=excluded.eco,name=excluded.name"))) {
            return false;
        }
        query.addBindValue(static_cast<qlonglong>(id));
        query.addBindValue(opening.datasetVersion);
        query.addBindValue(openings::recognitionPolicyVersion);
        query.addBindValue(opening.match ? QByteArray::fromStdString(opening.match->eco)
                                         : QByteArray{});
        query.addBindValue(opening.match ? QByteArray::fromStdString(opening.match->name)
                                         : QByteArray{});
        return query.exec();
    };
    if (!write()) {
        openingCacheUsable_ = false;
        qWarning("The game opening cache could not be written.");
        return false;
    }
    return true;
}

void StorageSession::saveOpeningCache(GameId id, const OpeningResolution& opening) {
    if (!openingCacheUsable_ || schemaVersion_ < 12) {
        return;
    }
    QSqlQuery savepoint(database_);
    if (!savepoint.exec(QStringLiteral("SAVEPOINT opening_cache"))) {
        openingCacheUsable_ = false;
        return;
    }
    if (!writeOpeningCache(id, opening)) {
        savepoint.exec(QStringLiteral("ROLLBACK TO opening_cache"));
    }
    savepoint.exec(QStringLiteral("RELEASE opening_cache"));
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

InsertGameResult
StorageSession::insertGameInTransaction(const EncodedGame& game, const GameSummary& metadata,
                                        const std::optional<OpeningResolution>& opening,
                                        const GameSource& source,
                                        std::vector<GameId>& newlyValidated) {
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
        const bool cached = validatedGames_.contains(id) ||
                            std::ranges::find(newlyValidated, id) != newlyValidated.end();
        if (!cached) {
            const auto stored = loadGame(id);
            if (const auto* error = std::get_if<GameError>(&stored)) {
                return *error;
            }
            newlyValidated.push_back(id);
        }
        return Duplicate{id};
    }
    if (query.lastError().isValid()) {
        return storageError();
    }
    query.finish();
    const auto idSource =
        schemaVersion_ >= 14 ? QStringLiteral("last_id+1") : QStringLiteral("NULL");
    const auto sequence =
        schemaVersion_ >= 14 ? QStringLiteral(" FROM game_id_sequence WHERE id=1") : QString{};
    if (!query.prepare(
            QStringLiteral(
                "INSERT INTO "
                "games(id,identity_version,content_hash,identity,original_pgn,snapshot,format_"
                "version,"
                "white_name,black_name,result,played_at,time_control,source_type,source_uri,"
                "imported_"
                "at) SELECT %1,2,?,?,?,?,2,CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),"
                "CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT)%2")
                .arg(idSource, sequence))) {
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
    if (id <= 0) {
        return storageError();
    }
    if (opening) {
        saveOpeningCache(id, *opening);
    }
    return Inserted{id};
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
    const auto opening = recognizeOpening(document);
    Transaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storageError();
    }
    std::vector<GameId> newlyValidated;
    const auto outcome = insertGameInTransaction(game, metadata, opening, source, newlyValidated);
    if (std::holds_alternative<GameError>(outcome)) {
        return outcome;
    }
    if (!transaction.commit()) {
        return storageError();
    }
    for (const auto id : newlyValidated) {
        if (validatedGames_.size() >= 256) {
            validatedGames_.clear();
        }
        validatedGames_.insert(id);
    }
    return outcome;
}

std::vector<InsertGameResult>
StorageSession::insertGameBatch(std::span<const std::shared_ptr<const pgn::PgnDocument>> documents,
                                const GameSource& source) {
    if (documents.empty()) {
        return {};
    }
    if (!usable_) {
        return {storageError()};
    }
    if (source.type.size() > 128 || source.uri.size() > 64 * 1024 ||
        source.type.contains(QChar::Null) || source.uri.contains(QChar::Null) ||
        !source.type.isValidUtf16() || !source.uri.isValidUtf16()) {
        return {invalid()};
    }
    struct PreparedGame {
        std::optional<EncodedGame> encoded;
        GameSummary metadata;
        std::optional<OpeningResolution> opening;
        std::optional<GameError> error;
    };
    std::vector<PreparedGame> prepared;
    prepared.reserve(documents.size());
    for (const auto& doc : documents) {
        PreparedGame item;
        if (!doc) {
            item.error = GameError{GameErrorCode::InvalidSnapshot,
                                   QStringLiteral("Missing imported snapshot.")};
            prepared.push_back(std::move(item));
            break;
        }
        auto encoded = encodeGame(*doc);
        if (const auto* error = std::get_if<GameError>(&encoded)) {
            item.error = *error;
            prepared.push_back(std::move(item));
            break;
        }
        item.encoded = std::move(std::get<EncodedGame>(encoded));
        item.metadata = gameMetadata(*doc);
        item.opening = recognizeOpening(*doc);
        prepared.push_back(std::move(item));
    }

    Transaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return {storageError()};
    }
    std::vector<InsertGameResult> results;
    results.reserve(prepared.size());
    std::vector<GameId> newlyValidated;
    const auto recordValidated = [&] {
        for (const auto id : newlyValidated) {
            if (validatedGames_.size() >= 256) {
                validatedGames_.clear();
            }
            validatedGames_.insert(id);
        }
    };
    QSqlQuery savepoint(database_);
    for (const auto& item : prepared) {
        if (item.error) {
            if (!results.empty()) {
                if (!transaction.commit()) {
                    return {storageError()};
                }
                recordValidated();
            }
            results.push_back(*item.error);
            return results;
        }
        if (!savepoint.exec(QStringLiteral("SAVEPOINT batch_item"))) {
            if (!results.empty()) {
                if (!transaction.commit()) {
                    return {storageError()};
                }
                recordValidated();
            }
            results.push_back(storageError());
            return results;
        }
        const auto outcome = insertGameInTransaction(*item.encoded, item.metadata, item.opening,
                                                     source, newlyValidated);
        if (std::holds_alternative<GameError>(outcome)) {
            if (!savepoint.exec(QStringLiteral("ROLLBACK TO batch_item")) ||
                !savepoint.exec(QStringLiteral("RELEASE batch_item"))) {
                usable_ = false;
                return {storageError()};
            }
            if (!results.empty()) {
                if (!transaction.commit()) {
                    return {storageError()};
                }
                recordValidated();
            }
            results.push_back(outcome);
            return results;
        }
        if (!savepoint.exec(QStringLiteral("RELEASE batch_item"))) {
            usable_ = false;
            return {storageError()};
        }
        results.push_back(outcome);
    }
    if (!transaction.commit()) {
        return {storageError()};
    }
    recordValidated();
    return results;
}

LoadGameResult StorageSession::loadGame(GameId id, bool includeOpening) {
    if (!usable_) {
        return storageError();
    }
    auto result = readGame(database_, id);
    if (auto* game = std::get_if<StoredGame>(&result); game && includeOpening) {
        readOpeningCache(std::span(&game->summary, 1));
        if (!game->summary.opening) {
            game->summary.opening = recognizeOpening(*game->document);
            if (game->summary.opening) {
                writeOpeningCache(id, *game->summary.opening);
            }
        }
    }
    return result;
}

ResolveGameOpeningResult StorageSession::resolveGameOpening(GameId id) {
    if (openingIndexLoaded_ && !openingIndex_) {
        return std::optional<OpeningResolution>{};
    }
    auto game = loadGame(id, true);
    if (const auto* error = std::get_if<GameError>(&game)) {
        return *error;
    }
    return std::move(std::get<StoredGame>(game).summary.opening);
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

GamePreviewResult StorageSession::previewGame(GameId id) {
    auto result = loadGame(id, true);
    if (const auto* error = std::get_if<GameError>(&result)) {
        return *error;
    }
    auto& game = std::get<StoredGame>(result);
    const auto& tree = game.document->tree;
    const auto plies = game.document->originalMainline().size();
    const auto mainLine = tree.mainLine(tree.rootId());
    const auto* nodes = std::get_if<std::vector<chess::NodeId>>(&mainLine);
    if (!nodes || plies >= nodes->size()) {
        return invalid();
    }
    // A merged variation can extend the tree past the last move actually played.
    const auto node = tree.node(nodes->at(plies));
    auto position = tree.position(nodes->at(plies));
    auto* board = std::get_if<chess::Position>(&position);
    if (!node || !board) {
        return invalid();
    }
    clipSummary(game.summary, 1024);
    clipText(game.source.uri, 4096, game.summary.metadataTruncated);
    const auto tag = [&game](std::string_view name) {
        const auto& tags = game.document->tags;
        const auto found = std::ranges::find(tags, name, &pgn::Tag::name);
        auto value = found == tags.end() ? QString{} : QString::fromStdString(found->value);
        clipText(value, 1024, game.summary.metadataTruncated);
        return value;
    };
    auto event = tag("Event");
    auto site = tag("Site");
    auto round = tag("Round");
    auto opening = tag("Opening");
    auto eco = tag("ECO");
    return GamePreview{std::move(game.summary),
                       std::move(game.source),
                       std::move(event),
                       std::move(site),
                       std::move(round),
                       std::move(opening),
                       std::move(eco),
                       board->boardIdentity(),
                       node->move,
                       plies};
}

DeleteGameResult StorageSession::deleteGame(GameId id) {
    Transaction transaction(database_, usable_);
    if (!usable_ || id <= 0 || !transaction.begin()) {
        return storageError();
    }
    QSqlQuery query(database_);
    const auto execute = [&](const QString& sql) {
        if (!query.prepare(sql)) {
            return false;
        }
        query.addBindValue(static_cast<qlonglong>(id));
        return query.exec();
    };
    if (!execute(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM review_jobs j JOIN reviews r ON r.id=j.review_id "
            "WHERE r.game_id=? AND j.status BETWEEN 0 AND 4)")) ||
        !query.next()) {
        return storageError();
    }
    const bool running = query.value(0).toBool();
    query.finish();
    if (running) {
        return GameError{GameErrorCode::Busy,
                         QStringLiteral("Stop the active review before deleting this game.")};
    }
    if (!execute(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM puzzle_jobs j JOIN reviews r ON r.id=j.source_review_id "
            "WHERE r.game_id=? AND j.status=0)")) ||
        !query.next()) {
        return storageError();
    }
    const bool generating = query.value(0).toBool();
    query.finish();
    if (generating) {
        return GameError{
            GameErrorCode::Busy,
            QStringLiteral("Stop puzzle generation for this game before deleting it.")};
    }

    // Drop game-owned review state in foreign-key order. Saved puzzles retain their
    // standalone definitions, with their source foreign keys cleared by SQLite.
    const auto reviewIds = QStringLiteral(" IN (SELECT id FROM reviews WHERE game_id=?)");
    if (!execute(QStringLiteral("DELETE FROM review_workspace WHERE game_id=?")) ||
        !execute(QStringLiteral("DELETE FROM review_workspace WHERE review_id") + reviewIds) ||
        !execute(QStringLiteral("DELETE FROM review_selection WHERE game_id=?")) ||
        !execute(QStringLiteral("DELETE FROM review_selection WHERE review_id") + reviewIds) ||
        !execute(QStringLiteral("DELETE FROM puzzle_jobs WHERE source_review_id") + reviewIds)) {
        return storageError();
    }
    for (const auto* table :
         {"review_moments", "review_classifications", "review_escalation_positions",
          "review_stage_positions", "review_pairs", "review_positions", "review_transitions",
          "review_evidence", "review_jobs"}) {
        if (!execute(QStringLiteral("DELETE FROM %1 WHERE review_id").arg(QLatin1String(table)) +
                     reviewIds)) {
            return storageError();
        }
    }
    if (!execute(QStringLiteral("DELETE FROM reviews WHERE game_id=?")) ||
        !execute(QStringLiteral("DELETE FROM games WHERE id=?"))) {
        return storageError();
    }
    if (query.numRowsAffected() == 0) {
        return GameError{GameErrorCode::Missing, QStringLiteral("Game not found.")};
    }
    query.finish();
    if (!transaction.commit()) {
        return storageError();
    }
    validatedGames_.erase(id);
    return std::monostate{};
}

LibraryStatisticsResult StorageSession::loadLibraryStatistics() {
    if (!usable_) {
        return storageError();
    }
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT (SELECT count(*) FROM games), count(DISTINCT reviews.game_id), count(*), "
            "(SELECT count(*) FROM puzzles) FROM review_jobs JOIN reviews "
            "ON reviews.id=review_jobs.review_id WHERE review_jobs.status=? "
            "AND review_jobs.completed_positions=review_jobs.total_positions"))) {
        return storageError();
    }
    query.addBindValue(static_cast<int>(review::JobStatus::Complete));
    if (!query.exec() || !query.next()) {
        return storageError();
    }
    return LibraryStatistics{query.value(0).toLongLong(), query.value(1).toLongLong(),
                             query.value(2).toLongLong(), query.value(3).toLongLong()};
}

ListGamesResult StorageSession::listGames(std::int64_t offset, int limit,
                                          const GameLibraryQuery& filter) {
    if (!usable_) {
        return storageError();
    }
    const QStringList results{QString{}, QStringLiteral("1-0"), QStringLiteral("0-1"),
                              QStringLiteral("1/2-1/2"), QStringLiteral("*")};
    const QStringList sorts{
        QStringLiteral("imported_at DESC,id DESC"),
        QStringLiteral("imported_at ASC,id ASC"),
        QStringLiteral("CASE WHEN played_at GLOB '[0-9][0-9][0-9][0-9].??.?"
                       "?' "
                       "THEN replace(played_at,'?','0') ELSE '' END DESC,id DESC"),
        QStringLiteral("white_name='',white_name COLLATE NOCASE,id DESC"),
        QStringLiteral("black_name='',black_name COLLATE NOCASE,id DESC"),
        QStringLiteral("activity DESC,games.id DESC")};
    const auto resultIndex = static_cast<int>(filter.result);
    const auto sortIndex = static_cast<int>(filter.sort);
    const bool recent = filter.sort == GameSort::RecentActivity;
    if (offset < 0 || limit < 1 || limit > 200 || (recent && (offset != 0 || filter.filtered())) ||
        resultIndex < 0 || resultIndex >= results.size() || sortIndex < 0 ||
        sortIndex >= sorts.size() || filter.review < GameReviewFilter::All ||
        filter.review > GameReviewFilter::NoReview ||
        filter.players.size() > GameLibraryQuery::maxSearchLength ||
        filter.players.contains(QChar::Null) || !filter.players.isValidUtf16()) {
        return GameError{GameErrorCode::InvalidSnapshot,
                         QStringLiteral("Invalid game search or page options.")};
    }
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    // Get the first page from two small candidate sets, then deduplicate by game.
    // The review ID and per-game review indexes locate each game's newest run.
    QString sql;
    if (recent) {
        sql = QStringLiteral(
            "WITH imports AS (SELECT id,substr(imported_at,1,65) AS activity FROM games "
            "ORDER BY imported_at DESC,id DESC LIMIT ?), "
            "recent_reviews AS (SELECT r.game_id AS id,"
            "strftime('%Y-%m-%dT%H:%M:%fZ',r.created_at/1000.0,'unixepoch') AS activity "
            "FROM reviews r WHERE r.id=(SELECT id FROM reviews WHERE game_id=r.game_id "
            "ORDER BY id DESC LIMIT 1) ORDER BY r.id DESC LIMIT ?), "
            "activity_by_game AS (SELECT id,max(activity) AS activity FROM "
            "(SELECT * FROM imports UNION ALL SELECT * FROM recent_reviews) GROUP BY id) ");
    }
    sql += QStringLiteral(
        "SELECT games.id,substr(CAST(white_name AS BLOB),1,65537),substr(CAST(black_name AS "
        "BLOB),1,65537),substr(CAST(result AS BLOB),1,17),substr(CAST(played_at AS "
        "BLOB),1,65537),substr(CAST(time_control AS BLOB),1,65537),substr(CAST(imported_at AS "
        "BLOB),1,65) ,(games.id>0 AND typeof(white_name)='text' AND typeof(black_name)='text' AND "
        "typeof(result)='text' AND typeof(played_at)='text' AND typeof(time_control)='text' "
        "AND typeof(imported_at)='text') FROM games");
    if (recent) {
        sql += QStringLiteral(" JOIN activity_by_game ON activity_by_game.id=games.id");
    }
    sql += QStringLiteral(" WHERE 1");
    const auto terms = filter.players.simplified().split(u' ', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < terms.size(); ++i) {
        sql += QStringLiteral(" AND (white_name GLOB ? OR black_name GLOB ?)");
    }
    if (filter.result != GameResultFilter::All) {
        sql += QStringLiteral(" AND result=?");
    }
    if (filter.review != GameReviewFilter::All) {
        sql += filter.review == GameReviewFilter::NoReview ? QStringLiteral(" AND NOT ")
                                                           : QStringLiteral(" AND ");
        sql += QStringLiteral("EXISTS(SELECT 1 FROM reviews WHERE game_id=games.id)");
    }
    sql += QStringLiteral(" ORDER BY ") + sorts.at(sortIndex) + QStringLiteral(" LIMIT ? OFFSET ?");
    if (!query.prepare(sql)) {
        return storageError();
    }
    if (recent) {
        query.addBindValue(limit);
        query.addBindValue(limit);
    }
    for (const auto& term : terms) {
        const auto pattern = playerPattern(term);
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    if (filter.result != GameResultFilter::All) {
        query.addBindValue(results.at(resultIndex));
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
        clipSummary(*metadata, 256);
        games.push_back(std::move(*metadata));
    }
    if (query.lastError().isValid()) {
        return storageError();
    }
    query.finish();
    if (!recent) {
        readOpeningCache(games);
    }
    return games;
}
} // namespace chess_review::persistence::detail
