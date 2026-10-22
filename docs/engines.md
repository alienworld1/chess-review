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
file again and rejects observed changes in content, size, modification time, or
access. Hashing reads a fixed observed length in cancellable chunks; a growing
file cannot extend the read indefinitely. There is no search probe yet: a successful
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
claim. Select a list row to inspect and copy its full name and executable path.
The list uses bounded plain-text previews. Invisible control and formatting
characters are shown as `\uXXXX` escapes to prevent misleading path/name display;
the original reported identity is saved unchanged.
Paths with spaces, Unicode, and shell punctuation are passed as paths, never shell
commands.

Malformed/conflicting options, invalid protocol transitions, missing names, and
invalid UTF-8 fail validation. Unknown informational lines are ignored. Limits
are 64 KiB per stdout line, 1 MiB combined stdout/stderr per validation, and 1,024
options. Raw output and executable paths are not written to diagnostic logs.

These checks detect ordinary errors and observed replacement. They do not protect
against malicious concurrent same-user filesystem changes or arbitrary filesystem
stalls. Keep engine binaries and the application database on trusted local storage.

## Live handshake connections

Select a saved engine and choose **Connect** to launch it and rediscover its UCI
identity and options. The executable hash must match registration. Reported name,
author and option declarations must also match; declaration order and option-name
casing are ignored. A mismatch leaves the saved profile unchanged. Replacement
management is not available yet.

**Ready** means the UCI handshake and readiness response completed. No position
or search is sent, no options are changed, and engine defaults remain in effect.
The capability summary describes advertised support; WDL output and analysis
correctness are not verified. The read-only option selector shows full identity,
option type, default, bounds and choices. Engine-provided text is escaped for
invisible formatting and displayed as plain text.

The connection stays alive across page navigation until **Disconnect**, failure,
or application exit. Disconnect also cancels startup. Registration and runtime
connections share one supervisor thread and allow only one child process at a
time. Disconnect before validating or connecting another engine. The active
engine remains identified even when another row is selected or another list page
is loaded.

The application monitors process exit and protocol violations but does not send
periodic readiness probes. A crash, timeout or invalid response triggers cleanup
and a visible failure; reconnect requires another explicit **Connect**. Cleanup
uses the same quit/terminate/kill deadlines as registration. Output limits apply
to the whole connected session, including idle stdout and stderr. Failed or
forcibly disconnected sessions are never reused. Opening Settings or restarting
the application never restores a live connection automatically. Saved validation
timestamps and profiles are not updated by connecting.
