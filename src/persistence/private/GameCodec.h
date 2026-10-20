#pragma once
#include "persistence/GameRepository.h"
#include <QByteArray>
namespace chess_review::persistence::detail {
constexpr int maxIdentityBytes = 1024 * 1024;
constexpr int maxSnapshotBytes = 64 * 1024 * 1024;
struct EncodedGame {
    QByteArray snapshot;
    QByteArray identity;
    QByteArray hash;
    QByteArray original;
};
using EncodeResult = std::variant<EncodedGame, GameError>;
EncodeResult encodeGame(const pgn::PgnDocument& document, int version = 2);
struct GameIdentity {
    QByteArray key;
    QByteArray hash;
};
// Only use after decoding/validating the immutable document.
GameIdentity gameIdentity(const pgn::PgnDocument& document, int version = 2);
GameSummary gameMetadata(const pgn::PgnDocument& document, int version = 2);
LoadGameResult decodeGame(const QByteArray& original, const QByteArray& snapshot,
                          int expectedVersion = 0);
} // namespace chess_review::persistence::detail
