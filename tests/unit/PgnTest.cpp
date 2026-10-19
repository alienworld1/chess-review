#include "pgn/Pgn.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <latch>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace chess_review;
using namespace chess_review::pgn;

namespace {
template <typename T, typename E> T unwrap(std::variant<T, E> result) {
    if (const auto* failure = std::get_if<E>(&result)) {
        FAIL(failure->message);
    }
    return std::get<T>(std::move(result));
}
PgnDocument document(PgnOutcome outcome) {
    if (const auto* failure = std::get_if<PgnFailure>(&outcome)) {
        INFO(failure->error.location.offset);
        FAIL(failure->error.message);
    }
    REQUIRE(std::holds_alternative<PgnDocument>(outcome));
    return std::get<PgnDocument>(std::move(outcome));
}
PgnDocument parse(std::string_view text, PgnLimits limits = {}) {
    PgnReader reader(text, limits);
    return document(reader.next());
}
PgnFailure failure(PgnOutcome outcome, DiagnosticCode code) {
    REQUIRE(std::holds_alternative<PgnFailure>(outcome));
    auto result = std::get<PgnFailure>(std::move(outcome));
    CHECK(result.error.code == code);
    CHECK(result.error.message.size() < 100);
    return result;
}
PgnFailure reject(std::string_view text, DiagnosticCode code, PgnLimits limits = {}) {
    PgnReader reader(text, limits);
    return failure(reader.next(), code);
}
std::vector<chess::NodeId> mainline(const PgnDocument& doc) {
    return unwrap(doc.tree.mainLine(doc.tree.rootId()));
}
bool warns(const PgnDocument& doc, DiagnosticCode code) {
    return std::any_of(doc.warnings.begin(), doc.warnings.end(),
                       [=](const auto& warning) { return warning.code == code; });
}
void validateTree(const PgnDocument& doc) {
    struct Frame {
        chess::NodeId id;
        std::size_t next{};
    };
    auto position = unwrap(doc.tree.position(doc.tree.rootId()));
    std::vector<Frame> stack{{doc.tree.rootId(), 0}};
    std::size_t visited = 1;
    while (!stack.empty()) {
        auto& frame = stack.back();
        const auto node = *doc.tree.node(frame.id);
        if (frame.next == node.children.size()) {
            if (node.parent) {
                REQUIRE(position.undo().has_value());
            }
            stack.pop_back();
            continue;
        }
        const auto id = node.children[frame.next++];
        const auto child = *doc.tree.node(id);
        REQUIRE(child.parent == node.id);
        REQUIRE(child.move.has_value());
        CHECK(unwrap(position.san(*child.move)) == *child.san);
        unwrap(position.play(*child.move));
        ++visited;
        REQUIRE(visited <= doc.tree.size());
        stack.push_back({id, 0});
    }
    CHECK(visited == doc.tree.size());
}
} // namespace

TEST_CASE("PGN documents own source, tags, results and original played moves", "[pgn][unit]") {
    PgnDocument doc = [] {
        const std::string text = "[Event \"test\"]\r\n[Unknown \"a\\\"b\\\\c\"]\r\n[Result "
                                 "\"1-0\"]\n1.e4 e5 2.Nf3 Nc6 1-0";
        auto parsed = parse(text);
        CHECK(parsed.originalText == text);
        CHECK(parsed.range.begin == 0);
        CHECK(parsed.range.end == text.size());
        return parsed;
    }();
    REQUIRE(doc.tags.size() == 3);
    CHECK(doc.tags[1] == Tag{"Unknown", "a\"b\\c"});
    CHECK(doc.result == GameResult::WhiteWin);
    CHECK(doc.originalMainline().size() == 4);
    CHECK(doc.warnings.empty());
    const std::vector<chess::Move> original(doc.originalMainline().begin(),
                                            doc.originalMainline().end());
    const auto line = mainline(doc);
    unwrap(doc.tree.removeSubtree(line[1]));
    CHECK(doc.tree.size() == 1);
    CHECK(std::vector<chess::Move>(doc.originalMainline().begin(), doc.originalMainline().end()) ==
          original);
}

TEST_CASE("PGN preserves nested alternatives, comments, numeric and symbolic NAGs", "[pgn][unit]") {
    auto doc = parse("{intro} 1.e4! {after} e5 ( {Sicilian} 1...c5 $255 $255 2.Nf3 "
                     "(2.Nc3?! {nested}) d6) 2.Nf3 ;line comment\n Nc6 *");
    const auto line = mainline(doc);
    REQUIRE(line.size() == 5);
    const auto e4 = *doc.tree.node(line[1]);
    CHECK(e4.annotations.commentBefore == "intro");
    CHECK(e4.annotations.commentAfter == "after");
    CHECK(e4.annotations.nags == std::vector<std::uint8_t>{1});
    REQUIRE(e4.children.size() == 2);
    CHECK(doc.tree.node(e4.children[0])->san == "e5");
    const auto c5 = *doc.tree.node(e4.children[1]);
    CHECK(c5.san == "c5");
    CHECK(c5.annotations.commentBefore == "Sicilian");
    CHECK(c5.annotations.nags == std::vector<std::uint8_t>{255, 255});
    REQUIRE(c5.children.size() == 2);
    CHECK(doc.tree.node(c5.children[1])->annotations.nags == std::vector<std::uint8_t>{6});
    CHECK(doc.tree.node(line[3])->annotations.commentAfter == "line comment");
    CHECK(doc.originalMainline().size() == 4);
    validateTree(doc);
}

TEST_CASE("PGN repeated branches merge in mainline first order without losing annotations",
          "[pgn][unit]") {
    auto doc = parse("1.e4 {first} (1.e4 {second} c5 $2) e5 (1...c5 $1) 2.Nf3 *");
    CHECK(warns(doc, DiagnosticCode::MergedBranch));
    const auto line = mainline(doc);
    REQUIRE(line.size() == 4);
    const auto e4 = *doc.tree.node(line[1]);
    CHECK(e4.annotations.commentAfter == "first\nsecond");
    REQUIRE(e4.children.size() == 2);
    CHECK(doc.tree.node(e4.children[0])->san == "e5");
    CHECK(doc.tree.node(e4.children[1])->san == "c5");
    CHECK(doc.tree.node(e4.children[1])->annotations.nags == std::vector<std::uint8_t>{2, 1});
    CHECK(doc.originalMainline().size() == 3);
    validateTree(doc);
}

TEST_CASE("PGN multi game reader isolates failures and reports source locations", "[pgn][unit]") {
    const std::string text =
        "1.e4 *\r\n[Event \"bad\"]\r\n1.e5 { [Event \"fake\"] * } *\r\n[Event \"good\"] 1.d4 *";
    PgnReader reader(text);
    auto first = document(reader.next());
    CHECK(first.ordinal == 1);
    const auto failed = failure(reader.next(), DiagnosticCode::IllegalMove);
    CHECK(failed.ordinal == 2);
    CHECK(!failed.unreadableTail);
    CHECK(failed.error.location.line == 3);
    CHECK(failed.error.location.column == 3);
    CHECK(failed.error.location.offset == text.find("e5"));
    CHECK(failed.error.ply == 1);
    const auto third = document(reader.next());
    CHECK(third.ordinal == 3);
    CHECK(third.tags[0].value == "good");
    CHECK(first.originalText == "1.e4 *");
    CHECK(std::holds_alternative<EndOfInput>(reader.next()));
    CHECK(std::holds_alternative<EndOfInput>(reader.next()));
}

TEST_CASE("PGN recovery follows variation nesting and treats lexical damage as unreadable",
          "[pgn][unit]") {
    const std::string text = "1.e4 e5 (1...e4 {fake * [Event \"x\"]} (1...c5)) 2.Nf3 * 1.d4 *";
    PgnReader reader(text);
    CHECK(!failure(reader.next(), DiagnosticCode::IllegalMove).unreadableTail);
    CHECK(document(reader.next()).originalMainline().size() == 1);
    for (const auto damaged : {"1.e4 {unterminated [Event \"fake\"] 1.d4 *", "1.e4 (1.d4 *",
                               "1.e4 ) * 1.d4 *", "[Event \"unterminated] 1.e4 *"}) {
        PgnReader broken(damaged);
        REQUIRE(std::holds_alternative<PgnFailure>(broken.next()));
        CHECK(std::holds_alternative<EndOfInput>(broken.next()));
    }
    PgnReader tags(
        "[Event \"a\"] [Event \"b\"] [Site \"same game\"] 1.e4 * [Event \"next\"] 1.d4 *");
    CHECK(!failure(tags.next(), DiagnosticCode::ConflictingTag).unreadableTail);
    CHECK(document(tags.next()).tags.front().value == "next");
}

TEST_CASE("PGN rejects illegal variations without publishing partial documents", "[pgn][unit]") {
    reject("1.e4 e5 (1...e4) 2.Nf3 *", DiagnosticCode::IllegalMove);
    reject("1.e4 () *", DiagnosticCode::InvalidSyntax);
    reject("(1.e4) *", DiagnosticCode::InvalidSyntax);
    reject("1.e4 (1.d4 1-0) *", DiagnosticCode::InvalidSyntax);
    reject("1.e4 -- *", DiagnosticCode::IllegalMove);
    reject("1.e4 $256 *", DiagnosticCode::InvalidNag);
    reject("1.e4 $ *", DiagnosticCode::InvalidNag);
    reject("1.e4 !!! *", DiagnosticCode::InvalidNag);
    reject("$1 1.e4 *", DiagnosticCode::InvalidSyntax);
}

TEST_CASE("PGN supports missing headers and termination without inventing results", "[pgn][unit]") {
    auto doc = parse("1.e4 e5");
    CHECK(doc.result == GameResult::Unknown);
    CHECK(warns(doc, DiagnosticCode::MissingTermination));
    CHECK(parse("[Result \"0-1\"] 1.e4").result == GameResult::BlackWin);
    reject("[Result \"0-1\"] 1.e4 1-0", DiagnosticCode::ConflictingResult);
    reject("[Result \"bad\"] 1.e4 *", DiagnosticCode::InvalidTag);
    PgnReader reader("1.e4\n[Event \"next\"] 1.d4 *");
    CHECK(warns(document(reader.next()), DiagnosticCode::MissingTermination));
    CHECK(document(reader.next()).tags.front().value == "next");
    auto empty = parse("{empty game} *");
    CHECK(empty.tree.node(empty.tree.rootId())->annotations.commentBefore == "empty game");
    CHECK(empty.originalMainline().empty());
    PgnReader noGames(" \r\n% ignored\r\n\t");
    CHECK(std::holds_alternative<EndOfInput>(noGames.next()));
}

TEST_CASE("PGN tags and move numbers follow explicit compatibility policies", "[pgn][unit]") {
    auto duplicate = parse("[Event \"a\"] [Event \"a\"] 7...e4 *");
    CHECK(duplicate.tags.size() == 2);
    CHECK(warns(duplicate, DiagnosticCode::DuplicateTag));
    CHECK(warns(duplicate, DiagnosticCode::MoveNumberMismatch));
    reject("[Event \"a\"] [Event \"b\"] *", DiagnosticCode::ConflictingTag);
    reject("[Event \"bad\\q\"] *", DiagnosticCode::InvalidTag);
    reject("[Variant \"Chess960\"] 1.e4 *", DiagnosticCode::UnsupportedVariant);
    CHECK(parse("[Variant \"Standard\"] 1 . e4 *").originalMainline().size() == 1);
    const auto glyphs = parse("1.e4! e5? 2.Nf3!! Nc6?? 3.Bb5!? a6?! *");
    const auto line = mainline(glyphs);
    for (std::size_t i = 1; i < line.size(); ++i) {
        CHECK(glyphs.tree.node(line[i])->annotations.nags ==
              std::vector<std::uint8_t>{static_cast<std::uint8_t>(i)});
    }
}

TEST_CASE("PGN FEN roots preserve unknown history and enforce setup consistency", "[pgn][unit]") {
    const std::string fen = "8/8/8/8/8/2k5/8/4K3 b - - 0 7";
    auto doc = parse("[FEN \"" + fen + "\"] 7...Kd4 *");
    CHECK(warns(doc, DiagnosticCode::MissingSetup));
    const auto root = unwrap(doc.tree.position(doc.tree.rootId()));
    CHECK(root.context().provenance == chess::RootProvenance::FenAnalysisRoot);
    CHECK(root.fullmoveNumber() == 7);
    CHECK(!warns(doc, DiagnosticCode::MoveNumberMismatch));
    reject("[FEN \"" + fen + "\"] [SetUp \"0\"] *", DiagnosticCode::InvalidSetup);
    reject("[SetUp \"1\"] *", DiagnosticCode::InvalidSetup);
    reject("[SetUp \"2\"] *", DiagnosticCode::InvalidSetup);
    reject("[FEN \"bad\"] [SetUp \"1\"] *", DiagnosticCode::InvalidFen);
    CHECK(parse("[SetUp \"0\"] *").tree.size() == 1);
}

TEST_CASE("PGN notation handles castles, en passant and underpromotion strictly", "[pgn][unit]") {
    const auto castled = parse("1.e4 e5 2.Nf3 Nc6 3.Bc4 Nf6 4.0-0 *");
    CHECK(castled.tree.node(mainline(castled).back())->san == "O-O");
    validateTree(castled);
    validateTree(parse("1.e4 a6 2.e5 d5 3.exd6 *"));
    validateTree(parse("[FEN \"8/P7/8/8/8/2k5/8/4K3 w - - 0 1\"] [SetUp \"1\"] 1.a8=N *"));
    reject("1.f3 e5 2.g4 Qh4 *", DiagnosticCode::IllegalMove);
    CHECK(parse("1.f3 e5 2.g4 Qh4# *").originalMainline().size() == 4);
}

TEST_CASE("PGN validates UTF-8 and treats commands and percent escapes as data", "[pgn][unit]") {
    const std::string valid = "\xef\xbb\xbf% ignore [Event \"fake\"]\n[White \"\xc3\xa9\"]\n1.e4 "
                              "{[%eval 7] <script> \xf0\x9f\x98\x80} *";
    const auto doc = parse(valid);
    CHECK(doc.range.begin == valid.find('[', valid.find('\n')));
    CHECK(doc.tags.front().value == "\xc3\xa9");
    CHECK(doc.tree.node(mainline(doc)[1])->annotations.commentAfter ==
          "[%eval 7] <script> \xf0\x9f\x98\x80");
    for (const auto& invalid :
         {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
          std::string("\xe2\x82"), std::string(1, '\0'), std::string(1, '\x7f')}) {
        reject("1.e4 {" + invalid + "} *", DiagnosticCode::InvalidEncoding);
    }
    PgnReader reader("1.e4 {\xff} * 1.d4 *");
    CHECK(!failure(reader.next(), DiagnosticCode::InvalidEncoding).unreadableTail);
    CHECK(document(reader.next()).originalMainline().size() == 1);
}

TEST_CASE("PGN enforces input, game, token and tree budgets including merged occurrences",
          "[pgn][unit]") {
    PgnLimits limits;
    limits.maxInputBytes = 6;
    CHECK(parse("1.e4 *", limits).tree.size() == 2);
    reject("1.e4 * ", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxGameBytes = 6;
    CHECK(parse("1.e4 *", limits).tree.size() == 2);
    reject("1.e4  *", DiagnosticCode::LimitExceeded, limits);
    reject("{1234567}", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxTags = 1;
    parse("[A \"x\"] *", limits);
    reject("[A \"x\"] [B \"y\"] *", DiagnosticCode::LimitExceeded, limits);
    limits.maxTagValueBytes = 1;
    reject("[A \"xx\"] *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxNodes = 3;
    parse("1.e4 (1.e4) *", limits);
    reject("1.e4 (1.e4) (1.e4) *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxDepth = 1;
    parse("1.e4 *", limits);
    reject("1.e4 e5 *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxVariationDepth = 1;
    parse("1.e4 (1.d4) *", limits);
    reject("1.e4 (1.d4 (1.c4)) *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxCommentBytes = 3;
    parse("1.e4 {abc} *", limits);
    reject("1.e4 {abcd} *", DiagnosticCode::LimitExceeded, limits);
    reject("1.e4 {ab} (1.e4 {c}) *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxAnnotationBytes = 3;
    parse("1.e4 {ab} $1 *", limits);
    reject("1.e4 {ab} $1 $2 *", DiagnosticCode::LimitExceeded, limits);
    reject("1.e4 {a} (1.e4 {b}) $1 *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxDiagnostics = 0;
    auto suppressed = parse("9.e4 9...e5", limits);
    CHECK(suppressed.warnings.empty());
    CHECK(suppressed.suppressedWarnings == 3);
    limits.maxGames = 1;
    PgnReader reader("1.e4 * 1.d4 *", limits);
    document(reader.next());
    CHECK(failure(reader.next(), DiagnosticCode::LimitExceeded).unreadableTail);
    CHECK(std::holds_alternative<EndOfInput>(reader.next()));
    limits.tree.maxNodes = 0;
    reject("*", DiagnosticCode::InvalidLimits, limits);
}

TEST_CASE("PGN cancellation is terminal and does not invalidate earlier documents", "[pgn][unit]") {
    std::stop_source stop;
    PgnReader reader("1.e4 * 1.d4 *", {}, stop.get_token());
    const auto first = document(reader.next());
    stop.request_stop();
    CHECK(std::holds_alternative<Cancelled>(reader.next()));
    CHECK(std::holds_alternative<Cancelled>(reader.next()));
    CHECK(first.originalMainline().size() == 1);
    std::stop_source concurrentStop;
    const std::string longComment = "1.e4 {" + std::string(4 * 1024 * 1024, 'x') + "} *";
    PgnLimits limits;
    limits.tree.maxCommentBytes = longComment.size();
    std::latch started(1);
    PgnOutcome outcome = EndOfInput{};
    std::jthread worker([&] {
        PgnReader working(longComment, limits, concurrentStop.get_token());
        started.count_down();
        outcome = working.next();
    });
    started.wait();
    concurrentStop.request_stop();
    worker.join();
    // Completion may win the race with the stop request on a busy scheduler.
    // Either cancellation or an entirely owned, complete document is valid.
    if (const auto* completed = std::get_if<PgnDocument>(&outcome)) {
        CHECK(completed->originalText == longComment);
        CHECK(completed->originalMainline().size() == 1);
    } else {
        CHECK(std::holds_alternative<Cancelled>(outcome));
    }
}

TEST_CASE("PGN bulk construction handles long lines and nested branches iteratively",
          "[pgn][unit]") {
    std::string text;
    for (int i = 0; i < 3000; ++i) {
        text += "Nf3 Nf6 Ng1 Ng8 ";
    }
    text += '*';
    auto doc = parse(text);
    CHECK(doc.tree.size() == 12'001);
    CHECK(doc.originalMainline().size() == 12'000);
    validateTree(doc);
    std::string nested = "1.e4";
    for (int i = 0; i < 100; ++i) {
        nested += " (1.e4";
    }
    nested += std::string(100, ')') + " *";
    doc = parse(nested);
    CHECK(doc.tree.size() == 2);
    CHECK(doc.warnings.size() == 100);
}

TEST_CASE("PGN deterministic byte mutations terminate with bounded coherent outcomes",
          "[pgn][unit]") {
    std::mt19937 random(0x50474e);
    const std::string seed =
        "[Event \"test\"] 1.e4 {note} e5 (1...c5 $1) 2.Nf3 * [Event \"next\"] 1.d4 *";
    for (int iteration = 0; iteration < 1500; ++iteration) {
        std::string text = seed;
        const auto index = static_cast<std::size_t>(random()) % text.size();
        switch (iteration % 3) {
        case 0:
            text[index] = static_cast<char>(random() % 256);
            break;
        case 1:
            text.erase(index, 1);
            break;
        default:
            text.insert(index, 1, static_cast<char>(random() % 256));
            break;
        }
        PgnReader reader(text);
        std::size_t attempts = 0;
        while (true) {
            auto outcome = reader.next();
            if (std::holds_alternative<EndOfInput>(outcome)) {
                break;
            }
            REQUIRE(++attempts <= text.size() + 1);
            if (const auto* doc = std::get_if<PgnDocument>(&outcome)) {
                REQUIRE(doc->range.begin <= doc->range.end);
                REQUIRE(doc->range.end <= text.size());
                CHECK(doc->originalText ==
                      text.substr(doc->range.begin, doc->range.end - doc->range.begin));
                validateTree(*doc);
            } else {
                REQUIRE(std::holds_alternative<PgnFailure>(outcome));
                const auto& failed = std::get<PgnFailure>(outcome);
                CHECK(failed.error.message.size() < 100);
                CHECK(failed.range.begin <= failed.range.end);
                CHECK(failed.range.end <= text.size());
                CHECK(failed.error.location.offset <= text.size());
            }
        }
    }
}

TEST_CASE("PGN budgets handle zero values, escaped tag bytes and next header lookahead",
          "[pgn][unit]") {
    PgnLimits limits;
    limits.maxGameBytes = 6;
    PgnReader split("1.e4\n[A \"longer than previous game\"] *", limits);
    CHECK(warns(document(split.next()), DiagnosticCode::MissingTermination));
    failure(split.next(), DiagnosticCode::LimitExceeded);
    limits = {};
    limits.maxTagValueBytes = 1;
    CHECK(parse("[A \"\\\"\"] *", limits).tags[0].value == "\"");
    limits.maxTagValueBytes = 0;
    CHECK(parse("[A \"\"] *", limits).tags[0].value.empty());
    reject("[A \"x\"] *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxTags = 0;
    parse("*", limits);
    reject("[A \"\"] *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxVariationDepth = 0;
    parse("e4*", limits);
    reject("e4 (d4)*", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxDepth = 0;
    parse("*", limits);
    reject("e4*", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxCommentBytes = 0;
    parse("{} *", limits);
    reject("{x} *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxAnnotationBytes = 0;
    parse("{} e4 *", limits);
    reject("e4 $0 *", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxGames = 0;
    reject("*", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.maxGameBytes = 0;
    reject("*", DiagnosticCode::InvalidLimits, limits);
    limits = {};
    limits.maxInputBytes = 0;
    reject("*", DiagnosticCode::InvalidLimits, limits);
    limits = {};
    limits.maxDiagnostics = 1;
    const auto doc = parse("9.e4 9...e5", limits);
    CHECK(doc.warnings.size() == 1);
    CHECK(doc.suppressedWarnings == 2);
    reject("% truncated UTF-8 \xe2\x82", DiagnosticCode::InvalidEncoding);
    CHECK(parse("1.e4*1.d4*").originalMainline().size() == 1);
}

TEST_CASE("PGN broad trees preserve all legal root choices with bounded duplicate warnings",
          "[pgn][unit]") {
    const auto root = chess::Position::initial();
    std::string text = "1.e4 ";
    for (const auto& move : root.legalMoves()) {
        text += "(1." + unwrap(root.san(move)) + ") ";
    }
    for (int i = 0; i < 1000; ++i) {
        text += "(1.d4) ";
    }
    text += '*';
    const auto doc = parse(text);
    CHECK(doc.tree.size() == 21);
    CHECK(doc.tree.node(doc.tree.rootId())->children.size() == 20);
    CHECK(doc.warnings.size() == 100);
    CHECK(doc.suppressedWarnings == 901);
    validateTree(doc);
}

TEST_CASE("PGN header recovery retains errors in preceding ignored text", "[pgn][unit]") {
    PgnReader encoding("1.e4\n% \xff\n[Event \"next\"] 1.d4 *");
    const auto rejected = failure(encoding.next(), DiagnosticCode::InvalidEncoding);
    CHECK(!rejected.unreadableTail);
    CHECK(rejected.error.location.line == 2);
    CHECK(document(encoding.next()).tags.front().value == "next");
    PgnLimits limits;
    limits.maxGameBytes = 6;
    PgnReader whitespace("e4       [A \"\"]*", limits);
    CHECK(!failure(whitespace.next(), DiagnosticCode::LimitExceeded).unreadableTail);
    // The following game also exceeds this deliberately tiny byte budget, but it
    // must receive its own outcome instead of being consumed during recovery.
    CHECK(failure(whitespace.next(), DiagnosticCode::LimitExceeded).ordinal == 2);
}

TEST_CASE("PGN recovery cannot interpret quoted unsupported text as a game header", "[pgn][unit]") {
    PgnReader reader("1.e5 \"[Event \\\"fake\\\"] *\" * [Event \"real\"] 1.e4 *");
    CHECK(!failure(reader.next(), DiagnosticCode::IllegalMove).unreadableTail);
    CHECK(document(reader.next()).tags.front().value == "real");
    PgnReader broken("1.e5 \"unterminated [Event fake] *");
    CHECK(failure(broken.next(), DiagnosticCode::IllegalMove).unreadableTail);
    CHECK(std::holds_alternative<EndOfInput>(broken.next()));
}

TEST_CASE("PGN move number indications require a following move", "[pgn][unit]") {
    for (const auto text : {"1.", "1. *", "1.e4 1... *", "1.e4 (1.d4 1...) *", "1.e4 1...! e5 *"}) {
        reject(text, DiagnosticCode::InvalidSyntax);
    }
    PgnReader reader("1.e4 1...\n[Event \"next\"] 1.d4 *");
    CHECK(!failure(reader.next(), DiagnosticCode::InvalidSyntax).unreadableTail);
    CHECK(document(reader.next()).tags.front().value == "next");
}

TEST_CASE("PGN import accepts self delimiting tags and token gap escapes", "[pgn][unit]") {
    const std::string source =
        "[Event\"compact\"][Site\n% ignored [Event \"fake\"]\n\"here\"] 1.e4*";
    const auto doc = parse(source);
    REQUIRE(doc.tags.size() == 2);
    CHECK(doc.tags[0] == Tag{"Event", "compact"});
    CHECK(doc.tags[1] == Tag{"Site", "here"});
    CHECK(doc.originalText == source);
    CHECK(doc.warnings.empty());
    const auto beforeBracket = parse("[Event \"x\"\n% ignored ] \"\n] e4 *");
    CHECK(beforeBracket.tags[0].value == "x");
}

TEST_CASE("PGN import accepts separated periods and surplus correct move numbers", "[pgn][unit]") {
    for (const auto text :
         {"1 . e4 1 . . . e5 *", "1.e4 1 e5 *", "1.1.e4 1...1...e5 *", "1\n%ignored\n. e4 *"}) {
        const auto doc = parse(text);
        CHECK(doc.warnings.empty());
        validateTree(doc);
    }
    CHECK(warns(parse("1.2.e4 *"), DiagnosticCode::MoveNumberMismatch));
}

TEST_CASE("PGN castling cleanup accepts only the documented zero spellings", "[pgn][unit]") {
    const std::string header = "[FEN \"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1\"] [SetUp \"1\"] ";
    for (const auto san : {"0-0", "0-0-0", "O-O", "O-O-O"}) {
        CHECK(parse(header + san + " *").originalMainline().size() == 1);
    }
    for (const auto san : {"0-0-O", "O-0-0", "0-O-O", "O-O-0", "0-O", "O-0"}) {
        reject(header + san + " *", DiagnosticCode::IllegalMove);
    }
}

TEST_CASE("PGN original played line may end before a merged analysis continuation", "[pgn][unit]") {
    const auto doc = parse("1.e4 (1.e4 e5) *");
    CHECK(warns(doc, DiagnosticCode::MergedBranch));
    CHECK(doc.originalMainline().size() == 1);
    CHECK(mainline(doc).size() == 3);
    validateTree(doc);
}

TEST_CASE("PGN result boundaries respect reserved self delimiting tokens", "[pgn][unit]") {
    for (const auto suffix : {".", "<future>", ">future<"}) {
        const std::string source = std::string("1.e4 1-0") + suffix;
        PgnReader reader(source);
        CHECK(document(reader.next()).result == GameResult::WhiteWin);
        failure(reader.next(), DiagnosticCode::InvalidSyntax);
    }
}

TEST_CASE("PGN preserves comments between header and move number tokens", "[pgn][unit]") {
    const std::string source = "[{a}Event{b}\"x\"{c}] 1{d}.{e}e4 1.{f}.{g}.e5*";
    const auto doc = parse(source);
    CHECK(doc.tags.front().value == "x");
    const auto line = mainline(doc);
    CHECK(doc.tree.node(line[1])->annotations.commentBefore == "a\nb\nc\nd\ne");
    CHECK(doc.tree.node(line[1])->annotations.commentAfter == "f\ng");
    CHECK(doc.warnings.empty());
    CHECK(doc.originalText == source);
    PgnLimits limits;
    limits.tree.maxCommentBytes = 3;
    reject("[Event{abcd}\"x\"]*", DiagnosticCode::LimitExceeded, limits);
    limits = {};
    limits.tree.maxAnnotationBytes = 3;
    reject("[Event{abcd}\"x\"]*", DiagnosticCode::LimitExceeded, limits);
    PgnReader broken("[Event {unterminated [Site \"fake\"] *");
    CHECK(failure(broken.next(), DiagnosticCode::InvalidSyntax).unreadableTail);
    PgnReader multiple("e4\n[Event{intro}\"next\"] d4*");
    CHECK(document(multiple.next()).originalMainline().size() == 1);
    const auto next = document(multiple.next());
    CHECK(next.tree.node(mainline(next)[1])->annotations.commentBefore == "intro");
}
