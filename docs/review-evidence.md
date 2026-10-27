# Review evidence and configuration

The first game-review increment supplies evidence conversion and configuration.
Running a game review, move classification, accuracy, and durable review storage
are later increments. Changing settings now does not change manual analysis limits
or launch an engine.

## Advanced configuration

The app reads optional `config.toml` in Qt's application configuration directory
once at startup, on a background worker. On Linux this is normally
`$XDG_CONFIG_HOME/chess-review/config.toml`, or
`~/.config/chess-review/config.toml`. Use [config.example.toml](config.example.toml)
as a starting point. The app does not create or overwrite this file. A missing file
uses built-in defaults. Changes take effect after restarting.

Configuration version 1 accepts the following fields; omitted fields use the
example's defaults. Unknown keys, unsupported versions, invalid types or values,
and unreadable files reject the entire configuration. Review configuration then
remains unavailable; import, the game library, and manual analysis remain usable.
A nonmodal notice explains the error. Help → Diagnostics includes recognized
field paths and source locations without raw values, unknown key names, or private
filesystem paths. Correct the file and restart; invalid files do not silently
activate defaults or last-known-good settings.

| Fields | Accepted values |
| --- | --- |
| `version` | Integer `1` |
| `analysis.fast/standard/thorough.quickScanNodes`, `.deepeningNodes`, `.verificationNodes` | Integers in `[1, 1000000000000]`, with quick ≤ deepening ≤ verification within each preset |
| `classification.inaccuracyLoss`, `.mistakeLoss`, `.blunderLoss` | Finite numbers strictly between 0 and 1, strictly increasing in this order |
| `review.quality` | `"fast"`, `"standard"` (default), `"thorough"` |
| `review.preferWdl` | Boolean; default `true` |
| `expectedScore.fallbackScaleCp` | Finite number in `[1,10000]`; default `300` |

Input is capped at 64 KiB and diagnostics at 32 entries. To bound parser memory
and stack use, input may contain at most 256 dots (including comments and strings),
and array/inline-table value nesting is limited to 16. The raw dot budget prevents
deep dotted-key/table structures before constructing the TOML syntax tree; it is a
conservative complexity limit, not a replacement TOML parser. Trim excessive
comment punctuation if the complexity diagnostic appears.

Existing leaf symlinks and non-regular configuration files are rejected. Unix
loading opens without following the leaf symlink or waiting on a FIFO and checks
the opened descriptor before reading. Permission failures are errors, never a
reason to silently select defaults. The notice shows the first error; full details
remain available in Help → Diagnostics. Settings are separate from UI
preferences in QSettings. Budgets and label boundaries are provisional defaults
pending empirical tuning; this increment defines no Excellent/Good/Best rules.

## Domain contract

`chess_review_review` links only the chess domain. Owning values preserve raw
centipawn/mate scores, bounds, WDL, explicit perspectives, candidate lines, measured
strength, and originating position/engine/attempt identities. Application code
must prepare and pair evidence off the GUI thread. `pairEvidence` requires a board
witness matching the full before-context, legally applies the played move, and
checks the complete resulting history-aware context against after-evidence.
Matching FENs do not establish equivalent histories.

Expected-score model v1 prefers native WDL, using `(wins + draws/2)/1000`. Each WDL
component must be nonnegative and sum to 1000. With no preferred WDL, exact
centipawn evidence uses the explicitly approximate mapping:

```text
E(cp) = 1 / (1 + exp(-ln(3) * cp / fallbackScaleCp))
```

At default scale, 0 cp maps to 0.5 and +300 cp to 0.75. This is a transparent
heuristic, not a calibrated engine adapter or a claim that all engines use the
same centipawn scale. `preferWdl=false` deliberately selects the fallback instead.
The source, version, parameters, and perspective accompany derived values.

Before/after values use the mover's perspective. Perspective is normalized in the
raw WDL/centipawn inputs before conversion so equivalent mirrored evidence does
not acquire a tiny negative delta through floating-point subtraction. Loss is
`before - after` only
when evidence is compatible and sufficient. Negative deltas are retained but
withhold loss. Mixed native/fallback methods, incomplete attempts, restricted
searches, bounded scores, incompatible engine/model contexts, and engine mate
claims also withhold ordinary loss with explicit reasons. Mate distance and bound
remain inspectable separately, including perspective inversion. Exact terminal
outcomes derive only from local checkmate/stalemate proof; result tags, draw
claims, and material/repetition/move-count adjudication are not used. Terminal
outcomes can pair with either conversion source.

Candidate ranks are retained only after successful pairing of complete,
unrestricted, legal, distinct candidate sets with coherent positive score depths,
exact non-mate scores, comparable conversion sources and consistent ordering.
Contradictory ranks, secondary bounds/mate claims, unsupported models or malformed
candidates withhold derived loss and rank; no rank is invented for an absent
played move. The `complete` field represents attempt coverage/strength and does
not by itself certify valid interpretation. Negative loss retains only the signed
delta while withholding loss/rank. A completed engine
attempt alone does not prove sufficient evidence: measured budget, requested
width, coherent positive depth, and consistency of bestmove with the first PV
are required by the adapter. The score set's common depth, rather than a later
statistics-only depth, describes its strength. Validation reuses one restored
history witness instead of replaying the same history twice. Time/infinite attempts are never treated as node
budget substitutes. Bounded scores and mate claims remain raw evidence.

`chess_review_analysis` adapts existing results without launching an engine or
accessing SQL. It retains full shared immutable engine provenance and requested
search metadata alongside its owning domain values. Shared provenance survives
the originating service/result. Structural validation is reused from engine
infrastructure, while cache eligibility remains unchanged. Engines with unverified
external resources may supply review evidence; that does not authorize cache hits.

## Versioned identities

Configuration publishes separate SHA-256 identities for classification thresholds,
expected-score conversion, and analysis budgets/selected preset/WDL preference.
Canonical encoding uses Qt_6_0 QDataStream framing, explicit big-endian/double
precision encoding and v1 domain tags. Independently computed default golden
vectors protect these identities against accidental encoding changes.
Whitespace, comments, key order, and omitted versus explicit defaults do not affect
identity. Threshold changes do not change conversion or analysis identities.
Future review storage must retain the applicable identities, model versions, and
source evidence. Accuracy remains a separate future methodology.

The analysis adapter optionally accepts a borrowed `Position` replay witness.
It reuses that board only when the complete history-aware context matches; a
mismatch falls back to reconstruction. Independent context size/version and
analysis validation still apply, and the pointer is never retained. This avoids
replaying a history already validated by storage without changing cache eligibility
or evidence sufficiency.
