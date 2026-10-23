#include "engine/private/Fingerprint.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <utility>

namespace chess_review::engine {
QByteArray fingerprintDigest(const EngineFingerprint& fingerprint) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    // QString equality treats null and empty alike; give them one wire representation.
    const auto text = [&stream](const QString& value) {
        stream << (value.isEmpty() ? QByteArray("") : value.toUtf8());
    };
    stream << QByteArray("chess-review.engine-fingerprint")
           << static_cast<quint32>(fingerprint.schemaVersion)
           << static_cast<quint32>(fingerprint.adapterPolicyVersion)
           << fingerprint.executableSha256;
    text(fingerprint.reportedName);
    stream << fingerprint.reportedAuthor.has_value();
    if (fingerprint.reportedAuthor) {
        text(*fingerprint.reportedAuthor);
    }
    stream << static_cast<quint32>(fingerprint.resources)
           << static_cast<quint32>(fingerprint.options.size());
    for (const auto& option : fingerprint.options) {
        text(option.name);
        stream << static_cast<quint32>(option.type);
        text(option.value);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
EngineFingerprint makeFingerprint(const ValidatedEngine& engine,
                                  std::vector<EffectiveOption> options) {
    EngineFingerprint fingerprint;
    fingerprint.executableSha256 = engine.executableSha256;
    fingerprint.reportedName = engine.reportedName;
    fingerprint.reportedAuthor = engine.reportedAuthor;
    fingerprint.options = std::move(options);
    fingerprint.digest = fingerprintDigest(fingerprint);
    return fingerprint;
}
} // namespace chess_review::engine
