#include "engine/AnalysisCache.h"
#include "engine/private/Fingerprint.h"
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chess_review::engine {
namespace {
Error invalid() {
    return {ErrorCode::InvalidRecord, QStringLiteral("Invalid analysis cache evidence.")};
}
bool validStats(const SearchStatistics& s) {
    const auto nonnegative = [](const std::optional<int>& n) { return !n || *n >= 0; };
    return nonnegative(s.depth) && nonnegative(s.selectiveDepth) && nonnegative(s.hashFull) &&
           (!s.hashFull || *s.hashFull <= 1000);
}
bool validLimit(const SearchLimit& limit) {
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, NodeLimit>) {
                return value.nodes > 0 && value.nodes <= static_cast<std::uint64_t>(
                                                             std::numeric_limits<qint64>::max());
            } else if constexpr (std::is_same_v<T, DepthLimit>) {
                return value.depth > 0 &&
                       value.depth <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            } else if constexpr (std::is_same_v<T, TimeLimit>) {
                return value.milliseconds > 0 &&
                       value.milliseconds <=
                           static_cast<std::uint64_t>(std::numeric_limits<int>::max() - 5000);
            } else {
                return true;
            }
        },
        limit);
}
using MoveKey = std::tuple<int, int, std::optional<chess::PieceType>>;
MoveKey key(const chess::Move& m) { return {m.from.index(), m.to.index(), m.promotion}; }
std::set<MoveKey> roots(const std::vector<chess::Move>& moves) {
    std::set<MoveKey> values;
    for (const auto& move : moves) {
        values.insert(key(move));
    }
    return values;
}
bool validRoots(const chess::Position& position,
                const std::optional<std::vector<chess::Move>>& moves) {
    if (!moves) {
        return true;
    }
    if (moves->empty() || moves->size() > 256 || roots(*moves).size() != moves->size()) {
        return false;
    }
    return std::all_of(moves->begin(), moves->end(),
                       [&](const auto& m) { return position.isLegal(m); });
}
bool validText(const QString& text) {
    return text.size() <= 65536 && !text.contains(QChar(u'\0')) && text.isValidUtf16();
}
} // namespace
chess::Result<chess::Position> restorePosition(const chess::PositionContext& context) {
    const auto bad = [] {
        return chess::Error{chess::ErrorCode::InvalidPosition, "Invalid cached position context."};
    };
    if (context.version != 1 || context.variant != chess::Variant::Standard ||
        context.rootFen.size() > 256 || context.moves.size() > 13107) {
        return bad();
    }
    auto restored = context.provenance == chess::RootProvenance::InitialPosition
                        ? chess::Result<chess::Position>(chess::Position::initial())
                        : chess::Position::fromFen(context.rootFen);
    auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return bad();
    }
    for (const auto& move : context.moves) {
        if (std::holds_alternative<chess::Error>(position->play(move))) {
            return bad();
        }
    }
    if (position->context() != context) {
        return bad();
    }
    return restored;
}
bool verifiedResources(const EngineFingerprint& f) {
    if (f.schemaVersion != 2 || f.adapterPolicyVersion != 1 || f.resourcePolicyVersion != 1 ||
        f.resources != ResourceVerification::EmbeddedResourcesVerified ||
        f.executableSha256 !=
            QByteArray::fromHex(
                "0f83d24cc46d2c66c60f16001af5444873bc112b7d028594513426894c12da19") ||
        f.reportedName != QStringLiteral("Stockfish 19") ||
        f.reportedAuthor !=
            std::optional<QString>(QStringLiteral("the Stockfish developers (see AUTHORS file)"))) {
        return false;
    }
    // Closed list for this exact reviewed binary, not a generic interpretation of UCI strings.
    static const std::vector<std::pair<QString, OptionType>> expected{
        {QStringLiteral("debug log file"), OptionType::String},
        {QStringLiteral("evalfile"), OptionType::String},
        {QStringLiteral("hash"), OptionType::Spin},
        {QStringLiteral("move overhead"), OptionType::Spin},
        {QStringLiteral("multipv"), OptionType::Spin},
        {QStringLiteral("nodestime"), OptionType::Spin},
        {QStringLiteral("numapolicy"), OptionType::String},
        {QStringLiteral("ponder"), OptionType::Check},
        {QStringLiteral("skill level"), OptionType::Spin},
        {QStringLiteral("syzygy50moverule"), OptionType::Check},
        {QStringLiteral("syzygypath"), OptionType::String},
        {QStringLiteral("syzygyprobedepth"), OptionType::Spin},
        {QStringLiteral("syzygyprobelimit"), OptionType::Spin},
        {QStringLiteral("threads"), OptionType::Spin},
        {QStringLiteral("uci_chess960"), OptionType::Check},
        {QStringLiteral("uci_elo"), OptionType::Spin},
        {QStringLiteral("uci_limitstrength"), OptionType::Check},
        {QStringLiteral("uci_showwdl"), OptionType::Check}};
    if (f.options.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (f.options[i].name != expected[i].first || f.options[i].type != expected[i].second) {
            return false;
        }
    }
    const auto value = [&f](const char* name) -> QString {
        for (const auto& o : f.options) {
            if (o.name == QLatin1String(name)) {
                return o.value;
            }
        }
        return QStringLiteral("<missing>");
    };
    return value("evalfile") == QStringLiteral("nn-1a298aa575a0.nnue") &&
           value("syzygypath").isEmpty() && value("debug log file").isEmpty() &&
           value("threads") == QStringLiteral("1") && value("nodestime") == QStringLiteral("0") &&
           value("uci_chess960") == QStringLiteral("false") &&
           value("ponder") == QStringLiteral("false");
}
bool validFingerprint(const EngineFingerprint& f) {
    if ((f.schemaVersion != 1 && f.schemaVersion != 2) || f.adapterPolicyVersion != 1 ||
        f.executableSha256.size() != 32 || f.reportedName.isEmpty() || !validText(f.reportedName) ||
        (f.reportedAuthor && !validText(*f.reportedAuthor)) || f.options.size() > 4096) {
        return false;
    }
    qsizetype bytes = 0;
    QString previous;
    for (const auto& o : f.options) {
        if (o.name.isEmpty() || !validText(o.name) || !validText(o.value) ||
            o.name != o.name.toCaseFolded() || (!previous.isEmpty() && previous >= o.name) ||
            o.type == OptionType::Button ||
            static_cast<unsigned>(o.type) > static_cast<unsigned>(OptionType::String)) {
            return false;
        }
        if (o.type == OptionType::Check && o.value != QStringLiteral("true") &&
            o.value != QStringLiteral("false")) {
            return false;
        }
        if (o.type == OptionType::Spin) {
            bool ok = false;
            const auto number = o.value.toLongLong(&ok);
            if (!ok || QString::number(number) != o.value) {
                return false;
            }
        }
        bytes += o.name.size() + o.value.size();
        previous = o.name;
    }
    if (bytes > 1024 * 1024) {
        return false;
    }
    if (f.resources == ResourceVerification::ExternalResourcesUnverified) {
        if (f.resourcePolicyVersion != 0) {
            return false;
        }
    } else if (!verifiedResources(f)) {
        return false;
    }
    return f.digest.size() == 32 && f.digest == fingerprintDigest(f);
}
std::optional<Error> validateCacheQuery(const CacheQuery& q) {
    auto position = restorePosition(q.position);
    const auto* root = std::get_if<chess::Position>(&position);
    if (!root || !validFingerprint(q.fingerprint) || !validLimit(q.limit) || q.multiPv < 1 ||
        !validRoots(*root, q.searchMoves)) {
        return invalid();
    }
    return std::nullopt;
}
std::optional<Error> validateCacheResult(const AnalysisResult& result) {
    const auto& s = result.evidence;
    if (result.outcome != AnalysisOutcome::Completed || result.error || !s.searchStarted ||
        !s.provenance || s.session.isNull() || s.request.isNull() || s.requestedMultiPv < 1 ||
        s.effectiveMultiPv < 1 || s.effectiveMultiPv > 256 ||
        s.effectiveMultiPv > s.requestedMultiPv ||
        s.lines.size() > static_cast<std::size_t>(s.effectiveMultiPv) ||
        !validStats(s.statistics) || !s.provenance->createdAt.isValid() ||
        s.provenance->createdAt.offsetFromUtc() != 0) {
        return invalid();
    }
    const auto& p = *s.provenance;
    CacheQuery q{p.position,         p.fingerprint,  s.limit,
                 s.requestedMultiPv, s.wdlRequested, p.searchMoves};
    if (validateCacheQuery(q)) {
        return invalid();
    }
    bool hasMultiPv = false;
    bool hasWdl = false;
    for (const auto& option : p.fingerprint.options) {
        if (option.name == QStringLiteral("multipv") && option.type == OptionType::Spin) {
            hasMultiPv = true;
            // Fingerprints omit declaration bounds. An unverified engine may advertise
            // an unsupported max=1 MultiPV option whose default was never overridden.
            if (verifiedResources(p.fingerprint) &&
                option.value != QString::number(s.effectiveMultiPv)) {
                return invalid();
            }
        }
        if (option.name == QStringLiteral("uci_showwdl") && option.type == OptionType::Check) {
            hasWdl = true;
            if (option.value !=
                (s.wdlRequested ? QStringLiteral("true") : QStringLiteral("false"))) {
                return invalid();
            }
        }
    }
    if ((!hasMultiPv && s.effectiveMultiPv != 1) || hasWdl != s.wdlSupported) {
        return invalid();
    }
    auto position = std::get<chess::Position>(restorePosition(p.position));
    int width = 0;
    int previous = 0;
    std::optional<int> depth;
    bool coherent = true;
    std::set<MoveKey> seen;
    for (const auto& line : s.lines) {
        if (line.rank <= previous || line.rank > s.effectiveMultiPv || line.pv.size() > 1024 ||
            !validStats(line.statistics)) {
            return invalid();
        }
        previous = line.rank;
        if (line.score && (static_cast<unsigned>(line.score->type) > 1 ||
                           static_cast<unsigned>(line.score->bound) > 2)) {
            return invalid();
        }
        if (line.wdl &&
            (line.wdl->wins < 0 || line.wdl->draws < 0 || line.wdl->losses < 0 ||
             line.wdl->wins > 1000 || line.wdl->draws > 1000 || line.wdl->losses > 1000 ||
             line.wdl->wins + line.wdl->draws + line.wdl->losses != 1000)) {
            return invalid();
        }
        auto continuation = position;
        for (const auto& move : line.pv) {
            if (std::holds_alternative<chess::Error>(continuation.play(move))) {
                return invalid();
            }
        }
        if (!line.pv.empty() && p.searchMoves &&
            !roots(*p.searchMoves).contains(key(line.pv.front()))) {
            return invalid();
        }
        if (line.rank == width + 1 && line.score && !line.pv.empty() &&
            seen.insert(key(line.pv.front())).second) {
            ++width;
            if (width == 1) {
                depth = line.statistics.depth;
            }
            coherent = coherent && depth && line.statistics.depth == depth;
        }
    }
    if (s.achievedWidth != width ||
        s.commonDepth != (coherent && width == s.effectiveMultiPv ? depth : std::nullopt)) {
        return invalid();
    }
    if (s.bestMove.has_value() != !position.legalMoves().empty()) {
        return invalid();
    }
    if (s.bestMove && (!position.isLegal(*s.bestMove) ||
                       (p.searchMoves && !roots(*p.searchMoves).contains(key(*s.bestMove))))) {
        return invalid();
    }
    if (s.ponder) {
        if (!s.bestMove || std::holds_alternative<chess::Error>(position.play(*s.bestMove)) ||
            !position.isLegal(*s.ponder)) {
            return invalid();
        }
    }
    return std::nullopt;
}
std::uint64_t cacheStrength(const AnalysisResult& r) {
    if (std::holds_alternative<NodeLimit>(r.evidence.limit)) {
        return r.evidence.statistics.nodes.value_or(0);
    }
    return static_cast<std::uint64_t>(r.evidence.commonDepth.value_or(0));
}
bool cacheDominates(const AnalysisResult& r, const CacheQuery& q) {
    if (validateCacheQuery(q) || validateCacheResult(r) || !verifiedResources(q.fingerprint)) {
        return false;
    }
    const auto& s = r.evidence;
    const auto& p = *s.provenance;
    if (p.position != q.position || p.fingerprint != q.fingerprint ||
        p.searchMoves.has_value() != q.searchMoves.has_value() ||
        (p.searchMoves && roots(*p.searchMoves) != roots(*q.searchMoves)) ||
        s.requestedMultiPv < q.multiPv || s.achievedWidth < q.multiPv || !s.commonDepth ||
        *s.commonDepth < 1 || (q.wantWdl && (!s.wdlRequested || !s.wdlSupported))) {
        return false;
    }
    for (int i = 0; i < q.multiPv; ++i) {
        const auto& line = s.lines[static_cast<std::size_t>(i)];
        if (!line.score || line.score->bound != ScoreBound::Exact || (q.wantWdl && !line.wdl)) {
            return false;
        }
    }
    if (const auto* nodes = std::get_if<NodeLimit>(&q.limit)) {
        const auto* original = std::get_if<NodeLimit>(&s.limit);
        return original && original->nodes >= nodes->nodes && s.statistics.nodes &&
               *s.statistics.nodes >= nodes->nodes;
    }
    if (const auto* depth = std::get_if<DepthLimit>(&q.limit)) {
        const auto* original = std::get_if<DepthLimit>(&s.limit);
        return original && original->depth >= depth->depth &&
               static_cast<std::uint32_t>(*s.commonDepth) >= depth->depth;
    }
    return false;
}
} // namespace chess_review::engine
