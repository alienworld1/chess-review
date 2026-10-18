# SQLite foundation adversarial review

Reviewed 2026-10-19 against AGENTS.md, the locked persistence/threading/migration
architecture, the PRD's local data requirements, and the desktop startup rules.
This review covers initialization, migrations, backup creation, and lifecycle;
product repositories and serialization do not exist yet.

## Findings and corrections

| Finding | Correction and regression evidence |
| --- | --- |
| A 64-bit ledger version could overflow to a supported integer without a conversion error. | Read as 64-bit and validate before narrowing. Positive overflow fixtures that the original code accepted are now rejected. |
| Matching ledger rows could hide a forged table or a trigger suppressing future history writes. | Validate the ledger's columns, key, absence of extra columns, triggers, indexes, and foreign keys. Require each history insert to affect one row. Fixtures cover forged columns, triggers, indexes, and extra columns. |
| Opening an unrelated or incompatible database changed its persistent journal mode before rejecting it. | Validate history before enabling WAL. An unrelated fixture is compared byte-for-byte before and after rejection. |
| Default creation permissions could expose future game data and backups to other local accounts under a permissive umask. | Exclusively create new files with owner-only read/write permissions before SQLite writes data. Test Unix permission bits and refusal to overwrite an existing file. Repair permissive permissions on recognized existing databases and their WAL/SHM sidecars too. |
| Nonregular files could cause unexpected I/O, and hard-link aliases bypassed a path-based lock. | Reject special files and Unix hard-linked databases; canonicalize symbolic aliases. Test FIFO rejection, hard-link rejection, and symbolic-alias contention. |
| Direct application quit could bypass the coordinator; a thread's finished signal also precedes native thread-local cleanup. | Defer application quit until asynchronous storage closure, and report closed only after a successful nonblocking join. Test close/quit during startup and repeated connection cleanup. |
| Metadata queries could materialize unnecessary rows and oversized values. | Use existence checks, forward-only reads, a registry-sized row limit, and byte-bounded text projections. Test oversized values and embedded NUL characters. |

The original implementation failed five added regression cases before the fixes
(two overflow cases, a ledger trigger, a forged definition, and byte-for-byte
preservation on rejection). These are concrete correctness fixes.

## SQL and durability assessment

No input-driven SQL concatenation was found. Executable statements are compiled
inside persistence. Migration names and backup paths are bound values, and public
interfaces expose neither SQL execution nor a database handle. Regression fixtures
put quotes and SQL punctuation in filenames and migration names, exercise an
upgrade/backup, and verify the ledger and data remain intact. UI diagnostics use
plain text rather than rich-text interpretation.

The write connection verifies disabled trusted-schema execution, foreign keys,
full synchronization, and the busy timeout. The backup reader also disables
trusted-schema execution. Each migration and its ledger entry share a transaction;
failed migrations roll back and never produce readiness. Upgrades require a
verified SQLite snapshot that includes committed WAL data. Only verified snapshots
are advertised as recovery backups. Expensive backup/integrity work occurs on
upgrade, off the GUI thread, rather than on every already-current startup.

Primary references: [Qt integer conversion caveat](https://doc.qt.io/qt-6/qvariant.html#toInt),
[SQLite schema trust](https://www.sqlite.org/pragma.html#pragma_trusted_schema),
[SQLite synchronization](https://www.sqlite.org/pragma.html#pragma_synchronous),
[SQLite snapshot backup](https://www.sqlite.org/lang_vacuum.html#vacuum_with_an_into_clause),
and [Qt native thread completion](https://doc.qt.io/qt-6/qthread.html#finished).

## Supported boundaries

- Use a user-controlled local data directory with normal OS access controls.
  A process able to rewrite that directory or the running process as the same user
  can bypass application locks; the module does not authenticate database contents
  or provide a hostile-file sandbox. Keep the Qt/SQLite runtime security-patched.
- The lock coordinates cooperating application instances, not arbitrary external
  SQLite writers. Do not edit or replace the live database externally. Do not use
  hard links or network/cloud-synchronized filesystems for it.
- Recognized application databases and existing WAL/SHM sidecars are restricted
  to owner-only access, including files created by older versions. Multi-account
  file sharing is not supported. Rejected databases are left unchanged.
- Released migration definitions and the ledger shape remain immutable. Future
  migrations must be transactional and preserve product invariants; repositories
  still require their own input-validation and data-integrity tests.
- Normal quit/close is asynchronous. Forced termination or direct event-loop exit
  bypasses graceful shutdown; committed transactions remain the recovery boundary.
  A running initialization finishes before queued close.
- Power-loss durability depends on the filesystem, storage hardware, and their
  implementation of SQLite's synchronization requests. Tests establish logical
  rollback and snapshot recovery, not hardware power-loss certification.

Focused Qt regressions cover these fixes alongside sequential upgrades, preserved
fixture data, rollback, corrupt input, backup failure, WAL snapshots, lock timeout
responsiveness, and startup/failure/shutdown presentation. Product tables, generic
database APIs, custom SQL parsing, and new dependencies remain outside this slice.

Validation passed on Linux with Qt 6.12: Debug build, both CTest suites, formatting,
clang-tidy, and an app smoke run using an isolated XDG data directory. Both suites
also passed with AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer.
The Qt 6.8 baseline was not separately runtime-tested in this workspace.
