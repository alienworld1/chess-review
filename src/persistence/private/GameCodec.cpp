#include "persistence/private/GameCodec.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>
#include <algorithm>

namespace chess_review::persistence::detail {
namespace {
QString text(const std::string& value) { return QString::fromUtf8(value); }
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
QJsonObject model(const pgn::PgnDocument& document) {
    QJsonArray tags;
    for (const auto& tag : document.tags) {
        tags.append(QJsonArray{text(tag.name), text(tag.value)});
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
                        {"before", text(node->annotations.commentBefore)},
                        {"after", text(node->annotations.commentAfter)},
                        {"nags", nags}});
    }
    const auto position = document.tree.position(document.tree.rootId());
    const auto* root = std::get_if<chess::Position>(&position);
    if (!root) {
        return {};
    }
    return {{"formatVersion", 1},
            {"tags", tags},
            {"result", resultCode(document.result)},
            {"rootFen", text(root->fen())},
            {"fenRoot", root->context().provenance == chess::RootProvenance::FenAnalysisRoot},
            {"nodes", nodes},
            {"originalMainline", line}};
}
QByteArray identity(const pgn::PgnDocument& document) {
    const auto root = document.tree.position(document.tree.rootId());
    QJsonArray line;
    for (const auto& move : document.originalMainline()) {
        line.append(moveText(move));
    }
    QJsonArray fields{1, text(std::get<chess::Position>(root).fen()), line,
                      resultCode(document.result)};
    for (const auto* name : {"White", "Black", "Date", "Event", "Site", "Round", "TimeControl"}) {
        QString value;
        for (const auto& tag : document.tags) {
            if (tag.name == name) {
                value = text(tag.value);
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
EncodeResult encodeGame(const pgn::PgnDocument& document) {
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
    const auto object = model(*validated);
    if (object.isEmpty()) {
        return invalid();
    }
    auto snapshot = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (snapshot.size() > maxSnapshotBytes) {
        return invalid();
    }
    auto key = identity(*validated);
    auto hash = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    return EncodedGame{std::move(snapshot), std::move(key), std::move(hash), std::move(original)};
}
GameIdentity gameIdentity(const pgn::PgnDocument& document) {
    auto key = identity(document);
    auto hash = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    return {std::move(key), std::move(hash)};
}
LoadGameResult decodeGame(const QByteArray& original, const QByteArray& snapshot) {
    if (snapshot.size() > maxSnapshotBytes ||
        original.size() > static_cast<qsizetype>(pgn::PgnLimits{}.maxGameBytes)) {
        return invalid();
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(snapshot);
    Q_UNUSED(decoded);
    if (decoder.hasError()) {
        return invalid();
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(snapshot, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return invalid();
    }
    if (json.object().value("formatVersion") != QJsonValue(1)) {
        return GameError{GameErrorCode::UnsupportedVersion,
                         QStringLiteral("Unsupported game snapshot version.")};
    }
    auto document = parse(original);
    if (!document || model(*document) != json.object()) {
        return invalid();
    }
    return StoredGame{{}, {}, std::move(document)};
}
} // namespace chess_review::persistence::detail
