# Chess core

[Back to README](../README.md)

`chess_review_chess` is a pure C++20 static library. Include `chess/Position.h`
and link that target. It has no Qt, SQL, engine, filesystem, or network dependency.
Disservin/chess-library is vendored and private; all consumers use the types in
`chess_review::chess`. There is no board screen in this increment.

## Positions and moves

`Position::initial()` creates the initial position with known history.
`Position::fromFen(text)` returns `Result<Position>`, a `std::variant<Position,
Error>`. Other fallible operations use the same result convention. `play()`
returns `Status` (`std::monostate` on success); callers must inspect the result.
`undo()` returns the removed move, or `std::nullopt` at the root. Normal malformed
input produces typed errors rather than exceptions. Allocation failures retain
normal standard-library exception behavior; future application/event boundaries
must handle exceptional resource failures appropriately.

Positions are independent deep-copyable values. Moving transfers ownership;
moved-from positions support destruction and assignment only. Instances do not
share mutable state; callers synchronize access to the same mutable instance.
`Square` can only represent an in-bounds square. Its coordinate factories return
`std::optional`; files/ranks are zero-based and indices run from a1=0 to h8=63.

A `Move` contains source, destination, and optional promotion. Castling uses the
king's normal destination (e1g1, e1c1, e8g8, e8c8). Only knight, bishop, rook,
and queen promotions can be legal. Moves are position-independent descriptions:
every conversion/application revalidates legality against the current position.
They are not capabilities bound to an earlier position. No null-move encoding
or automatic queen promotion is accepted. Invalid moves and counter overflow
leave board, counters, en-passant metadata, and history unchanged.

## FEN and notation

Only standard chess is supported. FEN is limited to 256 bytes and six fields,
with no NULs or extra tokens. ASCII field whitespace is accepted and emitted as
single spaces. Castling order and numeric formatting are canonicalized. Validate
placement, material/promotion feasibility (including bishop square colors), both
kings, pawn ranks, orthodox castling pieces, and en-passant rank/pawn/origin/clock
consistency. A position may have its current king in check, but cannot leave the
previous mover's king in check. This is local validation, not proof of historical
reachability; counter values are not used to reconstruct a missing game.

FEN output preserves a double-push en-passant target even without a legal capture.
The adapter owns this metadata because upstream removes uncapturable targets.
It also owns unsigned 32-bit halfmove/fullmove counters: halfmove starts at zero,
fullmove at one; a black move increments fullmove, and pawn moves/captures reset
halfmove. A move that would overflow is rejected without wrapping. Upstream's
small counters are private and are never used for application state or draws.

`parseSan()` accepts only canonical SAN (maximum 32 bytes), including captures,
disambiguation, promotion, and accurate check/mate suffixes. Zero-based castling,
annotations, omitted check suffixes, and extra whitespace are rejected. A future
PGN importer can perform explicit token cleanup before invoking this API.
`parseUci()` accepts exactly four lowercase coordinates plus a required lowercase
promotion suffix when applicable. Both resolve only legal moves. SAN and UCI
output use the upstream generators; SAN generation uses fresh, counter-neutral
board context so prior draws cannot alter check/mate notation.

## Identity and history

`BoardIdentity` includes pieces, side, castling rights, and only *legally*
capturable en passant. Counters are excluded. Pinned en-passant pawns do not
make an en-passant target significant to this identity.

`PositionContext` is an exact conservative value containing the board identity,
halfmove clock, standard variant, version, root provenance, canonical root FEN,
and ordered moves since the root. Root FEN retains all six fields, so context
comparison can distinguish histories that board comparison intentionally merges.
Different paths remain distinct even when they transpose into identical FENs.
Unknown prior history is explicit for every FEN root, including initial-position
FEN. It never matches the known initial-position factory. Identical FEN roots
and identical continuations may match as explicit analysis roots; this says
nothing about the real game's earlier repetitions. Future engine requests must
use that same root and continuation rather than silently reconstructing only
the current FEN.

Snapshots copy history only on request; make/undo do not copy it. Comparisons use
full values, with no custom hash and no reliance on upstream Zobrist keys.
Contexts have an identity version but no persisted wire format in this slice.
No draw claim, repetition count, game result, or cache service is implemented.
