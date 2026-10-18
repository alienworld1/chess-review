# Application paths adversarial review

Reviewed against AGENTS.md, architecture sections 25–26, and the PRD's local
storage/configuration requirements. Scope: the paths foundation and its startup
integration, including tests. Configuration parsing, logging, and cache deletion
do not exist yet.

## Findings and corrections

| Finding | Correction and evidence |
| --- | --- |
| Public value construction could produce a relative or NUL-containing database/configuration filename. The current database worker rejects these, but a future configuration writer could accidentally use the working directory. | Both filename helpers now reject non-absolute and NUL-containing directories. New relative, current-directory, and embedded-NUL regression rows failed before the fix and pass afterward. Empty directories continue to return an empty path. |
| Startup integration tests shared one data root and depended on execution order. | Each test now receives fresh temporary XDG roots. Running the failure test before the success test reproduced the failure before the fix; reversed and repeated selections now pass. |
| The lookup-side-effects assertion used a directory listing that excluded hidden files, weakening coverage of `.config`, `.cache`, and `.local`. | Assert that each expected root is absent and inspect hidden/system entries too. |
| Tests checked for working-directory fallback in the build/source directory. A future regression could write there before being detected. | Both test executables run with a temporary working directory and restore the original directory before cleanup. Linux HOME and tested XDG roots remain temporary. |

## Security, correctness, and performance assessment

- Qt selects application-specific paths. Relative XDG overrides are ignored by Qt
  in the tested runtime, as required by the
  [XDG specification](https://specifications.freedesktop.org/basedir/latest/).
  No custom environment parser, shell execution, SQL, downloads, or system-wide
  search-path fallback was introduced.
- The database location and file name remain compatible with existing installs.
  Main sets the application identity before resolving one paths snapshot; it
  injects the database filename into startup. The paths target depends only on
  Qt Core and has no worker, global mutable state, or UI dependency.
- Lookup performs no directory creation, permission changes, canonicalization, or
  writability probes. There are three Qt location queries once at startup, plus
  lexical filename validation/joining. There is no polling or added per-frame work;
  no measured speedup is claimed.
- The storage worker still owns filesystem writes, SQL, locking, backup handling,
  and detailed failures. Plain-text UI diagnostics preserve the attempted path.
  Unavailable data does not trigger an alternative database or a reset. Tests
  cover asynchronous success, explicit failure, and shutdown.
- Analysis remains durable database content. Configuration/cache directories and
  placeholder files are not created by startup. Existing persistence tests cover
  file permissions, symbolic aliases, hard links, special files, and lock behavior;
  this change does not replace those controls.

## Supported boundaries and future consumers

- A resolved path is a location, not proof that it exists, is writable, is private,
  or is on a filesystem suitable for SQLite WAL. Follow the storage boundaries in
  [persistence-review.md](persistence-review.md). Filesystem races and malicious
  processes able to modify the user's storage remain outside this path resolver's
  protection. Do not run the desktop app with elevated privileges.
- Application identity must remain stable after initialization. Renaming it or
  adding an organization prefix later requires an explicit data-migration decision.
- XDG overrides can put data, configuration, and cache in the same directory;
  symbolic links can alias them too. Their semantic roles do not prove physical
  separation. Future cache cleanup must delete only known subsystem-owned cache
  files, never recursively delete a resolved root. There is no cache-deletion API
  in this foundation.
- Future configuration/log writers must reject an unavailable derived path,
  validate their inputs, use appropriate private/atomic file creation, and report
  actual write failures. Existing directory permissions remain unchanged as agreed
  for this task. Logs under cache are disposable by the documented architecture.
- Native Windows/macOS mappings and the Qt 6.8 minimum have not been runtime-tested
  here. They use the same existing Qt APIs without Linux path concatenation in
  production. Linux validation uses Qt 6.12.

## Validation

Debug build and all three CTest suites pass, including the pre-existing
persistence suite. Reordered/repeated startup path cases pass. Formatting and
curated clang-tidy checks pass. The sanitizer build passes all three suites with
AddressSanitizer and UndefinedBehaviorSanitizer enabled and leak detection disabled
(`ASAN_OPTIONS=detect_leaks=0`, `UBSAN_OPTIONS=halt_on_error=1`). With leak detection
enabled, test assertions pass but LeakSanitizer exits with a fatal ptrace restriction
in this sandbox; leak checking is not established by this run. An offscreen executable
smoke run initializes the isolated XDG database without creating configuration or
cache roots. Interactive window verification is unavailable because the session's
X display is inaccessible; Qt widget tests exercise normal close and shutdown.
