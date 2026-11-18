#include "openings/PackagedOpenings.h"
#include "pgn/Pgn.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

using namespace chess_review;

namespace {
std::optional<openings::Opening> recognize(const openings::OpeningIndex& index,
                                           std::string_view pgnText) {
    pgn::PgnReader reader(pgnText);
    auto parsed = reader.next();
    const auto* game = std::get_if<pgn::PgnDocument>(&parsed);
    REQUIRE(game);
    auto root = game->tree.position(game->tree.rootId());
    REQUIRE(std::holds_alternative<chess::Position>(root));
    return index.recognize(std::get<chess::Position>(std::move(root)), game->originalMainline());
}

openings::OpeningIndex indexFrom(std::string_view volume) {
    const auto references = openings::compileReferences(std::array{volume});
    REQUIRE(references);
    auto index = openings::OpeningIndex::build(*references);
    REQUIRE(index);
    return std::move(*index);
}
} // namespace

TEST_CASE("Bundled openings resolve legal mainlines and ignore PGN tags and variations",
          "[openings][unit]") {
    const auto index = openings::loadPackagedOpenings();
    REQUIRE(index);
    const auto result =
        recognize(*index, "[Opening \"Queen's Gambit\"]\n[ECO \"D06\"]\n\n"
                          "1. e4 (1. d4 d5 2. c4) c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6 5. Nc3 a6 *");
    REQUIRE(result);
    CHECK(result->eco == "B90");
    CHECK(result->family() == "Sicilian Defense");
    CHECK(result->variation() == "Najdorf Variation");
    CHECK((recognize(*index, "1. c4 d5 2. d4 *") == openings::Opening{"D06", "Queen's Gambit"}));
    CHECK_FALSE(recognize(*index, "[Opening \"Sicilian Defense\"]\n[ECO \"B20\"]\n\n*"));
}

TEST_CASE("Opening recognition prefers a unique reference order at ambiguous positions",
          "[openings][unit]") {
    const auto index = indexFrom("eco\tname\tpgn\n"
                                 "A04\tZukertort Opening\t1. Nf3\n"
                                 "D00\tFirst Name\t1. d4 d5 2. Nf3 Nf6\n"
                                 "D01\tSecond Name\t1. Nf3 Nf6 2. d4 d5\n"
                                 "D02\tThird Name\t1. d4 Nf6 2. Nf3 d5\n"
                                 "D03\tAnother Third Name\t1. d4 Nf6 2. Nf3 d5\n");
    CHECK((recognize(index, "1. d4 d5 2. Nf3 Nf6 *") == openings::Opening{"D00", "First Name"}));
    CHECK((recognize(index, "1. Nf3 Nf6 2. d4 d5 *") == openings::Opening{"D01", "Second Name"}));
    CHECK((recognize(index, "1. Nf3 d5 2. d4 Nf6 *") ==
           openings::Opening{"A04", "Zukertort Opening"}));
    CHECK_FALSE(recognize(index, "1. d4 Nf6 2. Nf3 d5 *"));
}

TEST_CASE("Opening recognition uses normalized rights and legal en passant", "[openings][unit]") {
    const auto index = indexFrom("eco\tname\tpgn\n"
                                 "A05\tBoth Castling Rights\t1. Nf3 Nf6\n"
                                 "A06\tFewer Castling Rights\t1. Nf3 Nf6 2. Rg1 Rg8 3. Rh1 Rh8\n"
                                 "B00\tEn Passant Available\t1. e4 h6 2. e5 d5\n"
                                 "D00\tTransposed\t1. d4 d5 2. Nf3 Nf6\n");
    CHECK((recognize(index, "1. Nf3 Nf6 2. Rg1 Rg8 3. Rh1 Rh8 *") ==
           openings::Opening{"A06", "Fewer Castling Rights"}));
    CHECK((recognize(index, "1. e4 h6 2. e5 d5 *") ==
           openings::Opening{"B00", "En Passant Available"}));
    CHECK_FALSE(recognize(index, "1. e4 d5 2. e5 h6 *"));
    CHECK((recognize(index, "1. Nf3 Nf6 2. d4 d5 *") == openings::Opening{"D00", "Transposed"}));
}

TEST_CASE("Opening recognition skips repeat positions and bounds its scan", "[openings][unit]") {
    const auto index = indexFrom("eco\tname\tpgn\n"
                                 "A04\tEarlier\t1. Nf3\n"
                                 "A05\tDeeper\t1. Nf3 Nf6\n"
                                 "D00\tToo Late\t1. d4 d5 2. c4 e6 3. Nc3\n");
    CHECK(
        (recognize(index, "1. Nf3 Nf6 2. Ng1 Ng8 3. Nf3 *") == openings::Opening{"A05", "Deeper"}));
    CHECK_FALSE(recognize(index, "1. Nc3 Nc6 2. Nb1 Nb8 3. d4 d5 4. c4 e6 5. Nc3 *"));
}

TEST_CASE("Opening recognition accepts only the exact standard starting position",
          "[openings][unit]") {
    const auto index = indexFrom("eco\tname\tpgn\nB20\tSicilian Defense\t1. e4 c5\n");
    CHECK(recognize(index, "[SetUp \"1\"]\n"
                           "[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\"]\n\n"
                           "1. e4 c5 *"));
    CHECK_FALSE(recognize(index,
                          "[SetUp \"1\"]\n"
                          "[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 1 1\"]\n\n"
                          "1. e4 c5 *"));
    CHECK_FALSE(
        recognize(index, "[SetUp \"1\"]\n"
                         "[FEN \"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1\"]\n\n"
                         "1... c5 *"));
}

TEST_CASE("An invalid opening dataset does not publish a partial lookup", "[openings][unit]") {
    using namespace std::string_view_literals;
    CHECK_FALSE(openings::compileReferences(std::array{"eco\tname\tpgn\n"sv}));
    CHECK_FALSE(
        openings::compileReferences(std::array{"eco\tname\tpgn\nA04\tZukertort Opening\t1. Nf3\n"sv,
                                               "eco\tname\tpgn\nB30\tInvalid Opening\t1. e5\n"sv}));
    CHECK_FALSE(openings::OpeningIndex::build("eco\tname\tepd\tuci\nE05\tBroken\tinvalid\tg1f3\n"));
}

TEST_CASE("Book needs supported position and move pairs until the first departure",
          "[openings][unit]") {
    const auto index = indexFrom("eco\tname\tpgn\n"
                                 "A04\tFirst\t1. Nf3 Nf6 2. d4 d5\n"
                                 "D00\tSecond\t1. d4 d5 2. Nf3 Nf6 3. c4\n"
                                 "D01\tThird\t1. d4 d5 2. Nf3 Nf6 3. c4 e6\n"
                                 "D02\tAlias\t1. Nf3 Nf6 2. d4 d5\n");
    const auto prefix = [&](std::string_view text, openings::BookConfig config) {
        pgn::PgnReader reader(text);
        const auto parsed = reader.next();
        const auto* game = std::get_if<pgn::PgnDocument>(&parsed);
        REQUIRE(game);
        auto root = game->tree.position(game->tree.rootId());
        return index.bookPrefix(std::get<chess::Position>(std::move(root)),
                                game->originalMainline(), config);
    };
    CHECK(prefix("1. Nf3 Nf6 2. d4 d5 3. c4 *", {true, 24, 1}) == 5);
    CHECK(prefix("1. Nf3 Nf6 2. d4 d5 3. c4 *", {}) == 0); // Aliases count once.
    CHECK(prefix("1. d4 d5 2. Nf3 Nf6 3. c4 e6 *", {}) == 5);
    CHECK(prefix("1. d4 d5 2. Nf3 Nf6 3. c4 e6 *", {true, 3, 1}) == 3);
    CHECK(prefix("1. d4 d5 2. Nf3 Nf6 3. c4 e6 *", {false, 24, 1}) == 0);
    CHECK(prefix("1. d4 d5 2. Nc3 Nc6 3. Nb1 Nb8 4. Nf3 Nf6 5. c4 *", {}) == 2);
    CHECK(prefix("[SetUp \"1\"]\n"
                 "[FEN \"rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2\"]\n\n"
                 "2. Nf3 Nf6 3. c4 *",
                 {}) == 0);
}
