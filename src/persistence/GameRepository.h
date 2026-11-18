#pragma once

#include "openings/OpeningIndex.h"
#include "pgn/Pgn.h"
#include <QMetaType>
#include <QString>
#include <QUuid>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace chess_review::persistence {
using GameId = std::int64_t;
using RequestId = QUuid;
enum class GameErrorCode {
    Missing,
    InvalidSnapshot,
    UnsupportedVersion,
    Storage,
    Unavailable,
    Closing,
    Busy
};
struct GameError {
    GameErrorCode code = GameErrorCode::Storage;
    QString message; // Safe static diagnostic; no source or native SQL error.
};
struct GameSource {
    QString type;
    QString uri;
};
struct OpeningResolution {
    // Empty only for a review explicitly saved while recognition was unavailable.
    QString datasetVersion;
    std::optional<openings::Opening> match;
};
struct GameSummary {
    GameId id{};
    QString white;
    QString black;
    QString result;
    QString playedAt;
    QString timeControl;
    QString importedAt;
    // List summaries clip display fields; load returns full values. Always display as plain text.
    bool metadataTruncated = false;
    // A damaged metadata row remains identifiable without hiding healthy rows on the page.
    std::optional<GameError> metadataError = std::nullopt;
    // An engaged value with no match is a completed lookup.
    std::optional<OpeningResolution> opening = std::nullopt;
};
struct StoredGame {
    GameSummary summary;
    GameSource source;
    std::shared_ptr<const pgn::PgnDocument> document;
};
struct Inserted {
    GameId id;
};
struct Duplicate {
    GameId id;
};
using InsertGameResult = std::variant<Inserted, Duplicate, GameError>;
using LoadGameResult = std::variant<StoredGame, GameError>;
using ListGamesResult = std::variant<std::vector<GameSummary>, GameError>;
using ResolveGameOpeningResult = std::variant<std::optional<OpeningResolution>, GameError>;
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::InsertGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::LoadGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListGamesResult)
Q_DECLARE_METATYPE(chess_review::persistence::ResolveGameOpeningResult)
