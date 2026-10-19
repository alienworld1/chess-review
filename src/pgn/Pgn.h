#pragma once

#include "chess/GameTree.h"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chess_review::pgn {

struct SourceLocation {
    std::size_t offset{}; // Zero-based bytes, including any initial BOM.
    std::size_t line{1};
    std::size_t column{1}; // One-based byte column; CRLF counts as one line break.
};
struct SourceRange {
    std::size_t begin{};
    std::size_t end{}; // Exclusive.
};
enum class DiagnosticCode {
    InvalidLimits,
    LimitExceeded,
    InvalidEncoding,
    InvalidSyntax,
    InvalidTag,
    ConflictingTag,
    UnsupportedVariant,
    InvalidSetup,
    InvalidFen,
    IllegalMove,
    InvalidNag,
    ConflictingResult,
    MissingTermination,
    DuplicateTag,
    MissingSetup,
    MoveNumberMismatch,
    MergedBranch
};
struct Diagnostic {
    DiagnosticCode code;
    std::string message; // Bounded static text, never includes source content.
    SourceLocation location;
    std::optional<std::size_t> ply; // One-based depth of the affected move, when applicable.
};
struct Tag {
    std::string name;
    std::string value;
    bool operator==(const Tag&) const = default;
};
enum class GameResult { Unknown, WhiteWin, BlackWin, Draw };
struct PgnLimits {
    std::size_t maxInputBytes{64 * 1024 * 1024};
    std::size_t maxGames{10'000};
    std::size_t maxGameBytes{8 * 1024 * 1024};
    std::size_t maxVariationDepth{128};
    std::size_t maxTags{256};
    std::size_t maxTagValueBytes{64 * 1024};
    std::size_t maxDiagnostics{100};
    chess::GameTreeLimits tree;
};

class PgnDocument {
  public:
    PgnDocument(std::size_t ordinal, SourceRange range, std::vector<Tag> tags, GameResult result,
                chess::GameTree tree, std::string originalText, std::vector<Diagnostic> warnings,
                std::size_t suppressedWarnings, std::vector<chess::Move> originalMainline);
    std::size_t ordinal;
    SourceRange range;
    std::vector<Tag> tags;
    GameResult result;
    chess::GameTree tree;
    std::string originalText;
    std::vector<Diagnostic> warnings;
    std::size_t suppressedWarnings;
    [[nodiscard]] std::span<const chess::Move> originalMainline() const {
        return originalMainline_;
    }

  private:
    std::vector<chess::Move> originalMainline_;
};
struct PgnFailure {
    std::size_t ordinal;
    SourceRange range;
    Diagnostic error;
    bool unreadableTail; // False means next() can attempt a safely delimited next game.
};
struct EndOfInput {};
struct Cancelled {};
using PgnOutcome = std::variant<PgnDocument, PgnFailure, EndOfInput, Cancelled>;

// Synchronous, single-owner reader. Source bytes must outlive the reader and remain unchanged.
// Results own their data. Run large imports on an application worker, never the GUI thread.
// Cancellation is terminal; ordinary allocation failures propagate, with no partial document.
// After an exceptional failure, destroy the reader rather than trying to resume it.
class PgnReader {
  public:
    explicit PgnReader(std::string_view bytes, PgnLimits limits = {}, std::stop_token stop = {});
    [[nodiscard]] PgnOutcome next();

  private:
    std::string_view bytes_;
    PgnLimits limits_;
    std::stop_token stop_;
    std::size_t offset_{};
    std::size_t ordinal_{};
    std::size_t line_{1};
    std::size_t column_{1};
    bool terminal_{};
    bool cancelled_{};
};

} // namespace chess_review::pgn
