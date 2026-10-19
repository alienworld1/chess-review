#include "pgn/Pgn.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <system_error>
#include <utility>

namespace chess_review::pgn {
namespace {
using chess::GameTreeInputNode;
using chess::Move;
using chess::NodeAnnotations;
using chess::Position;

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool tagLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '_';
}
Diagnostic diagnostic(DiagnosticCode code, const char* message, SourceLocation at,
                      std::optional<std::size_t> ply = std::nullopt) {
    return {code, message, at, ply};
}
enum class Kind { End, Tag, Comment, Number, Symbol, Nag, Glyph, Open, Close, Invalid };
struct Token {
    Kind kind{Kind::End};
    SourceLocation at;
    std::string_view text;
    std::string_view value;
    std::size_t dots{};
    std::optional<Diagnostic> error;
    bool unsafe{};
};

// The scanner tracks lexical state even while skipping a failed game. Recovery never
// searches raw bytes for a header that could actually be inside a string or comment.
class Lexer {
  public:
    Lexer(std::string_view bytes, SourceLocation at, const PgnLimits& limits, std::stop_token stop)
        : bytes_(bytes), at_(at), limits_(limits), stop_(stop) {}
    SourceLocation location() const { return at_; }
    bool cancelled() const { return stop_.stop_requested(); }
    void rewind(SourceLocation at) { at_ = at; }
    void setGameStart(std::size_t start) { gameStart_ = start; }
    Token next() {
        problem_.reset();
        remaining_ = 0;
        while (!ended()) {
            if (space(current())) {
                advance();
            } else if (current() == '%' &&
                       (at_.column == 1 ||
                        (at_.offset == 3 && bytes_.starts_with("\xef\xbb\xbf")))) {
                while (!ended() && current() != '\n' && current() != '\r') {
                    advance();
                }
            } else {
                break;
            }
        }
        Token token;
        token.at = at_;
        const auto begin = at_.offset;
        if (ended()) {
            if (remaining_ != 0) {
                fail(DiagnosticCode::InvalidEncoding, "Invalid UTF-8 input.", at_);
            }
            token.error = problem_;
            return token;
        }
        const char c = current();
        advance();
        if (c == '{' || c == ';') {
            token.kind = Kind::Comment;
            const auto content = at_.offset;
            while (!ended() &&
                   (c == '{' ? current() != '}' : current() != '\n' && current() != '\r')) {
                advance();
            }
            token.text = bytes_.substr(content, at_.offset - content);
            if (c == '{') {
                if (ended()) {
                    fail(DiagnosticCode::InvalidSyntax, "Unterminated comment.", token.at);
                    token.unsafe = true;
                } else {
                    advance();
                }
            }
        } else if (c == '[') {
            token.kind = Kind::Tag;
            skipSpace();
            const auto name = at_.offset;
            while (!ended() && tagLetter(current())) {
                advance();
            }
            token.text = bytes_.substr(name, at_.offset - name);
            const bool separated = !ended() && space(current());
            skipSpace();
            if (token.text.empty() || !separated || ended() || current() != '"') {
                fail(DiagnosticCode::InvalidTag, "Malformed tag pair.", token.at);
                token.unsafe = true;
            } else {
                advance();
                const auto value = at_.offset;
                while (!ended() && current() != '"') {
                    if (current() == '\\') {
                        advance();
                        if (ended() || (current() != '\\' && current() != '"')) {
                            fail(DiagnosticCode::InvalidTag, "Invalid tag escape.", at_);
                        }
                    } else if (static_cast<unsigned char>(current()) < 32) {
                        fail(DiagnosticCode::InvalidTag, "Control byte in tag value.", at_);
                    }
                    if (!ended()) {
                        advance();
                    }
                }
                token.value = bytes_.substr(value, at_.offset - value);
                if (ended()) {
                    fail(DiagnosticCode::InvalidTag, "Unterminated tag string.", token.at);
                    token.unsafe = true;
                } else {
                    advance();
                    skipSpace();
                    if (ended() || current() != ']') {
                        fail(DiagnosticCode::InvalidTag, "Missing tag closing bracket.", token.at);
                        token.unsafe = true;
                    } else {
                        advance();
                    }
                }
            }
        } else if (c == '"') {
            // Strings outside tag pairs are unsupported, but still consume their
            // complete lexical extent so recovery cannot see a header inside one.
            token.kind = Kind::Invalid;
            fail(DiagnosticCode::InvalidSyntax, "String outside tag pair.", token.at);
            while (!ended() && current() != '"') {
                if (current() == '\\') {
                    advance();
                }
                if (!ended()) {
                    advance();
                }
            }
            if (ended()) {
                token.unsafe = true;
            } else {
                advance();
            }
        } else if (c == '*') {
            token.kind = Kind::Symbol;
            token.text = bytes_.substr(begin, 1);
        } else if (c == '(' || c == ')') {
            token.kind = c == '(' ? Kind::Open : Kind::Close;
        } else if (c == '$') {
            token.kind = Kind::Nag;
            const auto value = at_.offset;
            while (!ended() && digit(current())) {
                advance();
            }
            token.text = bytes_.substr(value, at_.offset - value);
        } else if (c == '!' || c == '?') {
            token.kind = Kind::Glyph;
            while (!ended() && (current() == '!' || current() == '?')) {
                advance();
            }
            token.text = bytes_.substr(begin, at_.offset - begin);
        } else {
            token.kind = Kind::Symbol;
            if (digit(c)) {
                while (!ended() && digit(current())) {
                    advance();
                }
                if (ended() || space(current()) || current() == '.' || delimiter(current())) {
                    token.kind = Kind::Number;
                    token.text = bytes_.substr(begin, at_.offset - begin);
                    skipSpace();
                    while (!ended() && current() == '.') {
                        ++token.dots;
                        advance();
                    }
                }
            }
            if (token.kind == Kind::Symbol) {
                while (!ended() && !space(current()) && !delimiter(current())) {
                    advance();
                }
                token.text = bytes_.substr(begin, at_.offset - begin);
            }
            if (c == ']' || c == '}' || c == '"' || c == '.' || c == '\\') {
                fail(DiagnosticCode::InvalidSyntax, "Unexpected PGN token.", token.at);
            }
        }
        if (remaining_ != 0) {
            fail(DiagnosticCode::InvalidEncoding, "Invalid UTF-8 input.", token.at);
        }
        token.error = problem_;
        return token;
    }

  private:
    bool ended() const { return at_.offset == bytes_.size() || cancelled(); }
    char current() const { return bytes_[at_.offset]; }
    static bool delimiter(char c) {
        return c == '*' || c == '[' || c == ']' || c == '{' || c == '}' || c == '(' || c == ')' ||
               c == ';' || c == '$' || c == '!' || c == '?' || c == '"';
    }
    void fail(DiagnosticCode code, const char* message, SourceLocation at) {
        if (!problem_) {
            problem_ = diagnostic(code, message, at);
        }
    }
    void skipSpace() {
        while (!ended() && space(current())) {
            advance();
        }
    }
    void advance() {
        const auto c = static_cast<unsigned char>(current());
        if (remaining_ != 0) {
            if (c < continuationMin_ || c > continuationMax_) {
                fail(DiagnosticCode::InvalidEncoding, "Invalid UTF-8 input.", at_);
                remaining_ = 0;
            } else {
                --remaining_;
            }
            continuationMin_ = 0x80;
            continuationMax_ = 0xbf;
        } else if (c >= 0x80) {
            continuationMin_ = 0x80;
            continuationMax_ = 0xbf;
            if (c >= 0xc2 && c <= 0xdf) {
                remaining_ = 1;
            } else if (c >= 0xe0 && c <= 0xef) {
                remaining_ = 2;
                if (c == 0xe0) {
                    continuationMin_ = 0xa0;
                }
                if (c == 0xed) {
                    continuationMax_ = 0x9f;
                }
            } else if (c >= 0xf0 && c <= 0xf4) {
                remaining_ = 3;
                if (c == 0xf0) {
                    continuationMin_ = 0x90;
                }
                if (c == 0xf4) {
                    continuationMax_ = 0x8f;
                }
            } else {
                fail(DiagnosticCode::InvalidEncoding, "Invalid UTF-8 input.", at_);
            }
        } else if (c == 0 || (c < 32 && !space(static_cast<char>(c))) || c == 127) {
            fail(DiagnosticCode::InvalidEncoding, "Forbidden control byte.", at_);
        }
        const auto previous = at_.offset;
        ++at_.offset;
        if (c == '\r') {
            ++at_.line;
            at_.column = 1;
        } else if (c == '\n') {
            if (previous == 0 || bytes_[previous - 1] != '\r') {
                ++at_.line;
            }
            at_.column = 1;
        } else {
            ++at_.column;
        }
        if (gameStart_ && at_.offset - *gameStart_ > limits_.maxGameBytes) {
            fail(DiagnosticCode::LimitExceeded, "Game byte budget exceeded.", at_);
        }
    }
    std::string_view bytes_;
    SourceLocation at_;
    const PgnLimits& limits_;
    std::stop_token stop_;
    std::optional<std::size_t> gameStart_;
    std::optional<Diagnostic> problem_;
    int remaining_{};
    unsigned char continuationMin_{0x80};
    unsigned char continuationMax_{0xbf};
};

std::optional<GameResult> resultOf(std::string_view text) {
    if (text == "*") {
        return GameResult::Unknown;
    }
    if (text == "1-0") {
        return GameResult::WhiteWin;
    }
    if (text == "0-1") {
        return GameResult::BlackWin;
    }
    if (text == "1/2-1/2") {
        return GameResult::Draw;
    }
    return std::nullopt;
}
std::size_t decodedSize(std::string_view text) {
    std::size_t size = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        }
        ++size;
    }
    return size;
}
std::string unescape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        }
        result.push_back(text[i]);
    }
    return result;
}
struct Occurrence {
    GameTreeInputNode node;
    std::vector<std::size_t> children;
    SourceLocation at;
    std::size_t depth{};
};

class Parser {
  public:
    Parser(Lexer& lexer, std::string_view bytes, const PgnLimits& limits, std::stop_token stop,
           std::size_t ordinal)
        : lexer_(lexer), bytes_(bytes), limits_(limits), stop_(stop), ordinal_(ordinal) {}
    PgnOutcome parse(Token token) {
        start_ = token.at;
        lexer_.setGameStart(start_.offset);
        occurrences_.push_back({{}, {}, start_, 0});
        while (true) {
            if (lexer_.cancelled()) {
                return Cancelled{};
            }
            // Header lookahead belongs to the next game, including any scanner error
            // within that header. Do not charge its bytes to this unterminated game.
            if (token.kind == Kind::Tag && movesStarted_ && frames_.empty() && !token.unsafe) {
                const auto previousError =
                    token.error && token.error->location.offset < token.at.offset ? token.error
                                                                                  : std::nullopt;
                lexer_.rewind(token.at);
                if (previousError) {
                    return PgnFailure{
                        ordinal_, {start_.offset, token.at.offset}, *previousError, false};
                }
                if (token.at.offset - start_.offset > limits_.maxGameBytes) {
                    return PgnFailure{
                        ordinal_, {start_.offset, token.at.offset}, limit(token.at), false};
                }
                if (numberPending_) {
                    return PgnFailure{
                        ordinal_, {start_.offset, token.at.offset}, syntax(token), false};
                }
                warn(DiagnosticCode::MissingTermination, "Missing game termination marker.",
                     token.at);
                break;
            }
            if (lexer_.location().offset - start_.offset > limits_.maxGameBytes) {
                return recover(limit(token.at), token);
            }
            if (token.error) {
                return recover(*token.error, token);
            }
            if (token.kind == Kind::Tag && movesStarted_) {
                return recover(syntax(token), token);
            }
            if (token.kind == Kind::Tag) {
                if (auto failure = tag(token)) {
                    return recover(*failure, token);
                }
            } else if (token.kind == Kind::Comment) {
                if (auto failure = comment(token)) {
                    return recover(*failure, token);
                }
            } else {
                if (!board_) {
                    if (auto failure = initialize()) {
                        return recover(*failure, token);
                    }
                }
                if (token.kind == Kind::End) {
                    if (!frames_.empty() || numberPending_) {
                        return recover(syntax(token), token);
                    }
                    warn(DiagnosticCode::MissingTermination, "Missing game termination marker.",
                         token.at);
                    break;
                }
                movesStarted_ = true;
                if (token.kind == Kind::Symbol && resultOf(token.text)) {
                    if (!frames_.empty() || numberPending_) {
                        return recover(syntax(token), token);
                    }
                    termination_ = resultOf(token.text);
                    if (headerResult_ && *headerResult_ != *termination_) {
                        return recover(diagnostic(DiagnosticCode::ConflictingResult,
                                                  "Conflicting game results.", token.at),
                                       token);
                    }
                    break;
                }
                if (auto failure = movetext(token)) {
                    return recover(*failure, token);
                }
            }
            token = lexer_.next();
        }
        if (!board_) {
            if (auto failure = initialize()) {
                return recover(*failure, token);
            }
        }
        if (!pendingComment_.empty()) {
            auto& target = occurrences_[current_].node.annotations.commentBefore;
            if (!appendComment(target, pendingComment_)) {
                return recover(limit(token.at), token);
            }
        }
        return finish();
    }

  private:
    Diagnostic syntax(const Token& token) const {
        return diagnostic(DiagnosticCode::InvalidSyntax, "Malformed PGN movetext.", token.at);
    }
    Diagnostic limit(SourceLocation at) const {
        return diagnostic(DiagnosticCode::LimitExceeded, "PGN resource budget exceeded.", at);
    }
    void warn(DiagnosticCode code, const char* message, SourceLocation at) {
        if (warnings_.size() < limits_.maxDiagnostics) {
            warnings_.push_back(diagnostic(code, message, at));
        } else {
            ++suppressed_;
        }
    }
    std::optional<Diagnostic> tag(const Token& token) {
        if (tags_.size() >= limits_.maxTags || token.text.size() > 255 ||
            decodedSize(token.value) > limits_.maxTagValueBytes) {
            return limit(token.at);
        }
        auto value = unescape(token.value);
        for (const auto& existing : tags_) {
            if (existing.name == token.text) {
                if (existing.value != value) {
                    return diagnostic(DiagnosticCode::ConflictingTag, "Conflicting tag values.",
                                      token.at);
                }
                warn(DiagnosticCode::DuplicateTag, "Repeated identical tag.", token.at);
                break;
            }
        }
        tags_.push_back({std::string(token.text), std::move(value)});
        tagLocations_.push_back(token.at);
        return std::nullopt;
    }
    std::optional<Diagnostic> initialize() {
        std::optional<std::string_view> setup;
        SourceLocation fenAt = start_;
        for (std::size_t i = 0; i < tags_.size(); ++i) {
            const auto& entry = tags_[i];
            const auto at = tagLocations_[i];
            if (entry.name == "Variant" && entry.value != "Standard") {
                return diagnostic(DiagnosticCode::UnsupportedVariant, "Unsupported chess variant.",
                                  at);
            }
            if (entry.name == "Result") {
                headerResult_ = resultOf(entry.value);
                if (!headerResult_) {
                    return diagnostic(DiagnosticCode::InvalidTag, "Invalid result tag.", at);
                }
            }
            if (entry.name == "FEN") {
                fen_ = entry.value;
                fenAt = at;
            }
            if (entry.name == "SetUp") {
                setup = entry.value;
            }
        }
        if ((setup && *setup != "0" && *setup != "1") || (fen_ && setup && *setup == "0") ||
            (!fen_ && setup && *setup == "1")) {
            return diagnostic(DiagnosticCode::InvalidSetup, "Inconsistent setup tags.", start_);
        }
        if (fen_) {
            auto loaded = Position::fromFen(*fen_);
            if (std::holds_alternative<chess::Error>(loaded)) {
                return diagnostic(DiagnosticCode::InvalidFen, "Invalid FEN root.", fenAt);
            }
            board_.emplace(std::get<Position>(std::move(loaded)));
            if (!setup) {
                warn(DiagnosticCode::MissingSetup, "FEN without SetUp tag.", fenAt);
            }
        } else {
            board_.emplace(Position::initial());
        }
        return std::nullopt;
    }
    bool appendComment(std::string& target, std::string_view text) {
        const std::size_t separator = target.empty() ? 0 : 1;
        if (target.size() > limits_.tree.maxCommentBytes ||
            separator > limits_.tree.maxCommentBytes - target.size() ||
            text.size() > limits_.tree.maxCommentBytes - target.size() - separator) {
            return false;
        }
        if (separator != 0) {
            target.push_back('\n');
        }
        target.append(text);
        return true;
    }
    std::optional<Diagnostic> comment(const Token& token) {
        // Pending opening comments are charged when read, not when a move finally appears.
        const auto separator =
            opening_ ? (pendingComment_.empty() ? 0U : 1U)
                     : (occurrences_[current_].node.annotations.commentAfter.empty() ? 0U : 1U);
        if (separator > limits_.tree.maxAnnotationBytes - annotationBytes_ ||
            token.text.size() > limits_.tree.maxAnnotationBytes - annotationBytes_ - separator) {
            return limit(token.at);
        }
        auto& target =
            opening_ ? pendingComment_ : occurrences_[current_].node.annotations.commentAfter;
        if (!appendComment(target, token.text)) {
            return limit(token.at);
        }
        annotationBytes_ += token.text.size() + separator;
        return std::nullopt;
    }
    std::optional<Diagnostic> movetext(const Token& token) {
        if (token.kind == Kind::Number) {
            if (numberPending_) {
                return syntax(token);
            }
            numberPending_ = true;
            std::uint32_t number{};
            const auto parsed =
                std::from_chars(token.text.data(), token.text.data() + token.text.size(), number);
            const bool black = token.dots > 1;
            if (parsed.ec != std::errc{} || number != board_->fullmoveNumber() ||
                black != (board_->sideToMove() == chess::Color::Black)) {
                warn(DiagnosticCode::MoveNumberMismatch, "Move number disagrees with position.",
                     token.at);
            }
            return std::nullopt;
        }
        if (token.kind == Kind::Open) {
            if (opening_ || current_ == 0 || numberPending_) {
                return syntax(token);
            }
            if (frames_.size() >= limits_.maxVariationDepth) {
                return limit(token.at);
            }
            const auto parent = occurrences_[current_].node.parent;
            frames_.push_back({current_, parent});
            if (!board_->undo()) {
                return syntax(token);
            }
            current_ = parent;
            opening_ = true;
            return std::nullopt;
        }
        if (token.kind == Kind::Close) {
            if (frames_.empty() || opening_ || numberPending_) {
                return syntax(token);
            }
            const auto frame = frames_.back();
            while (current_ != frame.parent) {
                if (lexer_.cancelled()) {
                    return std::nullopt;
                }
                if (!board_->undo()) {
                    return syntax(token);
                }
                current_ = occurrences_[current_].node.parent;
            }
            const auto played = board_->play(*occurrences_[frame.resume].node.move);
            if (std::holds_alternative<chess::Error>(played)) {
                return syntax(token);
            }
            current_ = frame.resume;
            frames_.pop_back();
            opening_ = false;
            return std::nullopt;
        }
        if (token.kind == Kind::Nag || token.kind == Kind::Glyph) {
            if (opening_ || current_ == 0 || numberPending_) {
                return syntax(token);
            }
            unsigned int value{};
            if (token.kind == Kind::Nag) {
                const auto parsed = std::from_chars(token.text.data(),
                                                    token.text.data() + token.text.size(), value);
                if (token.text.empty() || parsed.ec != std::errc{} || value > 255) {
                    return diagnostic(DiagnosticCode::InvalidNag, "Invalid numeric annotation.",
                                      token.at);
                }
            } else {
                constexpr std::string_view glyphs[]{"!", "?", "!!", "??", "!?", "?!"};
                const auto found = std::find(std::begin(glyphs), std::end(glyphs), token.text);
                if (found == std::end(glyphs)) {
                    return diagnostic(DiagnosticCode::InvalidNag, "Invalid annotation glyph.",
                                      token.at);
                }
                value = static_cast<unsigned int>(found - std::begin(glyphs)) + 1;
            }
            if (annotationBytes_ >= limits_.tree.maxAnnotationBytes) {
                return limit(token.at);
            }
            occurrences_[current_].node.annotations.nags.push_back(
                static_cast<std::uint8_t>(value));
            ++annotationBytes_;
            return std::nullopt;
        }
        if (token.kind != Kind::Symbol) {
            return syntax(token);
        }
        if (occurrences_.size() >= limits_.tree.maxNodes ||
            occurrences_[current_].depth >= limits_.tree.maxDepth || token.text.size() > 32) {
            return limit(token.at);
        }
        std::string san(token.text);
        if (san.starts_with("0-0")) {
            std::replace(san.begin(), san.end(), '0', 'O');
        }
        auto parsed = board_->parseSan(san);
        if (std::holds_alternative<chess::Error>(parsed)) {
            return diagnostic(DiagnosticCode::IllegalMove, "Invalid or illegal SAN move.", token.at,
                              occurrences_[current_].depth + 1);
        }
        const auto move = std::get<Move>(parsed);
        const auto played = board_->play(move);
        if (std::holds_alternative<chess::Error>(played)) {
            return diagnostic(DiagnosticCode::IllegalMove, "Move application failed.", token.at,
                              occurrences_[current_].depth + 1);
        }
        const auto index = occurrences_.size();
        const auto depth = occurrences_[current_].depth + 1;
        occurrences_[current_].children.push_back(index);
        occurrences_.push_back({GameTreeInputNode{current_, move, {}}, {}, token.at, depth});
        current_ = index;
        occurrences_[current_].node.annotations.commentBefore = std::move(pendingComment_);
        pendingComment_.clear();
        if (frames_.empty()) {
            originalMainline_.push_back(move);
        }
        opening_ = false;
        numberPending_ = false;
        return std::nullopt;
    }
    PgnOutcome recover(Diagnostic error, const Token& failed) {
        if (lexer_.cancelled()) {
            return Cancelled{};
        }
        std::size_t nesting = frames_.size();
        Token token = failed;
        bool bodySeen = movesStarted_ || (failed.kind != Kind::Tag && failed.kind != Kind::Comment);
        // Semantic failures on a result have already consumed a complete game boundary.
        if (token.kind == Kind::Symbol && resultOf(token.text) && nesting == 0) {
            return PgnFailure{
                ordinal_, {start_.offset, lexer_.location().offset}, std::move(error), false};
        }
        while (true) {
            if (lexer_.cancelled()) {
                return Cancelled{};
            }
            if (token.unsafe || token.kind == Kind::End) {
                return PgnFailure{ordinal_, {start_.offset, bytes_.size()}, std::move(error), true};
            }
            if (token.kind == Kind::Open) {
                ++nesting;
            }
            if (token.kind == Kind::Close) {
                if (nesting == 0) {
                    return PgnFailure{
                        ordinal_, {start_.offset, bytes_.size()}, std::move(error), true};
                }
                --nesting;
            }
            token = lexer_.next();
            if (token.kind != Kind::Tag && token.kind != Kind::Comment) {
                bodySeen = true;
            }
            if (token.kind == Kind::Tag && !token.unsafe && nesting == 0 && bodySeen) {
                lexer_.rewind(token.at);
                return PgnFailure{
                    ordinal_, {start_.offset, token.at.offset}, std::move(error), false};
            }
            if (token.kind == Kind::Symbol && resultOf(token.text) && nesting == 0) {
                return PgnFailure{
                    ordinal_, {start_.offset, lexer_.location().offset}, std::move(error), false};
            }
        }
    }
    PgnOutcome finish() {
        // Depth-first source order visits the complete played continuation before its
        // alternatives. Merging before this traversal would let a repeated variation
        // incorrectly replace the original main continuation.
        std::vector<GameTreeInputNode> normalized(1);
        std::vector<std::size_t> destinations(occurrences_.size());
        std::vector<std::vector<std::size_t>> children(1);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (auto it = occurrences_[0].children.rbegin(); it != occurrences_[0].children.rend();
             ++it) {
            stack.emplace_back(*it, 0);
        }
        while (!stack.empty()) {
            if (lexer_.cancelled()) {
                return Cancelled{};
            }
            const auto [source, parent] = stack.back();
            stack.pop_back();
            auto& occurrence = occurrences_[source];
            auto& siblings = children[parent];
            const auto found = std::find_if(siblings.begin(), siblings.end(), [&](std::size_t id) {
                return normalized[id].move == occurrence.node.move;
            });
            std::size_t target{};
            if (found != siblings.end()) {
                target = *found;
                warn(DiagnosticCode::MergedBranch, "Repeated branch merged.", occurrence.at);
            } else {
                target = normalized.size();
                siblings.push_back(target);
                normalized.push_back({parent, occurrence.node.move, {}});
                children.emplace_back();
            }
            destinations[source] = target;
            for (auto it = occurrence.children.rbegin(); it != occurrence.children.rend(); ++it) {
                stack.emplace_back(*it, target);
            }
        }
        // Structure follows mainline-first traversal; merged annotation payload follows
        // textual occurrence order, so normalizing a branch never reorders its NAGs.
        for (std::size_t index = 0; index < occurrences_.size(); ++index) {
            if (lexer_.cancelled()) {
                return Cancelled{};
            }
            auto& annotations = normalized[destinations[index]].annotations;
            const auto& incoming = occurrences_[index].node.annotations;
            for (const auto before : {true, false}) {
                auto& text = before ? annotations.commentBefore : annotations.commentAfter;
                const auto& addition = before ? incoming.commentBefore : incoming.commentAfter;
                if (!addition.empty()) {
                    if (!text.empty()) {
                        if (annotationBytes_ >= limits_.tree.maxAnnotationBytes) {
                            return PgnFailure{ordinal_,
                                              {start_.offset, lexer_.location().offset},
                                              limit(occurrences_[index].at),
                                              false};
                        }
                        ++annotationBytes_;
                    }
                    if (!appendComment(text, addition)) {
                        return PgnFailure{ordinal_,
                                          {start_.offset, lexer_.location().offset},
                                          limit(occurrences_[index].at),
                                          false};
                    }
                }
            }
            annotations.nags.insert(annotations.nags.end(), incoming.nags.begin(),
                                    incoming.nags.end());
        }
        const auto fenView = fen_ ? std::optional<std::string_view>{*fen_} : std::nullopt;
        auto built = chess::GameTree::fromNodes(fenView, normalized, limits_.tree, stop_);
        if (const auto* failure = std::get_if<chess::GameTreeError>(&built)) {
            if (failure->code == chess::GameTreeErrorCode::Cancelled) {
                return Cancelled{};
            }
            return PgnFailure{
                ordinal_, {start_.offset, lexer_.location().offset}, limit(start_), false};
        }
        const SourceRange range{start_.offset, lexer_.location().offset};
        return PgnDocument{ordinal_,
                           range,
                           std::move(tags_),
                           termination_.value_or(headerResult_.value_or(GameResult::Unknown)),
                           std::get<chess::GameTree>(std::move(built)),
                           std::string(bytes_.substr(range.begin, range.end - range.begin)),
                           std::move(warnings_),
                           suppressed_,
                           std::move(originalMainline_)};
    }
    struct Frame {
        std::size_t resume;
        std::size_t parent;
    };
    Lexer& lexer_;
    std::string_view bytes_;
    const PgnLimits& limits_;
    std::stop_token stop_;
    std::size_t ordinal_;
    SourceLocation start_;
    std::vector<Tag> tags_;
    std::vector<SourceLocation> tagLocations_;
    std::vector<Diagnostic> warnings_;
    std::size_t suppressed_{};
    std::vector<Occurrence> occurrences_;
    std::vector<Frame> frames_;
    std::optional<Position> board_;
    std::optional<std::string> fen_;
    std::optional<GameResult> headerResult_;
    std::optional<GameResult> termination_;
    std::vector<Move> originalMainline_;
    std::string pendingComment_;
    std::size_t annotationBytes_{};
    std::size_t current_{};
    bool movesStarted_{};
    bool opening_{true};
    bool numberPending_{};
};
} // namespace

PgnDocument::PgnDocument(std::size_t sourceOrdinal, SourceRange sourceRange,
                         std::vector<Tag> sourceTags, GameResult gameResult,
                         chess::GameTree gameTree, std::string sourceText,
                         std::vector<Diagnostic> sourceWarnings, std::size_t suppressed,
                         std::vector<Move> mainline)
    : ordinal(sourceOrdinal), range(sourceRange), tags(std::move(sourceTags)), result(gameResult),
      tree(std::move(gameTree)), originalText(std::move(sourceText)),
      warnings(std::move(sourceWarnings)), suppressedWarnings(suppressed),
      originalMainline_(std::move(mainline)) {}

PgnReader::PgnReader(std::string_view bytes, PgnLimits limits, std::stop_token stop)
    : bytes_(bytes), limits_(limits), stop_(stop) {
    if (bytes_.starts_with("\xef\xbb\xbf")) {
        offset_ = 3;
        column_ = 4;
    }
}
PgnOutcome PgnReader::next() {
    if (cancelled_ || stop_.stop_requested()) {
        terminal_ = true;
        cancelled_ = true;
        return Cancelled{};
    }
    if (terminal_) {
        return EndOfInput{};
    }
    const SourceLocation at{offset_, line_, column_};
    if (limits_.tree.maxNodes == 0 || limits_.maxInputBytes == 0 || limits_.maxGameBytes == 0) {
        terminal_ = true;
        return PgnFailure{0,
                          {offset_, bytes_.size()},
                          diagnostic(DiagnosticCode::InvalidLimits, "Invalid PGN limits.", at),
                          true};
    }
    if (bytes_.size() > limits_.maxInputBytes) {
        terminal_ = true;
        return PgnFailure{
            0,
            {offset_, bytes_.size()},
            diagnostic(DiagnosticCode::LimitExceeded, "Input byte budget exceeded.", at),
            true};
    }
    Lexer lexer(bytes_, at, limits_, stop_);
    auto token = lexer.next();
    if (lexer.cancelled()) {
        cancelled_ = true;
        terminal_ = true;
        return Cancelled{};
    }
    if (token.kind == Kind::End && !token.error) {
        terminal_ = true;
        return EndOfInput{};
    }
    if (ordinal_ >= limits_.maxGames) {
        terminal_ = true;
        return PgnFailure{
            ordinal_ + 1,
            {token.at.offset, bytes_.size()},
            diagnostic(DiagnosticCode::LimitExceeded, "Game count budget exceeded.", token.at),
            true};
    }
    Parser parser(lexer, bytes_, limits_, stop_, ++ordinal_);
    auto outcome = parser.parse(std::move(token));
    const auto end = lexer.location();
    offset_ = end.offset;
    line_ = end.line;
    column_ = end.column;
    if (const auto* failure = std::get_if<PgnFailure>(&outcome)) {
        terminal_ = failure->unreadableTail;
    }
    if (std::holds_alternative<Cancelled>(outcome)) {
        terminal_ = true;
        cancelled_ = true;
    }
    return outcome;
}
} // namespace chess_review::pgn
