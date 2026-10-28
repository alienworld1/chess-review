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
`app_paths`, `game_repository`, `analysis`, `move_navigation`,
`move_navigation_adversarial`, and `move_navigation_state`; Qt suites have `qt`.
All except `app_paths` and `move_navigation_state` also have `integration`.
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
Four deterministic seeds run 800 mixed add/promote/delete/annotation operations;
after every edit, bounded graph traversal checks reachability, unique parents,
acyclicity, sibling move uniqueness, non-reused IDs, main-line order, annotation
budgets, canonical SAN, and contexts against independent Position replay.
Focused regressions cover late fullmove overflow, late node exhaustion, and
256 caller-supplied promotion representations that cannot match a legal path.
`game_tree_allocation_tests` is a separate executable with a test-only global
ordinary-allocation override. It fails each allocation in turn during line
addition, annotation replacement, subtree deletion, and copy assignment, checking
unchanged nodes, budgets, root context, and next allocated ID after every failure.
A separate regression forces the first allocation to fail and verifies that
reusing an existing child or entire path still succeeds without allocation.
The override is absent from the app and ordinary chess/Qt test binaries. These
labels describe entire suites: individual suites can cover several boundaries.

Qt Test also supports selecting individual functions and data rows directly:

```sh
./build/dev/tests/app_paths_tests standardLocations:overrides-with-spaces-and-unicode
QT_QPA_PLATFORM=offscreen ./build/dev/tests/startup_tests unusableDataDirectoryFails resolvedPathsInitializeLazily
./build/dev/src/persistence/tests/persistence_tests closeDuringInitialization
```

CTest sets the offscreen platform for widget suites, including startup, shell,
analysis, move navigation, engine settings and live analysis. Direct widget
invocation must supply it when there is no desktop display. Use absolute
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

All Qt runners construct `TestEnvironment` before the Qt application, isolating
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
clang-tidy run with the Debug configuration. Static analysis runs in a separate
job with two translation units at a time, so it has its own time budget and does
not delay the Debug test result. Failure artifacts include
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
engine orchestration is implemented. The fake fixture below does not replace
real-engine compatibility validation.


## PGN tests

`ctest --preset dev -L pgn` runs the pure C++ `pgn_tests` executable. Fixtures
exercise owned source/tag/result data, immutable original played history, nested
variations, comment attachment, ordered/duplicate NAGs, repeated-branch merging,
FEN roots, castling/en passant/underpromotion, strict SAN suffixes, UTF-8, and
percent escape lines. Recovery cases include valid–invalid–valid input, misleading
headers inside comments, conflicting header blocks, nested failures, and
unreadable lexical tails, unsupported quoted text, and encoding errors in
ignored text before a following header. Incomplete move-number indications
are rejected rather than published as empty games. Limits include exact boundaries, zero budgets, decoded
tag escapes, source occurrence accounting, newline separators, warning suppression,
and next-header lookahead. Cancellation covers terminal behavior, earlier document
ownership, and a concurrent stop request against a long-comment workload (allowing complete
publication if completion wins the race).

A 12,000-ply line, 100 nested variations, and a broad root with 1,000 repeated
branches exercise iterative construction/destruction. A deterministic seed mutates
1,500 inputs using insertions/deletions/arbitrary bytes; every outcome must be
bounded and every accepted tree is independently walked with Position make/undo.
This shares the chess adapter, so it is a consistency oracle rather than an
independent chess-rules implementation. No test depends on a performance threshold.
The chess suite separately validates the public bulk-tree factory against malformed
records, duplicate siblings, illegal moves, FEN provenance, budgets, cancellation,
and subsequent editing/ID allocation.


PGN import conformance regressions also cover compact tag pairs, comments and
percent escapes between tag tokens, separated move-number periods, surplus move
numbers, bare black move numbers without false side warnings, mixed castling
spellings, reserved token boundaries after a completed result, and original played
lines that end before a merged analysis continuation. Token-gap comments retain
content and still obey comment/aggregate budgets; unterminated comments remain
unreadable tails.

The isolated `game_tree_allocation_tests` executable now also fails each ordinary
allocation during bulk construction and PGN parsing. A PGN failure must propagate
as an exceptional resource failure, leave earlier documents unchanged, and leak
no staged state. Allocation interception lives in `tests/support/AllocationFailure.cpp`;
its `FailAfter` guard is private to this executable. Splitting the hook from test
logic avoids analyzer confusion through an inline global delete override and
libstdc++ stop-token reference counting. No static-analysis rule is suppressed.
No hook is linked into ordinary test binaries or production. The PGN fault case
has a `pgn` label and is included by `ctest --preset dev -L pgn`.


## Game persistence and import

`ctest --preset dev -R '^(persistence|game_repository)$' --output-on-failure`
runs storage migration/lifecycle tests and transactional game/import regressions.
All SQL fixtures remain under `src/persistence/tests/`; application/UI tests do
not issue queries. Game tests cover reopened snapshots and IDs, Unicode and
unknown tags, annotation-independent identity, per-game partial imports,
INSERT/COMMIT rollback with retry, corrupt/oversized records, migration from
version 1, bounded request queues, and cancellation/shutdown while SQLite is
waiting on a fixture write lock. The latter keeps a GUI-thread heartbeat running
and verifies that the accepted game is committed and reported before closure.


The adversarial game-persistence regressions also cover embedded NUL and invalid
UTF-8 in database metadata, oversized/damaged rows alongside healthy summaries,
small retained display buffers and Unicode clipping, warning-rich import reports,
byte-mutated accepted PGNs remaining persistable, and duplicate-validation cache
invalidation on both storage-connection and external SQLite writes. A synthetic
repeated-duplicate probe reports elapsed time without a machine-specific timing
assertion. Foreign UTF-16 database fixtures must be rejected without changing
file contents; application-created databases use UTF-8.


Snapshot/identity v1 fixtures also exercise migration 003 into v2, including
field-level U+FEFF, preserved game IDs/import times/source bytes, mixed healthy
and damaged records, and correct duplicate behavior after reopening. Injected
row-transform errors and exceptions must roll back schema/data/ledger changes
and retain the verified backup. Frozen v1/v2 identity digests guard wire changes.

## Analyze board tests

`ctest --preset dev -R '^(analysis|shell|startup)$'` covers controller move validation,
branch preservation, per-node annotations and undo/redo, promotion cancellation,
castling, en passant, underpromotion, stale input rejection, and worker shutdown.
Widget tests exercise both orientations, boundary hit testing, drag/click entry,
keyboard modes, annotations, hide cancellation, and accessible square actions.
Regressions also cover direct jumps across forks, numbered histories from a
Black-to-move FEN root, separate annotations at transpositions, existing-node
reuse, annotation/undo limits, and releases without intermediate pointer events.
Cancelled promotion actions cannot submit after flipping, hiding, disabling, or
changing preferences. Accessibility checks include unavailable square actions and
legal quiet/capture/en-passant descriptions, including a quiet knight move to an
en-passant target without inheriting the pawn’s capture marker.
Preference tests round-trip board choices and reject malformed colors.

The production process-exit probe also creates a live analysis and tests window,
menu, and application Quit, both accepting discard and cancelling before a later
confirmed exit. Cancellation must preserve the live position; confirmed exit must
finish services and return normally from the real application event loop.

For board screenshots, use the large offscreen screen configuration shown above
and run `analysis_tests layout` with `CHESS_REVIEW_SCREENSHOT_DIR` set to an
absolute output directory. Repeat with `QT_SCALE_FACTOR=1`, `1.25`, `1.5`, and `2`.
The rows cover 1360×850, 1100×720 flipped, 800×600 constrained, and enlarged fonts.
Window sizes clamp normally on smaller screens. Images support visual inspection,
not pixel-identical golden comparisons. Offscreen popup tests permit exactly the
backend's propagateSizeHints, raise, and keyboard-grab warnings; other warnings
still fail. Native accessibility and mixed-monitor/Windows behavior remain
separate release checks.

## Move timeline and variation navigation

`ctest --preset dev -R '^(move_navigation(_state|_adversarial)?|analysis|shell|startup)$'` covers the
chosen-line timeline, numbered FEN roots, PGN token hit testing and per-move
accessibility, branch expansion, remembered choices, snapshot reuse, orientation,
and validated UI preferences. Queued navigation tests issue multiple intents
before delivering worker results and verify only the latest target is displayed.
The worker retains one in-flight operation plus one replaceable navigation target.
The 512-ply construction driver uses event-loop deliveries with a 15-second
deadline, avoiding polling sleeps that artificially throttle worker round trips.

The Widgets-free `move_navigation_state` suite reaches the supported 16,384-ply depth without
recursive construction. UI fixtures exercise a 512-ply line, all 20 initial move
branches, and 96 nested variations, with an event-loop heartbeat while building
the line. These are correctness/responsiveness checks, not portable timing claims.
PGN and layout screenshots can be captured with `CHESS_REVIEW_SCREENSHOT_DIR` set
to an existing absolute directory. Native screen readers, actual monitor changes,
and Windows still need platform QA.

The `move_navigation_adversarial` suite additionally verifies explicit fork choices
during validation, timeline accessibility selection after external navigation,
visibility of wrapped moves in short panes, per-view accessible child ownership,
visible focus on selected moves, inherited palette/font, hover invalidation after
scrolling, and initial navigation boundaries. Its rendered fixtures use the same
`CHESS_REVIEW_SCREENSHOT_DIR` option as the workspace fixtures.


## Local PGN import and Games

`ctest --preset dev -R '^(import|shell|startup|game_repository)$'` covers local
acquisition, multi-file mixed outcomes, duplicates and provenance, empty input,
strict Unicode, busy rejection, cancellation before parsing and after save
submission, batch game/warning budgets, oversized files, pagination, stale/null
callbacks, startup failure, paste confirmation, shortcut focus, copy-only drops,
and error/truncation display. SQL fixtures remain in persistence; the presentation
fixture injects typed repository responses for damaged metadata and read errors.
The 10,000-game stress case uses an actual event loop so polling sleep does not
artificially throttle each worker round trip. A GUI heartbeat checks that the
loop remains responsive during the batch.

The production-main exit probes also close during file acquisition and during an
active multi-game import. They require normal process exit, saved Games
navigation state, and orderly storage/session completion. No test driver is
compiled into the production app.

Large paste regressions verify bounded read-only previews, surrogate clipping,
explicit full-input confirmation and unchanged source submission; small pastes
retain native editing and undo.

`import_tests layouts` accepts the existing `CHESS_REVIEW_SCREENSHOT_DIR` option.
Run it with `QT_QPA_PLATFORM=offscreen` and `QT_SCALE_FACTOR=1`, `1.25`, `1.5`, or
`2` to capture 1100×720, 1360×850, 1800×1000, and constrained layouts. These checks
do not establish native file-picker, screen-reader, Windows, or physical monitor
behavior; those remain manual platform QA.


The import adversarial regressions additionally reject host/UNC/relative and
malformed encoded file URLs before acquisition, compare stored pasted Unicode
identities against distinct ordinary-space games, preserve unchanged source
bytes and edited NBSP/line-separator content, reject oversized or invalid Unicode
clipboard insertions, and verify terminal notification/admission/closure ordering.
They inject unrelated request-scoped progress and flap readiness while a read is
pending, verify exact cross-file game-budget exhaustion, and preserve diagnostic
selection/focus and viewport position during updates. `visibleReportStress`
presents 2,000 failed-game results with a UI heartbeat and reports local duration /
maximum heartbeat gap; these measurements are observations, not portable latency
budgets. No SQL fixture or query was moved outside persistence.

## Phase 2 exit gate

`ctest --preset dev -R '^game_opening$'` imports a multi-game PGN containing the
complete Opera Game, an en passant setup, and an underpromotion. It opens each
through the Games UI, walks all nodes/variations, checks mate, FEN numbering and
plain-text comments/NAGs, and reopens the same stored game and variation after
deleting the source. Replacement/discard, unrelated replies, failed loads and
readiness invalidation have focused regressions.

A second regression launches the production `main.cpp` twice with the existing
test-only UI driver: import by drop, select/open an annotated variation, close,
delete the source, restart, and verify the same game/node and orderly process
exit. Drivers are never linked into the production binary. Linux XDG profiles are
temporary. The test supports `CHESS_REVIEW_SCREENSHOT_DIR` for a rendered imported
game workspace. Offscreen checks do not certify native desktop accessibility.

## Deterministic fake UCI engine

On Unix builds with `BUILD_TESTING=ON`, `build/dev/tests/fake-uci-engine` is a
standalone scripted process, linked only to Qt Core. It performs no chess search
or legality validation. It is not installed or linked into the application.
Linux is validated; Windows pipe support is not implemented. Other Unix systems
are not validated. No Stockfish installation is required.

```sh
cmake --build --preset dev --target fake_uci_engine fake_uci_tests
ctest --preset dev -L fake_uci
./build/dev/tests/fake-uci-engine --help
```

Launch the executable directly with `QProcess` and separate arguments, not through
a shell: program `build/dev/tests/fake-uci-engine`, arguments `--scenario` and an
absolute scenario path. Stdin, stdout and stderr must be pipes exclusively owned
by this child, as with the default QProcess channel setup. Merged stdout/stderr
pipes are also supported. Direct terminal use, forwarded channels, and file/device
redirection are rejected before descriptor flags change. This prevents a crash or
forced termination from leaving shared terminal/file descriptions nonblocking.
Drain both output channels and enforce a parent-side deadline. Keep scenario files
in ordinary private test directories; FIFOs/devices/directories are rejected. A
symlink to a regular scenario file is allowed. Concurrent replacement by another
process with the same user is outside this fixture's file-safety boundary.
There is no default conversation: `--scenario <path>` is required. `--help` exits
successfully; missing/unknown arguments fail. Each process has independent state.

### Scenario format (version 1)

```json
{
  "version": 1,
  "steps": [
    {"expect": "uci", "actions": [
      {"type": "write", "stream": "stdout", "data": "id name Example fake\nuciok\n"}
    ]},
    {"expect": "isready", "actions": [
      {"type": "write", "stream": "stdout", "data": "readyok\n", "afterMs": 20}
    ]},
    {"expect": "quit", "actions": [{"type": "exit"}]}
  ]
}
```

`version`, `steps`, and each step's `expect` and `actions` are required. Actions
require `type`; `afterMs` defaults to zero. Unknown fields, types, invalid numeric
values, unsupported versions, and missing required fields are rejected before
commands are read. Configuration errors identify the step/action when applicable.
JSON input must be valid UTF-8, and decoded commands/output must contain valid
Unicode (unpaired surrogate escapes are rejected). Commands are compared as ASCII
space/tab-separated tokens, with leading/trailing whitespace ignored. LF, CRLF,
and CR delimit input lines; empty lines are ignored. Other bytes are significant.
Expected commands cannot be empty or contain a line delimiter. Commands have no
chess, wildcard, regex, interpolation, or scripting semantics.

| Action | Fields and behavior |
| --- | --- |
| `write` | Required `stream` (`stdout` or `stderr`) and string `data`. Emit exactly its UTF-8 bytes, without appending a newline. JSON escapes allow NUL, tabs, CR/LF, partial lines, and unterminated output. |
| `cancelPending` | Immediate; `afterMs` must be zero. Cancel all scheduled actions accepted so far, including earlier actions in this step. Already-issued output still drains. |
| `exit` | Cancel future actions, stop accepting input, flush already-issued output, then exit successfully. |
| `crash` | Cancel future actions, flush already-issued output and the stderr marker `fake-uci: intentional crash`, then abort with SIGABRT. Core files are disabled. |
| `hang` | Cancel future actions and ignore subsequent commands until terminated externally. Already-issued output still drains; stdin EOF also exits successfully. |

Offsets are nonnegative integer milliseconds (maximum 2,147,483,647) from the
acceptance of that step's command, not from the previous action. Writes and
terminal actions run in deadline order; equal deadlines preserve declaration and
command-acceptance order. Delays are minimum scheduling offsets, not wall-clock
accuracy promises. Use observable output and generous bounded test deadlines.
Reported nodes, depth, scores, and engine time are literal scripted data and never
depend on real elapsed time. Input remains responsive during delays and output
backpressure. Ordering is preserved separately for each output stream; a parent
cannot infer a total arrival order between stdout and stderr.

An empty action list models responsive silence, but does not make EOF successful.
An optional step field `allowEof: true` explicitly permits successful EOF after that
step; it requires an empty action list and defaults to false. Use it only for
intentional silence probes, never ordinary `setoption`/`position` steps.
Pending actions survive subsequent commands unless explicitly cancelled. A scenario must contain at least one
`exit`, `crash`, or `hang` action; consuming the final expected command alone does
not terminate the process. Additional commands before a delayed terminal action
are mismatches. EOF without a line delimiter does not accept the partial command:
it fails as incomplete. EOF with no partial command exits successfully after an
intentional hang or after an `allowEof` step; otherwise it fails as incomplete.
Successful EOF termination discards future scheduled actions and flushes
already-issued bytes. Failures discard pending output to prioritize diagnostics.

Limits are 1 MiB of scenario bytes, 4,096 steps, 4,096 actions total, 64 KiB per
input/expected command line excluding delimiters, and 1 MiB of outstanding output
across scheduled writes and buffered streams. Oversized input lines fail without
waiting for a delimiter. Output drains through nonblocking pipes; a parent must
keep draining both channels and enforce its own deadline for deliberate hangs or
blocked output. The fixture has no hidden watchdog or automatic retry.

| Exit | Meaning |
| --- | --- |
| 0 | Explicit normal exit, help, or EOF in an explicit `allowEof`/hanging state. |
| 2 | Invalid arguments, unreadable/invalid scenario, or scenario configuration limits. |
| 3 | Unexpected command or oversized command line. |
| 4 | Premature stdin EOF. |
| 5 | Transport initialization/read/write failure or outstanding-output limit. |

Intentional crashes use signal termination, not these normal exit codes.
Runtime failure messages identify the one-based expected step, or the originating
step for a scheduled write failure. Command mismatches
include bounded hexadecimal previews so input cannot inject diagnostic lines.
Fixture stderr is test-local and is not sent to production diagnostic logs.

### Included scenarios

Files under `tests/fake-uci-engine/scenarios/` are ordered conversations, so send
the commands in their `expect` fields. They are also readable examples for new
regressions:

- `analysis.json`: option types, MultiPV/WDL, explicit node progression, repeated
  searches, and centipawn/mate scores. The normal starting-position lines are
  legal; the second position has the concrete `d8h4` mate after f3/e5/g4.
- `delayed.json`: delayed handshake/readiness and analysis responses. Deliberately
  sends `isready` before `uciok` to prove that delayed output does not block input.
- `missing-wdl.json`: a generic engine without advertised WDL or WDL output.
- `malformed.json`: output chunks, CRLF, malformed metrics, embedded control and
  Unicode characters, separate stderr, and an invalid unterminated best move.
- `cancel.json` / `late-output.json`: responsive readiness during analysis,
  cancellation and best move, with pending output cancelled or deliberately kept.
- `silence.json`, `hang.json`, `crash.json`: withheld responses, unresponsive
  command handling, and intentional process death during analysis.

The `fake_uci` Qt Test suite launches real children, validates exact output and
failure status, and always reaps them before removing private fixtures. Process
hang/crash tests do not establish production engine supervision: that subsystem
is a separate implementation task.

Adversarial fixture regressions additionally verify premature EOF after ordinary
no-response commands, opt-in silent EOF, paired/unpaired Unicode, FIFO scenario
rejection, unchanged inherited file flags, merged output pipes, genuine broken
pipe errors, and cancellation/command responsiveness while the parent deliberately
leaves stdout unread. That last probe bypasses QProcess's automatic output drain. A maximum-size pending
action backlog also exercises deadline ordering and cancellation without an
arbitrary performance threshold.

## Engine registration

`engine_validation`, `engine_repository`, `engine_settings`, and `engine_exit`
cover this increment. Run them with:

```sh
ctest --preset dev -R '^engine_'
```

The validator uses the real deterministic child process through a private test
launch seam; production registration exposes no arguments or timeout overrides.
Tests lower handshake deadlines and, for aggregate-output rejection, the output
budget without changing production defaults. Cases cover typed options/codec,
fragmented protocol output, Unicode/punctuation paths, symbolic links, invalid
executables, replacement during validation, malformed output, crashes, hung
shutdown, cancellation, persistence, and application exit. All SQL fixtures stay
under `src/persistence/tests`.

The Settings fixture verifies explicit registration, duplicate feedback, cancelled
queued success, and reopening without engine launch. Optional offscreen screenshots
can be written to an existing directory by setting
`CHESS_REVIEW_ENGINE_SCREENSHOTS` when running `engine_settings_tests`.
The exit probe compiles production `main.cpp` with a test-only driver and requires
normal process exit during active validation, saving, and after registration. It
reopens committed profiles in a second isolated process. Linux XDG profile isolation
and the Unix fake-engine transport do not establish Windows support.

The fake fixture also supports a terminal `stubbornHang` action. Like `hang`, it
ignores protocol commands and observes stdin EOF, but it additionally ignores
SIGTERM. This exercises the production validator's terminate-to-kill escalation;
native signal handling remains confined to the fixture's pipe transport module.

Adversarial registration regressions cover unsolicited readiness bundled with the
UCI response, reuse after protocol rejection, inconsistent capability flags,
maximum-size combo declarations, growth during hashing, missing registry defaults,
and inserts that fail their readback postcondition. Settings checks full details,
visible escapes for invisible engine text, and no table data changes during
selection/progress. These tests supplement the original cancellation, shutdown,
output-limit, persistence, and application-exit coverage.

## Runtime UCI handshake

`engine_runtime` exercises the reusable `EngineSupervisor` through real scripted
children. Registration and runtime use the same handshake implementation. Runtime
cases cover persistent Ready, single-child admission, discovery equivalence and
mismatch, changed saved hashes, unsupported overrides, fragmented/minimal output,
idle crashes and invalid output, output bounds, handshake cancellation/timeouts,
forced cleanup, startup shutdown, and reconnect with fresh state.

`engine_settings` also checks explicit Connect/Disconnect, read-only full option
declarations, live state across navigation/selection, stale callback rejection and
unchanged saved profiles. `engine_exit` closes production main while connected and
reopens the saved registry without launching an engine. Screenshot capture through
`CHESS_REVIEW_ENGINE_SCREENSHOTS` includes connected Settings at 1100×720 and
1360×850. These checks retain the existing Linux/Unix platform limits.

Runtime adversarial regressions cover late discovery/readiness replies and partial
lines during clean disconnect, retained cleanup output/exit checks, actual binary
changes before launch and replacement during the post-readiness integrity check.
A 1,024-option stress case with long shared name prefixes verifies discovery at
the count bound and records caller event-loop heartbeat timings without asserting
a machine-specific latency threshold. Existing CTest deadlines are unchanged.

Settings regressions distinguish keyboard focus from the selected row, preserve
the active executable path across list changes, escape supplementary Unicode
formatting characters while preserving ordinary supplementary text, and constrain
long unbroken names to the viewport. The same screenshot option also writes
`hostile-1100.png` for long/invisible engine identity. No SQL fixture is introduced
outside persistence.

## Engine process worker lifecycle

`engine_process_worker` tests the private worker and coordinator, alongside the
existing public engine suites. Run all engine checks with
`ctest --preset dev -R '^engine_'`.

The suite checks allowed/rejected transitions and real registration ordering;
synchronous internal state hooks exercise repeated cancellation/close at executable
checking, startup, UCI/readiness waits, integrity rechecking, Ready and cleanup.
It also verifies failed launch, buffered exit-output drainage, first-error retention
through forced cleanup, admission held until worker destruction, fresh workers,
reconnect after cancellation/protocol failure/forced cleanup, and one-shot terminal
and service-close notifications. Boundary checks use explicit state synchronization;
production cleanup deadlines are unchanged. Unix missing-interpreter and signal
fixtures retain the platform limits described above. No SQL fixtures live here.

Lifecycle adversarial regressions also cancel directly from runtime/progress
notifications, verify immutable snapshots for later listeners, and require
pre-launch cancellation to start no child. A real buffered-exit fixture requires
process deadlines to stop when exit is observed. Codec cases reject case-insensitive
duplicates in ASCII, Greek and supplementary Unicode while preserving declaration
order. The 1,024-option stress case additionally records heartbeat gaps on the
engine's own event-loop thread; measurements are diagnostic, not portable timing
thresholds. Streaming and decoded declarations use the same private indexed
collector, with the existing option/byte bounds and persisted format unchanged.


## Position analysis

`engine_analysis` exercises the public search API against the deterministic child:
all four limits, history-preserving position commands, repeated searches and option
reset, capability downgrade, mixed-depth MultiPV, optional WDL, restricted root
moves, and validated score/PV/bestmove/ponder evidence. It covers terminal roots,
castling/promotion/en-passant lines, malformed numeric/evidence fields, oversized
PV/history/line bounds, stdout/stderr flooding, stale IDs, Busy rejection,
configuration cancellation, repeated stop, readiness probes overlapping bestmove,
fragmented replies, timeouts, crashes, disconnect/reconnect, and service closure.

A streaming regression lowers the injected output budget to exercise several
one-second windows beyond the former lifetime allowance. It checks coalesced
progress and a caller heartbeat without imposing a machine-specific latency SLA.
The production budget remains 1 MiB per window. Existing framing/drain bounds and
handshake totals remain tested by the previous suites.

`engine_process_worker` additionally closes synchronously from search phase
notifications before/after go and checks the extended transition graph.
`engine_exit` closes the real production QApplication during infinite analysis,
requires orderly exit, and reopens the saved registry without launching a child.
All fixtures and protocol scenarios remain outside persistence without introducing
SQL; database implementation and SQL fixtures stay inside persistence.


Position-analysis adversarial regressions additionally check distinct-candidate
coverage, malformed move tails that could resemble extensions, numeric evidence
at representable integer boundaries, and same-read protocol tails after explicit
teardown begins. A deliberately stalled caller must receive one latest progress
update rather than a backlog; another case checks final progress precedes terminal
completion with no later stale progress. The engine thread continues independently.
A 4,000-ply/20-candidate stress case records caller heartbeat gaps and progress
frames while processing 1,000 candidate updates. These are measured observations,
not portable performance guarantees.

## Search protocol parser

`ctest --preset dev -R '^search_protocol$'` runs the process-free Qt Core parser
suite on every supported platform, independently of the Unix-only fake UCI engine.
It covers command guards, auxiliary-field validation, duplicate and partial fields,
opaque strings, extension boundaries, numeric limits, legal continuations and root
restrictions, special moves, terminal bestmoves, ponder grammar, position immutability,
1,024-ply limits, and conservative evidence updates. The `engine_analysis` suite adds
fragmented whitespace/output, final evidence delivery, and invalid-record retirement.

Parser adversarial cases additionally generate 200 legal mixed-field records and
4,000 deterministic mutations, checking legal accepted moves, static diagnostics,
atomic failed-record handling, and unchanged request histories. Repeated auxiliary
records exercise a 12,000-ply root history and report elapsed parsing observations;
these measurements are not portable timing guarantees. Mixed auxiliary/PV sequences
also verify independent root replay for castling, en passant, and promotion.


## Analysis priority scheduling

`ctest --preset dev -R '^engine_scheduling$'` exercises the public scheduled API
against the deterministic fake engine. It covers all seven priorities, FIFO,
repeated preemption of each eligible class, full-budget restarts, distinct attempt
identities, queue overflow, direct/scheduled coexistence, and invalid submissions
that must not preempt. It checks cancellation before go, during analysis and while
preempted, bestmove races, job cancellation and session-long tombstones, retained
interruption evidence, failure propagation, explicit reconnect, and shutdown.

The suite exercises the actual 65,536-entry request and cancelled-job registry
bounds, so its CTest budget is 60 seconds. Production protocol deadlines are
unchanged. A deliberate caller stall checks latest-only provisional delivery;
terminal evidence and interruption snapshots remain lossless. Owning-thread
callbacks and no progress after terminal completion are also checked. Existing
engine lifecycle and production-main exit probes remain part of regression testing.

The crash-propagation case allows two seconds for an instrumented child's fatal
signal to be reported, rather than racing its usual 500 ms injected stop deadline.
The separate stop/synchronization timeout cases retain their shorter deadlines;
production search-response deadlines are unchanged.

Navigation and engine-analysis suites also have 60-second CTest guards for their
long-history, layout, and real-process cases under sanitizers. The navigation
fixture allows 30 seconds to construct 512 moves asynchronously and still checks
event-loop heartbeats. These are test completion guards, not responsiveness SLAs;
production behavior and engine timeout tests retain their existing limits.

Scheduling adversarial regressions stall the receiver across unrelated queue
changes, duplicate rejection and attempt replacement, requiring one latest update
with state-before-progress ordering. Terminal-root restarts followed by close or
disconnect must report Completed with evidence from the completing attempt.
Long-history admission/cancellation records timing and caller heartbeats without
a machine-specific latency assertion. Parser checks compare admission with full
serialization for initial, castling, en-passant, promotion and terminal roots,
invalid limits and restrictions, and promotion-bearing history exactly at and
above the 64 KiB command bound. Full adapter replay before dispatch remains required.

## Raw analysis provenance

`engine_fingerprint` is a process-free Qt Core suite covering deterministic golden
vectors, case/order normalization, typed effective defaults, unknown options,
button exclusion, binary/identity/configuration/version changes, metadata
exclusions, and unambiguous framing (including null/empty string equivalence).
Run it with `ctest --preset dev -R '^engine_fingerprint$'`.

`engine_analysis` checks provenance against the fake engine's expected commands,
including Hash defaults, Threads, MultiPV downgrade and WDL reset, restrictions,
rejection and cancellation before go. A 4,000-ply history receives multiple progress
frames sharing the same provenance object and retains its result after service
destruction. Exact context distinguishes that history from an equivalent FEN root.
`engine_scheduling` checks fresh attempt provenance, retained original evidence,
failure/reconnect, and configuration cancellation. Fingerprint equality in these
tests is not a cache-reuse assertion.

Raw-evidence adversarial regressions additionally stall the direct API receiver
across two accepted searches, with the second still active or already completed.
They require request phase events before progress, every final update before its
terminal result, no progress after terminal, and matching immutable provenance.
Duplicate and unrelated Busy rejections must leave active progress intact.
A process-free fixture measures 256 admissions with 1,007 legal advertised options;
full capture at dispatch still includes all 1,006 non-button effective values.
Timings are observations rather than portable pass/fail latency thresholds.


## Strict analysis cache

Run `ctest --preset dev -R '^(analysis_repository|engine_fingerprint|persistence)$'`.
The cache suite covers compatibility and resource policy, exact history and root
sets, node/depth dominance, budget overshoot, width downgrade, WDL, bounded and
mixed-depth scores, legal PVs, codec truncation and unsigned integer boundaries,
reopen durability, deterministic result selection, idempotency/conflicts,
retention, corrupt-row fallback, rollback after failed writes/commit/cleanup,
upgrade backup/rollback, asynchronous admission/drainage, and lock-contention
heartbeats. SQL fixtures stay under `src/persistence/tests`.

A deterministic fake UCI conversation produces and persists real Completed
provenance, then confirms its unknown executable remains ineligible for hits.
The optional `pinnedEngineRoundTrip` case runs only with `CHESS_REVIEW_STOCKFISH`
set to an explicitly selected executable. It validates, searches, stores and
retrieves a hit only for the pinned build; the normal suite skips this case.
It does not download or discover binaries automatically. Example from the repo:

```sh
CHESS_REVIEW_STOCKFISH=/absolute/path/to/stockfish \
  build/dev/src/persistence/tests/analysis_repository_tests pinnedEngineRoundTrip
```

The independently encoded empty-option v2 fingerprint golden vector complements
the unchanged v1 vectors. Resource-policy changes require new versioned fixtures
and a renewed source/runtime review of the pinned build.


Cache adversarial regressions exercise valid-looking score bit corruption, one-bit changes at every
byte position, truncations with deliberately recomputed checksums,
unprotected legacy-row removal, impossible pinned-option values, replay-witness
fallback/bounds, stable limit tags, damaged TEXT byte accounting and bounded lookup
rollback without a partial hit. Payload checksums detect corruption, not owner
forgery; tests deliberately recompute them to reach the semantic decoder.

For the optional reproducible workload observation:

```sh
CHESS_REVIEW_CACHE_BENCH=1 \
  build/dev/src/persistence/tests/analysis_repository_tests cacheWorkloadMeasurement
```

It measures lookup over 64 matching 2,000-ply histories and five inserts alongside
32 MiB of cache BLOBs. These are host-specific observations, not timing assertions.


### Live engine Analysis panel

`ctest --preset dev -R '^live_analysis$'` exercises the application controller and
panel against the deterministic fake UCI engine. Cases cover explicit start/stop,
history-preserving position changes, rapid navigation, stale logical IDs,
selection/annotation/flip stability, retained stopped/failed evidence, page exit,
reset, explicit reconnect, configuration/active-search shutdown, and unsolicited
infinite-search completion. Formatting cases cover White/Black perspective,
score bounds, integer extremes, missing scores, mates, SAN numbering, castling,
en passant, promotion, invalid PV rejection and line-specific depth.

The test uses `tests/support/desktop-screen.json` for a large virtual desktop;
otherwise Qt's default 800×800 offscreen screen constrains requested window sizes.
`layouts` renders live light/dark panels at 1100×720, 1360×850 and 1800×1000,
including enlarged fonts. To capture images, set `CHESS_REVIEW_SCREENSHOT_DIR` to
an existing absolute directory and use
`QT_QPA_PLATFORM=offscreen:configfile=$PWD/tests/support/desktop-screen.json`.
Repeat with `QT_SCALE_FACTOR=1`, `1.25`, `1.5` and `2`. Screenshots are inspection
artifacts, not pixel-perfect golden tests.

Set `CHESS_REVIEW_STOCKFISH` to an explicitly selected local executable to enable
`live_analysis_tests stockfishPanel`. This validates and connects the executable,
starts real infinite analysis, receives SAN/evaluation/depth, stops it, optionally
captures `stockfish-stopped.png`, and shuts down the application services. No
engine is downloaded or selected by production code. Offscreen checks do not
certify native screen readers, physical mixed-DPI monitors, or Windows behavior.

Terminal-position regressions cover checkmate and stalemate completion through a
depth-one search, retained score without a PV, and a subsequent ordinary infinite
search on the same connection. The optional Stockfish panel probe also exercises
these terminal positions with the real executable.


Live-panel regressions additionally exercise Stop before GUI delivery of queued
attempt phases, closed-board admission, unrelated null-ID and duplicate-submission rejections, redirected
navigation, literal engine-name tooltips, accessible status text, and mate
bounds that must not claim a certain winner. Retirement tests delete consumers
after closed(), including during reset notifications and nested event loops in
progress, terminal and visibility notifications. These tests use the same real
supervisor/fake-process boundary; no alternate production scheduler is added.


## Review evidence and configuration

`review_evidence_tests` links only the review/chess domain and Catch2. It covers
perspectives, WDL/fallback conversion, history-aware pairing, mate/bound/terminal
handling, incompatible evidence, candidate ranks, and negative deltas.
`review_configuration` exercises strict TOML validation, canonical identities,
synthetic engine adaptation, asynchronous startup and shutdown, and invalid-config
availability and diagnostic privacy. Adversarial regressions cover contradictory
candidate sets, mirrored-score roundoff, unreadable paths, bounded-depth TOML in a
subprocess, Unix special-file handling, descriptor leaks, compact notices, and
independent configuration identity golden vectors. Run the focused set with:

```sh
ctest --test-dir build/dev -L review --output-on-failure
```

The app now also builds with pinned toml++ 3.4.0. For offline configuration use
`-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=/absolute/path/to/tomlplusplus-3.4.0`
after verifying the source against the archive/checksum in
[third_party/tomlplusplus](../third_party/tomlplusplus/README.md).

## Durable review storage

The `review_repository` integration suite covers separate runs/perspectives,
configuration and model validation, evidence/pairing round trips, cache eviction,
local terminal proof, stale/conflicting checkpoints, replacement evidence,
transaction and COMMIT rollback, migration 005→006, startup recovery and its rollback,
explicit continuation, bounded corrupt-record reads, asynchronous admission and
shutdown drainage. A child process is killed after a committed checkpoint; reopening
must retain that evidence and mark its job Interrupted without starting an engine.
All database fixtures remain inside the persistence module and use temporary storage.

Durable review regressions also distinguish broken job/evidence relationships from
ordinary absence, reject impossible or disconnected history entries across page
boundaries, and verify that restart cannot hide unsupported settings or damaged
history beneath a new Interrupted entry. Adapter witness tests cover matching and
mismatching histories and an exactly matching context exceeding the engine history
limit. An optional local workload probe measures twenty checkpoints at ply 2,000:

```sh
CHESS_REVIEW_REVIEW_STORAGE_BENCHMARK=1 build/dev/src/persistence/tests/review_repository_tests reviewStorageWorkloadMeasurement
```

Run timing probes without competing builds. Their timings are observations, not
CTest latency assertions or portable performance guarantees.

## Quick-review journey

`ctest --preset dev -R '^quick_review$' --output-on-failure` exercises the real
application controller, database, scheduler and deterministic fake UCI process.
Cases cover full mainline/final-position scans, FEN and terminal roots, weak/bounded
results without WDL, committed coverage, cancellation/resume and independent runs,
restart with changed current quality, storage failure, conflicting revisions,
damaged evidence, engine failure, shared-service shutdown, admission backpressure
and background-navigation controls. The fake executable has unverified resources:
it must not qualify for compatible-cache reuse. Existing analysis-repository and
fingerprint suites cover cache dominance and the pinned-resource policy.

For local visual inspection, create a destination directory and run:

```sh
mkdir -p /tmp/quick-review-renders
QT_QPA_PLATFORM=offscreen:configfile=tests/support/desktop-screen.json \
  CHESS_REVIEW_QUICK_RENDER_DIR=/tmp/quick-review-renders \
  build/dev/tests/quick_review_tests renderWorkspace
```

This writes light, dark and enlarged-font workspace PNGs. Repeat with
`QT_SCALE_FACTOR=1.25`, `1.5` and `2` for scaling probes. The renderer case skips
when no directory is supplied. These offscreen checks do not establish physical
mixed-DPI or native screen-reader acceptance.

`pinnedEngineCacheReuse` is an optional end-to-end probe:

```sh
QT_QPA_PLATFORM=offscreen CHESS_REVIEW_STOCKFISH=/absolute/path/to/stockfish \
  build/dev/tests/quick_review_tests pinnedEngineCacheReuse
```

Select the reviewed pinned Stockfish executable for this test. It compares actual
compatible cache hits with searches submitted by a second run, then verifies
cache-independent evidence and reopening without an engine. Completed searches
with insufficient strength or non-exact evidence are expected to miss reuse.


Adversarial quick-review regressions cover destruction after closing from a
`changed()` notification (including a nested event loop), resume during delayed
engine disconnect, malformed fingerprint input, bounded PGN/engine labels,
invisible engine-character escaping, rapid run-selection coalescing, history page
reset and accidental setup changes from wheel input. The 81-position heartbeat
case reports elapsed time and cache-lookup count; an unverified fake engine must
issue no cache lookups. Timing output is observational, not a portable performance
assertion. Public cache APIs still validate and gate all compatible hits.

## Normal classifications and transparency

`review_classification_tests` exercises normal labels, inclusive thresholds and
adjacent floating-point values, error-band precedence, verified rank conflicts,
measured alternatives, malformed/weak/bounded/mate evidence, explicit perspectives,
and locally proven terminal outcomes. Run it directly or use CTest label
`classification`.

`review_configuration` verifies Excellent validation, identity separation, and
independently encoded legacy/current golden hashes. `review_repository` covers
atomic classification checkpoints, rollback, idempotent backfill, revision conflicts,
corrupt/future derived records, legacy settings, and migration 7 with a verified
backup. `quick_review` covers native move-row keyboard selection, inspectable
provisional rationale, rapid paging with unprocessed moves, and existing lifecycle,
shutdown and event-loop responsiveness scenarios. Its optional `renderWorkspace`
probe now includes the move list and evidence inspector.

Adversarial classification regressions cover native-WDL decimal boundaries,
baseline/draw/perspective independence, released model-1 read compatibility,
model-2 evidence replacement, root corruption on empty pages, stale errors after
deselection, Busy on a background page, and explicit UI retry after a failed read.
The 81-position heartbeat fixture also reports move-page request counts. Optional
Review render probes verify that move paging and Retry remain reachable after
scrolling with enlarged fonts, and save enlarged-detail and error-state images.

Increment 5 adds pure-domain summary tests to `review_classification_tests` and
schema-8 refinement/accuracy cases to `review_repository`. `quick_review` exercises
automatic stronger passes, MultiPV alternatives, changing summaries, chronological
opponent context, cancellation and missing-only refinement resume. Its long-game
heartbeat now covers the adaptive review, rather than just the initial scan.
The render probe additionally saves light/dark Summary-tab images. Optional real
engine cache checks count quick searches separately from fresh verification work.

Adversarial summary regressions reject orphaned/out-of-mainline stage mappings,
stages on legacy runs or incomplete quick scans, and invalid/incompatible stability
inputs (including NaN and unsupported models). A persistence test uses identical
owned raw evidence with different label thresholds to establish that accuracy does
not depend on labels. Qt cases cover summary refresh during Busy backoff and clear
last-successful labeling after a failed refresh. The render probe also saves
`review-summary-error.png` and checks that Refresh summary remains reachable.
