# chess-review

A local-first desktop chess review app built with C++20 and Qt Widgets.
Import PGN games, explore moves on an interactive board, and review games using
local UCI engines. Games, review evidence, and preferences stay on your computer.

- **Games:** import PGN files or pasted text, browse saved games, and reopen them.
- **Analyze:** play legal moves, explore variations, add annotations, and follow
  live engine analysis.
- **Review:** scan a saved game, inspect classifications and evidence, explore
  alternatives, and resume interrupted work. Saved reviews can be inspected without
  an engine.
- **Settings:** register trusted local UCI engines and manage connections.
  Puzzles is currently a placeholder.

## Requirements

- C++20 compiler (GCC or Clang on Linux)
- Qt **6.12+**, including Widgets, SVG, SQL, and the QSQLITE driver
- CMake **3.24+**, Ninja, and GNU Make
- Network access on initial configuration for pinned toml++ and, when testing,
  Catch2, unless local source overrides are supplied
- Qt Test for tests; clang-format 18 and clang-tidy 18 for formatting and linting

## Build and run

With the Qt SDK installed, run from the repository root:

```sh
make run
```

The presets look for Qt in `.tools/Qt/6.12.0/gcc_64`, then system locations.
See [development setup](docs/development.md) for installation and SDK overrides.

| Command | Action |
| --- | --- |
| `make build` | Build a debug binary |
| `make run PRESET=release` | Build and launch an optimized binary |
| `make test` | Build and run tests |
| `make test PRESET=sanitized` | Run ASan/UBSan tests on Linux |
| `make format` / `make format-check` | Format or check C++ source |
| `make lint` | Run clang-tidy |

Run `make` for all shortcuts. Development runs use the installed app's storage
locations; see [storage paths](docs/storage.md#application-storage-paths) for
isolated manual runs. Automated tests use temporary storage.

## Documentation

**Using the app**

- [Importing games](docs/import.md) · [Analyze board](docs/analysis-board.md) ·
  [Quick review](docs/quick-review.md) · [Reopen and recovery](docs/reopen-recovery.md)
- [Local UCI engines](docs/engines.md) · [Diagnostics and troubleshooting](docs/diagnostics.md)

**Developing**

- [Development setup](docs/development.md) · [Testing](docs/testing.md) ·
  [Storage](docs/storage.md)
- [Chess core](docs/chess-core.md) · [PGN parsing](docs/pgn.md) ·
  [Review evidence and configuration](docs/review-evidence.md) ·
  [Review methodology](docs/review-summaries.md)
