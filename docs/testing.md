# Testing

Tests use CMake/CTest, Qt Test, and Catch2. Run the full suite from the repository
root:

```sh
make test
make test PRESET=release
make test PRESET=sanitized
```

Each command configures and builds first. The presets use separate build folders;
the sanitized preset requires GCC or Clang on Linux. For focused work, build and
select a CTest label or test name:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --preset dev -L chess
ctest --preset dev -L persistence
ctest --preset dev -R '^quick_review$'
ctest --preset dev --repeat until-fail:5 --parallel 4
```

Test presets show failure output and fail if no tests match. Catch2 cases can
also be run directly, for example `./build/dev/tests/chess_tests`. Qt Test
executables accept individual function and data-row names. CTest sets the
offscreen platform for widget suites; direct widget invocations without a
working display need `QT_QPA_PLATFORM=offscreen`. Use absolute paths for Qt Test
`-o` reports because temporary working directories are removed at exit.

## Dependencies and tooling

With `BUILD_TESTING=ON` (the default), CMake fetches pinned Catch2 3.8.1 and
verifies its archive hash. For an offline build, verify a local source tree and
configure with:

```sh
cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_CATCH2=/absolute/path/to/Catch2-3.8.1
```

The standard FetchContent source override bypasses archive verification. For
pinned toml++ 3.4.0, use
`-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=/absolute/path/to/tomlplusplus-3.4.0`
after verification against [the dependency record](../third_party/tomlplusplus/README.md).
With `BUILD_TESTING=OFF`, Catch2 and Qt Test are not required; use a separate
build directory. See [development setup](development.md) for Qt and compiler
selection. Do not change the compiler in an existing build cache.

`make format-check` and `make lint` check project sources and tests. CI uses
clang-format and clang-tidy 18. `CHESS_REVIEW_WARNINGS_AS_ERRORS=ON` promotes
project compiler warnings to errors.

The sanitized preset enables ASan/UBSan and leak detection. If the local
environment prevents LeakSanitizer from operating, override only that run:

```sh
cmake --build --preset sanitized
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/sanitized --output-on-failure --no-tests=error
```

This does not establish leak-check results. Do not commit global leak-disable
settings; investigate failures and document any narrowly scoped suppression.

## Writing tests

Choose tests for meaningful risks and observable behavior. Pure domain tests
belong in `tests/unit/`; Qt application and widget tests belong in `tests/qt/`;
private database tests and SQL fixtures belong in `src/persistence/tests/`.
Domain tests should link only the relevant domain target and
`Catch2::Catch2WithMain`, apply `chess_review_apply_options(target)`, and register
with `catch_discover_tests(target PROPERTIES TIMEOUT 30 LABELS unit)`. Register Qt
suites with `chess_review_add_qt_test`; use `WIDGETS` and `INTEGRATION` only when
applicable.

Qt runners construct `TestEnvironment` before the Qt application and fail if it
is invalid. Keep it alive until workers and Qt shut down. Use valid temporary
fixtures and isolated HOME/XDG roots; never share writable databases or use normal
user application locations. For asynchronous behavior, use observable conditions
and bounded waits, and drain workers before destroying fixtures. Test intentional
fatal conditions in subprocesses, verifying the intended probe and termination.
Handle expected warnings narrowly rather than suppressing warnings globally.

The [deterministic fake UCI engine](fake-uci.md) supports protocol and failure
scenarios without installing a real engine. The review acceptance fixture details
are in [tests/fixtures/review](../tests/fixtures/review/README.md). The
[curated intelligence corpus](../tests/fixtures/intelligence/README.md) adds real
game scores and exact reasoning witnesses; run it with `ctest --preset dev -L corpus`.

## CI and desktop checks

Linux CI runs Debug and Release tests, ASan/UBSan with leak detection, formatting,
and clang-tidy. It also builds the app with `BUILD_TESTING=OFF`. Offscreen tests
exercise widget behavior but do not replace native checks of window management,
file dialogs, screen readers, physical mixed-DPI transitions, or Windows.

For the connected review journey and optional real-engine checks:

```sh
ctest --preset dev -R '^review_acceptance$'
CHESS_REVIEW_STOCKFISH=/absolute/path/to/stockfish \
  build/dev/tests/review_acceptance_tests realEngineJourney
QT_QPA_PLATFORM=offscreen CHESS_REVIEW_STOCKFISH=/absolute/path/to/stockfish \
  build/dev/tests/quick_review_tests stockfishProducesUsefulReview pinnedEngineCacheReuse
```

The cache probe requires the reviewed pinned Stockfish binary. Other conformant
engines can run reviews but cannot establish that resource policy's reuse gate.

For review screenshots, create an output directory and set
`CHESS_REVIEW_ACCEPTANCE_RENDER_DIR` when running `review_acceptance_tests renderJourney`.
The default matrix covers both themes, normal/enlarged fonts and three window sizes.
`CHESS_REVIEW_ACCEPTANCE_RENDER_STATES=summary,review,practice` limits captured screens;
`CHESS_REVIEW_ACCEPTANCE_RENDER_COMPACT=1` captures only normal fonts at 1100×720.
Use `QT_SCALE_FACTOR=1.25`, `1.5`, or `2` for additional DPI checks. The probe checks
square boards, usable primary actions, physical output dimensions, and that normal
Review layouts fit without scrolling the workspace. Keep expensive screenshot
generation separate from the full timing-sensitive engine test suite.

The optional `stockfishProducesUsefulReview` probe uses an explicitly captured Fast
fixture budget (50,000 quick / 100,000 deepening / 500,000 verification nodes) to
check useful classifications, accuracy and cache-independent reopening with the
explicitly selected local engine. It keeps its 90-second deadline. Standard and
Thorough budgets, bounded rounds and exact work counts are covered by deterministic
fake-engine tests; this real-engine probe is not a hardware performance benchmark.

## Puzzle coverage

`puzzle_tests` exercises pure verification, mate and ambiguity handling, exact
history, effort limits, and multi-decision sequences for both colors.
`puzzle_repository` covers codecs, completed-review candidate selection,
transaction rollback, deletion suppression, and interrupted checkpoints.
`puzzle_ui` uses the deterministic UCI engine for finite manual verification,
cancellation/resume, weak evidence, offline solving, and independently verified
unlisted alternatives. It also exercises hints, reveal, linked Analysis, and
asynchronous shutdown. These fixtures do not calibrate human puzzle difficulty.

For an optional native-widget capture, invoke the Qt executable with an absolute
output directory:

```sh
QT_QPA_PLATFORM=offscreen CHESS_REVIEW_PUZZLE_RENDER_DIR=/tmp/puzzle-renders \
  ./build/dev/tests/puzzle_ui_tests offlineSolveHintsRevealAnalyzeAndDelete
```

Repeat with `QT_SCALE_FACTOR=2` for enlarged-layout inspection. Ordinary tests do
not produce screenshots or modify user storage.

An optional functional probe uses an explicitly selected, trusted local engine,
creates a puzzle with a 50,000-node baseline, then solves it offline:

```sh
QT_QPA_PLATFORM=offscreen CHESS_REVIEW_REAL_PUZZLE_ENGINE=/absolute/path/to/stockfish \
  ./build/dev/tests/puzzle_ui_tests realEngineCreatesAnOfflinePuzzle
```

This probe checks integration rather than production-preset performance or puzzle
difficulty calibration. It is skipped unless the environment variable is set.
