# Release Notes - chess-review v1.0.0

Release date: 2026-11-25
Target platform: Linux x86-64 (AppImage)

## Highlights

- **Standalone Linux AppImage packaging:** Distributed as
  `chess-review-1.0.0-x86_64.AppImage` alongside a `.sha256` checksum,
  bundling Qt 6.12 runtime libraries, ICU libraries, required plugins (`qxcb`,
  `qoffscreen`, `qwayland`, `qsqlite`, `qsvg`, `qopensslbackend`), and
  `libxcb-cursor.so.0` so clean desktop Linux installations run without Qt SDK
  or developer tools.
- **Engine detection and missing-engine recovery:** **Settings -> Engines**
  can scan standard directories (`PATH`, `/usr/games`, `/usr/bin`,
  `/usr/local/bin`, `~/.local/bin`) for installed engines without executing
  them, flags missing executables on disk, and lets you relocate a moved
  executable (verifying matching SHA-256 identity and preserving profile options)
  to resume interrupted reviews. Launching external engines sanitizes AppImage
  library overrides (`LD_LIBRARY_PATH`, `QT_PLUGIN_PATH`, `APPDIR`).
- **Database upgrades and large library performance:** Schema v16 adds query
  indexes (`review_jobs_status`, `games_result_imported`), batches multi-game
  PGN imports (ordinal 1 committed immediately, subsequent games committed in
  chunks up to 32 games or 4 MiB), and records opening recognition caches inside
  the import savepoint transaction. Verified migration chain preservation with
  automatic backups across legacy schema versions (v2, v4, v10, v11, v14, v15
  to v16).
- **Accessibility, themes, and keyboard navigation:** Added `Ctrl+1` through
  `Ctrl+5` workspace shortcuts, `Ctrl+F` search focus in Games, WCAG AA (>=
  4.5:1) verified text and classification colors across Light and Dark modes,
  accessible names and tooltips on controls, High-DPI validation across 1x to
  2x scale factors, and **Help -> Third-Party Notices…** alongside piece
  artwork provenance (`THIRD_PARTY_NOTICES.md`).
- **Genuine Stockfish and regression coverage:** Added deterministic moved-engine
  recovery and opt-in real Stockfish end-to-end tests (`stockfish_regression`
  and `review_acceptance`) covering blunder classification, puzzle generation
  and solving, live MultiPV analysis, and clean process teardown.

## Core capabilities in v1.0.0

- **Games:** Single-file, pasted, and multi-game PGN import; optional public
  Chess.com and Lichess link import; local SQLite persistence; opening
  recognition from bundled Lichess CC0 ECO data; search, filter, and sort.
- **Review:** Bounded quick-scan, deepening, verification, and escalation
  review pipeline; human-calibrated win probability alongside optional raw WDL;
  interrupted review detection and resume; key-moment explanations, interactive
  retry, and annotated PGN export.
- **Analyze:** Interactive board with legal move tree, variations, comments,
  NAGs, board arrows/squares, and live MultiPV engine analysis with previewing.
- **Puzzles:** Automatic puzzle generation from reviewed blunders, manual
  position saving from Analyze, offline solving with hints, alternative grading,
  reveal, and solution replay.

## Requirements and notes

- **External UCI engine required for analysis:** `chess-review` intentionally
  does not bundle any chess engine executable. Install an external UCI engine
  such as Stockfish separately and add it under **Settings -> Engines**.
- **FUSE-less Linux hosts:** If the host does not permit FUSE mounts (such as
  unprivileged containers), launch with
  `./chess-review-1.0.0-x86_64.AppImage --appimage-extract-and-run` or extract
  first with `--appimage-extract`.
- **Host graphics and TLS dependencies:** The AppImage relies on host `glibc`,
  `libstdc++`, OpenGL/Mesa/Wayland/X11 system libraries, and OpenSSL 3 if
  using optional game-link URL imports. Core local PGN import, review,
  analysis, and puzzle features require zero internet access.
- **Platform scope:** v1.0.0 targets Linux x86-64; Windows packaging is deferred
  to a later release.
