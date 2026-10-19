#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chess_review::chess {

enum class Color { White, Black };
enum class PieceType { Pawn, Knight, Bishop, Rook, Queen, King };

struct Piece {
    Color color{};
    PieceType type{};
    bool operator==(const Piece&) const = default;
};

// Every constructible Square is in bounds. Default is a1; indices run a1=0 to h8=63.
class Square {
  public:
    Square() = default;
    [[nodiscard]] static std::optional<Square> fromName(std::string_view name);
    [[nodiscard]] static std::optional<Square> fromCoordinates(int file, int rank);
    [[nodiscard]] int index() const { return index_; }
    [[nodiscard]] int file() const { return index_ % 8; }
    [[nodiscard]] int rank() const { return index_ / 8; }
    [[nodiscard]] std::string name() const;
    bool operator==(const Square&) const = default;

  private:
    explicit Square(int index) : index_(index) {}
    int index_{};
};

struct Move {
    Square from;
    Square to;
    std::optional<PieceType> promotion;
    bool operator==(const Move&) const = default;
};

struct CastlingRights {
    bool whiteKingSide{};
    bool whiteQueenSide{};
    bool blackKingSide{};
    bool blackQueenSide{};
    bool operator==(const CastlingRights&) const = default;
};

enum class ErrorCode { InvalidFen, InvalidPosition, InvalidNotation, IllegalMove, CounterOverflow };
struct Error {
    ErrorCode code{};
    std::string message; // Bounded static diagnostic; never echoes untrusted input.
};
template <typename T> using Result = std::variant<T, Error>;
using Status = Result<std::monostate>;

struct BoardIdentity {
    std::array<std::optional<Piece>, 64> pieces;
    Color sideToMove{};
    CastlingRights castlingRights;
    std::optional<Square> legalEnPassant;
    bool operator==(const BoardIdentity&) const = default;
};

enum class Variant { Standard };
enum class RootProvenance { InitialPosition, FenAnalysisRoot };

// Exact conservative identity, not a hash or a draw/repetition adjudication API.
// Full history is copied only when this snapshot is requested. No persisted wire format yet.
struct PositionContext {
    std::uint32_t version{1};
    Variant variant{Variant::Standard};
    BoardIdentity board;
    std::uint32_t halfmoveClock{};
    RootProvenance provenance{};
    std::string rootFen;
    std::vector<Move> moves;
    bool operator==(const PositionContext&) const = default;
};

// Independent values; concurrent access to the same mutable instance needs external
// synchronization. Moved-from values may only be destroyed or assigned to. No Qt or I/O is
// involved.
class Position {
  public:
    [[nodiscard]] static Position initial();
    [[nodiscard]] static Result<Position> fromFen(std::string_view fen);
    ~Position();
    Position(const Position& other);
    Position& operator=(const Position& other);
    Position(Position&&) noexcept;
    Position& operator=(Position&&) noexcept;

    [[nodiscard]] std::string fen() const;
    [[nodiscard]] std::optional<Piece> pieceAt(Square square) const;
    [[nodiscard]] Color sideToMove() const;
    [[nodiscard]] CastlingRights castlingRights() const;
    [[nodiscard]] std::optional<Square> enPassantTarget() const;
    [[nodiscard]] std::uint32_t halfmoveClock() const;
    [[nodiscard]] std::uint32_t fullmoveNumber() const;
    [[nodiscard]] bool inCheck() const;
    [[nodiscard]] std::vector<Move> legalMoves() const;
    [[nodiscard]] bool isLegal(const Move& move) const;
    [[nodiscard]] Result<Move> parseSan(std::string_view san) const;
    [[nodiscard]] Result<Move> parseUci(std::string_view uci) const;
    [[nodiscard]] Result<std::string> san(const Move& move) const;
    [[nodiscard]] Result<std::string> uci(const Move& move) const;
    // Invalid input and counter overflow leave the entire value unchanged.
    [[nodiscard]] Status play(const Move& move);
    [[nodiscard]] std::optional<Move> undo();
    [[nodiscard]] BoardIdentity boardIdentity() const;
    [[nodiscard]] PositionContext context() const;

  private:
    struct Impl;
    explicit Position(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace chess_review::chess
