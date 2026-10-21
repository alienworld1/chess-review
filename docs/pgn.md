# PGN parsing

[Back to README](../README.md)

Include `pgn/Pgn.h` and link `chess_review_pgn`. This pure C++20 target links
`chess_review_chess` and has no Qt, SQL, filesystem, network, or engine dependency.
The desktop shell uses this parser through the asynchronous import service; see
[importing games](import.md) for file, paste, and drop workflows.

## Reading games

```cpp
std::string source = "1.e4 e5 * 1.d4 d5 *";
chess_review::pgn::PgnReader reader(source);
for (;;) {
    auto outcome = reader.next();
    if (auto* game = std::get_if<chess_review::pgn::PgnDocument>(&outcome)) {
        // Consume or transfer this owned document before requesting the next game.
    } else if (auto* failure = std::get_if<chess_review::pgn::PgnFailure>(&outcome)) {
        // Present failure->error. unreadableTail means no further games can be recovered.
    } else {
        break; // EndOfInput or Cancelled; distinguish these in application task state.
    }
}
```

The source must outlive the reader and remain unchanged. A reader instance is not
safe for concurrent access. Every returned document owns its tags, source game
text, tree, diagnostics, and original mainline. Results remain valid after the
reader/source is destroyed or another game is requested. The private original
mainline is exposed as `span<const Move>`; editing/promoting/deleting tree nodes
does not change it. Assignment replaces the document and invalidates its views.

Outcomes carry one-based source game ordinals. Ranges are half-open byte offsets
into the entire supplied input, including an initial BOM. Leading whitespace and
percent escape lines before the first token are outside the game range; whitespace
within an unterminated game is retained. Locations use one-based lines and byte
columns, count CRLF as one line break, and include the BOM's three bytes in the
first line's columns. Move failures carry a one-based root-relative ply, including
inside variations. A per-game failure rejects every node of that game.

The parser is synchronous and performs no logging or I/O. Schedule expensive
parsing on the application's bounded worker pool. An optional `std::stop_token`
allows terminal cancellation during scanning, recovery, normalization, and tree
construction. The unfinished game is discarded; earlier documents remain owned
by the caller. Cancellation is cooperative, not a time/deadline guarantee.
Allocation failures propagate normally; after an exception, destroy the reader
rather than attempting to resume it. Catch exceptional resource failures at the
application worker/event boundary.

## Accepted input and normalization

The grammar follows the [PGN specification](https://www.saremba.de/chessgml/standards/pgn/pgn-complete.htm),
with explicit compatibility policy:

- Standard chess only; absent `Variant` or exactly `Standard` is supported.
- UTF-8 only, with an optional BOM at input offset zero. Reject NUL, DEL, other
  non-whitespace ASCII controls, malformed UTF-8, invalid escapes, and malformed
  delimiters. Validate comment and percent-line bytes as well as visible text.
- Ordered standard/unknown tags and identical duplicate tags are retained.
  Whitespace is optional between tag tokens, including before the quoted value.
  Comments and column-one percent escape lines are supported between tag tokens.
  Identical duplicates warn; different values for the same case-sensitive name
  reject the game. Tag strings decode only `\"` and `\\` escapes. Tag names use
  ASCII alphanumeric/underscore characters and are limited to 255 bytes.
- Headers and move numbers are optional. Incorrect supplied move numbers warn;
  the actual position determines side and counters. Repeated move-number
  indications and periods separated by whitespace/comments are accepted. A bare
  number has no side indicator. A final indication still requires a following move.
  Missing termination at EOF
  or a recognized following header warns. Use the header result if available,
  otherwise unknown. Explicit header and movetext results must agree, including
  `*`. Results are source claims, not board-derived adjudication.
- FEN uses the existing adapter validation and unknown-history provenance.
  Missing `SetUp` with FEN warns. Reject `SetUp "0"` with FEN, `SetUp "1"`
  without FEN, and other setup values.
- Legal canonical SAN is required, with `0-0`/`0-0-0` explicitly normalized to
  letter-O castling, including their canonical check/mate suffixes. Mixed digit/
  letter forms such as `0-0-O` are rejected. Do not
  repair omitted check suffixes, accept coordinate moves, or guess moves.
- Preserve recursive variations, brace/semicolon comments, numeric NAGs
  `$0`–`$255`, and symbolic suffixes `! ? !! ?? !? ?!` as NAGs 1–6.
  Empty variations and result markers inside variations are rejected.
- Comments between tag tokens join the opening comments. Comments between
  move-number tokens attach at that line's current comment position.
  Opening comments attach before the next move of that line; subsequent comments
  attach after the preceding move. Empty-game comments attach before the root.
  Join multiple comments with newlines, removing PGN delimiters while preserving
  content. Percent escape lines in column one are ignored as syntax; they are
  retained wherever they fall inside the owned source range.
- Merge repeated same-move branches and warn. Traverse the source mainline first
  to choose tree sibling order; merge comment/NAG payload in original textual
  occurrence order. Comments gain newline separators; NAG duplicates remain.
  The exact original source is also retained, but this is not an exporter.

The structural mainline of the editable tree is not the original played game.
For example, `1.e4 (1.e4 e5) *` merges into a tree containing `e4 e5`, while
`originalMainline()` correctly contains only `e4`. Review, game length, and source
result association must use the original sequence from the document's root,
including its original FEN provenance. Promoting branches also changes tree
mainlines without changing this sequence. Replacing the document's tree/root
requires the game owner to retain the original root separately; this is an
in-memory value model, not an authenticated record or persistence format.

Comments, including `[%eval]`, `[%clk]`, and markup-looking text, remain opaque
text. Render as plain text or explicitly escape markup. They are never engine
evidence, executable input, or logging payload.

## Failures and recovery

Diagnostics use typed codes and bounded static messages without source echoes.
Recovery continues lexical scanning to a top-level result or recognized header;
it never searches raw bytes for header-shaped substrings. Comments, quoted
strings, and nested variations cannot inject a recovery boundary. Conflicting
tags do not turn the remaining tags of that same header block into another game.
When strings/comments are unterminated or unmatched nesting prevents safe
synchronization, report an unreadable tail and stop. Consequently, a corrupted
game can prevent later otherwise-valid-looking bytes from being recovered.

Empty input/whitespace is ordinary end-of-input. Input-wide invalid limits or
size failure uses ordinal zero. Game-count exhaustion reports the next ordinal.
After an unreadable tail, subsequent calls return end-of-input; after cancellation,
subsequent calls remain cancelled. A later requested stop also returns cancelled.

## Resource budgets and cost

Defaults are 64 MiB input, 10,000 attempted games, 8 MiB per game, 128 nested
variations, 256 tags per game, 64 KiB decoded bytes per tag value, and 100 warnings
per game with a suppression count. The primary failure is stored separately from
the warning cap. `GameTreeLimits` additionally bounds move occurrences, depth,
comments, and total annotation payload. Count repeated source move occurrences
against the node budget before merging; count comment separators and one byte
per NAG. Zero node/input-byte/game-byte limits are invalid; other zero budgets
are supported. Limits are caller-configurable and should come from an approved
application budget.

One game is staged at a time. Caller retention of prior documents is outside the
reader's budget. Input storage is borrowed, but accepted source game text is
copied. Source occurrences, normalization, bulk construction, and transient SAN
conversion need additional memory: logical limits are not exact RSS limits.
Comments temporarily collected inside a tag or number token are bounded by the
smaller of the comment and aggregate annotation limits before copying, and then
charged through the normal node annotation boundary. Recovery can scan the remaining input after a game limit is exceeded, with no
unbounded token allocations and with cancellation checks.

Move reconstruction and final tree validation use working positions with
make/undo; there is no full history prefix stored at each node or root replay per
move. Tree insertion takes O(N log N) map work; legal chess bounds the search
through unique siblings. Scanning and payload normalization are linear in source
bytes and admitted occurrences/payload. Tag duplicate checks compare preceding
tag names: O(T²) comparisons in the configured tag count T, bounded to 256 tags
by default. Application-approved limits must account for this when raised. Traversal and ownership are iterative,
including deeply nested or long games.

The standard library has no PGN parser or UTF-8 validation facility; Qt is excluded
from this domain target. The vendored chess library's PGN reader explicitly skips
variations and NAGs. Its use would lose required data. The project parser adds
bounded lexical recovery, annotations, source locations, and cancellation around
the existing chess adapter rather than introducing another chess engine/library.


For asynchronous text imports, durable snapshots, and duplicate policy, see
[game storage and import](storage.md#immutable-imported-games). The PGN module
itself remains pure C++ and performs no database or application scheduling work.
