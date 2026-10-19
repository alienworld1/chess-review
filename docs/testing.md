# Testing

Tests use C++20, CMake/CTest, Qt Test, and Catch2 3.8.1. Qt 6.12 is the API
baseline. Pure chess-domain cases live in `tests/unit/` and link
only the chess target and Catch2. Use risk-based regression tests rather than a
coverage quota.

## Configure, build, and run

```sh
make test
make test PRESET=release
make test PRESET=sanitized
```

Each command configures and builds before running CTest. Equivalent direct commands:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --preset dev --parallel 4
ctest --preset dev -L persistence
ctest --preset dev --repeat until-fail:5 --parallel 4
```

The dev, release, and sanitized configure/build/test presets use separate build
folders. Sanitizers require GCC or Clang on Linux. To select Clang, configure a
fresh directory with `-DCMAKE_CXX_COMPILER=clang++-18`. A compiler cannot safely be
changed in an existing build cache. Presets use CMake 3.24-compatible syntax.
See [development setup](development.md) for Qt SDK selection and
`CMakeUserPresets.json` overrides.

All test presets show failure output and fail if no tests match, including an
accidentally empty label/regex selection. Every Qt suite has a 30-second CTest
limit. Component labels are `diagnostics`, `persistence`, `startup`, `shell`, and
`app_paths`; Qt suites have `qt`, and all except `app_paths` also have `integration`.
Chess cases have `unit` and `chess` labels, assigned from their Catch2 tags.
Run them with `ctest --preset dev -L chess` or `./build/dev/tests/chess_tests`.
They cover established perft fixtures, strict FEN, legal moves, SAN/UCI,
make/undo, counter overflow/rollover, and history-aware identity. Adversarial cases
reject impossible checks before upstream move generation and verify en-passant
predecessors. Deterministic legal walks and 4,000 mutated byte inputs exercise
FEN/notation safety and restoration invariants under sanitizers.

Game-tree cases cover main lines, nested variations, shared-prefix reuse,
local promotion, deletion, stable IDs, independent copies, annotation accounting,
atomic failures, exact root/history preservation, special moves, and transpositions.
A 20,000-ply line and a broad two-ply tree exercise iterative navigation,
resolution, copying, destruction, and subtree deletion without timing assertions.
`game_tree_allocation_tests` is a separate executable with a test-only global
ordinary-allocation override. It fails each allocation in turn during line
addition, annotation replacement, subtree deletion, and copy assignment, checking
unchanged nodes, budgets, root context, and next allocated ID after every failure.
The override is absent from the app and ordinary chess/Qt test binaries. These
labels describe entire suites: individual suites can cover several boundaries.

Qt Test also supports selecting individual functions and data rows directly:

```sh
./build/dev/tests/app_paths_tests standardLocations:overrides-with-spaces-and-unicode
QT_QPA_PLATFORM=offscreen ./build/dev/tests/startup_tests unusableDataDirectoryFails resolvedPathsInitializeLazily
./build/dev/src/persistence/tests/persistence_tests closeDuringInitialization
```

CTest sets the offscreen platform only for the startup and shell widget suites. Direct
widget invocation must supply it when there is no desktop display. Use absolute
paths for Qt Test `-o` reports: temporary working directories are removed at exit.
No global
platform override is needed for Qt Core suites.

## Test ownership and safety

- `tests/qt/` owns application, widget, paths, and diagnostics tests.
- `src/persistence/tests/` owns private storage tests and SQL fixtures. Keep SQL
  fixture creation and private migration interfaces inside this module.
- Keep pure C++ domain tests under `tests/unit/` when domain modules exist.
  Link only their domain target and `Catch2::Catch2WithMain`, apply
  `chess_review_apply_options(target)`, and register with
  `catch_discover_tests(target PROPERTIES TIMEOUT 30 LABELS unit)`.
- Register Qt suites with `chess_review_add_qt_test(target component SOURCES ...
  LIBRARIES ...)`; use `WIDGETS` and `INTEGRATION` only when applicable. The helper
  applies project warnings and optional sanitizers without exporting test
  dependencies into production targets. Unknown options and missing keyword
  values fail configuration.

All five Qt runners construct `TestEnvironment` before the Qt application, isolating
working directories and Linux HOME/XDG roots. Exit with failure if it is invalid.
Declare it before the application
so workers and Qt shut down before the root is removed. Internal diagnostics
subprocesses inherit their parent's isolated environment and fixture directory;
fatal children allocate no unowned temporary root. Widget cases also receive
fresh XDG roots individually. Check `QTemporaryDir::isValid()` before using each
fixture. Never use normal user application locations or share writable databases
or fixture directories across parallel tests. Qt can cache paths, so preserve
this initialization order.

Use observable conditions and bounded waits for asynchronous behavior. Preserve
orderly worker shutdown before fixture destruction. Intentional fatal conditions
and process-wide logging probes run in subprocesses. A death test must verify
that the intended probe ran and produced its expected termination; an unrelated
crash must fail the test. Expected warnings must be
handled narrowly; do not globally suppress warnings or sanitizer findings.
Each suite fails on unexpected warnings reaching Qt Test. The offscreen widget
suite permits only the exact backend `propagateSizeHints()` warning, which remains
visible in its test log. Logging-handler tests deliberately intercept framework
messages and verify their safe capture/restoration separately.

## Catch2 dependency

With `BUILD_TESTING=ON` (the default), CMake downloads the pinned release archive
and verifies its SHA-256. The initial configure needs network access. For an
offline build, supply an already extracted, verified Catch2 3.8.1 source tree:

```sh
cmake --preset dev -DFETCHCONTENT_SOURCE_DIR_CATCH2=/absolute/path/to/Catch2-3.8.1
```

The source override is CMake's standard FetchContent override and bypasses archive
verification; verifying that supplied source is the developer's responsibility.
Upstream examples and self-tests are disabled. Catch2 is not linked into the app.
With `-DBUILD_TESTING=OFF`, neither Catch2 nor Qt Test is required, and no tests
are registered. Use a separate build directory for this configuration.

## Sanitizers and tooling

`CHESS_REVIEW_SANITIZERS=ON` instruments project targets with ASan/UBSan and links
executable runtimes; it does not instrument the prebuilt Qt SDK or Catch2.
The sanitized test preset enables leak detection and stops on sanitizer errors.
For a local environment that prevents LeakSanitizer from operating (for example,
a ptrace-restricted sandbox), override only that invocation:

```sh
cmake --build --preset sanitized
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/sanitized --output-on-failure --no-tests=error
```

The explicit `--test-dir` invocation avoids the test preset replacing the local
environment override. This establishes ASan/UBSan results, not leak-check results.
Do not commit global leak-disable settings. Investigate hosted failures; any
external-library suppression needs a narrow match and documented evidence.

`make format-check` and `make lint` inspect only project sources/tests, excluding
downloaded dependencies. CI uses clang-format/clang-tidy 18 and the checked-in
configurations. `CHESS_REVIEW_WARNINGS_AS_ERRORS=ON` promotes project compiler
warnings to errors; third-party targets do not receive these project flags.

## Continuous integration and desktop QA

Linux CI runs GCC Debug and Release tests on Qt 6.12.0,
and Clang 18 ASan/UBSan with leak detection on 6.12.0. Formatting and curated
clang-tidy run with the Debug configuration. Failure artifacts include
CTest logs and JUnit output when available, retained for seven days. Debug CI also
builds the application separately with `BUILD_TESTING=OFF` to guard against test
dependencies leaking into the application build. New commits cancel obsolete
runs of this workflow for the same branch or pull request.

CI uses GitHub-hosted disposable runners, read-only repository permissions,
commit-pinned Actions, and no persisted checkout credentials. It runs untrusted
pull requests without privileged triggers, repository secrets, deployment steps,
or shared dependency caches. The fixed aqtinstall version, PyPI dependencies,
Qt SDK/mirrors, and signed runner OS packages remain trusted build inputs; this
is not a fully hermetic toolchain. Review dependency and Action updates rather
than moving pins automatically.

Offscreen widget tests exercise startup, error presentation, clipboard privacy,
shell navigation, preferences, layout recovery, and graceful shutdown. The `shell`
suite uses temporary explicit settings filenames and covers settings failures
independently of database failures. `UiStateStore::closed()` guarantees its worker
has joined; retain fixtures/services until completion.

Persistence regressions check existing and fresh database locations with symbolic,
dangling, hard-linked, FIFO, and directory targets at each WAL/SHM/rollback-journal
and lock path. Oversized lock metadata and pre-existing cleanup locks fail before
Qt/SQLite opens them. Rejection must preserve database bytes, unrelated file bytes
and permissions, and the unsafe target, without creating a fresh database.

Linux shell subprocess tests compile the actual production `main.cpp` into a
`shell_exit_probe` executable with a test-only UI driver. They exercise window
close, File → Quit, application Quit, closing during startup/after storage failure,
and closing with Diagnostics open. Each child must exit normally after recording
`session.stopped` and saving UI state. A service-completion signal alone is not
sufficient: Qt can reject Quit while a visible window refuses its close event.
The children use temporary XDG roots; the full-profile process cases are skipped
on other platforms. No test driver or environment switch is compiled into the
production executable, and the probe is absent with `BUILD_TESTING=OFF`.

For optional shell render inspection, create an absolute output directory and run:

```sh
mkdir -p /tmp/chess-review-shell-renders
cat > /tmp/chess-review-shell-renders/screen.json <<'JSON'
{"screens":[{"name":"qa","width":3840,"height":2400}]}
JSON
QT_QPA_PLATFORM=offscreen:configfile=/tmp/chess-review-shell-renders/screen.json \
  QT_SCALE_FACTOR=1.25 \
  CHESS_REVIEW_SCREENSHOT_DIR=/tmp/chess-review-shell-renders \
  ./build/dev/tests/shell_tests layout
```

This captures five window sizes with enlarged text and normal/failure states.
The matrix includes a constrained 640×480 case and checks that navigation and
Copy diagnostics can be reached through the shell's scroll area. These images
are inspection artifacts, not pixel-perfect golden tests.

Native desktop behavior, accessibility, Windows behavior,
and mixed-DPI rendering still require separate release validation. Optional real
engine compatibility tests must supplement a deterministic fake UCI engine when
engine orchestration is implemented; this infrastructure task adds neither.
