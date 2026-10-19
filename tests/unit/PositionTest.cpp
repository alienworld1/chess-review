#include "chess/Position.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace chess_review::chess;

namespace {
template <typename T> T value(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result)) {
        FAIL(error->message);
    }
    return std::get<T>(std::move(result));
}
Position load(std::string_view fen) { return value(Position::fromFen(fen)); }
Move play(Position& position, std::string_view uci) {
    const auto move = value(position.parseUci(uci));
    value(position.play(move));
    return move;
}
std::uint64_t perft(Position& position, int depth) {
    if (depth == 0) {
        return 1;
    }
    std::uint64_t nodes = 0;
    for (const auto& move : position.legalMoves()) {
        value(position.play(move));
        nodes += perft(position, depth - 1);
        REQUIRE(position.undo() == move);
    }
    return nodes;
}
void rejectsFen(std::string_view fen) {
    CAPTURE(fen);
    REQUIRE(std::holds_alternative<Error>(Position::fromFen(fen)));
}
} // namespace

TEST_CASE("Legal move generation matches established perft positions", "[chess][unit]") {
    SECTION("initial position") {
        auto position = Position::initial();
        const auto before = position.context();
        CHECK(perft(position, 3) == 8902);
        CHECK(position.context() == before);
    }
    SECTION("Kiwipete castling and pins") {
        auto position =
            load("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
        CHECK(perft(position, 3) == 97862);
    }
    SECTION("rook and pawn ending with en passant") {
        auto position = load("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
        CHECK(perft(position, 3) == 2812);
    }
    SECTION("promotions and check evasions") {
        auto position = load("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
        CHECK(perft(position, 3) == 9467);
    }
}

TEST_CASE("FEN is strict bounded and locally legal", "[chess][unit]") {
    for (const auto fen : {
             "",
             "8/8/8/8/8/8/8/8 w - - 0 1", // no kings
             "4k3/8/8/8/8/8/8/4K3 w - -", // missing counters
             "4k3/8/8/8/8/8/8/4K3 w - - 0 1 extra",
             "4k3/8/8/8/8/8/8/4K3 x - - 0 1",
             "4k3/8/8/8/8/8/8/4K3 w - - -1 1",
             "4k3/8/8/8/8/8/8/4K3 w - - 0 0",
             "4k3/8/8/8/8/8/8/4K3 w - - 4294967296 1",
             "4k3/8/8/8/8/8/8/4K3 w - - 0 4294967296",
             "4k3/8/8/8/8/8/8/4K3 w - - +0 1",
             "4k3/8/8/8/8/8/8/4K3 w - - 1x 1",
             "4k3/8/8/8/8/8/8/4K3 w - - 0 1x",
             "4k3/8/8/8/8/8/8/4K3/8 w - - 0 1",
             "4k3/8/8/8/8/8/8/4K2 w - - 0 1",
             "4k3/8/8/8/8/8/8/4K4 w - - 0 1",
             "4k3/8/8/8/8/8/8/4K21 w - - 0 1",
             "4k3/8/8/8/8/8/8/4K2X w - - 0 1",
             "4k3/8/8/8/8/8/8/4K2P w - - 0 1", // back-rank pawn
             "4k3/8/8/8/8/8/4K3/4K3 w - - 0 1",
             "8/8/8/8/8/8/4k3/4K3 w - - 0 1",   // adjacent kings
             "4k3/8/8/8/8/8/4R3/4K3 w - - 0 1", // previous mover in check
             "4k3/8/8/8/8/8/8/4K3 w K - 0 1",   // absent rook
             "4k3/8/8/8/8/8/8/4K2R w KK - 0 1",
             "4k3/8/8/8/8/8/8/3K3R w K - 0 1", // displaced king
             "4k3/8/8/8/8/8/8/4K2R w H - 0 1", // Chess960
             "4k3/8/8/8/8/8/8/4K2R w K- - 0 1",
             "4k3/8/8/8/8/8/8/4K3 w - e6 0 2",          // no double-moved pawn
             "4k3/8/8/4p3/8/8/8/4K3 w - e6 1 2",        // wrong clock
             "4k3/8/8/4p3/8/8/8/4K3 w - e6 0 1",        // impossible move number
             "4k3/4p3/8/4p3/8/8/8/4K3 w - e6 0 2",      // occupied origin
             "4k3/8/4N3/4p3/8/8/8/4K3 w - e6 0 2",      // occupied target
             "4k3/8/8/4p3/8/8/8/4K3 w - e3 0 2",        // wrong rank
             "4k3/8/8/8/P7/PPPPPPPP/8/4K3 w - - 0 1",   // nine pawns
             "4k3/8/8/8/8/8/PPPPPPPP/3QKQ2 w - - 0 1",  // no promotion pawn
             "4k3/8/8/8/8/8/PPPPPPPP/2B1K1B1 w - - 0 1" // same-color bishops
         }) {
        rejectsFen(fen);
    }
    rejectsFen(std::string(257, ' '));
    rejectsFen(std::string("4k3/8/8/8/8/8/8/4K3 w - - 0 1") + '\0');
    CHECK(load(" \t4k3/8/8/8/8/8/8/4K3\tw - - 000 001\n").fen() == "4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(load("4k3/8/8/8/8/8/4r3/4K3 w - - 0 1").inCheck());
    CHECK(!(Square::fromName("A1")));
    CHECK(!(Square::fromName("a0")));
    CHECK(!(Square::fromCoordinates(-1, 0)));
    CHECK(!(Square::fromCoordinates(0, 8)));
}

TEST_CASE("Standard FEN en passant survives normalization and undo", "[chess][unit]") {
    auto position = Position::initial();
    const auto start = position.context();
    play(position, "e2e4");
    CHECK(position.fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    CHECK(position.enPassantTarget() == Square::fromName("e3"));
    CHECK(!(position.boardIdentity().legalEnPassant));
    CHECK(load(position.fen()).fen() == position.fen());
    const auto afterPawn = position.context();
    play(position, "g8f6");
    CHECK(!(position.enPassantTarget()));
    REQUIRE(position.undo());
    CHECK(position.context() == afterPawn);
    REQUIRE(position.undo());
    CHECK(position.context() == start);
    CHECK(!(position.undo()));
}

TEST_CASE("Legality guards pins castling en passant and caller-created moves", "[chess][unit]") {
    auto pin = load("k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1");
    CHECK(std::holds_alternative<Error>(pin.parseUci("e2f2")));
    auto castle = load("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    const auto before = castle.context();
    auto move = value(castle.parseSan("O-O"));
    CHECK(value(castle.uci(move)) == "e1g1");
    value(castle.play(move));
    CHECK(castle.pieceAt(*Square::fromName("f1")) == Piece{Color::White, PieceType::Rook});
    CHECK(!(castle.castlingRights().whiteQueenSide));
    REQUIRE(castle.undo() == move);
    CHECK(castle.context() == before);
    auto attacked = load("r3k2r/8/8/8/8/5r2/8/R3K2R w KQkq - 0 1");
    CHECK(std::holds_alternative<Error>(attacked.parseUci("e1g1")));
    auto discovered = load("k7/8/8/4KPpr/8/8/8/8 w - g6 0 2");
    CHECK(!(discovered.boardIdentity().legalEnPassant));
    CHECK(std::holds_alternative<Error>(discovered.parseUci("f5g6")));
    auto push = load("k7/6p1/8/4KP1r/8/8/8/8 b - - 0 1");
    play(push, "g7g5");
    CHECK(!(push.boardIdentity().legalEnPassant));
    CHECK(push.enPassantTarget() == Square::fromName("g6"));
    auto ep = load("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    const auto epBefore = ep.context();
    auto capture = value(ep.parseSan("exd6"));
    value(ep.play(capture));
    CHECK(!(ep.pieceAt(*Square::fromName("d5"))));
    REQUIRE(ep.undo() == capture);
    CHECK(ep.context() == epBefore);

    auto initial = Position::initial();
    const auto snapshot = initial.context();
    Move invalid{*Square::fromName("e2"), *Square::fromName("e4"), PieceType::King};
    CHECK(!(initial.isLegal(invalid)));
    CHECK(std::holds_alternative<Error>(initial.play(invalid)));
    CHECK(std::holds_alternative<Error>(initial.san(invalid)));
    CHECK(std::holds_alternative<Error>(initial.uci(invalid)));
    CHECK(initial.context() == snapshot);
    auto stale = value(initial.parseUci("g1f3"));
    play(initial, "e2e4");
    const auto after = initial.context();
    CHECK(std::holds_alternative<Error>(initial.play(stale)));
    CHECK(initial.context() == after);
}

TEST_CASE("Notation is canonical contextual and round trips legal moves", "[chess][unit]") {
    for (const auto fen : {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                           "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "7k/P7/8/8/8/8/8/7K w - - 0 1",
                           "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2"}) {
        auto position = load(fen);
        const auto before = position.context();
        for (const auto& move : position.legalMoves()) {
            CHECK(value(position.parseSan(value(position.san(move)))) == move);
            CHECK(value(position.parseUci(value(position.uci(move)))) == move);
        }
        CHECK(position.context() == before);
    }
    auto promotion = load("7k/P7/8/8/8/8/8/7K w - - 0 1");
    for (const auto uci : {"a7a8n", "a7a8b", "a7a8r", "a7a8q"}) {
        auto branch = promotion;
        const auto before = branch.context();
        play(branch, uci);
        REQUIRE(branch.undo());
        CHECK(branch.context() == before);
    }
    CHECK(value(promotion.san(value(promotion.parseUci("a7a8q")))) == "a8=Q+");
    CHECK(std::holds_alternative<Error>(promotion.parseSan("a8=Q")));
    CHECK(std::holds_alternative<Error>(promotion.parseUci("a7a8")));
    auto ambiguity = load("4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1");
    CHECK(value(ambiguity.san(value(ambiguity.parseUci("b1d2")))) == "Nbd2");
    CHECK(std::holds_alternative<Error>(ambiguity.parseSan("Nd2")));
    auto rankDisambiguation = load("4k3/8/8/8/8/R7/8/R3K3 w - - 0 1");
    CHECK(value(rankDisambiguation.san(value(rankDisambiguation.parseUci("a1a2")))) == "R1a2");
    auto fullDisambiguation = load("4k3/8/8/8/8/1N6/8/1N3N1K w - - 0 1");
    CHECK(value(fullDisambiguation.san(value(fullDisambiguation.parseUci("b1d2")))) == "Nb1d2");
    auto checkingCastle = load("5k2/8/8/8/8/8/8/4K2R w K - 0 1");
    CHECK(value(checkingCastle.san(value(checkingCastle.parseUci("e1g1")))) == "O-O+");
    CHECK(std::holds_alternative<Error>(checkingCastle.parseSan("O-O")));
    auto capturePromotion = load("1r5k/P7/8/8/8/8/8/7K w - - 0 1");
    CHECK(value(capturePromotion.san(value(capturePromotion.parseUci("a7b8r")))) == "axb8=R+");
    auto mate = load("7k/8/5KQ1/8/8/8/8/8 w - - 4294967295 2");
    CHECK(value(mate.san(value(mate.parseUci("g6g7")))) == "Qg7#");
    CHECK(std::holds_alternative<Error>(mate.parseSan("Qg7+")));
    auto initial = Position::initial();
    for (const auto text : {"e4+", "e4!", "e4 ", "e2e4", "0-0", "", "Nf3#"}) {
        CHECK(std::holds_alternative<Error>(initial.parseSan(text)));
    }
    CHECK(std::holds_alternative<Error>(initial.parseSan(std::string(33, 'x'))));
    CHECK(std::holds_alternative<Error>(initial.parseSan(std::string("e4\0", 3))));
    for (const auto text : {"0000", "E2e4", "e2e4Q", "e2e4x", "e2e4 ", "e2", "e9e4", "e2e5"}) {
        CHECK(std::holds_alternative<Error>(initial.parseUci(text)));
    }
}

TEST_CASE("Wide counters are checked without upstream truncation", "[chess][unit]") {
    auto position = load("4k3/8/8/8/8/8/8/R3K3 b - - 255 32768");
    play(position, "e8f8");
    CHECK(position.halfmoveClock() == 256);
    CHECK(position.fullmoveNumber() == 32769);
    CHECK(position.fen() == "5k2/8/8/8/8/8/8/R3K3 w - - 256 32769");
    auto half = load("4k3/8/8/8/8/8/8/R3K3 w - - 4294967295 1");
    const auto before = half.context();
    const auto error = half.play(value(half.parseUci("a1a2")));
    REQUIRE(std::holds_alternative<Error>(error));
    CHECK(std::get<Error>(error).code == ErrorCode::CounterOverflow);
    CHECK(half.context() == before);
    auto full = load("4k3/8/8/8/8/8/8/R3K3 b - - 0 4294967295");
    const auto fullBefore = full.context();
    CHECK(std::holds_alternative<Error>(full.play(value(full.parseUci("e8f8")))));
    CHECK(full.context() == fullBefore);
    auto reset = load("4k3/8/8/8/8/8/P7/4K3 w - - 4294967295 4294967295");
    play(reset, "a2a3");
    CHECK(reset.halfmoveClock() == 0);
    CHECK(reset.fullmoveNumber() == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("Board identity and analysis context express different equivalence", "[chess][unit]") {
    auto initial = Position::initial();
    auto fenRoot = load(initial.fen());
    CHECK(initial.boardIdentity() == fenRoot.boardIdentity());
    CHECK(initial.context() != fenRoot.context());
    CHECK(fenRoot.context() == load(initial.fen()).context());
    auto clocks = load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 7 12");
    CHECK(initial.boardIdentity() == clocks.boardIdentity());
    CHECK(fenRoot.context() != clocks.context());
    auto noRights = load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(initial.boardIdentity() != noRights.boardIdentity());
    auto otherSide = load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    CHECK(initial.boardIdentity() != otherSide.boardIdentity());
    auto ep = load("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    auto noEp = load("4k3/8/8/3pP3/8/8/8/4K3 w - - 0 2");
    CHECK(ep.boardIdentity() != noEp.boardIdentity());
    auto uncapturable = load("4k3/8/8/3p4/8/8/8/4K3 w - d6 0 2");
    auto omitted = load("4k3/8/8/3p4/8/8/8/4K3 w - - 0 2");
    CHECK(uncapturable.boardIdentity() == omitted.boardIdentity());
    CHECK(uncapturable.context() != omitted.context());
    auto first = initial;
    auto second = initial;
    for (auto move : {"g1f3", "g8f6", "b1c3", "b8c6"}) {
        play(first, move);
    }
    for (auto move : {"b1c3", "b8c6", "g1f3", "g8f6"}) {
        play(second, move);
    }
    CHECK(first.fen() == second.fen());
    CHECK(first.boardIdentity() == second.boardIdentity());
    CHECK(first.context() != second.context());
    auto copy = first;
    CHECK(copy.context() == first.context());
    REQUIRE(copy.undo());
    CHECK(copy.context() != first.context());
    auto assigned = Position::initial();
    assigned = first;
    CHECK(assigned.context() == first.context());
    auto moved = std::move(assigned);
    CHECK(moved.context() == first.context());
    assigned = Position::initial();
    CHECK(assigned.context() == initial.context());
}

TEST_CASE("Long reversible lines do not inherit upstream counter rollover", "[chess][unit]") {
    auto position = Position::initial();
    auto probe = position;
    std::vector<Move> cycle;
    for (auto uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
        cycle.push_back(play(probe, uci));
    }
    const auto before = position.context();
    constexpr int plies = 65540;
    for (int i = 0; i < plies; ++i) {
        value(position.play(cycle[static_cast<std::size_t>(i % 4)]));
    }
    CHECK(position.halfmoveClock() == plies);
    CHECK(position.fullmoveNumber() == 32771);
    CHECK(position.boardIdentity() == before.board);
    CHECK(value(position.san(cycle.front())) == "Nf3");
    for (int i = 0; i < plies; ++i) {
        REQUIRE(position.undo());
    }
    CHECK(position.context() == before);
}

TEST_CASE("Terminal boards expose legal moves and check without draw adjudication",
          "[chess][unit]") {
    auto mate = load("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    CHECK(mate.inCheck());
    CHECK(mate.legalMoves().empty());
    auto stalemate = load("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    CHECK(!stalemate.inCheck());
    CHECK(stalemate.legalMoves().empty());
}
