#include "openings/OpeningIndex.h"
#include "pgn/Pgn.h"
#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace chess_review::openings {
namespace {
constexpr std::string_view separator = ": ";
constexpr std::string_view sourceHeader = "eco\tname\tpgn";
constexpr std::string_view lookupHeader = "eco\tname\tepd\tuci";

std::string_view takeLine(std::string_view& input) {
    const auto newline = input.find('\n');
    const auto line = input.substr(0, newline);
    input.remove_prefix(newline == std::string_view::npos ? input.size() : newline + 1);
    return line;
}

bool readColumns(std::string_view row, std::span<std::string_view> columns) {
    for (auto& column : columns.first(columns.size() - 1)) {
        const auto tab = row.find('\t');
        if (tab == std::string_view::npos) {
            return false;
        }
        column = row.substr(0, tab);
        row.remove_prefix(tab + 1);
    }
    columns.back() = row;
    return row.find('\t') == std::string_view::npos;
}

// Used at build time and when an ambiguous position requires an exact order comparison.
std::optional<std::string> replay(chess::Position& position, std::span<const chess::Move> moves) {
    std::string order;
    for (const auto& move : moves) {
        const auto uci = position.uci(move);
        const auto* text = std::get_if<std::string>(&uci);
        if (!text || std::holds_alternative<chess::Error>(position.play(move))) {
            return std::nullopt;
        }
        if (!order.empty()) {
            order.push_back(' ');
        }
        order += *text;
    }
    return order;
}

std::size_t referencePly(std::string_view order) {
    std::size_t ply = 0;
    for (const auto token : order | std::views::split(' ')) {
        const std::string_view uci(token.begin(), token.end());
        if ((uci.size() != 4 && uci.size() != 5) || !chess::Square::fromName(uci.substr(0, 2)) ||
            !chess::Square::fromName(uci.substr(2, 2)) ||
            (uci.size() == 5 && std::string_view("qrbn").find(uci[4]) == std::string_view::npos)) {
            return 0;
        }
        ++ply;
    }
    return ply;
}
} // namespace

std::string_view Opening::family() const {
    return std::string_view(name).substr(0, name.find(separator));
}

std::string_view Opening::variation() const {
    const auto colon = name.find(separator);
    return colon == std::string::npos ? std::string_view{}
                                      : std::string_view(name).substr(colon + separator.size());
}

bool validOpening(std::string_view eco, std::string_view name) {
    if (eco.size() != 3 || eco[0] < 'A' || eco[0] > 'E' || eco[1] < '0' || eco[1] > '9' ||
        eco[2] < '0' || eco[2] > '9' || name.empty() || name.size() > maxOpeningNameBytes ||
        name.front() == ' ' || name.back() == ' ' ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; })) {
        return false;
    }
    const auto colon = name.find(':');
    return colon == std::string_view::npos ||
           (colon > 0 && colon + separator.size() < name.size() &&
            name.substr(colon).starts_with(separator) &&
            name.find(':', colon + 1) == std::string_view::npos);
}

std::optional<std::string> compileReferences(std::span<const std::string_view> volumes) {
    if (volumes.empty()) {
        return std::nullopt;
    }
    std::string output(lookupHeader);
    output.push_back('\n');
    for (auto volume : volumes) {
        if (takeLine(volume) != sourceHeader || volume.empty()) {
            return std::nullopt;
        }
        while (!volume.empty()) {
            std::array<std::string_view, 3> columns;
            auto& [eco, name, line] = columns;
            if (!readColumns(takeLine(volume), columns) || !validOpening(eco, name) ||
                line.empty() || line.size() > 512) {
                return std::nullopt;
            }
            const auto input = std::string(line) + " *";
            pgn::PgnReader reader(input);
            auto outcome = reader.next();
            const auto* document = std::get_if<pgn::PgnDocument>(&outcome);
            if (!document || !document->tags.empty() || !document->warnings.empty() ||
                document->originalMainline().empty() ||
                document->tree.size() != document->originalMainline().size() + 1 ||
                !std::holds_alternative<pgn::EndOfInput>(reader.next())) {
                return std::nullopt;
            }
            auto position = chess::Position::initial();
            const auto order = replay(position, document->originalMainline());
            if (!order) {
                return std::nullopt;
            }
            output.append(eco).append("\t").append(name).append("\t").append(position.epd());
            output.append("\t").append(*order).append("\n");
        }
    }
    return output;
}

std::optional<OpeningIndex> OpeningIndex::build(std::string_view references) {
    if (takeLine(references) != lookupHeader || references.empty()) {
        return std::nullopt;
    }
    OpeningIndex index;
    while (!references.empty()) {
        std::array<std::string_view, 4> columns;
        auto& [eco, name, epd, order] = columns;
        if (!readColumns(takeLine(references), columns) || !validOpening(eco, name) ||
            epd.size() > 96 || order.size() > 512 || order.empty() || order.back() == ' ') {
            return std::nullopt;
        }
        const auto ply = referencePly(order);
        const auto position =
            chess::Position::fromFen(std::string(epd) + " 0 " + std::to_string(ply / 2 + 1));
        const auto* board = std::get_if<chess::Position>(&position);
        if (ply == 0 || !board || board->epd() != epd) {
            return std::nullopt;
        }
        index.maxPly_ = std::max(index.maxPly_, ply);
        index.positions_[std::string(epd)].push_back(
            {{std::string(eco), std::string(name)}, std::string(order)});
    }
    return index;
}

std::optional<Opening> OpeningIndex::recognize(chess::Position position,
                                               std::span<const chess::Move> moves) const {
    static const auto initialFen = chess::Position::initial().fen();
    if (position.fen() != initialFen) {
        return std::nullopt;
    }
    const Opening* deepest = nullptr;
    std::vector<const std::vector<Reference>*> seen;
    for (std::size_t ply = 0; ply < std::min(maxPly_, moves.size()); ++ply) {
        if (std::holds_alternative<chess::Error>(position.play(moves[ply]))) {
            return std::nullopt;
        }
        const auto found = positions_.find(position.epd());
        if (found == positions_.end() || std::ranges::find(seen, &found->second) != seen.end()) {
            continue;
        }
        const auto& references = found->second;
        seen.push_back(&references);
        const auto& first = references.front().opening;
        if (std::ranges::all_of(references,
                                [&first](const auto& ref) { return ref.opening == first; })) {
            deepest = &first;
            continue;
        }
        auto initial = chess::Position::initial();
        const auto order = replay(initial, moves.first(ply + 1));
        if (!order) {
            return std::nullopt;
        }
        const Opening* exact = nullptr;
        for (const auto& ref : references) {
            if (ref.moveOrder != *order) {
                continue;
            }
            if (exact && *exact != ref.opening) {
                exact = nullptr;
                break;
            }
            exact = &ref.opening;
        }
        if (exact) {
            deepest = exact;
        }
    }
    return deepest ? std::optional{*deepest} : std::nullopt;
}
} // namespace chess_review::openings
