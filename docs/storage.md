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
schema migrations, backups, and the dedicated storage thread. Its lifecycle/repository facade is `src/persistence/Database.h`, with game value
types in `src/persistence/GameRepository.h`. Callers receive initialization
progress, a ready schema version, structured failures, typed repository outcomes,
and shutdown completion. SQL types and generic
query execution are not part of the public interface. Game repository operations use that same worker-owned connection; SQL remains
private to persistence, including all SQL fixtures in its tests.

`Database` is a one-shot service. Call `initialize(absolutePath)` and `close()` on
its owning application thread. Before destroying it, await `closed()`; do not
quit the application event loop first. Closing during initialization lets the
current operation finish, suppresses readiness/failure delivery, and queues
connection cleanup. The startup coordinator uses this asynchronous shutdown path.
Database failures leave storage unavailable and display the diagnostic in the
window; Exit and window-manager close both finish storage shutdown before quitting.

Application databases use SQLite’s default UTF-8 text encoding, verified before
migrations or product writes. Foreign UTF-16 database files are rejected without
conversion or replacement; PGN text import remains the supported interchange path.
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
`schema_migrations` ledger (version, name, UTC application timestamp). Migration
002 creates `games` and its import-order index. Migration 003 upgrades healthy
games to snapshot/identity v2 while preserving IDs, original PGN, source metadata,
and import times. Damaged records are copied unchanged and remain individually
unreadable. Migration 004 adds engine profiles. Review and puzzle tables remain
future work.

To add a migration:

1. Add a numbered C++ migration definition containing its version, stable name,
   and ordered SQL statements; register it in the private registry and CMake.
2. Use the next contiguous integer version. Never edit a released migration.
3. Provide one statement per entry. Do not include transaction control, ledger
   updates, or operations that cannot run in a SQLite transaction. The runner owns
   transactions and ledger writes; no SQL script parsing is performed. A compiled
   optional row transform can run within that same transaction when serialized
   documents need migration. It must use bounded reads and propagate failures;
   returned errors and standard exceptions roll back both DDL and data changes.
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


## Immutable imported games

`Database::insertGame(request, document, source)`, `loadGame(request, id)`, and
`listGames(request, offset, limit)` are asynchronous repository calls on the
Database object's application thread. Use a distinct `QUuid::createUuid()` request
ID and correlate the corresponding `gameInserted`, `gameLoaded`, or `gamesListed`
signal. No operation returns a SQL handle or accepts executable query text.
A supplied `shared_ptr<const PgnDocument>` must remain immutable through all
aliases until completion. Loaded documents have immutable shared ownership;
make an independent copy for in-memory exploration.

The repository accepts at most 16 outstanding requests; additional requests
receive `Busy`. Before readiness it returns `Unavailable`; after close begins it
returns `Closing`. Accepted requests finish and deliver results before `closed()`.
Ordinary absence returns `Missing`. Snapshot-validation errors, unsupported
versions, and storage failures are separate typed results with safe messages.
Database startup diagnostics retain their existing, separate error identifiers.

Listing defaults to 50 rows and accepts limits 1–200 and a nonnegative offset.
It sorts by UTC import timestamp descending, then ID descending, and loads only
summary columns. Display names, played date, and time control are limited to 256
UTF-16 code units without splitting surrogate pairs; `metadataTruncated` indicates
that full values require `loadGame`. Small copied strings release the original
large buffers. A damaged metadata row returns its ID and `metadataError` with
blank display fields, allowing healthy rows on the same page to remain available.
Consumers must check `metadataError` and render all imported text as plain text.
Pagination is for the current library view, not a frozen query
snapshot across concurrent imports. Full games are loaded explicitly by ID.

The current schema stores original per-game PGN, a JSON snapshot with
`formatVersion: 2` and `textEncoding: "utf8-base64"`, normalized library fields,
source type/URI, UTC import time, and version-2 duplicate identity. Tag names,
tag values, and node comments are Base64-encoded exact UTF-8 bytes in snapshot
JSON; ASCII moves and FEN remain ordinary JSON strings. This preserves field-level
U+FEFF as content. Version 1 remains an explicit legacy decoder; migration 003
validates each old game and writes v2 without changing owning game IDs. Each insert uses one explicit transaction;
the duplicate check and write share that transaction. A failed write or COMMIT
rolls back the current game. If rollback fails, further product operations on
that storage session fail closed. No automatic retry or alternate database is used.

Snapshot JSON retains ordered tags, resolved result, root FEN/provenance,
ordered variation children, decimal-string node IDs, comments, NAGs, and original
played moves independently of the displayed tree continuation. Wire result values
are explicitly 0=unknown, 1=white win, 2=black win, 3=draw. Insertion verifies that
the supplied document is the unchanged parsed import. Loading reconstructs from
the retained PGN through the validated parser/chess APIs and compares the entire
semantic snapshot before returning it. The source file or URL is never reread.
Reader ordinals/ranges in loaded documents are relative to their retained per-game
PGN; the import report preserves the original multi-game ordinals/ranges.

Readers bound projected database values before decoding: original PGN at 8 MiB,
snapshot at 64 MiB, and identity at 1 MiB. Parser defaults additionally bound tree,
tag, annotation, and diagnostic content. Invalid rows return a load error without
removal; other rows can still be listed/loaded. Stored identity, hash, and indexed
game metadata are checked against the validated document. Metadata is projected
as bounded raw bytes, checked for UTF-8/NUL/type/length errors, then decoded; text
truncation and replacement decoding must never erase evidence of damage. The retained PGN is not an automatic repair path for a mismatching snapshot.

Future format changes must retain explicit supported-version decoders or
introduce a tested migration before writing a new version. Parser normalization
changes must preserve reconstruction compatibility or migrate existing snapshots.
Column and document versions must agree. Unsupported
versions are rejected, never silently rewritten or guessed. These checks detect
inconsistent content; they do not authenticate a database against its owner.

### Duplicate identity

Identity v2 is compact ASCII JSON for the ordered array
`[2, canonicalSixFieldRootFen, originalUciMoves, resultCode, base64(White),
base64(Black), base64(Date), base64(Event), base64(Site), base64(Round),
base64(TimeControl)]`. Tag values use their exact decoded UTF-8 bytes; missing and
`?` both become empty strings before Base64 encoding. Other casing/whitespace
and field-level U+FEFF are preserved. SHA-256 uses Qt's `QCryptographicHash`;
matching hashes also require exact identity content and a valid existing snapshot.
An inconsistent identity or hash collision is an error rather than a duplicate
claim. Version 1's original encoding is retained for validation during upgrade;
frozen digests cover both versions. Encoding changes require an explicit version
transition and migration.

Metadata writes bind UTF-8 bytes through `CAST(? AS TEXT)` and reads validate
byte-bounded BLOB projections before decoding with field-level BOM preservation.
This avoids SQLite UTF-16 binding's initial-BOM removal; keep this explicit
persistence boundary when adding text columns.

Formatting, annotations, variations, unknown tags, and source metadata do not
participate. Root provenance is preserved in the snapshot but is not a duplicate
key: identical canonical starting FENs match even when one was explicitly supplied.
All six FEN fields participate, including counters. Incomplete identifying tags
use the same available-data rule; matching does not prove real-world game identity.

`Inserted(id)` means a committed new game. `Duplicate(existingId)` means the
existing snapshot was retained unchanged, including its annotations, source,
and original import time. There is no merge, replacement, or explicit-copy mode.


Repeated duplicate checks retain at most 256 already-validated IDs, never trees
or source payloads. Identity content is still checked on every match. Cache reuse
occurs inside the write transaction and requires unchanged SQLite `data_version`
and connection `total_changes()` counters, invalidating on other-connection
commits and same-connection DML. Startup migrations precede this cache's lifetime.
An uncached game is fully validated before reporting a duplicate; explicit loads
always validate independently. Cache capacity is a work optimization, not a bound
on total import duration or protection against external filesystem modification.

## PGN text import service

`app::GameImportService` receives an explicitly injected `Database&` and accepts
owned `std::string` PGN bytes plus optional source metadata. It performs no file,
network, widget, or engine work. `StartupCoordinator` owns the production instance
and exposes `gameImports()` and `gameRepository()` for presentation code.

One import may run per service; overlaps receive a separate `rejected(Busy)`
signal. Input beyond the default 64 MiB parser limit is rejected before scheduling.
The service's private parser pool has one worker and publishes one parsed game at
a time, awaiting its repository outcome before reading the next. It retains the
bounded input and report, not a queue of parsed trees. Source type is limited to
128 UTF-16 code units and URI to 65,536, with valid UTF-16 and no embedded NUL.
These fields are opaque provenance, not instructions to fetch anything.

`savingGame(request, ordinal)` identifies submission for persistence; it does not promise
success. `progress(request, item)` reports completed per-game outcomes and retains source
ordinal/range, parser warnings, and suppressed-warning count. Progress consumers
must correlate the request ID, just as they do for finished/rejected outcomes.
Reports retain at most 1,000 warnings across the entire import, with additional warnings reflected
in each game's suppression count. This bounds retained capacity as well as logical
warning count; per-game parser failures and all per-game outcomes remain present.
`finished(report)` returns the complete report with `Completed`, `Cancelled`, or `Failed` status.
Recoverable parser failures are reported and skipped. An unreadable tail or
storage failure stops the import. Earlier committed games remain available.
There is no automatic game analysis.

`cancel()` interrupts parsing and prevents the next game transaction. An already
submitted game finishes; its committed/duplicate/error outcome remains in the
report. `close()` prevents new imports, cancels production, and emits `closed()`
only after the parser worker has joined and accepted persistence has completed.
Keep the service alive until that signal and keep Database alive/available until
then. Startup shutdown follows this order before the existing storage/preferences/
logging drain. All waits are asynchronous; normal user callbacks perform no SQL.

Reports and source metadata remain local, in memory or the game record. The
service does not log PGN, players, comments, personal URLs, SQL, or native errors.

### Board UI preferences

The existing asynchronous `UiStateStore` additionally stores input mode, coordinate
visibility, move animation, manual board orientation, and four validated
`#RRGGBB` annotation colors under `board/v1` in `ui-state.ini`. Missing keys retain defaults. Invalid board values
reset only the board preference group and report an invalid-state notice. Existing
shell validation, atomic publication, zero-wait lock contention handling, and
unknown-key preservation still apply. The analysis board share is stored as a
validated integer percentage (1–99, default 60) under `analysis/v1/split`. Missing
orientation defaults to White at bottom. Moves and annotations from the scratch
Analyze session are not saved here or in SQLite.


### Desktop import and library controllers

`StartupCoordinator::localImports()` exposes the Widgets-free `ImportController`.
It owns file/text acquisition on one private worker and coordinates the injected
`GameImportService`; do not submit directly to that service while using the
controller. File inspection and reads stay in `import/LocalPgnInput`, outside the
widgets and parser. The controller keeps the latest batch report in memory only.

`GameImportService::importText` additionally accepts `ImportBudget`: games must
be 1–10,000, warnings 0–1,000. Defaults preserve the original API behavior.
`ImportReport::attemptedGames` excludes the extra game-count boundary diagnostic;
`gameLimitReached` distinguishes that boundary from ordinary PGN tail failures.
The controller passes each file the remaining shared game/warning allowances.

`StartupCoordinator::library()` exposes `GameLibraryController`. It requests
51 summaries for a 50-row page through the existing typed repository API, with
one request in flight and revision-based rejection of stale results. Committed
imports schedule coalesced refreshes; completion resets to the newest page.
No count query, SQL handle, new schema, or widget dependency was added.

Production shutdown disables library reads, closes the import controller, joins
its acquisition worker asynchronously, closes/drains the import service, then
closes storage. The controller and its injected service must remain alive until
`ImportController::closed()`. Window/page dependencies must outlive their widgets.

Import controller admission stays closed through terminal `changed`/`finished`
notification delivery so observers see one stable completed report. Queue any
new submission requested by a `finished()` slot to the next event-loop turn;
ordinary UI actions become available after terminal delivery. Closing from a
terminal observer does not emit `closed()` until all those observers have
returned and acquisition/parsing have drained. `ImportBatchStatus` and the
per-file `CompletedWithErrors` state distinguish completion, cancellation,
terminal failure and game-budget exhaustion without inferring success from a
zero inserted count. Library readiness changes invalidate displayed rows but
retain the accepted request until its completion, preserving one-read backpressure.

### Saved-game workspace recovery

The `workspace/v1` UI-state group holds only a positive saved game ID and unsigned
node ID. It contains no PGN, player names, moves, or annotations. After storage is
ready, the library loads and validates that game through its existing worker API;
the analysis worker copies the tree and builds its presentation. The remembered
node is selected only if it exists in the recovered tree; otherwise the root is
shown. The library never overwrites a saved snapshot during exploration. Missing,
unsupported, or damaged records produce a visible error and remain untouched.
These pointers are saved on orderly exit and cleared after New Analysis.

There is at most one open request in flight, independent of the paged list request.
Readiness generations reject obsolete responses without releasing admission until
the accepted response arrives. Library refreshes do not erase game-opening errors.
Service shutdown disables admission and invalidates loading before draining storage.

## Engine profiles

Migration 004 adds engine profiles without modifying earlier migrations. Existing
backup and transactional migration rules apply. `Database::registerEngine` and
`Database::listEngines` are typed asynchronous operations on the existing storage
worker, sharing its 16-request admission bound. Listing defaults to 50 records;
limits must be 1–200, with a nonnegative offset.

Registration returns a saved profile plus a duplicate flag, or a typed engine
error. One transaction covers identity checks, first-engine default assignment,
and insertion. Duplicate registration also checks that a nonempty registry has
exactly one default. Before commit, insertion reads back and validates the saved
profile against the requested values; it does not confirm missing or altered rows.
Executable SHA-256 and resolved paths are unique; a partial unique
index permits at most one default. Failed insertion/commit rolls back, and failed
rollback disables further product operations for that storage session. Existing
records are never silently replaced or relocated.

Capabilities use a bounded version-1 JSON document containing typed options.
Capability flags are derived again on read, and registration rejects values whose
flags disagree with their options. Numeric option-type identifiers are stable
within that format version. Option overrides are explicitly empty
and detected version is absent in this increment. Invalid types, encodings,
versions, oversized metadata, and invalid option definitions fail closed without
repairing records. Reads bound bytes before Qt materializes values. A damaged page
fails explicitly; healthy pages remain accessible through the repository API.

SQL, migrations, SQLite handles, and SQL fixture tests remain exclusively in
`src/persistence`. Engine/application/UI code exchanges values and request IDs.
Shutdown stops engine registration and import producers before closing storage;
accepted writes finish and deliver their outcomes before storage closure.


## Analysis cache

Migration 005 adds independent analysis records to the existing local database.
No game/profile deletion cascades into cache data, and removing corrupt cache rows
never removes library records. Existing migration backups and rollback rules apply.

`Database::storeAnalysis(requestId, result)` accepts completed attempts and emits
`analysisStored` with `AnalysisStored{id, duplicate}` or `CacheError`. Repeating the
same canonical evidence for a session/attempt returns the existing ID without
refreshing its age; conflicting evidence returns `Conflict`. An acknowledgment
means the transaction committed. It does not promise a reusable hit.

`Database::findAnalysis(requestId, query)` emits `analysisFound` with a
`CacheLookup` or `CacheError`. `CacheLookup::hit` is optional; `discardedRecords`
counts individually invalid/unsupported matching rows removed by this lookup.
A hit retains the engine's original session/request/provenance, independently of
the storage operation's correlation ID. The strongest eligible measured result
wins; newest insertion breaks ties. Unverified configurations return a normal miss.

Both calls run on the owning application thread and queue CPU/database work onto
the existing storage worker. They share the 16-request repository admission bound;
errors distinguish invalid input, conflict, busy, closing, unavailable and storage
failure. Accepted operations drain and deliver outcomes before `closed()`. There
is no automatic scheduler interception or analysis UI integration yet.

Payload format 2 uses pinned Qt_6_0 big-endian QDataStream primitives with bounded
length/count framing, explicit optional presence and fixed numeric tags. It retains
raw scores/bounds, WDL, PVs, statistics (including full unsigned 64-bit values),
limits, configuration and attempt provenance. The position identity format stores
its version, context version, variant, root provenance, exact canonical root FEN
and complete legal move history. Board identity and halfmove clock are reconstructed
and compared during validation; no history-equivalence shortcuts are used.

Readers bound database projections before allocation, validate UTF-8 and primitive
ranges, replay legal history/PVs, recompute fingerprints and position SHA-256,
compare indexed identity with decoded evidence, and require canonical re-encoding.
Format 2 appends a SHA-256 checksum over the complete preceding payload, verified
before parsing. This detects accidental changes to scores/statistics as well as
structural corruption. It does not authenticate a file against its owner. Earlier
unprotected format-1 cache rows are treated as unsupported and discarded on lookup;
game records, database schema version and engine fingerprint framing do not change.
Malformed or unsupported matching records are deleted transactionally and lookup
continues. A bounded numeric diagnostic reports committed cleanup counts. Failure
to delete or commit returns a storage error and rolls back cleanup; a failed
rollback disables the session. A corrupt index key may make a row unreachable by
that lookup; it never authorizes a match and remains subject to normal eviction.
These checks detect inconsistent data, not malicious rewriting by the database owner.

Retention is oldest-insertion-first, with a 256 MiB logical budget for encoded
payload plus position identity, 16 MiB per record including payload/checksum and position identity, and 100,000 records. Retention
and insertion share one transaction. SQLite pages, indexes and WAL overhead are
outside the logical budget; eviction does not shrink the physical database file,
and no automatic VACUUM runs. No settings UI or total-file-size guarantee is added.


Lookup reuses an independently validated chess position only after exact full
context comparison; this avoids replaying the same long history for every record.
Mismatch falls back to complete replay. Every record still validates its checksum,
configuration, coverage and legal PVs. At most 100,000 candidates are examined;
exceeding the supported retention bound returns an explicit error and rolls back
cleanup rather than returning a potentially weaker partial hit. Retention scans
use BLOB length metadata, with exact byte-counting fallbacks for damaged TEXT data.
