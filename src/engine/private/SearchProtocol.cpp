#include "engine/private/SearchProtocol.h"
#include <QMap>
#include <QStringView>
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>

namespace chess_review::engine {
namespace {
Error invalidRequest() {
    return {ErrorCode::InvalidRequest,
            QStringLiteral("Choose a valid search limit, position, and legal root moves.")};
}
Error invalidEvidence() {
    return {ErrorCode::Protocol,
            QStringLiteral(
                "The engine returned invalid search evidence. Reconnect a compatible engine.")};
}
bool allowed(const AnalysisRequest& request, const chess::Move& move) {
    return !request.searchMoves ||
           std::find(request.searchMoves->begin(), request.searchMoves->end(), move) !=
               request.searchMoves->end();
}
const UciOption* option(const Capabilities& capabilities, const QString& name) {
    const auto found = std::find_if(capabilities.options.begin(), capabilities.options.end(),
                                    [&name](const UciOption& value) {
                                        return value.name.compare(name, Qt::CaseInsensitive) == 0;
                                    });
    return found == capabilities.options.end() ? nullptr : &*found;
}
bool infoField(QStringView token) {
    static constexpr std::array fields{
        u"depth",    u"seldepth",       u"multipv", u"nodes",  u"time",       u"nps",
        u"tbhits",   u"hashfull",       u"score",   u"wdl",    u"pv",         u"string",
        u"currmove", u"currmovenumber", u"cpuload", u"sbhits", u"refutation", u"currline"};
    return std::any_of(fields.begin(), fields.end(),
                       [token](const auto* field) { return token == field; });
}
bool looksLikeMove(QStringView token) {
    // Unknown extension keys terminate a PV. Move-shaped tokens still undergo full
    // adapter validation, so an illegal continuation cannot become accepted evidence.
    return token.size() >= 2 && token[0] >= u'a' && token[0] <= u'h' && token[1] >= u'0' &&
           token[1] <= u'9';
}
bool unsignedNumber(const QString& text, std::uint64_t& value) {
    if (text.isEmpty() ||
        std::any_of(text.begin(), text.end(), [](QChar c) { return c < u'0' || c > u'9'; })) {
        return false;
    }
    bool ok = false;
    value = text.toULongLong(&ok);
    return ok;
}
bool integer(const QString& text, int& value, int maximum = std::numeric_limits<int>::max()) {
    std::uint64_t number = 0;
    if (!unsignedNumber(text, number) || number > static_cast<std::uint64_t>(maximum)) {
        return false;
    }
    value = static_cast<int>(number);
    return true;
}
struct SearchSetup {
    PreparedSearch commands;
    chess::PositionContext context;
};
std::variant<SearchSetup, Error> prepareSearchSetup(const AnalysisRequest& request,
                                                    const Capabilities& capabilities) {
    if (request.id.isNull() || request.multiPv < 1) {
        return invalidRequest();
    }
    PreparedSearch prepared;
    bool valid = true;
    prepared.go = std::visit(
        [&valid](const auto& limit) -> QByteArray {
            using T = std::decay_t<decltype(limit)>;
            if constexpr (std::is_same_v<T, InfiniteLimit>) {
                return "go infinite";
            } else if constexpr (std::is_same_v<T, NodeLimit>) {
                valid = limit.nodes > 0 && limit.nodes <= static_cast<std::uint64_t>(
                                                              std::numeric_limits<qint64>::max());
                return "go nodes " + QByteArray::number(static_cast<qulonglong>(limit.nodes));
            } else if constexpr (std::is_same_v<T, DepthLimit>) {
                valid = limit.depth > 0 &&
                        limit.depth <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
                return "go depth " + QByteArray::number(limit.depth);
            } else {
                valid = limit.milliseconds > 0 &&
                        limit.milliseconds <=
                            static_cast<std::uint64_t>(std::numeric_limits<int>::max() - 5000);
                return "go movetime " +
                       QByteArray::number(static_cast<qulonglong>(limit.milliseconds));
            }
        },
        request.limit);
    if (!valid) {
        return invalidRequest();
    }

    QMap<QString, EffectiveOption> effective;
    for (const auto& declared : capabilities.options) {
        if (declared.type == OptionType::Button) {
            continue;
        }
        const auto name = declared.name.toCaseFolded();
        const auto value = declared.type == OptionType::Spin
                               ? QString::number(declared.defaultValue.toLongLong())
                               : declared.defaultValue;
        effective.insert(name, {name, declared.type, value});
    }

    const auto legal = request.position.legalMoves();
    auto count = legal.size();
    if (request.searchMoves) {
        if (request.searchMoves->empty() || request.searchMoves->size() > legal.size()) {
            return invalidRequest();
        }
        std::vector<chess::Move> seen;
        for (const auto& move : *request.searchMoves) {
            if (!request.position.isLegal(move) ||
                std::find(seen.begin(), seen.end(), move) != seen.end()) {
                return invalidRequest();
            }
            seen.push_back(move);
        }
        count = seen.size();
    }
    const auto* multiPv = option(capabilities, QStringLiteral("MultiPV"));
    const auto maximum =
        multiPv && capabilities.supportsMultiPv ? std::min<qint64>(*multiPv->maximum, 256) : 1;
    // An option whose minimum exceeds the safe/effective width cannot represent this request.
    prepared.effectiveMultiPv = static_cast<int>(std::min<qint64>(
        {request.multiPv, maximum, static_cast<qint64>(std::max<std::size_t>(count, 1))}));
    const auto set = [&prepared, &effective](const UciOption& value, const QByteArray& setting) {
        effective[value.name.toCaseFolded()].value = QString::fromUtf8(setting);
        prepared.options += "setoption name " + value.name.toUtf8() + " value " + setting + '\n';
    };
    if (multiPv && capabilities.supportsMultiPv) {
        if (prepared.effectiveMultiPv < *multiPv->minimum) {
            return invalidRequest();
        }
        set(*multiPv, QByteArray::number(prepared.effectiveMultiPv));
    }
    if (const auto* threads = option(capabilities, QStringLiteral("Threads"));
        threads && capabilities.supportsThreads) {
        if (*threads->minimum > 1 || *threads->maximum < 1) {
            return invalidRequest();
        }
        set(*threads, "1");
    }
    const auto* wdl = option(capabilities, QStringLiteral("UCI_ShowWDL"));
    prepared.wdlSupported = wdl && capabilities.supportsWdl;
    if (prepared.wdlSupported) {
        set(*wdl, request.wantWdl ? "true" : "false");
    }
    // Position is an independently owned, validated adapter value. Its history
    // contains only legal standard-chess moves (4 UCI bytes, or 5 for promotion).
    // Admission checks exact command size without replaying the entire history.
    auto context = request.position.context();
    prepared.position = context.provenance == chess::RootProvenance::InitialPosition
                            ? QByteArray("position startpos")
                            : "position fen " + QByteArray::fromStdString(context.rootFen);
    if (!context.moves.empty()) {
        prepared.position += " moves";
    }
    if (prepared.position.size() >= 65536) {
        return invalidRequest();
    }
    auto remaining = static_cast<std::size_t>(65535 - prepared.position.size());
    if (context.moves.size() > remaining / 5) {
        return invalidRequest();
    }
    for (const auto& move : context.moves) {
        const std::size_t bytes = move.promotion ? 6 : 5; // Space plus UCI move.
        if (bytes > remaining) {
            return invalidRequest();
        }
        remaining -= bytes;
    }
    if (request.searchMoves) {
        prepared.go += " searchmoves";
        for (const auto& move : *request.searchMoves) {
            prepared.go +=
                ' ' + QByteArray::fromStdString(std::get<std::string>(request.position.uci(move)));
        }
    }
    prepared.go += '\n';
    if (prepared.position.size() > 65536 || prepared.go.size() > 65536 ||
        prepared.options.size() > 65536) {
        return invalidRequest();
    }
    for (const auto& value : effective) {
        prepared.effectiveOptions.push_back(value);
    }
    return SearchSetup{std::move(prepared), std::move(context)};
}
} // namespace

std::optional<Error> validateSearchRequest(const AnalysisRequest& request,
                                           const Capabilities& capabilities) {
    const auto setup = prepareSearchSetup(request, capabilities);
    if (const auto* error = std::get_if<Error>(&setup)) {
        return *error;
    }
    return std::nullopt;
}

std::variant<PreparedSearch, Error> prepareSearch(const AnalysisRequest& request,
                                                  const Capabilities& capabilities) {
    auto setup = prepareSearchSetup(request, capabilities);
    if (const auto* error = std::get_if<Error>(&setup)) {
        return *error;
    }
    auto prepared = std::get<SearchSetup>(std::move(setup));
    auto root = chess::Position::fromFen(prepared.context.rootFen);
    if (!std::holds_alternative<chess::Position>(root)) {
        return invalidRequest();
    }
    auto replay = std::get<chess::Position>(std::move(root));
    for (const auto& move : prepared.context.moves) {
        const auto uci = replay.uci(move);
        if (!std::holds_alternative<std::string>(uci)) {
            return invalidRequest();
        }
        prepared.commands.position += ' ' + QByteArray::fromStdString(std::get<std::string>(uci));
        if (prepared.commands.position.size() >= 65536 ||
            std::holds_alternative<chess::Error>(replay.play(move))) {
            return invalidRequest();
        }
    }
    prepared.commands.position += '\n';
    prepared.commands.context = std::move(prepared.context);
    return std::move(prepared.commands);
}

std::variant<InfoUpdate, Error> parseSearchInfo(const QStringList& tokens,
                                                const AnalysisRequest& request, int width) {
    if (tokens.isEmpty() || tokens.front() != QStringLiteral("info") || width < 1) {
        return invalidEvidence();
    }
    InfoUpdate update;
    std::set<QString> seen;
    // Copy the history only if moves need replaying, then reuse it for this record.
    std::optional<chess::Position> replay;
    for (qsizetype i = 1; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token == QStringLiteral("string")) {
            break;
        }
        const bool numeric =
            token == QStringLiteral("depth") || token == QStringLiteral("seldepth") ||
            token == QStringLiteral("multipv") || token == QStringLiteral("nodes") ||
            token == QStringLiteral("time") || token == QStringLiteral("nps") ||
            token == QStringLiteral("tbhits") || token == QStringLiteral("hashfull") ||
            token == QStringLiteral("currmovenumber") || token == QStringLiteral("cpuload") ||
            token == QStringLiteral("sbhits");
        if (numeric || token == QStringLiteral("score") || token == QStringLiteral("wdl") ||
            token == QStringLiteral("pv") || token == QStringLiteral("currmove")) {
            if (!seen.insert(token).second || i + 1 >= tokens.size()) {
                return invalidEvidence();
            }
        }
        if (numeric) {
            std::uint64_t value = 0;
            if (!unsignedNumber(tokens[++i], value)) {
                return invalidEvidence();
            }
            if (token == QStringLiteral("nodes")) {
                update.line.statistics.nodes = value;
            } else if (token == QStringLiteral("time")) {
                update.line.statistics.timeMs = value;
            } else if (token == QStringLiteral("nps")) {
                update.line.statistics.nps = value;
            } else if (token == QStringLiteral("tbhits")) {
                update.line.statistics.tablebaseHits = value;
            } else if (token == QStringLiteral("sbhits")) {
                // Validated auxiliary statistics never become analysis evidence.
            } else {
                const auto maximum =
                    token == QStringLiteral("multipv") ? width
                    : (token == QStringLiteral("hashfull") || token == QStringLiteral("cpuload"))
                        ? 1000
                        : std::numeric_limits<int>::max();
                if (value > static_cast<std::uint64_t>(maximum)) {
                    return invalidEvidence();
                }
                const auto number = static_cast<int>(value);
                if (token == QStringLiteral("depth")) {
                    update.line.statistics.depth = number;
                } else if (token == QStringLiteral("seldepth")) {
                    update.line.statistics.selectiveDepth = number;
                } else if (token == QStringLiteral("hashfull")) {
                    update.line.statistics.hashFull = number;
                } else if (token == QStringLiteral("multipv") ||
                           token == QStringLiteral("currmovenumber")) {
                    if (number == 0) {
                        return invalidEvidence();
                    }
                    if (token == QStringLiteral("multipv")) {
                        update.line.rank = number;
                    }
                }
            }
        } else if (token == QStringLiteral("currmove")) {
            const auto move = request.position.parseUci(tokens[++i].toStdString());
            if (!std::holds_alternative<chess::Move>(move) ||
                !allowed(request, std::get<chess::Move>(move))) {
                return invalidEvidence();
            }
        } else if (token == QStringLiteral("score")) {
            if (i + 2 >= tokens.size()) {
                return invalidEvidence();
            }
            const auto& kind = tokens[++i];
            const auto& value = tokens[++i];
            const auto digits =
                value.startsWith(u'-') || value.startsWith(u'+') ? value.mid(1) : value;
            std::uint64_t magnitude = 0;
            bool ok = false;
            const auto signedValue = value.toLongLong(&ok);
            if ((kind != QStringLiteral("cp") && kind != QStringLiteral("mate")) ||
                !unsignedNumber(digits, magnitude) || !ok) {
                return invalidEvidence();
            }
            AnalysisScore score{kind == QStringLiteral("cp") ? ScoreType::Centipawns
                                                             : ScoreType::Mate,
                                signedValue};
            if (i + 1 < tokens.size() && (tokens[i + 1] == QStringLiteral("lowerbound") ||
                                          tokens[i + 1] == QStringLiteral("upperbound"))) {
                score.bound = tokens[++i] == QStringLiteral("lowerbound") ? ScoreBound::Lower
                                                                          : ScoreBound::Upper;
            }
            update.line.score = score;
            update.hasEvidence = true;
        } else if (token == QStringLiteral("lowerbound") || token == QStringLiteral("upperbound")) {
            return invalidEvidence();
        } else if (token == QStringLiteral("wdl")) {
            Wdl wdl{};
            if (i + 3 >= tokens.size() || !integer(tokens[i + 1], wdl.wins, 1000) ||
                !integer(tokens[i + 2], wdl.draws, 1000) ||
                !integer(tokens[i + 3], wdl.losses, 1000) ||
                wdl.wins + wdl.draws + wdl.losses != 1000) {
                return invalidEvidence();
            }
            i += 3;
            update.line.wdl = wdl;
            update.hasEvidence = true;
        } else if (token == QStringLiteral("pv") || token == QStringLiteral("refutation") ||
                   token == QStringLiteral("currline")) {
            const bool evidence = token == QStringLiteral("pv");
            const bool currentLine = token == QStringLiteral("currline");
            if (currentLine && i + 1 < tokens.size()) {
                const auto& next = tokens[i + 1];
                if (!next.isEmpty() && ((next.front() >= u'0' && next.front() <= u'9') ||
                                        next.front() == u'-' || next.front() == u'+')) {
                    int cpu = 0;
                    if (!integer(next, cpu) || cpu == 0) {
                        return invalidEvidence();
                    }
                    ++i;
                }
            }
            std::size_t plies = 0;
            while (++i < tokens.size()) {
                if (infoField(tokens[i]) || !looksLikeMove(tokens[i])) {
                    --i; // Resume parsing fields after the variable-length continuation.
                    break;
                }
                if (plies == 1024) {
                    return invalidEvidence();
                }
                if (!replay) {
                    replay.emplace(request.position);
                }
                const auto move = replay->parseUci(tokens[i].toStdString());
                if (!std::holds_alternative<chess::Move>(move)) {
                    return invalidEvidence();
                }
                const auto parsed = std::get<chess::Move>(move);
                if ((evidence && plies == 0 && !allowed(request, parsed)) ||
                    std::holds_alternative<chess::Error>(replay->play(parsed))) {
                    return invalidEvidence();
                }
                ++plies;
                if (evidence) {
                    update.line.pv.push_back(parsed);
                }
            }
            // Each auxiliary line and PV starts at the request root, not the prior tail.
            for (std::size_t undone = 0; undone < plies; ++undone) {
                if (!replay->undo()) {
                    return invalidEvidence();
                }
            }
            if (!currentLine && plies == 0) {
                return invalidEvidence();
            }
            if (evidence) {
                update.hasEvidence = true;
            }
        }
    }
    return update;
}

std::variant<BestMove, Error> parseBestMove(const QStringList& tokens,
                                            const AnalysisRequest& request) {
    if (tokens.isEmpty() || tokens.front() != QStringLiteral("bestmove") ||
        (tokens.size() != 2 && (tokens.size() != 4 || tokens[2] != QStringLiteral("ponder")))) {
        return invalidEvidence();
    }
    BestMove result;
    if (tokens[1] == QStringLiteral("0000") || tokens[1] == QStringLiteral("(none)")) {
        if (!request.position.legalMoves().empty() || tokens.size() != 2) {
            return invalidEvidence();
        }
        return result;
    }
    const auto move = request.position.parseUci(tokens[1].toStdString());
    if (!std::holds_alternative<chess::Move>(move) ||
        !allowed(request, std::get<chess::Move>(move))) {
        return invalidEvidence();
    }
    result.move = std::get<chess::Move>(move);
    if (tokens.size() == 4) {
        auto position = request.position;
        if (std::holds_alternative<chess::Error>(position.play(*result.move))) {
            return invalidEvidence();
        }
        const auto ponder = position.parseUci(tokens[3].toStdString());
        if (!std::holds_alternative<chess::Move>(ponder)) {
            return invalidEvidence();
        }
        result.ponder = std::get<chess::Move>(ponder);
    }
    return result;
}
void mergeStatistics(SearchStatistics& target, const SearchStatistics& update) {
    if (update.depth) {
        target.depth = update.depth;
    }
    if (update.selectiveDepth) {
        target.selectiveDepth = update.selectiveDepth;
    }
    if (update.nodes) {
        target.nodes = update.nodes;
    }
    if (update.timeMs) {
        target.timeMs = update.timeMs;
    }
    if (update.nps) {
        target.nps = update.nps;
    }
    if (update.tablebaseHits) {
        target.tablebaseHits = update.tablebaseHits;
    }
    if (update.hashFull) {
        target.hashFull = update.hashFull;
    }
}
void updateEvidence(AnalysisSnapshot& snapshot, InfoUpdate update) {
    mergeStatistics(snapshot.statistics, update.line.statistics);
    if (!update.hasEvidence) {
        return;
    }
    auto found =
        std::lower_bound(snapshot.lines.begin(), snapshot.lines.end(), update.line.rank,
                         [](const AnalysisLine& line, int rank) { return line.rank < rank; });
    if (found == snapshot.lines.end() || found->rank != update.line.rank) {
        snapshot.lines.insert(found, std::move(update.line));
    } else {
        *found = std::move(update.line);
    }
    snapshot.achievedWidth = 0;
    snapshot.commonDepth.reset();
    std::optional<int> depth;
    bool coherent = true;
    std::set<std::tuple<int, int, std::optional<chess::PieceType>>> roots;
    for (const auto& line : snapshot.lines) {
        if (line.rank != snapshot.achievedWidth + 1 || !line.score || line.pv.empty()) {
            break;
        }
        const auto& root = line.pv.front();
        if (!roots.emplace(root.from.index(), root.to.index(), root.promotion).second) {
            break; // Rank changes can temporarily overlap; they do not supply extra candidates.
        }
        ++snapshot.achievedWidth;
        if (snapshot.achievedWidth == 1) {
            depth = line.statistics.depth;
        }
        coherent = coherent && depth.has_value() && line.statistics.depth == depth;
    }
    if (coherent && snapshot.achievedWidth == snapshot.effectiveMultiPv) {
        snapshot.commonDepth = depth;
    }
}
} // namespace chess_review::engine
