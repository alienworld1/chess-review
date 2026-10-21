# chess-review

A local-first desktop chess review app built with C++20 and Qt Widgets.
Analyze now provides a playable board with legal move entry, branching history,
promotion, keyboard controls, and annotations. Analysis is session-only; an exit
confirmation protects against accidental loss. Engine analysis and guided review
are not implemented yet. Home, Games, Puzzles, and Settings remain placeholders.

The shell includes navigation, saved window preferences, local SQLite storage,
and diagnostics. A pure C++ chess core now provides validated positions, legal
moves, notation, game trees, and history-aware identity. A pure C++ PGN reader
parses single and multi-game input. An asynchronous application service imports
owned PGN text into transactional SQLite snapshots with duplicate detection;
games can be listed and reopened through the repository. Import/library widgets
are not connected yet.

## Requirements

- C++20 compiler (GCC or Clang on Linux)
- Qt **6.12+** with Widgets, SVG, SQL, and the QSQLITE driver
- CMake **3.24+**, Ninja, and GNU Make
- Qt Test for tests (enabled by default); initial test configuration needs network
  access to fetch pinned Catch2
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
- [Diagnostics](docs/diagnostics.md): troubleshooting, local logs, and privacy
- [Analyze board](docs/analysis-board.md): moves, navigation, annotations, and preferences
- [Chess core](docs/chess-core.md): adapter API, FEN policy, moves, and identity
- [PGN parsing](docs/pgn.md): documents, compatibility, recovery, limits, and cancellation
