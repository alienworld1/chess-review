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
previous mover's king in check. Impossible multiple-check configurations are
rejected before upstream move generation runs, including during en-passant
normalization. When en passant identifies the preceding double push, its implied
predecessor must also leave the nonmoving king safe. This is local validation,
not proof of historical
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

Snapshots and position copies take O(history length) time and space. Request
contexts on demand; storing a complete context for every ply of a long game
would consume quadratic space. Make/undo do not copy history. Comparisons use
full values, with no custom hash and no reliance on upstream Zobrist keys.
Contexts have an identity version but no persisted wire format in this slice.
No draw claim, repetition count, game result, or cache service is implemented.

## Game trees

Include `chess/GameTree.h` and link the same `chess_review_chess` target.
`GameTree::initial(limits)` and `GameTree::fromFen(text, limits)` return
`GameTreeResult<GameTree>`. FEN roots preserve unknown earlier history, even
when the supplied FEN describes the initial board. A tree cannot be rooted at
an existing played `Position` in this increment.

A tree owns one root position and a flat map of nodes. Each node has a strongly
typed, tree-local `NodeId`, optional parent/move/canonical SAN, ordered children,
and `NodeAnnotations`. The root has no incoming move, SAN, or parent. The first
child is the main continuation at that branch; other children are variations.
Transpositions remain distinct nodes and retain distinct position contexts.

`node(id)` returns an optional, independently owned snapshot. Its parent and
children provide navigation; snapshots remain valid after edits or destruction.
`mainLine(start)` includes start and then follows first children. `position(id)`
replays the root-to-node path and returns an independent `Position`, including
its complete root provenance, move history, clocks, and en-passant metadata.
No position/context history is stored per node. Selection, preview, review
navigation, and engine scheduling belong to consumers, not the tree.

### Editing

- `addMove(parent, move)` returns the child ID. Reuse an existing same-move
  child without changing its annotations or sibling order, reconstructing the
  board, or allocating; otherwise append a variation. The first child of an
  empty node becomes its main continuation.
- `addLine(start, spanOfMoves)` reuses existing prefixes and returns the final
  node ID. Empty input returns start. Stored prefixes were already validated;
  following a wholly existing line needs no allocation. New moves are validated
  against the preceding position and SAN is generated internally. A failure
  anywhere leaves the whole tree unchanged, including its ID allocator.
- `promote(id)` moves that child to first among its siblings, preserving their
  other relative ordering. Ancestors are not promoted. Root promotion is an error.
- `removeSubtree(id)` deletes the selected node and every descendant. If the
  main continuation is removed, the next sibling becomes main. Root deletion is
  an error. Deleted IDs are never reused during that tree's lifetime.
- `setAnnotations(id, annotations)` atomically replaces all comments and NAGs.
  Comments are opaque bytes intended for UTF-8, preserved verbatim including
  NULs. The tree does not validate Unicode, interpret markup, or normalize PGN.
  NAGs are ordered 8-bit numbers; duplicates and all values are preserved.
  Rendering must present comments as plain text or escape markup explicitly.
  Future PGN export must handle delimiters/control bytes according to its format;
  these opaque bytes are not already validated export text.

Fallible tree operations return `GameTreeResult<T>` or `GameTreeStatus`
(`std::monostate` on success). `GameTreeError` carries a stable code and bounded
static message. Position failures retain the adapter's `Error`. Failures while
processing a line identify its zero-based move index (`addMove` uses index zero).
Invalid starting IDs have no move index. Missing snapshots use `std::nullopt`.
Allocation failures propagate normally as exceptions; edits also preserve the
original tree in that case. Application boundaries must handle resource failures.

### Limits, ownership, and cost

Default limits are 100,000 nodes including root, 16,384 plies below root,
64 KiB for each individual comment, and 16 MiB total annotation payload.
Payload counts both comment byte lengths and one byte per NAG across all nodes,
including root. Replacing/deleting annotations reclaims this budget. SAN,
container overhead, and temporary transaction storage are not annotation payload;
these limits are not an exact resident-memory cap. A zero node budget is invalid;
zero depth/comment/annotation budgets are permitted. Exhausted IDs never wrap.

Copies own independent nodes and preserve their IDs. Moving transfers ownership;
moved-from trees support destruction and assignment only. IDs are not globally
unique and cannot detect accidental use with a different tree; keep them with
their owning tree/snapshot. Assignment replaces the entire tree and its ID scope.
Concurrent access to the same mutable instance requires external synchronization.
Async results must carry their owning game/snapshot identity as well as a node ID:
independent copies can subsequently assign the same numeric ID to different moves.

Map lookup costs O(log N); a node snapshot additionally copies its child IDs and
annotations. Position resolution costs O(D log N) map work plus D legal move
applications, with O(D) temporary/history storage for depth D. Adding a line
scans matching stored prefixes without replay. If a suffix is new, it resolves
only the divergence position, validates that suffix, and stages only new nodes
and the attachment's child list. Reusing an existing child costs O(log N) lookup
work per examined sibling, independent of its depth; an existing line costs that
work along its supplied moves. Map insertion costs O(log N) per new node.
Prefer `addLine` over repeatedly resolving increasingly deep nodes for batch
input. Long path replay and batch import belong off the GUI thread; this
synchronous core does not schedule work
or promise constant-time navigation. Trees store O(N + annotation
payload + SAN bytes), rather than quadratic complete position-history prefixes.
Copies take linear storage/work in the stored data. Subtree deletion takes
O(S log N) map work for S deleted nodes plus sibling removal, using O(S)
temporary IDs. Traversal and subtree deletion are iterative; flat ownership
prevents destruction from recursing through game ancestry.

This model is an editable analysis tree. It does not preserve an immutable
original played line: a later game/import owner must keep that separately.
Undo/redo, arrows/highlights, PGN, durable serialization, database integration,
draw adjudication, and a desktop board/tree view remain separate increments.
