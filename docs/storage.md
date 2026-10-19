# Storage

[Back to README](../README.md)

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
current/default layout remains usable independently of the game database. Notices
and safe diagnostics distinguish invalid state, read/write failure, and lock
contention. If another settings writer holds the lock, saving is skipped promptly
instead of delaying shutdown. Writes preserve unknown keys and publish atomically;
unsafe settings/lock targets are rejected without replacing them.
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
opening it. Before Qt reads lock metadata, the app rejects symbolic/nonregular
locks, Unix multiply linked locks, metadata larger than 16 KiB, and pre-existing
`.lock.rmlock` cleanup files. Before opening SQLite, it also rejects unsafe
WAL, SHM, and rollback-journal sidecars, so recovery cannot follow those aliases
or open special files. Ordinary database symbolic aliases remain supported.
Stale process locks are handled by Qt; do not delete a live process's lock.
These checks assume private user-controlled directories; they do not protect
against concurrent malicious filesystem replacement by the same user.
No database or backup is automatically replaced or restored after failure.
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
