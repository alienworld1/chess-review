# chess-review

Native C++20 / Qt Widgets application foundation with a main window and five
empty destinations: Home, Games, Puzzles, Analyze, and Settings. The sidebar and
View menu share navigation actions. Chess screens and product features will be
added in later tasks.

Startup restores window preferences, then opens the local SQLite database
asynchronously. Navigation remains available during startup and storage failure.
File → Quit and window-manager close keep a closing indication visible until
storage, UI preferences, and logging finish shutting down. Help provides
Diagnostics and About; View → Reset Layout restores fitted default geometry and
Home. Native Qt styling is used for this foundation; branded themes are deferred.

## Requirements

- C++20 compiler (GCC or Clang on Linux)
- Qt **6.12+** development SDK with Widgets, SQL, the QSQLITE driver, and Qt Test
  (Qt Test is needed only when `BUILD_TESTING=ON`, the default)
- CMake **3.24+**, Ninja, and GNU Make
- Developer tools: clang-format 15+, clang-tidy, and clangd for editor integration

## Everyday commands

From the repository root:

```sh
make run
```

This configures, builds with Ninja, and opens the window. Close the window to
return to the terminal. The Makefile is just a set of command shortcuts; CMake
and Ninja handle the actual build.

| Command | Action |
| --- | --- |
| `make` | Show command help |
| `make build` | Configure and build a debug binary |
| `make run` | Build and launch the app |
| `make run PRESET=release` | Build and launch an optimized binary |
| `make clean` | Clean the selected preset's compiled outputs |
| `make format` | Format C++ source |
| `make format-check` | Verify C++ formatting |
| `make lint` | Build and run the curated clang-tidy checks |
| `make test` | Build and run all five Qt Test suites through CTest |
| `make test PRESET=sanitized` | Build and test with ASan/UBSan and leak detection (Linux) |

Builds are incremental. Debug and release outputs live in `build/dev` and
`build/release`. `build/dev/compile_commands.json` is available for C++ editors.

## Finding Qt

The presets look for Qt 6.12.0 in `.tools/Qt/6.12.0/gcc_64`, then CMake's normal
system locations. This workspace has its SDK in that ignored project directory.
To install the same minimal SDK on another Linux x86-64 machine (Python 3 and
venv support required):

```sh
python3 -m venv .tools/aqt
.tools/aqt/bin/pip install aqtinstall==3.3.0
.tools/aqt/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O .tools/Qt --archives qtbase icu
```

The matching ICU package is a runtime dependency of Qt's prebuilt SDK.
Linux also needs Qt's usual platform libraries. On Ubuntu/Mint, install
`libxcb-cursor0` if the X11 platform plugin reports it missing. This workspace
keeps a copy of that library in the local SDK because system installation needs
an administrator password.
For another SDK location, pass its prefix when configuring:

```sh
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/gcc_64
cmake --build --preset dev --target run
```

For repeated use of a different SDK with the Make shortcuts, set the `Qt6_DIR`
cache entry to `/path/to/Qt/6.12.0/gcc_64/lib/cmake/Qt6`, or keep your own presets
in ignored `CMakeUserPresets.json`. `Qt6_DIR` stays in the local build cache.

## Formatting and static analysis

`.clang-format` defines the shared C++ style. VS Code workspace settings enable
format-on-save through the recommended clangd extension. Other editors can use
the same file with their clang-format integration. `make format` formats all
source files, and `make format-check` reports drift without editing files.

`.clang-tidy` enables a small set of analyzer, lifetime, modernization, and copy
checks. `make lint` runs them against the actual build compilation database;
clangd also shows these diagnostics in the editor.

## Direct CMake commands

Without the Make shortcuts, the equivalent commands are:

```sh
cmake --preset dev
cmake --build --preset dev
cmake --build --preset dev --target run
```

## Diagnostics and troubleshooting

Failures appear inline with an explanation and suggested next steps. **Show
technical details** reveals the attempted database path, native error, and any
verified backup path. These details can contain private information and appear in
a scrollable plain-text pane, so long messages leave the main controls accessible. Fix external
conditions and relaunch to retry; the app never resets or restores a library
on your behalf.

Use **Help → Diagnostics** to inspect logging health and recent safe events.
The dialog shows a stable snapshot: use **Refresh diagnostics** to update it; its
Copy button copies exactly the displayed report without resetting your selection.
**Copy diagnostics** always copies a redacted report, even when private details
are expanded. It includes app/Qt versions, session/error identifiers, safe
metadata, and up to 200 recent events. Nothing is uploaded automatically.
**Open log folder** is available while the local logging sink is active; opening
the folder exposes the local files to your chosen desktop file manager.
**Show private log location** reveals the attempted path locally, including when
logging failed; that path is excluded from the report and its Copy action.

On Linux, logs default to `~/.cache/chess-review/logs/`. Qt honors the cache
location override described below. Files `events-0.jsonl` (current) through
`events-4.jsonl` hold newline-delimited JSON, capped at 2 MiB each (10 MiB total).
Logs are disposable, rotate by size, and have no age-based expiry. A cooperating
second process uses memory-only diagnostics while the log directory is locked.
Only known log filenames rotate; unrelated cache and library files are preserved.

Normal records cover session/storage lifecycle, schema version, backup/migration
outcomes, elapsed time, and stable error codes. For extra safe intermediate events
on one launch:

```sh
./build/dev/chess-review --debug-logging
```

Debug mode keeps the same privacy rules and size limits. Logs and copied reports
exclude filesystem paths, native error strings, SQL, game text, personal URLs,
and raw Qt/engine output. Framework warnings retain only normalized category,
severity, and occurrence count. The scoped Qt handler does not forward raw text
to the console while installed. Qt messages emitted before logging initializes
retain Qt's normal behavior.

If logs cannot be created or writing fails, one inline notice identifies the
problem and explains that recent diagnostics remain in memory; ordinary app use
continues. Safe issue codes distinguish an unavailable location, unsafe target,
lock contention, open/protection failure, write failure, and rotation failure.
Check free space, access to the cache directory, and other running instances. A
bounded queue drops excess disk records rather than blocking the app and reports
a persistent loss count. Recent memory history continues updating even while the
disk queue is full; disk records may have sequence gaps. Orderly shutdown
drains accepted events; forced termination can lose queued records. A subsequent
launch discards an incomplete final log line before appending. There is no crash
capture, crash upload, or guaranteed fatal-event flush.

## Application storage paths

`AppPaths` resolves application-specific locations through `QStandardPaths` after
the application name is set to `chess-review`, without an organization prefix.
On Linux the layout is:

| Purpose | Qt location | Linux default | Contents |
| --- | --- | --- | --- |
| Durable data | `AppLocalDataLocation` | `~/.local/share/chess-review/` | `chess-review.sqlite3`, migration `backups/` |
| Configuration | `AppConfigLocation` | `~/.config/chess-review/` | `ui-state.ini`; reserved `config.toml` |
| Disposable cache | `CacheLocation` | `~/.cache/chess-review/` | Rotating `logs/`; reserved `transient/` |

Qt honors `XDG_DATA_HOME`, `XDG_CONFIG_HOME`, and `XDG_CACHE_HOME`, using its
standard defaults when unset, empty, or relative. Other platforms use Qt's native mappings;
data uses the local rather than roaming location on Windows.

Resolving paths creates nothing. Each subsystem creates its directory when it
first writes there. Database initialization runs on the storage thread; logging
creates its directory on a separate writer thread. A private, single-worker
UI-state store reads/writes `ui-state.ini` through `QSettings::IniFormat`, without
GUI-thread file I/O. It saves window geometry, maximized state, and the last
selected destination during orderly shutdown. It does not restore minimization.
The initial window targets 1360×850 logical pixels, fitted to the available
screen; valid saved geometry is restored and re-fitted when screen geometry changes.
View → Reset Layout unmaximizes, restores fitted defaults, and selects Home.

Missing preferences use defaults without creating a file until shutdown. Invalid
or inaccessible preferences show one nonmodal notice and a safe diagnostic;
current/default layout remains usable independently of the game database.
A quit request before preference loading completes preserves the existing file.
There is no fallback settings path or organization prefix. Product configuration
parsing and general filesystem cache management are not implemented yet.
Product configuration will use TOML; `QSettings` is reserved for UI-local state.
There are no placeholder configuration files or unused directories. SQL migrations remain
compiled into the application, without an on-disk `migrations/` directory.

Saved engine analysis belongs in the durable SQLite database, so clearing the
filesystem cache must not remove saved analysis. Database failures are reported
explicitly; the app never silently switches to temporary or alternative storage.
Existing directory permission behavior is preserved, along with the private file
protections described below. No existing data is relocated.

Use private, user-controlled local storage. Path resolution does not verify access
rights or filesystem capabilities. Keep cache roots separate from data and
configuration roots: custom XDG values or symbolic links can make them overlap.
Any future cache cleanup must remove only files owned by that cache subsystem,
never recursively delete a resolved root.

Development launches share the installed application's storage locations. To
isolate a manual run on Linux, supply temporary XDG roots explicitly:

```sh
storage_root=$(mktemp -d)
XDG_DATA_HOME="$storage_root/data" \
XDG_CONFIG_HOME="$storage_root/config" \
XDG_CACHE_HOME="$storage_root/cache" make run
# After closing the app, inspect the files under "$storage_root" as needed.
```

Tests use temporary locations and do not touch the normal user database.

## Local database

The default location is `QStandardPaths::AppLocalDataLocation/chess-review.sqlite3`.
On Linux this is normally `$XDG_DATA_HOME/chess-review/chess-review.sqlite3`, or
`~/.local/share/chess-review/chess-review.sqlite3` when XDG_DATA_HOME is unset.
The data directory is created on the storage thread. Keep this database on a local
filesystem supporting SQLite WAL and locking. Packaging must include Qt's QSQLITE
driver plugin.

The `chess_review_persistence` target owns all SQL, connection configuration,
schema migrations, backups, and the dedicated storage thread. Its public header
is `src/persistence/Database.h`; callers receive progress, a ready schema version,
structured failure diagnostics, and shutdown completion. SQL types and generic
query execution are not part of the public interface. Future repositories belong
inside persistence and will use the same worker-owned write connection.

`Database` is a one-shot service. Call `initialize(absolutePath)` and `close()` on
its owning application thread. Before destroying it, await `closed()`; do not
quit the application event loop first. Closing during initialization lets the
current operation finish, suppresses readiness/failure delivery, and queues
connection cleanup. The startup coordinator uses this asynchronous shutdown path.
Database failures leave storage unavailable and display the diagnostic in the
window; Exit and window-manager close both finish storage shutdown before quitting.

Connections enable and verify foreign keys, a 5000 ms busy timeout, full durability
synchronization, and disabled trusted-schema execution. WAL is enabled only after
schema-history validation, so rejected databases are not converted to WAL.
New database and backup files are exclusively created with owner-only read/write
permissions. Recognized existing databases and their WAL/SHM sidecars are also
restricted to owner-only access before further writes. Special files and
dangling symbolic links are rejected. Symbolic aliases resolve to the same lock;
Unix hard-linked databases are rejected because aliases make WAL and locking unsafe.
A `.lock` file beside the database prevents concurrent application instances from
opening it. Stale process locks are handled by Qt; do not delete a live process's
lock. No database or backup is automatically replaced or restored after failure.
Normal application quit requests also go through storage shutdown; `closed()` is
emitted only after a nonblocking native-thread join has completed. Code should use
the coordinator's shutdown path rather than bypassing it with immediate process
exit or application destruction.

## Adding migrations

Migration definitions live under `src/persistence/migrations/`; the ordered
registry and runner are private to the module. Version 1 creates only the
`schema_migrations` ledger (version, name, UTC application timestamp). There are
no game, engine, review, or other product tables yet.

To add a migration:

1. Add a numbered C++ migration definition containing its version, stable name,
   and ordered SQL statements; register it in the private registry and CMake.
2. Use the next contiguous integer version. Never edit a released migration.
3. Provide one statement per entry. Do not include transaction control, ledger
   updates, or operations that cannot run in a SQLite transaction. The runner owns
   transactions and ledger writes; no SQL script parsing is performed.
4. Add representative previous-version fixtures and preservation/failure tests
   under `src/persistence/tests/`. Keep fixture SQL inside that module too.

Before upgrading a recognized older database, the runner creates a uniquely named
SQLite snapshot in the adjacent `backups/` directory, opens it read-only, and runs
an integrity check. This includes committed WAL contents. Backup failure prevents
the upgrade. Fresh databases and already-current databases do not create backups.
Backups are retained; there is no automatic retention or restore policy yet.

Each migration and its ledger entry commit together. A failed migration rolls
back, initialization stops, and the diagnostic identifies the failed version and
available backup path. Earlier migrations committed during that run remain
committed. Fix the cause and reopen with a compatible application, or preserve
the database and its sidecars before performing a deliberate offline restore.
Newer schemas, malformed history, and nonempty databases without recognized
metadata are rejected rather than reset.
Ledger columns, keys, and unexpected ledger triggers/indexes/foreign keys are
validated too. Versions are read without integer narrowing, and metadata text and
row counts are bounded before delivery to Qt. The ledger remains infrastructure
metadata: future migrations must not repurpose or add triggers/indexes to it.

## Tests

See [the testing guide](docs/testing.md) for presets, labels, individual case
selection, Catch2 setup, sanitizers, and Linux CI.

```sh
make test
# Or, after configuring and building:
ctest --test-dir build/dev --output-on-failure
```

Path tests cover Linux XDG overrides and defaults, spaces/Unicode, and side-effect-free
resolution. Startup tests also verify lazy creation and explicit failure for an
unusable resolved data directory.

Persistence tests use temporary databases and internal fixture migrations to cover
initialization, upgrades, rollback, WAL-aware backups, invalid history, lock
contention, busy timeouts, and asynchronous lifecycle cleanup. The busy-timeout
case deliberately takes about five seconds and checks event-loop responsiveness.
Qt widget tests smoke-run the actual startup coordinator/main window through readiness,
failure presentation, and close during initialization. CTest runs these with Qt's
offscreen platform plugin. Tests do not touch the normal user database.

Logging tests exercise bounded queues and memory history, thread/process
contention, privacy, rotation, unsafe targets, interrupted records, injected sink
failure, concurrent producers, and Qt fatal termination. Widget tests also cover
redacted clipboard contents, technical-detail disclosure, the diagnostics dialog,
and storage-before-logging shutdown. Shell tests cover shared navigation actions,
keyboard activation, saved state, malformed/unavailable settings, reset behavior,
monitor-fitting calculations, and asynchronous shutdown with the window visible.

To build without test targets, configure with `-DBUILD_TESTING=OFF`.
