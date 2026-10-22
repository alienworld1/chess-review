# Local UCI engines

Open **Settings → Engines**, or **Engine Settings…** from Analyze. Choose a local
executable, review the displayed resolved path, then select **Validate and Add**.
Validation runs that software with your normal user permissions. Only select
software you trust. The application does not download, copy, own, or sandbox it.

The selected file must be a readable, executable regular file. Symbolic links
resolve to their target, and the target path is saved. Launch arguments, custom
environments, and working directories are not configurable in this increment.
Linux is verified; Windows release verification remains pending.

Validation hashes the file with SHA-256, requests its UCI identity and options,
requires `uciok` and `readyok`, and asks the process to quit. It then hashes the
file again and rejects observed changes. There is no search probe yet: a successful
registration establishes handshake compatibility, not analysis correctness.
The engine name is retained as reported; no version is inferred from it.

Startup has a five-second deadline. Each handshake stage has ten seconds. An
engine that does not quit within one second is terminated, then killed after
another second if required; forced cleanup fails registration. Cancellation keeps
the UI responsive and waits for child-process cleanup. Once **Saving validated
engine…** appears, the accepted storage operation finishes, including during quit.
No profile is created by failed or cancelled validation.

The first engine becomes default. Further additions preserve that default.
An identical binary returns the existing entry without changing its path or
validation time, even if another copy was selected. Changed content at an already
registered path is rejected. Rename, remove, relocation, replacement, changing the
default, option editing, and analysis scheduling are later increments.

Saved profiles live in the application SQLite database, never `ui-state.ini`.
Opening Settings or restarting the application reads those records without
launching software. **Last validated** is historical, not a current readiness
claim. Engine text is displayed literally, with bounded previews in the list.
Paths with spaces, Unicode, and shell punctuation are passed as paths, never shell
commands.

Malformed/conflicting options, invalid protocol transitions, missing names, and
invalid UTF-8 fail validation. Unknown informational lines are ignored. Limits
are 64 KiB per stdout line, 1 MiB combined stdout/stderr per validation, and 1,024
options. Raw output and executable paths are not written to diagnostic logs.

These checks detect ordinary errors and observed replacement. They do not protect
against malicious concurrent same-user filesystem changes or arbitrary filesystem
stalls. Keep engine binaries and the application database on trusted local storage.
