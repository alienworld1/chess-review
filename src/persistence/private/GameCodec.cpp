#include "persistence/private/GameCodec.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <algorithm>
#include <array>

namespace chess_review::persistence::detail {
namespace {
QString text(const std::string& value, int version = 2) {
    if (version == 1) {
        return QString::fromUtf8(value);
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    return decoder(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}
QString wireText(const std::string& value, int version) {
    if (version == 1) {
        return text(value, 1);
    }
    // v2 transports exact UTF-8 bytes using Qt's Base64 encoder. Field-level BOMs
    // must not acquire stream-header semantics during JSON string conversion.
    return QString::fromLatin1(
        QByteArray(value.data(), static_cast<qsizetype>(value.size())).toBase64());
}
QString moveText(const chess::Move& move) {
    QString result = text(move.from.name() + move.to.name());
    if (move.promotion) {
        switch (*move.promotion) {
        case chess::PieceType::Queen:
            result += u'q';
            break;
        case chess::PieceType::Rook:
            result += u'r';
            break;
        case chess::PieceType::Bishop:
            result += u'b';
            break;
        case chess::PieceType::Knight:
            result += u'n';
            break;
        default:
            result += u'?';
            break;
        }
    }
    return result;
}
int resultCode(pgn::GameResult result) {
    // Explicit v1 wire values, independent of enum declaration order.
    switch (result) {
    case pgn::GameResult::Unknown:
        return 0;
    case pgn::GameResult::WhiteWin:
        return 1;
    case pgn::GameResult::BlackWin:
        return 2;
    case pgn::GameResult::Draw:
        return 3;
    }
    return -1;
}
QJsonObject model(const pgn::PgnDocument& document, int version) {
    QJsonArray tags;
    for (const auto& tag : document.tags) {
        tags.append(QJsonArray{wireText(tag.name, version), wireText(tag.value, version)});
    }
    QJsonArray line;
    for (const auto& move : document.originalMainline()) {
        line.append(moveText(move));
    }
    QJsonArray nodes;
    std::vector<chess::NodeId> pending{document.tree.rootId()};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto node = document.tree.node(id);
        if (!node) {
            return {};
        }
        QJsonArray children;
        for (auto child : node->children) {
            children.append(QString::number(child.value));
        }
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            pending.push_back(*it);
        }
        QJsonArray nags;
        for (auto nag : node->annotations.nags) {
            nags.append(nag);
        }
        nodes.append(
            QJsonObject{{"id", QString::number(id.value)},
                        {"parent", node->parent ? QString::number(node->parent->value) : QString()},
                        {"move", node->move ? moveText(*node->move) : QString()},
                        {"children", children},
                        {"before", wireText(node->annotations.commentBefore, version)},
                        {"after", wireText(node->annotations.commentAfter, version)},
                        {"nags", nags}});
    }
    const auto position = document.tree.position(document.tree.rootId());
    const auto* root = std::get_if<chess::Position>(&position);
    if (!root) {
        return {};
    }
    QJsonObject object{
        {"formatVersion", version},
        {"tags", tags},
        {"result", resultCode(document.result)},
        {"rootFen", text(root->fen())},
        {"fenRoot", root->context().provenance == chess::RootProvenance::FenAnalysisRoot},
        {"nodes", nodes},
        {"originalMainline", line}};
    if (version == 2) {
        object.insert(QStringLiteral("textEncoding"), QStringLiteral("utf8-base64"));
    }
    return object;
}
QByteArray identity(const pgn::PgnDocument& document, int version) {
    const auto root = document.tree.position(document.tree.rootId());
    QJsonArray line;
    for (const auto& move : document.originalMainline()) {
        line.append(moveText(move));
    }
    QJsonArray fields{version, text(std::get<chess::Position>(root).fen()), line,
                      resultCode(document.result)};
    for (const auto* name : {"White", "Black", "Date", "Event", "Site", "Round", "TimeControl"}) {
        QString value;
        for (const auto& tag : document.tags) {
            if (tag.name == name) {
                value = version == 1
                            ? text(tag.value, 1)
                            : (tag.value == "?" ? QString{} : wireText(tag.value, version));
                break;
            }
        }
        if (value == QStringLiteral("?")) {
            value.clear();
        }
        fields.append(value);
    }
    return QJsonDocument(fields).toJson(QJsonDocument::Compact);
}
std::shared_ptr<const pgn::PgnDocument> parse(const QByteArray& original) {
    pgn::PgnReader reader(
        std::string_view(original.constData(), static_cast<std::size_t>(original.size())));
    auto outcome = reader.next();
    auto* document = std::get_if<pgn::PgnDocument>(&outcome);
    if (!document || !std::holds_alternative<pgn::EndOfInput>(reader.next())) {
        return {};
    }
    return std::make_shared<const pgn::PgnDocument>(std::move(*document));
}
GameError invalid() {
    return {GameErrorCode::InvalidSnapshot, QStringLiteral("Invalid imported game snapshot.")};
}
} // namespace
EncodeResult encodeGame(const pgn::PgnDocument& document, int version) {
    if (version != 1 && version != 2) {
        return invalid();
    }
    if (document.originalText.size() > pgn::PgnLimits{}.maxGameBytes) {
        return invalid();
    }
    QByteArray original(document.originalText.data(),
                        static_cast<qsizetype>(document.originalText.size()));
    const auto validated = parse(original);
    if (!validated) {
        return invalid();
    }
    if (document.tags != validated->tags || document.result != validated->result ||
        document.tree.size() != validated->tree.size() ||
        !std::equal(document.originalMainline().begin(), document.originalMainline().end(),
                    validated->originalMainline().begin(), validated->originalMainline().end())) {
        return invalid();
    }
    const auto root = document.tree.position(document.tree.rootId());
    const auto validRoot = validated->tree.position(validated->tree.rootId());
    const auto* position = std::get_if<chess::Position>(&root);
    const auto* expected = std::get_if<chess::Position>(&validRoot);
    if (!position || !expected || position->context() != expected->context()) {
        return invalid();
    }
    for (std::uint64_t index = 0; index < validated->tree.size(); ++index) {
        if (document.tree.node(chess::NodeId{index}) !=
            validated->tree.node(chess::NodeId{index})) {
            return invalid();
        }
    }
    const auto object = model(*validated, version);
    if (object.isEmpty()) {
        return invalid();
    }
    auto snapshot = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (snapshot.size() > maxSnapshotBytes) {
        return invalid();
    }
    auto key = identity(*validated, version);
    if (key.size() > maxIdentityBytes) {
        return invalid();
    }
    auto hash = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    return EncodedGame{std::move(snapshot), std::move(key), std::move(hash), std::move(original)};
}
GameIdentity gameIdentity(const pgn::PgnDocument& document, int version) {
    auto key = identity(document, version);
    auto hash = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    return {std::move(key), std::move(hash)};
}
GameSummary gameMetadata(const pgn::PgnDocument& document, int version) {
    const auto tag = [&](const char* name) {
        for (const auto& entry : document.tags) {
            if (entry.name == name) {
                return text(entry.value, version);
            }
        }
        return QStringLiteral("");
    };
    constexpr std::array results{"*", "1-0", "0-1", "1/2-1/2"};
    const int code = resultCode(document.result);
    const auto result = code >= 0 && code < 4
                            ? QString::fromLatin1(results[static_cast<std::size_t>(code)])
                            : QString{};
    return {0, tag("White"), tag("Black"), result, tag("Date"), tag("TimeControl"), {}};
}
LoadGameResult decodeGame(const QByteArray& original, const QByteArray& snapshot,
                          int expectedVersion) {
    if (snapshot.size() > maxSnapshotBytes ||
        original.size() > static_cast<qsizetype>(pgn::PgnLimits{}.maxGameBytes)) {
        return invalid();
    }
    if (!snapshot.isValidUtf8()) {
        return invalid();
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(snapshot, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return invalid();
    }
    const auto versionValue = json.object().value("formatVersion");
    if (versionValue != QJsonValue(1) && versionValue != QJsonValue(2)) {
        return GameError{GameErrorCode::UnsupportedVersion,
                         QStringLiteral("Unsupported game snapshot version.")};
    }
    const int version = versionValue.toInt();
    if (expectedVersion != 0 && version != expectedVersion) {
        return invalid();
    }
    auto document = parse(original);
    if (!document || model(*document, version) != json.object()) {
        return invalid();
    }
    return StoredGame{{}, {}, std::move(document)};
}
} // namespace chess_review::persistence::detail
