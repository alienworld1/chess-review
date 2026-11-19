#pragma once

#include "openings/OpeningIndex.h"
#include "pgn/Pgn.h"
#include <QMetaType>
#include <QString>
#include <QUuid>
#include <cstddef>
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
    // Present only for a review with a resolved book lookup.
    std::optional<int> bookPlies = std::nullopt;
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
enum class GameResultFilter { All, WhiteWin, BlackWin, Draw, Unrecorded };
enum class GameReviewFilter { All, HasReview, NoReview };
enum class GameSort { NewestImport, OldestImport, PlayedDate, White, Black, RecentActivity };
struct GameLibraryQuery {
    static constexpr int maxSearchLength = 160;
    QString players;
    GameResultFilter result = GameResultFilter::All;
    GameReviewFilter review = GameReviewFilter::All;
    GameSort sort = GameSort::NewestImport;
    bool filtered() const {
        return !players.isEmpty() || result != GameResultFilter::All ||
               review != GameReviewFilter::All;
    }
    bool operator==(const GameLibraryQuery&) const = default;
};
struct GamePreview {
    GameSummary summary;
    GameSource source;
    QString event;
    QString site;
    QString round;
    QString openingTag;
    QString ecoTag;
    chess::BoardIdentity board;
    std::optional<chess::Move> lastMove;
    std::size_t plies;
};
struct Inserted {
    GameId id;
};
struct Duplicate {
    GameId id;
};
using InsertGameResult = std::variant<Inserted, Duplicate, GameError>;
using LoadGameResult = std::variant<StoredGame, GameError>;
using GamePreviewResult = std::variant<GamePreview, GameError>;
using DeleteGameResult = std::variant<std::monostate, GameError>;
using ListGamesResult = std::variant<std::vector<GameSummary>, GameError>;
using ResolveGameOpeningResult = std::variant<std::optional<OpeningResolution>, GameError>;
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::InsertGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::LoadGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::GamePreviewResult)
Q_DECLARE_METATYPE(chess_review::persistence::DeleteGameResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListGamesResult)
Q_DECLARE_METATYPE(chess_review::persistence::ResolveGameOpeningResult)
