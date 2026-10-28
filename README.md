# chess-review

A local-first desktop chess review app built with C++20 and Qt Widgets.
Analyze now provides a playable board with legal move entry, branching history,
promotion, a chosen-line timeline, collapsible PGN variations, keyboard controls,
and annotations. Board orientation and the analysis split survive orderly exit.
Saved games open in Analyze with their full history, variations, comments, and NAGs.
The last opened game and selected stored move resume after orderly exit.
Exploratory moves and new board annotations are session-only; a discard confirmation
protects against accidental loss. An explicit Start/Stop engine panel shows a
White-perspective evaluation and bar, one read-only SAN principal variation, depth,
and engine status while following the board. Games → Review now runs a bounded
full-game quick scan with player/engine/quality selection, durable progress,
cancellation, background continuation and explicit resume. A paged move list shows
provisional normal classifications with inspectable scores, rules, thresholds and
evidence provenance. Older runs gain classifications from saved evidence without
an engine. The board-first Review workspace now offers per-side accuracy and
coverage, stable/provisional results, chronological key moments, full-game move
navigation, reversible hover previews and a White expected-score bar. Interactive
better-move exploration and retry remain future work.
Settings registers local UCI executables through a nonblocking validation
handshake and persists their profiles. Explicit Connect/Disconnect controls keep
an engine Ready and expose its advertised options without starting a search.
The engine infrastructure now supports position searches, MultiPV, optional WDL,
validated candidate lines, asynchronous stop, and bounded priority scheduling with
request/job cancellation through its typed C++ API. Explicit asynchronous SQLite
cache APIs retain completed evidence and reuse only strictly compatible results
under a verified engine-resource policy; live infinite analysis remains in memory
and does not use the cache.
Home offers import actions; Puzzles remains
a placeholder.

The shell includes navigation, saved window preferences, local SQLite storage,
and diagnostics. A pure C++ chess core now provides validated positions, legal
moves, notation, game trees, and history-aware identity. A pure C++ PGN reader
parses single and multi-game input. An asynchronous application service imports
owned PGN text into transactional SQLite snapshots with duplicate detection;
Games now lists saved snapshots with pagination and imports local PGN files through
file selection, paste confirmation, and drag-and-drop. Select a game and choose
**Open in Analyze**, double-click its row, or press Enter to explore it without an engine.

## Requirements

- C++20 compiler (GCC or Clang on Linux)
- Qt **6.12+** with Widgets, SVG, SQL, and the QSQLITE driver
- CMake **3.24+**, Ninja, and GNU Make
- Initial configuration needs network access to fetch pinned toml++ (or an offline source override)
- Qt Test for tests (enabled by default); initial test configuration also fetches pinned Catch2
- clang-format 18 and clang-tidy 18 for formatting and static analysis

## Build and run

From the repository root, with the Qt SDK installed:

```sh
make run
```

This configures, builds, and launches the app. Presets look for Qt in
`.tools/Qt/6.12.0/gcc_64`, then system locations. See
[development setup](docs/development.md) to install Qt or select another SDK.

| Command | Action |
| --- | --- |
| `make build` | Build a debug binary |
| `make run PRESET=release` | Build and launch an optimized binary |
| `make test` | Build and run tests |
| `make test PRESET=sanitized` | Run ASan/UBSan tests on Linux |
| `make format` | Format C++ source |
| `make format-check` | Check formatting |
| `make lint` | Run clang-tidy |

Run `make` for all shortcuts. Builds are incremental and use CMake + Ninja.

Development runs use the same storage locations as the installed app. See
[isolated manual runs](docs/storage.md#application-storage-paths) to keep test
libraries separate. Automated tests use temporary storage.

## Documentation

- [Development setup](docs/development.md): Qt installation, CMake, and tooling
- [Testing](docs/testing.md): test selection, sanitizers, and CI
- [Storage](docs/storage.md): paths, UI preferences, database lifecycle, and migrations
- [Local UCI engines](docs/engines.md): registration, connections, position-search API, and limits
- [Diagnostics](docs/diagnostics.md): troubleshooting, local logs, and privacy
- [Quick review](docs/quick-review.md): running scans, inspecting classifications, cancellation, and resume
- [Analyze board](docs/analysis-board.md): moves, navigation, annotations, and preferences
- [Chess core](docs/chess-core.md): adapter API, FEN policy, moves, and identity
- [Importing games](docs/import.md): local files, paste, drops, limits, and results
- [PGN parsing](docs/pgn.md): documents, compatibility, recovery, limits, and cancellation

Advanced product settings and the review evidence model are documented in
[Review evidence and configuration](docs/review-evidence.md).

Review methodology: [refinement, stability and summaries](docs/review-summaries.md).
