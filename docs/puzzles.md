# Personal puzzles

Puzzles turn positions from your games into optional practice. Everything stays
on your computer. There are no due dates, streaks, or required training sequences.

## Generate puzzles from a review

After a review finishes, the app looks for meaningful mistakes and missed
opportunities by the selected player. You can start reviewing immediately; puzzle
preparation is a separate background task. Completed older reviews offer
**Generate Puzzles** on their summary.

The app considers up to six ranked positions and keeps at most two generated
puzzles per game. It prefers concrete tactics, missed mates, retained material
gains, and unique defensive resources. Routine recaptures, unresolved sacrifices,
unstable lines, and ambiguous choices are filtered out. A missed mate in one or
free major piece can still be worth practicing. Finding no suitable puzzles is a
normal result.

Verification uses stronger finite searches and checks the decisions and replies
in the saved continuation. Searches can deepen within fixed effort and tree-size
limits. A position that remains uncertain is skipped. The app saves engine-backed
lines, not a claim that every possible defense has been exhaustively proven.

Interactive analysis and review work take priority over background puzzle
verification. **Pause generation** keeps completed work and accepted puzzles.
After cancellation, an engine failure, or restarting the app, resume explicitly
from Puzzles or the review summary with the original engine and settings. The app
does not start an engine automatically on relaunch.

## Solve and explore

Open **Puzzles**, select a record, and choose **Solve**. The library identifies
generated and manually saved puzzles without displaying the solution.

- Play the meaningful sequence, up to four moves by your side. The app supplies
  verified defensive replies and accepts saved equivalent solutions.
- **Hint** progressively offers an idea or thinking prompt, a piece, and a
  destination. Hints remain visible after unsuccessful attempts.
- **Retry** returns to the initial position and clears the current attempt's hints.
- **Reveal** opens a verified line for stepping; revealing does not count as solving.
- **Analyze** opens a separate analysis session. **Back to puzzle** restores the
  solving session. Edited analysis uses the normal discard confirmation.

Saved branches and moves demonstrated to be weaker work without a connected
engine. For an unfamiliar move, the app verifies its quality and remaining
continuation with the original compatible engine before accepting it. If it
cannot establish a grade, it says so and does not mark the move wrong. A locally
proven checkmate needs no engine search. Newly verified alternative branches are
retained only in the running session.

There is no durable solved flag or solve history. Puzzle records survive app
restarts; solving progress stays in the current session.

## Save a position manually

In Analysis, connect an engine and choose **Save Position as Puzzle**. The app
captures the clicked position, stops that analysis panel's live search, and
verifies a finite solution in the background. You can continue navigating.
**Open saved puzzle** opens the result after saving succeeds.

Manual puzzles may be quiet or easy, but still need stable, usable solutions.
There is no custom solution editor. Saving an already saved history-bearing
position opens the existing record and preserves its original origin.

Deleting a puzzle also remembers that exact position so automatic generation
does not immediately recreate it. A manual save can restore it. Positions with
different histories are distinct even when their boards look identical.

## Settings

Settings offers **Automatically generate puzzles after completed reviews**.
Changing it updates the product TOML file atomically and affects future automatic
starts; use Pause to stop work already running. The TOML formatter preserves
configuration values but may change formatting and comments.

Advanced limits live in the same configuration file:

```toml
[puzzles]
automatic = true
maximumPlayerMoves = 4
maximumPositions = 64
maximumBudgetUnits = 64
minimumMaterialGain = 2
immediateMaterialGain = 5
```

Player moves are limited to 1–4, positions to 1–64, and budget units to 6–64.
Material thresholds must satisfy `2 <= minimumMaterialGain <= immediateMaterialGain <= 9`.
Each budget unit is the captured review preset's verification-node budget.
Verification starts at two and four units per position, with one possible
escalation to eight; previously stronger review evidence raises the root's
starting requirement. The total candidate budget still applies. Exact node limits
do not promise a fixed wall-clock duration.

Puzzle policies capture their configuration when preparation starts. Changing
settings does not reinterpret saved puzzles or historical review results.
