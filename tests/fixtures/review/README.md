# Review acceptance fixtures

These local, synthetic PGNs are test inputs, not user games or engine evaluation
goldens. No downloads or accounts are needed. The fixture test independently
replays each mainline through the chess adapter before process acceptance runs.

| Fixture | Purpose and expected facts |
| --- | --- |
| `tactical.pgn` | Seven plies, ending after White captures the queen with `Nxg5`. Black's `Qg5` is a concrete material error. The real-engine test requires useful evaluated coverage, chosen-player key moments and inspectable continuations, without pinning scores or PV text. |
| `complete.pgn` | A 60-ply completed training game, with development, both castlings, exchanges and an agreed draw. Requires 61 durable position records and full original-mainline navigation. The PGN result is metadata, not proof of terminality or of move quality. |
| `terminal.pgn` | A checkmated Black king at a FEN root, no played moves. Requires one locally proven terminal position, no analysis search, and no invented move accuracy. |
| `stress.pgn` | 512 legal plies of reversible knight moves, intentionally unfinished. Exercises history-bearing preparation, timeline navigation to both ends, background navigation and cancellation. Repetition is intentional; it is not a realistic engine-strength benchmark. |

The fake engine supplies legal three-ply candidate lines and coherent synthetic
`cp 200` best scores from the side to move. When a candidate contains the played
move, its `cp -200` score agrees with the opposite-perspective child search;
lower-ranked candidates receive the same lower score to keep ordering coherent.
These deliberately create errors for both
players so refinement, stable classifications and equivalent-candidate retry are
exercised deterministically. They make no objective claim about the fixture games.
Its executable/resources remain unverified and must never qualify for cache reuse.
Real-engine checks use the separately selected local executable and actual output.
