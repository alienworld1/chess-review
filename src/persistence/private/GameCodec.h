#pragma once
#include "persistence/GameRepository.h"
#include <QByteArray>
namespace chess_review::persistence::detail {
constexpr int maxSnapshotBytes = 64 * 1024 * 1024;
struct EncodedGame {
    QByteArray snapshot;
    QByteArray identity;
    QByteArray hash;
    QByteArray original;
};
using EncodeResult = std::variant<EncodedGame, GameError>;
EncodeResult encodeGame(const pgn::PgnDocument& document);
struct GameIdentity {
    QByteArray key;
    QByteArray hash;
};
// Only use after decoding/validating the immutable document.
GameIdentity gameIdentity(const pgn::PgnDocument& document);
LoadGameResult decodeGame(const QByteArray& original, const QByteArray& snapshot);
} // namespace chess_review::persistence::detail
