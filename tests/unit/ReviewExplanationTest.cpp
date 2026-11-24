#include "review/Explanation.h"
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace chess_review;
using namespace chess_review::review;

namespace {

chess::Position position(const char* fen) {
    auto result = chess::Position::fromFen(fen);
    INFO(fen);
    REQUIRE(std::holds_alternative<chess::Position>(result));
    return std::get<chess::Position>(std::move(result));
}

chess::Move uciMove(const chess::Position& pos, const char* uci) {
    INFO(uci);
    auto parsed = pos.parseUci(uci);
    REQUIRE(std::holds_alternative<chess::Move>(parsed));
    return std::get<chess::Move>(parsed);
}

chess::Position playLine(chess::Position pos, std::initializer_list<const char*> ucis) {
    for (const auto* text : ucis) {
        const auto move = uciMove(pos, text);
        REQUIRE(std::holds_alternative<std::monostate>(pos.play(move)));
    }
    return pos;
}

chess::Square sq(const char* name) { return *chess::Square::fromName(name); }

MoveEvidence makePair(const chess::Position& before, const char* playedUci, const char* bestUci,
                      const char* replyUci, std::optional<RawScore> beforeScore = std::nullopt,
                      std::optional<RawScore> afterScore = std::nullopt) {
    MoveEvidence pair;
    pair.mover = before.sideToMove();
    pair.playedMove = uciMove(before, playedUci);
    pair.before.position = before.context();
    pair.before.complete = true;
    pair.before.engineIdentity = "engine";
    pair.before.sessionIdentity = "s";
    pair.before.attemptIdentity = "a";
    pair.before.evaluation.perspective = before.sideToMove();
    pair.before.evaluation.score =
        beforeScore ? *beforeScore : RawScore{ScoreKind::Centipawns, 0, Bound::Exact};
    pair.before.candidates = {
        {1, pair.before.evaluation, {uciMove(before, bestUci)}, {12, 1000, {}}}};

    auto after = before;
    REQUIRE(std::holds_alternative<std::monostate>(after.play(pair.playedMove)));
    pair.after.position = after.context();
    pair.after.complete = true;
    pair.after.engineIdentity = "engine";
    pair.after.sessionIdentity = "s";
    pair.after.attemptIdentity = "a";
    pair.after.evaluation.perspective = after.sideToMove();
    pair.after.evaluation.score =
        afterScore ? *afterScore : RawScore{ScoreKind::Centipawns, 0, Bound::Exact};
    if (replyUci && *replyUci != '\0') {
        pair.after.candidates = {
            {1, pair.after.evaluation, {uciMove(after, replyUci)}, {12, 1000, {}}}};
    }
    return pair;
}

ClassificationResult makeClass(std::optional<Classification> label, const chess::Position& before,
                               const char* bestUci, std::string ruleId = {}) {
    ClassificationResult c;
    c.label = label;
    c.mover = before.sideToMove();
    if (bestUci && *bestUci != '\0') {
        c.bestMove = uciMove(before, bestUci);
    }
    c.ruleId = std::move(ruleId);
    return c;
}

} // namespace

TEST_CASE("Capablanca-Tartakower 1924 regression ignores deep-PV hanging e4 artifacts",
          "[unit][review][explanation]") {
    const auto beforeBg5 =
        playLine(chess::Position::initial(), {"d2d4", "e7e6", "g1f3", "f7f5", "c2c4", "g8f6"});
    {
        const auto pair = makePair(beforeBg5, "c1g5", "c1g5", "f8e7");
        const auto cls = makeClass(Classification::Best, beforeBg5, "c1g5");
        const auto exp = explainMove(beforeBg5, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::CreatesPin);
        CHECK(exp.actor == chess::PieceType::Bishop);
        CHECK(exp.targetPiece == chess::PieceType::Knight);
        CHECK(exp.square == sq("f6"));
        CHECK(exp.secondaryPiece == chess::PieceType::Queen);
        CHECK(exp.secondarySquare == sq("d8"));
    }

    const auto beforeNc3 = playLine(beforeBg5, {"c1g5", "f8e7"});
    {
        const auto pair = makePair(beforeNc3, "b1c3", "e2e3", "e8g8");
        const auto cls = makeClass(Classification::Excellent, beforeNc3, "e2e3");
        TacticalFact deepArtifact;
        deepArtifact.kind = TacticalKind::HangingPiece;
        deepArtifact.source = FactSource::Played;
        deepArtifact.beneficiary = chess::Color::Black;
        deepArtifact.square = sq("e4");
        deepArtifact.material = 3;
        deepArtifact.linePlies = 6;
        const std::vector<TacticalFact> facts{deepArtifact};

        const auto exp = explainMove(beforeNc3, pair, cls, facts);
        CHECK(exp.kind == ExplanationKind::DevelopsPiece);
        CHECK(exp.actor == chess::PieceType::Knight);
        CHECK(exp.square == sq("c3"));
        CHECK(exp.central);
    }

    const auto beforeCastle = playLine(beforeNc3, {"b1c3"});
    {
        const auto pair = makePair(beforeCastle, "e8g8", "e8g8", "e2e3");
        const auto cls = makeClass(Classification::Best, beforeCastle, "e8g8");
        const auto exp = explainMove(beforeCastle, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::Castles);
        CHECK(exp.kingside);
    }

    const auto beforeBxe7 = playLine(
        beforeCastle, {"e8g8", "e2e3", "b7b6", "f1d3", "c8b7", "e1g1", "d8e8", "d1e2", "f6e4"});
    {
        const auto pair = makePair(beforeBxe7, "g5e7", "g5e7", "e4c3");
        const auto cls = makeClass(Classification::Best, beforeBxe7, "g5e7");
        const auto exp = explainMove(beforeBxe7, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::TradesPieces);
        CHECK(exp.actor == chess::PieceType::Bishop);
        CHECK(exp.targetPiece == chess::PieceType::Bishop);
        CHECK(exp.square == sq("e7"));
    }

    const auto beforeBxc3 = playLine(beforeBxe7, {"g5e7", "e4c3"});
    {
        const auto pair = makePair(beforeBxc3, "b2c3", "b2c3", "e8e7");
        const auto cls = makeClass(Classification::Best, beforeBxc3, "b2c3");
        const auto exp = explainMove(beforeBxc3, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::Recaptures);
        CHECK(exp.actor == chess::PieceType::Pawn);
        CHECK(exp.targetPiece == chess::PieceType::Knight);
        CHECK(exp.square == sq("c3"));
    }

    const auto beforeQxf3 = playLine(beforeBxc3, {"b2c3", "e8e7", "a2a4", "b7f3"});
    {
        const auto pair = makePair(beforeQxf3, "e2f3", "e2f3", "b8c6");
        const auto cls = makeClass(Classification::Best, beforeQxf3, "e2f3");
        const auto exp = explainMove(beforeQxf3, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::Recaptures);
        CHECK(exp.actor == chess::PieceType::Queen);
        CHECK(exp.targetPiece == chess::PieceType::Bishop);
        CHECK(exp.square == sq("f3"));
    }

    const auto beforeF4 = playLine(beforeQxf3, {"e2f3", "b8c6", "f1b1", "a8e8", "f3h3", "f8f6"});
    {
        const auto pair = makePair(beforeF4, "f2f4", "b1e1", "c6a5");
        const auto cls = makeClass(Classification::Excellent, beforeF4, "b1e1");
        const auto exp = explainMove(beforeF4, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::Generic);
    }
}

TEST_CASE("Positive tactical and positional themes are accurately identified",
          "[unit][review][explanation]") {
    SECTION("DeliversCheckmate") {
        const auto pos = position("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1");
        const auto pair = makePair(pos, "d1d8", "d1d8", nullptr);
        const auto cls = makeClass(Classification::Best, pos, "d1d8");
        CHECK(explainMove(pos, pair, cls, {}).kind == ExplanationKind::DeliversCheckmate);
    }

    SECTION("LeadsToMate") {
        const auto pos = position("5rk1/5ppp/8/8/8/3B1N2/5PPP/3Q2K1 w - - 0 1");
        const auto pair =
            makePair(pos, "d3h7", "d3h7", "g8h7", RawScore{ScoreKind::Mate, 4, Bound::Exact},
                     RawScore{ScoreKind::Mate, -3, Bound::Exact});
        const auto cls = makeClass(Classification::Best, pos, "d3h7");
        const auto exp = explainMove(pos, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::LeadsToMate);
        CHECK(exp.checksKing);
    }

    SECTION("PromotesPawn") {
        const auto pos = position("7k/P7/8/8/8/8/8/6K1 w - - 0 1");
        const auto pair = makePair(pos, "a7a8q", "a7a8q", "h8g7");
        const auto cls = makeClass(Classification::Best, pos, "a7a8q");
        const auto exp = explainMove(pos, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::PromotesPawn);
        CHECK(exp.actor == chess::PieceType::Queen);
        CHECK(exp.square == sq("a8"));
    }

    SECTION("ForksPieces checking and safe non-checking, rejecting hanging or pinned forks") {
        const auto checkFork = position("3q3k/8/8/4N3/8/8/8/K7 w - - 0 1");
        {
            const auto pair = makePair(checkFork, "e5f7", "e5f7", "h8g8");
            const auto cls = makeClass(Classification::Best, checkFork, "e5f7");
            const auto exp = explainMove(checkFork, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::ForksPieces);
            CHECK(exp.checksKing);
            CHECK(exp.secondaryPiece == chess::PieceType::Queen);
            CHECK(exp.secondarySquare == sq("d8"));
        }

        const auto safeFork = position("2r3q1/7k/2r5/5N2/8/8/8/K7 w - - 0 1");
        {
            const auto pair = makePair(safeFork, "f5e7", "f5e7", "c8c7");
            const auto cls = makeClass(Classification::Best, safeFork, "f5e7");
            const auto exp = explainMove(safeFork, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::ForksPieces);
            CHECK(!exp.checksKing);
            CHECK(exp.targetPiece == chess::PieceType::Queen);
            CHECK(exp.square == sq("g8"));
            CHECK(exp.secondaryPiece == chess::PieceType::Rook);
            CHECK(exp.secondarySquare == sq("c6"));
        }

        // Rook on f8 captures f7 knight safely, so Nf7+ is not a valid fork.
        const auto hangingFork = position("3q1r1k/8/8/4N3/8/8/8/K7 w - - 0 1");
        {
            const auto pair = makePair(hangingFork, "e5f7", "e5f7", "f8f7");
            const auto cls = makeClass(Classification::Best, hangingFork, "e5f7");
            CHECK(explainMove(hangingFork, pair, cls, {}).kind != ExplanationKind::ForksPieces);
        }

        // Queen pinned along e-file cannot falsely fork pieces off the e-file.
        const auto pinnedQueen = position("4r2k/8/8/3n1b2/8/8/4Q3/4K3 w - - 0 1");
        {
            const auto pair = makePair(pinnedQueen, "e2e4", "e2e4", "f5e4");
            const auto cls = makeClass(Classification::Best, pinnedQueen, "e2e4");
            CHECK(explainMove(pinnedQueen, pair, cls, {}).kind != ExplanationKind::ForksPieces);
        }
    }

    SECTION("CapturesFreePiece and x-ray rejection") {
        const auto freeQueen = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
        {
            const auto pair = makePair(freeQueen, "d1d5", "d1d5", "h8g8");
            const auto cls = makeClass(Classification::Best, freeQueen, "d1d5");
            const auto exp = explainMove(freeQueen, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::CapturesFreePiece);
            CHECK(exp.targetPiece == chess::PieceType::Queen);
            CHECK(exp.square == sq("d5"));
        }

        // Moving Rd3 to d5 uncovers Black's Rd1 along the d-file.
        const auto xray = position("7k/8/8/3r4/8/3R4/K7/3r4 w - - 0 1");
        {
            const auto pair = makePair(xray, "d3d5", "d3d5", "d1d5");
            const auto cls = makeClass(Classification::Best, xray, "d3d5");
            const auto exp = explainMove(xray, pair, cls, {});
            CHECK(exp.kind != ExplanationKind::CapturesFreePiece);
            CHECK(exp.kind == ExplanationKind::TradesPieces);
        }
    }

    SECTION("WinsExchangeImmediate") {
        const auto pos = position("4r2k/2n5/8/1B6/8/8/8/4K3 w - - 0 1");
        const auto pair = makePair(pos, "b5e8", "b5e8", "c7e8");
        const auto cls = makeClass(Classification::Best, pos, "b5e8");
        const auto exp = explainMove(pos, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::WinsExchangeImmediate);
        CHECK(exp.square == sq("e8"));
    }

    SECTION("CreatesPin rejects blocked rays") {
        const auto ruy = playLine(chess::Position::initial(), {"e2e4", "e7e5", "g1f3", "b8c6"});
        const auto pair = makePair(ruy, "f1b5", "f1b5", "a7a6");
        const auto cls = makeClass(Classification::Best, ruy, "f1b5");
        const auto exp = explainMove(ruy, pair, cls, {});
        CHECK(exp.kind == ExplanationKind::DevelopsPiece);
        CHECK(!exp.central);
    }

    SECTION("SavesPiece distinguishes threatened vs already-safe piece") {
        const auto threatened = position("4k3/8/3p4/4N3/8/8/8/4K3 w - - 0 1");
        {
            const auto pair = makePair(threatened, "e5f3", "e5f3", "e8d8");
            const auto cls = makeClass(Classification::Best, threatened, "e5f3");
            const auto exp = explainMove(threatened, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::SavesPiece);
            CHECK(exp.actor == chess::PieceType::Knight);
        }

        const auto safePiece = position("4k3/8/2n5/4N3/3P4/8/8/4K3 w - - 0 1");
        {
            const auto pair = makePair(safePiece, "e5f3", "e5f3", "c6e5");
            const auto cls = makeClass(Classification::Best, safePiece, "e5f3");
            CHECK(explainMove(safePiece, pair, cls, {}).kind == ExplanationKind::Generic);
        }
    }

    SECTION("CreatesThreat, CreatesDiscoveredAttack, DefendsPiece, EscapesCheck, GivesCheck") {
        const auto threatPos = playLine(chess::Position::initial(), {"e2e4", "g8f6"});
        {
            const auto pair = makePair(threatPos, "e4e5", "e4e5", "f6d5");
            const auto cls = makeClass(Classification::Best, threatPos, "e4e5");
            const auto exp = explainMove(threatPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::CreatesThreat);
            CHECK(exp.targetPiece == chess::PieceType::Knight);
            CHECK(exp.square == sq("f6"));
        }

        const auto discPos = position("4k3/8/8/4q3/8/4N3/4R3/4K3 w - - 0 1");
        {
            const auto pair = makePair(discPos, "e3c2", "e3c2", "e5e2");
            const auto cls = makeClass(Classification::Best, discPos, "e3c2");
            const auto exp = explainMove(discPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::CreatesDiscoveredAttack);
            CHECK(exp.actor == chess::PieceType::Rook);
            CHECK(exp.targetPiece == chess::PieceType::Queen);
            CHECK(exp.square == sq("e5"));
        }

        const auto defendPos = position("4k3/8/5b2/8/8/2N5/8/2B1K3 w - - 0 1");
        {
            const auto pair = makePair(defendPos, "c1d2", "c1d2", "e8d7");
            const auto cls = makeClass(Classification::Best, defendPos, "c1d2");
            const auto exp = explainMove(defendPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::DefendsPiece);
            CHECK(exp.targetPiece == chess::PieceType::Knight);
            CHECK(exp.square == sq("c3"));
        }

        const auto checkPos = position("4r1k1/8/8/8/8/8/3N4/4K3 w - - 0 1");
        {
            const auto pairBlock = makePair(checkPos, "d2e4", "d2e4", "g8f7");
            const auto clsBlock = makeClass(Classification::Best, checkPos, "d2e4");
            const auto expBlock = explainMove(checkPos, pairBlock, clsBlock, {});
            CHECK(expBlock.kind == ExplanationKind::EscapesCheck);
            CHECK(expBlock.blocksCheck);

            const auto pairKing = makePair(checkPos, "e1d1", "e1d1", "e8e2");
            const auto clsKing = makeClass(Classification::Good, checkPos, "e1d1");
            const auto expKing = explainMove(checkPos, pairKing, clsKing, {});
            CHECK(expKing.kind == ExplanationKind::EscapesCheck);
            CHECK(!expKing.blocksCheck);
        }

        const auto giveCheckPos = position("4k3/8/8/8/8/8/2B5/4K3 w - - 0 1");
        {
            const auto pair = makePair(giveCheckPos, "c2a4", "c2a4", "e8d8");
            const auto cls = makeClass(Classification::Best, giveCheckPos, "c2a4");
            const auto exp = explainMove(giveCheckPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::GivesCheck);
        }
    }
}

TEST_CASE("Error, miss, and special/forced restraints are accurately classified",
          "[unit][review][explanation]") {
    SECTION("AllowsMate and MissesMate") {
        const auto allowPos = position("4r1k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1");
        const auto allowPair = makePair(allowPos, "g1h1", "h2h3", "e8e1",
                                        RawScore{ScoreKind::Centipawns, 0, Bound::Exact},
                                        RawScore{ScoreKind::Mate, 1, Bound::Exact});
        const auto allowCls = makeClass(Classification::Blunder, allowPos, "h2h3");
        CHECK(explainMove(allowPos, allowPair, allowCls, {}).kind == ExplanationKind::AllowsMate);

        const auto missPos = position("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1");
        const auto missPair =
            makePair(missPos, "g1f1", "d1d8", "g8f8", RawScore{ScoreKind::Mate, 1, Bound::Exact},
                     RawScore{ScoreKind::Centipawns, 0, Bound::Exact});
        const auto missCls = makeClass(Classification::Miss, missPos, "d1d8");
        CHECK(explainMove(missPos, missPair, missCls, {}).kind == ExplanationKind::MissesMate);
    }

    SECTION("HangsPiece, FailsToSavePiece, LosesPieceImmediate, AllowsExchangeImmediate") {
        const auto hangPos = position("4k3/8/8/8/4q3/8/8/3R2K1 w - - 0 1");
        {
            const auto pair = makePair(hangPos, "d1d4", "d1d2", "e4d4");
            const auto cls = makeClass(Classification::Blunder, hangPos, "d1d2");
            const auto exp = explainMove(hangPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::HangsPiece);
            CHECK(exp.targetPiece == chess::PieceType::Rook);
            CHECK(exp.square == sq("d4"));
        }

        const auto failSavePos = position("4k3/8/8/4q3/8/8/4N3/6K1 w - - 0 1");
        {
            const auto pair = makePair(failSavePos, "g1h1", "e2c3", "e5e2");
            const auto cls = makeClass(Classification::Blunder, failSavePos, "e2c3");
            const auto exp = explainMove(failSavePos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::FailsToSavePiece);
            CHECK(exp.targetPiece == chess::PieceType::Knight);
            CHECK(exp.square == sq("e2"));
        }

        const auto badCapPos = position("4k3/8/5p2/6p1/8/5N2/8/4K3 w - - 0 1");
        {
            const auto pair = makePair(badCapPos, "f3g5", "f3d4", "f6g5");
            const auto cls = makeClass(Classification::Blunder, badCapPos, "f3d4");
            const auto exp = explainMove(badCapPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::LosesPieceImmediate);
            CHECK(exp.targetPiece == chess::PieceType::Knight);
            CHECK(exp.square == sq("g5"));
        }

        const auto exchangePos = position("4k3/8/3b4/R7/3P4/8/8/5K2 w - - 0 1");
        {
            const auto pair = makePair(exchangePos, "a5e5", "a5a2", "d6e5");
            const auto cls = makeClass(Classification::Mistake, exchangePos, "a5a2");
            const auto exp = explainMove(exchangePos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::AllowsExchangeImmediate);
            CHECK(exp.square == sq("e5"));
        }
    }

    SECTION("AllowsFork and AllowsPromotion") {
        const auto allowForkPos = position("4k3/8/8/4n3/8/8/7R/5K2 w - - 0 1");
        {
            const auto pair = makePair(allowForkPos, "f1g1", "h2e2", "e5f3");
            const auto cls = makeClass(Classification::Blunder, allowForkPos, "h2e2");
            const auto exp = explainMove(allowForkPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::AllowsFork);
            CHECK(exp.actor == chess::PieceType::Knight);
            CHECK(exp.square == sq("f3"));
        }

        const auto allowPromoPos = position("4k3/8/8/8/8/8/p7/4K3 w - - 0 1");
        {
            const auto pair = makePair(allowPromoPos, "e1d2", "e1d1", "a2a1q");
            const auto cls = makeClass(Classification::Blunder, allowPromoPos, "e1d1");
            const auto exp = explainMove(allowPromoPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::AllowsPromotion);
            CHECK(exp.square == sq("a1"));
        }
    }

    SECTION("MissesFreePiece, MissesFork, MissesPromotion") {
        const auto missFreePos = position("7k/8/8/3q4/8/8/8/K2R4 w - - 0 1");
        {
            const auto pair = makePair(missFreePos, "a1b1", "d1d5", "h8g8");
            const auto cls = makeClass(Classification::Miss, missFreePos, "d1d5");
            const auto exp = explainMove(missFreePos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::MissesFreePiece);
            CHECK(exp.targetPiece == chess::PieceType::Queen);
            CHECK(exp.square == sq("d5"));
        }

        const auto missForkPos = position("3q3k/8/8/4N3/8/8/8/K7 w - - 0 1");
        {
            const auto pair = makePair(missForkPos, "a1b1", "e5f7", "h8g8");
            const auto cls = makeClass(Classification::Miss, missForkPos, "e5f7");
            const auto exp = explainMove(missForkPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::MissesFork);
            CHECK(exp.checksKing);
            CHECK(exp.secondaryPiece == chess::PieceType::Queen);
        }

        const auto missPromoPos = position("7k/P7/8/8/8/8/8/6K1 w - - 0 1");
        {
            const auto pair = makePair(missPromoPos, "g1f1", "a7a8q", "h8g7");
            const auto cls = makeClass(Classification::Miss, missPromoPos, "a7a8q");
            const auto exp = explainMove(missPromoPos, pair, cls, {});
            CHECK(exp.kind == ExplanationKind::MissesPromotion);
            CHECK(exp.actor == chess::PieceType::Queen);
            CHECK(exp.square == sq("a8"));
        }
    }

    SECTION("Short retained line gains obey label, retention, and horizon bounds") {
        const auto pos = playLine(chess::Position::initial(), {"e2e4", "e7e5"});
        const auto pair = makePair(pos, "a2a3", "g1f3", "g8f6");
        TacticalFact lineLoss;
        lineLoss.kind = TacticalKind::Material;
        lineLoss.source = FactSource::Played;
        lineLoss.beneficiary = chess::Color::Black;
        lineLoss.material = 3;
        lineLoss.linePlies = 5;
        lineLoss.retainedOpportunity = true;

        // Excluded on Inaccuracy and on unretained/deep facts.
        CHECK(explainMove(pos, pair, makeClass(Classification::Inaccuracy, pos, "g1f3"),
                          std::span{&lineLoss, 1})
                  .kind == ExplanationKind::Generic);

        auto deepLoss = lineLoss;
        deepLoss.linePlies = 7;
        CHECK(explainMove(pos, pair, makeClass(Classification::Mistake, pos, "g1f3"),
                          std::span{&deepLoss, 1})
                  .kind == ExplanationKind::Generic);

        auto unretained = lineLoss;
        unretained.retainedOpportunity = false;
        CHECK(explainMove(pos, pair, makeClass(Classification::Mistake, pos, "g1f3"),
                          std::span{&unretained, 1})
                  .kind == ExplanationKind::Generic);

        const auto validLoss = explainMove(
            pos, pair, makeClass(Classification::Mistake, pos, "g1f3"), std::span{&lineLoss, 1});
        CHECK(validLoss.kind == ExplanationKind::LosesMaterialInLine);
        CHECK(validLoss.beneficiary == chess::Color::Black);
    }

    SECTION("Book, Brilliant, Great, and sole legal moves delegate via Generic") {
        const auto pos = position("3q3k/8/8/4N3/8/8/8/K7 w - - 0 1");
        const auto pair = makePair(pos, "e5f7", "e5f7", "h8g8");
        for (const auto label :
             {Classification::Book, Classification::Brilliant, Classification::Great}) {
            CHECK(explainMove(pos, pair, makeClass(label, pos, "e5f7"), {}).kind ==
                  ExplanationKind::Generic);
        }

        const auto solePos = position("7k/5R2/8/8/8/8/8/6K1 b - - 0 1");
        const auto solePair = makePair(solePos, "h8g8", "h8g8", "f7f8");
        const auto soleCls = makeClass(Classification::Best, solePos, "h8g8", "forced.best.v3");
        CHECK(explainMove(solePos, solePair, soleCls, {}).kind == ExplanationKind::Generic);
    }
}
