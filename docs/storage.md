# Storage

[Back to README](../README.md)

## Application storage paths

`AppPaths` resolves application-specific locations through `QStandardPaths` after
the application name is set to `chess-review`, without an organization prefix.
On Linux the layout is:

| Purpose | Qt location | Linux default | Contents |
| --- | --- | --- | --- |
| Durable data | `AppLocalDataLocation` | `~/.local/share/chess-review/` | `chess-review.sqlite3`, migration `backups/` |
| Configuration | `AppConfigLocation` | `~/.config/chess-review/` | `ui-state.ini`; optional `config.toml` (see [configuration](review-evidence.md)) |
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
uses TOML; `QSettings` is reserved for UI-local state.

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

## Database and backups

The database holds saved games, engine profiles, and review evidence. Keep it on
a local filesystem that supports SQLite WAL and locking. The app reports database
errors rather than silently switching storage or resetting your library.

Before upgrading an older database, the app creates and verifies a backup in the
adjacent `backups/` directory. Backups are retained; restoring one is a deliberate
offline operation, not an automatic recovery step. See
[Storage internals](storage-internals.md) for migrations, repository APIs, and
file-safety rules.
