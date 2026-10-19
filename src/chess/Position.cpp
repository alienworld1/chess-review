#include "chess/Position.h"

#include <chess.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <utility>

namespace chess_review::chess {
namespace {
// The private translation relies on the pinned upstream order, never on its public ABI.
static_assert(static_cast<int>(::chess::PieceType::PAWN) == static_cast<int>(PieceType::Pawn));
static_assert(static_cast<int>(::chess::PieceType::KNIGHT) == static_cast<int>(PieceType::Knight));
static_assert(static_cast<int>(::chess::PieceType::BISHOP) == static_cast<int>(PieceType::Bishop));
static_assert(static_cast<int>(::chess::PieceType::ROOK) == static_cast<int>(PieceType::Rook));
static_assert(static_cast<int>(::chess::PieceType::QUEEN) == static_cast<int>(PieceType::Queen));
static_assert(static_cast<int>(::chess::PieceType::KING) == static_cast<int>(PieceType::King));

constexpr std::string_view kInitialFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::optional<Piece> decodePiece(char symbol) {
    constexpr std::string_view symbols = "PNBRQKpnbrqk";
    const auto index = symbols.find(symbol);
    if (index == std::string_view::npos) {
        return std::nullopt;
    }
    return Piece{index < 6 ? Color::White : Color::Black, static_cast<PieceType>(index % 6)};
}

bool parseCounter(std::string_view text, std::uint32_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

struct FenData {
    std::array<std::optional<Piece>, 64> pieces{};
    std::array<std::string_view, 6> fields{};
    std::uint32_t halfmove{};
    std::uint32_t fullmove{};
    std::optional<Square> ep;
    std::string libraryFen;
};

Result<FenData> validateFen(std::string_view fen) {
    const auto syntax = Error{ErrorCode::InvalidFen,
                              "Expected a standard six-field FEN with valid bounded fields."};
    const auto invalid =
        Error{ErrorCode::InvalidPosition, "FEN describes an inconsistent standard chess position."};
    if (fen.size() > 256 || fen.find('\0') != std::string_view::npos) {
        return syntax;
    }
    FenData data;
    std::size_t field = 0;
    while (!fen.empty()) {
        if (isSpace(fen.front())) {
            fen.remove_prefix(1);
            continue;
        }
        if (field == data.fields.size()) {
            return syntax;
        }
        const auto end = std::find_if(fen.begin(), fen.end(), isSpace);
        const auto size = static_cast<std::size_t>(end - fen.begin());
        data.fields[field++] = fen.substr(0, size);
        fen.remove_prefix(size);
    }
    if (field != 6 || (data.fields[1] != "w" && data.fields[1] != "b") ||
        !parseCounter(data.fields[4], data.halfmove) ||
        !parseCounter(data.fields[5], data.fullmove) || data.fullmove == 0) {
        return syntax;
    }

    std::array<std::array<int, 6>, 2> counts{};
    int rank = 7;
    int file = 0;
    bool previousDigit = false;
    for (const char c : data.fields[0]) {
        if (c == '/') {
            if (file != 8 || rank == 0) {
                return syntax;
            }
            --rank;
            file = 0;
            previousDigit = false;
        } else if (c >= '1' && c <= '8') {
            if (previousDigit) {
                return syntax;
            }
            file += c - '0';
            previousDigit = true;
        } else {
            const auto piece = decodePiece(c);
            if (!piece || file >= 8) {
                return syntax;
            }
            data.pieces[static_cast<std::size_t>(rank * 8 + file)] = piece;
            ++counts[static_cast<std::size_t>(piece->color)][static_cast<std::size_t>(piece->type)];
            if (piece->type == PieceType::Pawn && (rank == 0 || rank == 7)) {
                return invalid;
            }
            ++file;
            previousDigit = false;
        }
        if (file > 8) {
            return syntax;
        }
    }
    if (rank != 0 || file != 8) {
        return syntax;
    }
    for (std::size_t color = 0; color < counts.size(); ++color) {
        const auto& count = counts[color];
        const int total = count[0] + count[1] + count[2] + count[3] + count[4] + count[5];
        const int promotions = std::max(0, count[1] - 2) + std::max(0, count[2] - 2) +
                               std::max(0, count[3] - 2) + std::max(0, count[4] - 1);
        if (count[5] != 1 || count[0] > 8 || total > 16 || promotions > 8 - count[0]) {
            return invalid;
        }
        // Two bishops on the same square color also require a promotion.
        for (int parity = 0; parity < 2; ++parity) {
            int bishops = 0;
            for (std::size_t i = 0; i < data.pieces.size(); ++i) {
                if (data.pieces[i] && data.pieces[i]->type == PieceType::Bishop &&
                    static_cast<std::size_t>(data.pieces[i]->color) == color &&
                    static_cast<int>((i / 8 + i % 8) % 2) == parity) {
                    ++bishops;
                }
            }
            const int bishopPromotions = std::max(0, bishops - 1);
            const int otherPromotions = promotions - std::max(0, count[2] - 2);
            if (otherPromotions + bishopPromotions > 8 - count[0]) {
                return invalid;
            }
        }
    }

    constexpr std::string_view rights = "KQkq";
    std::array<bool, 4> seen{};
    if (data.fields[2] != "-") {
        for (const char c : data.fields[2]) {
            const auto index = rights.find(c);
            if (index == std::string_view::npos || seen[index]) {
                return syntax;
            }
            seen[index] = true;
            const auto color = index < 2 ? Color::White : Color::Black;
            const std::size_t base = color == Color::White ? 0 : 56;
            if (data.pieces[base + 4] != Piece{color, PieceType::King} ||
                data.pieces[base + (index % 2 == 0 ? 7 : 0)] != Piece{color, PieceType::Rook}) {
                return invalid;
            }
        }
    }
    if (data.fields[3] != "-") {
        data.ep = Square::fromName(data.fields[3]);
        if (!data.ep) {
            return syntax;
        }
        const bool whiteToMove = data.fields[1] == "w";
        const int pawn = data.ep->index() + (whiteToMove ? -8 : 8);
        const int origin = data.ep->index() + (whiteToMove ? 8 : -8);
        if (data.ep->rank() != (whiteToMove ? 5 : 2) || data.halfmove != 0 ||
            (whiteToMove && data.fullmove == 1)) {
            return invalid;
        }
        if (data.pieces[static_cast<std::size_t>(data.ep->index())] ||
            data.pieces[static_cast<std::size_t>(origin)] ||
            data.pieces[static_cast<std::size_t>(pawn)] !=
                Piece{whiteToMove ? Color::Black : Color::White, PieceType::Pawn}) {
            return invalid;
        }
    }
    // Never feed unchecked placement, castling rights, or large counters to upstream.
    for (std::size_t i = 0; i < 3; ++i) {
        data.libraryFen += data.fields[i];
        data.libraryFen += ' ';
    }
    // Defer en passant: upstream normalizes it using move generation, whose check
    // assumptions must first be validated on the bare board.
    data.libraryFen += "- 0 1";
    return data;
}

// Use upstream attack primitives to establish its legal-position preconditions before
// invoking any move generation (including the EP normalization inside setFen).
bool hasPossibleCheckers(const ::chess::Board& board) {
    const auto side = board.sideToMove();
    const auto king = board.kingSq(side);
    const auto checkers = ::chess::attacks::attackers(board, ~side, king);
    const auto diagonal = ::chess::attacks::bishop(king, board.occ()) & checkers &
                          board.pieces(::chess::PieceType::BISHOP, ::chess::PieceType::QUEEN);
    const auto orthogonal = ::chess::attacks::rook(king, board.occ()) & checkers &
                            board.pieces(::chess::PieceType::ROOK, ::chess::PieceType::QUEEN);
    return checkers.count() <= 2 && diagonal.count() <= 1 && orthogonal.count() <= 1 &&
           (checkers & board.pieces(::chess::PieceType::PAWN)).count() <= 1 &&
           (checkers & board.pieces(::chess::PieceType::KNIGHT)).count() <= 1;
}

bool hasPossibleEpPredecessor(const ::chess::Board& board, Square ep) {
    const auto side = board.sideToMove();
    const auto opponent = ~side;
    const auto king = board.kingSq(side);
    const int direction = side == ::chess::Color::WHITE ? 8 : -8;
    const auto pawn = ::chess::Square(ep.index() - direction);
    const auto origin = ::chess::Square(ep.index() + direction);
    auto occupied = board.occ();
    occupied.clear(pawn.index());
    occupied.set(origin.index());
    auto pawns = board.pieces(::chess::PieceType::PAWN, opponent);
    pawns.clear(pawn.index());
    pawns.set(origin.index());
    // The nonmoving king cannot already have been attacked before the stated
    // double push. Only pawn and sliding attacks change when that pawn is restored.
    const auto unchanged = ::chess::attacks::attackers(board, opponent, king) &
                           board.pieces(::chess::PieceType::KNIGHT, ::chess::PieceType::KING);
    const auto pawnChecks = ::chess::attacks::pawn(side, king) & pawns;
    const auto diagonal = ::chess::attacks::bishop(king, occupied) & board.us(opponent) &
                          board.pieces(::chess::PieceType::BISHOP, ::chess::PieceType::QUEEN);
    const auto orthogonal = ::chess::attacks::rook(king, occupied) & board.us(opponent) &
                            board.pieces(::chess::PieceType::ROOK, ::chess::PieceType::QUEEN);
    return !(unchanged | pawnChecks | diagonal | orthogonal);
}

// Upstream enum-range false positives: only validated boards and generated moves reach these calls.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
Move toMove(const ::chess::Move& move) {
    const auto from = *Square::fromCoordinates(move.from().index() % 8, move.from().index() / 8);
    auto to = *Square::fromCoordinates(move.to().index() % 8, move.to().index() / 8);
    if (move.typeOf() == ::chess::Move::CASTLING) {
        to = *Square::fromCoordinates(move.to() > move.from() ? 6 : 2, from.rank());
    }
    std::optional<PieceType> promotion;
    if (move.typeOf() == ::chess::Move::PROMOTION) {
        // The move comes from legal generation; promotion bits represent only N/B/R/Q.
        promotion = static_cast<PieceType>(static_cast<int>(move.promotionType()));
    }
    return {from, to, promotion};
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

std::optional<::chess::Move> findMove(const ::chess::Board& board, const Move& move) {
    ::chess::Movelist moves;
    ::chess::movegen::legalmoves(moves, board);
    for (const auto candidate : moves) {
        if (toMove(candidate) == move) {
            return candidate;
        }
    }
    return std::nullopt;
}

Error illegalMove() { return {ErrorCode::IllegalMove, "Move is not legal in this position."}; }
} // namespace

struct Position::Impl {
    struct UndoState {
        Move move;
        ::chess::Move libraryMove;
        std::uint32_t halfmove;
        std::uint32_t fullmove;
        std::optional<Square> ep;
    };
    ::chess::Board board;
    std::uint32_t halfmove{};
    std::uint32_t fullmove{1};
    std::optional<Square> ep;
    RootProvenance provenance{RootProvenance::InitialPosition};
    std::string rootFen{kInitialFen};
    std::vector<UndoState> history;
};

std::optional<Square> Square::fromName(std::string_view name) {
    if (name.size() != 2 || name[0] < 'a' || name[0] > 'h' || name[1] < '1' || name[1] > '8') {
        return std::nullopt;
    }
    return fromCoordinates(name[0] - 'a', name[1] - '1');
}

std::optional<Square> Square::fromCoordinates(int file, int rank) {
    if (file < 0 || file > 7 || rank < 0 || rank > 7) {
        return std::nullopt;
    }
    return Square(rank * 8 + file);
}

std::string Square::name() const {
    return {static_cast<char>('a' + file()), static_cast<char>('1' + rank())};
}

Position::Position(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Position::~Position() = default;
Position::Position(const Position& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Position& Position::operator=(const Position& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}
Position::Position(Position&&) noexcept = default;
Position& Position::operator=(Position&&) noexcept = default;

Position Position::initial() { return Position(std::make_unique<Impl>()); }

Result<Position> Position::fromFen(std::string_view fen) {
    const auto validated = validateFen(fen);
    if (const auto* error = std::get_if<Error>(&validated)) {
        return *error;
    }
    const auto& data = std::get<FenData>(validated);
    auto impl = std::make_unique<Impl>();
    if (!impl->board.setFen(data.libraryFen)) {
        return Error{ErrorCode::InvalidFen, "Chess backend rejected the validated FEN."};
    }
    const auto side = impl->board.sideToMove();
    if (impl->board.isAttacked(impl->board.kingSq(~side), side)) {
        return Error{ErrorCode::InvalidPosition, "The side which just moved is in check."};
    }
    if (!hasPossibleCheckers(impl->board)) {
        return Error{ErrorCode::InvalidPosition,
                     "The checking pieces cannot result from a legal move."};
    }
    if (data.ep) {
        if (!hasPossibleEpPredecessor(impl->board, *data.ep)) {
            return Error{ErrorCode::InvalidPosition,
                         "En passant implies an illegal preceding position."};
        }
        auto withEp = impl->board.getFen(false);
        withEp.resize(withEp.rfind(' ') + 1);
        withEp += data.ep->name() + " 0 1";
        if (!impl->board.setFen(withEp)) {
            return Error{ErrorCode::InvalidFen,
                         "Chess backend rejected the validated en passant state."};
        }
    }
    impl->halfmove = data.halfmove;
    impl->fullmove = data.fullmove;
    impl->ep = data.ep;
    impl->provenance = RootProvenance::FenAnalysisRoot;
    Position position(std::move(impl));
    position.impl_->rootFen = position.fen();
    return position;
}

std::string Position::fen() const {
    auto result = impl_->board.getFen(false);
    result.resize(result.rfind(' ') + 1);
    result += impl_->ep ? impl_->ep->name() : "-";
    result += " " + std::to_string(impl_->halfmove) + " " + std::to_string(impl_->fullmove);
    return result;
}

std::optional<Piece> Position::pieceAt(Square square) const {
    const auto piece = impl_->board.at(::chess::Square(square.index()));
    if (piece == ::chess::Piece::NONE) {
        return std::nullopt;
    }
    return Piece{piece.color() == ::chess::Color::WHITE ? Color::White : Color::Black,
                 static_cast<PieceType>(static_cast<int>(piece.type()))};
}
Color Position::sideToMove() const {
    return impl_->board.sideToMove() == ::chess::Color::WHITE ? Color::White : Color::Black;
}
CastlingRights Position::castlingRights() const {
    const auto rights = impl_->board.getCastleString();
    return {rights.find('K') != std::string::npos, rights.find('Q') != std::string::npos,
            rights.find('k') != std::string::npos, rights.find('q') != std::string::npos};
}
std::optional<Square> Position::enPassantTarget() const { return impl_->ep; }
std::uint32_t Position::halfmoveClock() const { return impl_->halfmove; }
std::uint32_t Position::fullmoveNumber() const { return impl_->fullmove; }
// Upstream enum-range false positives: only validated boards and generated moves reach these calls.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
bool Position::inCheck() const {
    // Validated boards contain only White/Black; upstream XOR cannot produce another color.
    return impl_->board.inCheck();
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

std::vector<Move> Position::legalMoves() const {
    ::chess::Movelist moves;
    ::chess::movegen::legalmoves(moves, impl_->board);
    std::vector<Move> result;
    result.reserve(static_cast<std::size_t>(moves.size()));
    for (const auto move : moves) {
        result.push_back(toMove(move));
    }
    return result;
}
bool Position::isLegal(const Move& move) const { return findMove(impl_->board, move).has_value(); }

// Upstream enum-range false positives: only validated boards and generated moves reach these calls.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
Result<Move> Position::parseUci(std::string_view uciText) const {
    if ((uciText.size() != 4 && uciText.size() != 5) || !Square::fromName(uciText.substr(0, 2)) ||
        !Square::fromName(uciText.substr(2, 2)) ||
        (uciText.size() == 5 &&
         std::string_view("nbrq").find(uciText[4]) == std::string_view::npos)) {
        return Error{ErrorCode::InvalidNotation, "Expected a lowercase UCI coordinate move."};
    }
    ::chess::Movelist moves;
    ::chess::movegen::legalmoves(moves, impl_->board);
    for (const auto move : moves) {
        if (::chess::uci::moveToUci(move) == uciText) {
            return toMove(move);
        }
    }
    return illegalMove();
}
Result<Move> Position::parseSan(std::string_view sanText) const {
    if (sanText.empty() || sanText.size() > 32 || sanText.find('\0') != std::string_view::npos) {
        return Error{ErrorCode::InvalidNotation, "Expected canonical SAN for a legal move."};
    }
    ::chess::Movelist moves;
    ::chess::movegen::legalmoves(moves, impl_->board);
    // SAN suffixes describe legal mate, independent of upstream draw history/counters.
    // Generated FEN has valid kings, rights and squares; analyzer loses those constraints.
    const ::chess::Board notationBoard(impl_->board.getFen(false) + " 0 1");
    for (const auto move : moves) {
        if (::chess::uci::moveToSan(notationBoard, move) == sanText) {
            return toMove(move);
        }
    }
    return Error{ErrorCode::InvalidNotation, "SAN does not canonically identify a legal move."};
}
Result<std::string> Position::san(const Move& move) const {
    const auto candidate = findMove(impl_->board, move);
    if (!candidate) {
        return illegalMove();
    }
    // Generated FEN has valid kings, rights and squares; analyzer loses those constraints.
    const ::chess::Board notationBoard(impl_->board.getFen(false) + " 0 1");
    return ::chess::uci::moveToSan(notationBoard, *candidate);
}
Result<std::string> Position::uci(const Move& move) const {
    const auto candidate = findMove(impl_->board, move);
    if (!candidate) {
        return illegalMove();
    }
    return ::chess::uci::moveToUci(*candidate);
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

Status Position::play(const Move& move) {
    const auto candidate = findMove(impl_->board, move);
    if (!candidate) {
        return illegalMove();
    }
    const bool pawnMove = impl_->board.at(candidate->from()).type() == ::chess::PieceType::PAWN;
    const bool resetClock = pawnMove || impl_->board.isCapture(*candidate);
    if ((!resetClock && impl_->halfmove == std::numeric_limits<std::uint32_t>::max()) ||
        (sideToMove() == Color::Black &&
         impl_->fullmove == std::numeric_limits<std::uint32_t>::max())) {
        return Error{ErrorCode::CounterOverflow,
                     "Move would exceed the supported FEN counter range."};
    }
    impl_->history.push_back({move, *candidate, impl_->halfmove, impl_->fullmove, impl_->ep});
    try {
        impl_->board.makeMove<true>(*candidate);
    } catch (...) {
        // Upstream allocates its undo record before mutating. Preserve our record on allocation
        // failure too.
        impl_->history.pop_back();
        throw;
    }
    impl_->halfmove = resetClock ? 0 : impl_->halfmove + 1;
    if (sideToMove() == Color::White) {
        ++impl_->fullmove;
    }
    impl_->ep.reset();
    if (pawnMove && std::abs(move.to.rank() - move.from.rank()) == 2) {
        impl_->ep =
            Square::fromCoordinates(move.from.file(), (move.from.rank() + move.to.rank()) / 2);
    }
    return std::monostate{};
}
// Upstream enum-range false positives: only validated boards and generated moves reach these calls.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
std::optional<Move> Position::undo() {
    if (impl_->history.empty()) {
        return std::nullopt;
    }
    const auto previous = impl_->history.back();
    // Only a generated, successfully applied move enters this private undo stack.
    impl_->board.unmakeMove(previous.libraryMove);
    impl_->halfmove = previous.halfmove;
    impl_->fullmove = previous.fullmove;
    impl_->ep = previous.ep;
    impl_->history.pop_back();
    return previous.move;
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

BoardIdentity Position::boardIdentity() const {
    BoardIdentity identity;
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const auto square = *Square::fromCoordinates(file, rank);
            identity.pieces[static_cast<std::size_t>(square.index())] = pieceAt(square);
        }
    }
    identity.sideToMove = sideToMove();
    identity.castlingRights = castlingRights();
    const auto ep = impl_->board.enpassantSq();
    if (ep != ::chess::Square::NO_SQ) {
        identity.legalEnPassant = Square::fromCoordinates(ep.index() % 8, ep.index() / 8);
    }
    return identity;
}
PositionContext Position::context() const {
    PositionContext result;
    result.board = boardIdentity();
    result.halfmoveClock = impl_->halfmove;
    result.provenance = impl_->provenance;
    result.rootFen = impl_->rootFen;
    result.moves.reserve(impl_->history.size());
    for (const auto& entry : impl_->history) {
        result.moves.push_back(entry.move);
    }
    return result;
}
} // namespace chess_review::chess
