# chess-review

A local-first desktop chess review app built with C++20 and Qt Widgets.
Import PGN games, explore moves on an interactive board, and review games using
local UCI engines. Games, review evidence, and preferences stay on your computer.

- **Games:** import PGN files, pasted text, or public Chess.com and Lichess game links. Browse and reopen saved games offline.
- **Analyze:** play legal moves, explore variations, add annotations, and follow
  live engine analysis.
- **Review:** scan a saved game, inspect classifications and evidence, explore
  alternatives, resume interrupted work, and export annotated PGN. Saved reviews
  can be inspected and exported without an engine.
- **Puzzles:** generate verified lessons from reviews, save analysis positions,
  and solve saved sequences with hints, retry, reveal, and analysis.
- **Settings:** manage and test UCI engines, set review and resource preferences, and edit advanced TOML.

## Requirements

- Linux x86-64 desktop for prebuilt AppImage releases (`chess-review` does not
  bundle a chess engine; bring a local UCI engine such as Stockfish)
- For building from source:
  - C++20 compiler (GCC or Clang on Linux)
  - Qt **6.12+**, including Widgets, Network, SVG, SQL, and the QSQLITE driver
  - A working Qt TLS backend for optional game-link imports
  - CMake **3.24+**, Ninja, and GNU Make
  - Network access on initial configuration for pinned toml++ and, when testing,
    Catch2, unless local source overrides are supplied
  - Qt Test for tests; clang-format 18 and clang-tidy 18 for formatting and linting

## Linux AppImage

Prebuilt Linux releases ship as a standalone x86-64 AppImage (`1.0.0`):

```sh
sha256sum -c chess-review-1.0.0-x86_64.AppImage.sha256
chmod +x chess-review-1.0.0-x86_64.AppImage
./chess-review-1.0.0-x86_64.AppImage
```

If FUSE is not available on your host (for example inside containers), run with
`--appimage-extract-and-run`. Then open **Settings -> Engines** to auto-detect or
browse to your local UCI chess engine. See [Release notes](docs/release-notes.md)
and [Local UCI engines](docs/engines.md).

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
| `make package-linux` | Build Linux x86-64 AppImage in `dist/` |
| `make package-verify` | Verify Linux AppImage in a clean environment |

Run `make` for all shortcuts. Development runs use the installed app's storage
locations; see [storage paths](docs/storage.md#application-storage-paths) for
isolated manual runs. Automated tests use temporary storage.

## Documentation

**Using the app**

- [Importing games](docs/import.md) · [Analyze board](docs/analysis-board.md) ·
  [Quick review](docs/quick-review.md) · [Personal puzzles](docs/puzzles.md) ·
  [Reopen and recovery](docs/reopen-recovery.md)
- [Local UCI engines](docs/engines.md) · [Settings and TOML](docs/settings.md) · [Diagnostics and troubleshooting](docs/diagnostics.md)

**Developing**

- [Release notes (v1.0.0)](docs/release-notes.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)
- [Development setup](docs/development.md) · [Testing](docs/testing.md) ·
  [Storage](docs/storage.md) ·
  [Storage internals](docs/storage-internals.md) · [Engine infrastructure](docs/engine-internals.md)
- [Chess core](docs/chess-core.md) · [PGN parsing](docs/pgn.md) ·
  [Review evidence and configuration](docs/review-evidence.md) ·
  [Review methodology](docs/review-summaries.md)
