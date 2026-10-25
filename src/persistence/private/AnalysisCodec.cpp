#include "persistence/private/AnalysisCodec.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QStringDecoder>
#include <QTimeZone>
#include <limits>
#include <type_traits>
#include <utility>

namespace chess_review::persistence::detail {
namespace {
using namespace engine;
// These values are frozen by payload format 2. Changing them requires a codec transition.
static_assert(static_cast<int>(ScoreType::Centipawns) == 0 &&
              static_cast<int>(ScoreType::Mate) == 1);
static_assert(static_cast<int>(ScoreBound::Exact) == 0 &&
              static_cast<int>(ScoreBound::Lower) == 1 && static_cast<int>(ScoreBound::Upper) == 2);
static_assert(static_cast<int>(chess::PieceType::Pawn) == 0 &&
              static_cast<int>(chess::PieceType::Knight) == 1 &&
              static_cast<int>(chess::PieceType::Bishop) == 2 &&
              static_cast<int>(chess::PieceType::Rook) == 3 &&
              static_cast<int>(chess::PieceType::Queen) == 4 &&
              static_cast<int>(chess::PieceType::King) == 5);
static_assert(static_cast<int>(chess::Variant::Standard) == 0 &&
              static_cast<int>(chess::RootProvenance::InitialPosition) == 0 &&
              static_cast<int>(chess::RootProvenance::FenAnalysisRoot) == 1);

class Writer {
  public:
    QByteArray bytes;
    QDataStream stream{&bytes, QIODevice::WriteOnly};
    Writer() {
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::BigEndian);
    }
    void u32(std::uint32_t n) { stream << static_cast<quint32>(n); }
    void u64(std::uint64_t n) { stream << static_cast<quint64>(n); }
    void i64(std::int64_t n) { stream << static_cast<qint64>(n); }
    void flag(bool value) { stream << static_cast<quint8>(value ? 1 : 0); }
    void data(const QByteArray& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        stream.writeRawData(value.constData(), value.size());
    }
    void text(const QString& value) { data(value.toUtf8()); }
    void move(const chess::Move& m) {
        u32(static_cast<std::uint32_t>(m.from.index()));
        u32(static_cast<std::uint32_t>(m.to.index()));
        u32(m.promotion ? static_cast<std::uint32_t>(*m.promotion) + 1 : 0);
    }
    void moves(const std::vector<chess::Move>& values) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& m : values) {
            move(m);
        }
    }
    template <typename T, typename F> void optional(const std::optional<T>& v, F write) {
        flag(v.has_value());
        if (v) {
            write(*v);
        }
    }
    void stats(const SearchStatistics& s) {
        for (const auto& n : {s.depth, s.selectiveDepth, s.hashFull}) {
            optional(n, [this](int v) { u32(static_cast<std::uint32_t>(v)); });
        }
        for (const auto& n : {s.nodes, s.timeMs, s.nps, s.tablebaseHits}) {
            optional(n, [this](std::uint64_t v) { u64(v); });
        }
    }
    void context(const chess::PositionContext& c) {
        u32(1);
        u32(c.version);
        u32(static_cast<std::uint32_t>(c.variant));
        u32(static_cast<std::uint32_t>(c.provenance));
        data(QByteArray::fromStdString(c.rootFen));
        moves(c.moves);
    }
};
class Reader {
  public:
    QDataStream stream;
    bool good = true;
    explicit Reader(const QByteArray& bytes) : stream(bytes) {
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::BigEndian);
    }
    std::uint32_t u32() {
        quint32 n = 0;
        stream >> n;
        return n;
    }
    std::uint64_t u64() {
        quint64 n = 0;
        stream >> n;
        return n;
    }
    std::int64_t i64() {
        qint64 n = 0;
        stream >> n;
        return n;
    }
    int number() {
        const auto n = u32();
        if (n > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            good = false;
            return 0;
        }
        return static_cast<int>(n);
    }
    bool flag() {
        quint8 n = 0;
        stream >> n;
        if (n > 1) {
            good = false;
        }
        return n == 1;
    }
    QByteArray data(qsizetype maximum = 1024 * 1024) {
        const auto n = u32();
        if (!good || n > maximum || n > stream.device()->bytesAvailable()) {
            good = false;
            return {};
        }
        QByteArray bytes(static_cast<qsizetype>(n), Qt::Uninitialized);
        if (stream.readRawData(bytes.data(), n) != n) {
            good = false;
        }
        return bytes;
    }
    QString text() {
        const auto bytes = data(256 * 1024);
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
        const QString value = decoder(bytes);
        if (decoder.hasError()) {
            good = false;
        }
        return value;
    }
    chess::Move move() {
        const auto from = u32();
        const auto to = u32();
        const auto promotion = u32();
        if (from >= 64 || to >= 64 || promotion > 6) {
            good = false;
            return {};
        }
        return {
            *chess::Square::fromCoordinates(static_cast<int>(from % 8), static_cast<int>(from / 8)),
            *chess::Square::fromCoordinates(static_cast<int>(to % 8), static_cast<int>(to / 8)),
            promotion ? std::optional(static_cast<chess::PieceType>(promotion - 1)) : std::nullopt};
    }
    std::vector<chess::Move> moves(std::uint32_t maximum) {
        const auto count = u32();
        std::vector<chess::Move> values;
        if (!good || count > maximum ||
            static_cast<quint64>(count) * 12 >
                static_cast<quint64>(stream.device()->bytesAvailable())) {
            good = false;
            return values;
        }
        values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            values.push_back(move());
        }
        return values;
    }
    template <typename F> auto optional(F read) -> std::optional<decltype(read())> {
        if (!flag()) {
            return std::nullopt;
        }
        return read();
    }
    SearchStatistics stats() {
        SearchStatistics s;
        s.depth = optional([this] { return number(); });
        s.selectiveDepth = optional([this] { return number(); });
        s.hashFull = optional([this] { return number(); });
        s.nodes = optional([this] { return u64(); });
        s.timeMs = optional([this] { return u64(); });
        s.nps = optional([this] { return u64(); });
        s.tablebaseHits = optional([this] { return u64(); });
        return s;
    }
    chess::PositionContext context(const chess::Position* knownRoot) {
        const auto format = u32();
        const auto version = u32();
        const auto variant = u32();
        const auto provenance = u32();
        const auto fen = data(256);
        const auto history = moves(13107);
        if (!good || format != 1 || version != 1 || variant != 0 || provenance > 1) {
            good = false;
            return {};
        }
        if (knownRoot) {
            auto context = knownRoot->context();
            if (context.version == version &&
                static_cast<std::uint32_t>(context.variant) == variant &&
                context.rootFen == fen.toStdString() && context.moves == history &&
                static_cast<std::uint32_t>(context.provenance) == provenance) {
                return context;
            }
        }
        auto restored = provenance == 0 ? chess::Result<chess::Position>(chess::Position::initial())
                                        : chess::Position::fromFen(fen.toStdString());
        auto* position = std::get_if<chess::Position>(&restored);
        if (!position || position->context().rootFen != fen.toStdString()) {
            good = false;
            return {};
        }
        for (const auto& m : history) {
            if (std::holds_alternative<chess::Error>(position->play(m))) {
                good = false;
                return {};
            }
        }
        return position->context();
    }
    bool finished() { return good && stream.status() == QDataStream::Ok && stream.atEnd(); }
};
void writeFingerprint(Writer& w, const EngineFingerprint& f) {
    w.u32(f.schemaVersion);
    w.u32(f.adapterPolicyVersion);
    w.data(f.executableSha256);
    w.text(f.reportedName);
    w.optional(f.reportedAuthor, [&](const QString& s) { w.text(s); });
    w.u32(static_cast<std::uint32_t>(f.resources));
    w.u32(f.resourcePolicyVersion);
    w.data(f.digest);
    w.u32(static_cast<std::uint32_t>(f.options.size()));
    for (const auto& o : f.options) {
        w.text(o.name);
        w.u32(static_cast<std::uint32_t>(o.type));
        w.text(o.value);
    }
}
EngineFingerprint readFingerprint(Reader& r) {
    EngineFingerprint f;
    f.schemaVersion = r.u32();
    f.adapterPolicyVersion = r.u32();
    f.executableSha256 = r.data(32);
    f.reportedName = r.text();
    f.reportedAuthor = r.optional([&] { return r.text(); });
    f.resources = static_cast<ResourceVerification>(r.u32());
    f.resourcePolicyVersion = r.u32();
    f.digest = r.data(32);
    const auto count = r.u32();
    if (count > 4096) {
        r.good = false;
        return f;
    }
    for (std::uint32_t i = 0; i < count && r.good; ++i) {
        EffectiveOption o;
        o.name = r.text();
        o.type = static_cast<OptionType>(r.u32());
        o.value = r.text();
        f.options.push_back(std::move(o));
    }
    return f;
}
void writeLimit(Writer& w, const SearchLimit& limit) {
    // Stable wire tags must not depend on std::variant alternative ordering.
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, NodeLimit>) {
                w.u32(0);
                w.u64(value.nodes);
            } else if constexpr (std::is_same_v<T, DepthLimit>) {
                w.u32(1);
                w.u64(value.depth);
            } else if constexpr (std::is_same_v<T, TimeLimit>) {
                w.u32(2);
                w.u64(value.milliseconds);
            } else {
                static_assert(std::is_same_v<T, InfiniteLimit>);
                w.u32(3);
            }
        },
        limit);
}
SearchLimit readLimit(Reader& r) {
    const auto type = r.u32();
    if (type == 3) {
        return InfiniteLimit{};
    }
    const auto n = r.u64();
    if (type == 0) {
        return NodeLimit{n};
    }
    if (type == 1 && n <= std::numeric_limits<std::uint32_t>::max()) {
        return DepthLimit{static_cast<std::uint32_t>(n)};
    }
    if (type == 2) {
        return TimeLimit{n};
    }
    r.good = false;
    return InfiniteLimit{};
}
} // namespace
QByteArray positionIdentity(const chess::PositionContext& context) {
    Writer w;
    w.context(context);
    return w.bytes;
}
namespace {
// The caller has validated the complete value. Canonical re-encoding must not
// recursively repeat history/PV validation inside the decoder.
QByteArray encodeValidatedAnalysis(const engine::AnalysisResult& result) {
    const auto& s = result.evidence;
    const auto& p = *s.provenance;
    Writer w;
    w.u32(analysisFormatVersion);
    w.data(s.session.toRfc4122());
    w.data(s.request.toRfc4122());
    w.context(p.position);
    writeFingerprint(w, p.fingerprint);
    w.optional(p.searchMoves, [&](const auto& moves) { w.moves(moves); });
    w.i64(p.createdAt.toMSecsSinceEpoch());
    writeLimit(w, s.limit);
    w.u32(static_cast<std::uint32_t>(s.requestedMultiPv));
    w.u32(static_cast<std::uint32_t>(s.effectiveMultiPv));
    w.flag(s.wdlRequested);
    w.flag(s.wdlSupported);
    w.stats(s.statistics);
    w.u32(static_cast<std::uint32_t>(s.achievedWidth));
    w.optional(s.commonDepth, [&](int n) { w.u32(static_cast<std::uint32_t>(n)); });
    w.optional(s.bestMove, [&](const auto& m) { w.move(m); });
    w.optional(s.ponder, [&](const auto& m) { w.move(m); });
    w.u32(static_cast<std::uint32_t>(s.lines.size()));
    for (const auto& line : s.lines) {
        w.u32(static_cast<std::uint32_t>(line.rank));
        w.optional(line.score, [&](const auto& score) {
            w.u32(static_cast<std::uint32_t>(score.type));
            w.i64(score.value);
            w.u32(static_cast<std::uint32_t>(score.bound));
        });
        w.optional(line.wdl, [&](const auto& v) {
            w.u32(static_cast<std::uint32_t>(v.wins));
            w.u32(static_cast<std::uint32_t>(v.draws));
            w.u32(static_cast<std::uint32_t>(v.losses));
        });
        w.moves(line.pv);
        w.stats(line.statistics);
    }
    if (w.bytes.size() > maxAnalysisBytes - 32 || w.stream.status() != QDataStream::Ok) {
        return {};
    }
    w.bytes += QCryptographicHash::hash(w.bytes, QCryptographicHash::Sha256);
    return w.bytes;
}
} // namespace
std::variant<QByteArray, CacheError> encodeAnalysis(const engine::AnalysisResult& result) {
    if (engine::validateCacheResult(result)) {
        return CacheError{CacheErrorCode::InvalidInput,
                          QStringLiteral("Invalid completed analysis evidence.")};
    }
    const auto bytes = encodeValidatedAnalysis(result);
    if (bytes.isEmpty()) {
        return CacheError{CacheErrorCode::InvalidInput,
                          QStringLiteral("Analysis exceeds the cache record limit.")};
    }
    return bytes;
}
std::optional<engine::AnalysisResult> decodeAnalysis(const QByteArray& bytes,
                                                     const chess::Position* knownRoot) {
    if (bytes.size() > maxAnalysisBytes || bytes.size() < 36) {
        return std::nullopt;
    }
    const auto body = bytes.first(bytes.size() - 32);
    if (QCryptographicHash::hash(body, QCryptographicHash::Sha256) != bytes.last(32)) {
        return std::nullopt;
    }
    Reader r(body);
    if (r.u32() != analysisFormatVersion) {
        return std::nullopt;
    }
    engine::AnalysisResult result;
    result.outcome = engine::AnalysisOutcome::Completed;
    auto& s = result.evidence;
    const auto session = r.data(16);
    const auto request = r.data(16);
    if (session.size() != 16 || request.size() != 16) {
        return std::nullopt;
    }
    s.session = QUuid::fromRfc4122(session);
    s.request = QUuid::fromRfc4122(request);
    engine::AnalysisProvenance p;
    p.position = r.context(knownRoot);
    p.fingerprint = readFingerprint(r);
    p.searchMoves = r.optional([&] { return r.moves(256); });
    p.createdAt = QDateTime::fromMSecsSinceEpoch(r.i64(), QTimeZone::UTC);
    s.provenance = std::make_shared<const engine::AnalysisProvenance>(std::move(p));
    s.limit = readLimit(r);
    s.requestedMultiPv = r.number();
    s.effectiveMultiPv = r.number();
    s.wdlRequested = r.flag();
    s.wdlSupported = r.flag();
    s.statistics = r.stats();
    s.achievedWidth = r.number();
    s.commonDepth = r.optional([&] { return r.number(); });
    s.bestMove = r.optional([&] { return r.move(); });
    s.ponder = r.optional([&] { return r.move(); });
    const auto count = r.u32();
    if (count > 256) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < count && r.good; ++i) {
        engine::AnalysisLine line;
        line.rank = r.number();
        line.score = r.optional([&] {
            engine::AnalysisScore score;
            score.type = static_cast<engine::ScoreType>(r.u32());
            score.value = r.i64();
            score.bound = static_cast<engine::ScoreBound>(r.u32());
            return score;
        });
        line.wdl = r.optional([&] { return engine::Wdl{r.number(), r.number(), r.number()}; });
        line.pv = r.moves(1024);
        line.statistics = r.stats();
        s.lines.push_back(std::move(line));
    }
    s.searchStarted = true;
    if (!r.finished() || engine::validateCacheResult(result, knownRoot)) {
        return std::nullopt;
    }
    if (encodeValidatedAnalysis(result) != bytes) {
        return std::nullopt;
    }
    return result;
}
} // namespace chess_review::persistence::detail
