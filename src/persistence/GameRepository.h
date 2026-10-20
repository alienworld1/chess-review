#pragma once

#include "pgn/Pgn.h"
#include <QMetaType>
#include <QString>
#include <QUuid>
#include <cstdint>
#include <memory>
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
struct GameSummary {
    GameId id{};
    QString white;
    QString black;
    QString result;
    QString playedAt;
    QString timeControl;
    QString importedAt;
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
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::InsertGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::LoadGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListGamesResult)
