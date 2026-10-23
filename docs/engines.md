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
default and option editing are later increments. Priority scheduling is available
through the infrastructure API described below.

Saved profiles live in the application SQLite database, never `ui-state.ini`.
Opening Settings or restarting the application reads those records without
launching software. **Last validated** is historical, not a current readiness
claim. Select a list row to inspect and copy its full name and executable path.
The list uses bounded plain-text previews. Invisible control and formatting
characters are shown as `\uXXXX` escapes (or `\UXXXXXXXX` for supplementary
code points) to prevent misleading path/name display;
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

Connect follows the single selected row. Moving keyboard focus without changing
the selection does not change the target executable. Selected executable paths
and full live metadata are shown in copyable, scrollable read-only panes.

**Ready** means the UCI handshake and readiness response completed. No position
or search is sent, no options are changed, and engine defaults remain in effect.
The capability summary describes advertised support; WDL output and analysis
correctness are not verified. The read-only option selector shows full identity,
executable path, option type, default, bounds and choices. Engine-provided text is
escaped for invisible formatting and displayed as plain text.

The connection stays alive across page navigation until **Disconnect**, failure,
or application exit. Disconnect also cancels startup. Registration and runtime
connections share one supervisor thread and allow only one child process at a
time. Disconnect before validating or connecting another engine. The active
engine remains identified even when another row is selected or another list page
is loaded.

Idle connections monitor process exit and protocol violations without periodic
readiness probes. Active searches use the readiness checks described below. A crash, timeout or invalid response triggers cleanup
and a visible failure; reconnect requires another explicit **Connect**. Cleanup
uses the same quit/terminate/kill deadlines as registration. After connection readiness, the combined stdout/stderr limit is 1 MiB per
monotonic one-second window, including idle and cleanup output. The 64 KiB stdout
line limit remains. Handshake/registration retain their 1 MiB total limit. Failed or
forcibly disconnected sessions are never reused. Opening Settings or restarting
the application never restores a live connection automatically. Saved validation
timestamps and profiles are not updated by connecting. During explicit Disconnect,
late handshake replies and incomplete idle lines are discarded; combined output
limits, exit-status validation and forced-cleanup failures still apply.


## Position-search infrastructure

The C++ `engine::EngineSupervisor` now supports searches on an explicitly connected
Ready session. This API is not yet wired to the Analyze board or a Settings test
button. Connecting in Settings still performs only the handshake/integrity check.
No search is launched automatically, and no evidence is saved to the database.

```cpp
engine::AnalysisRequest request;
request.id = QUuid::createUuid();
request.position = position; // Independent chess::Position, including its root/history.
request.limit = engine::NodeLimit{200000};
request.multiPv = 3;
request.wantWdl = true;
supervisor.analyze(session, request);
// Later, if needed:
supervisor.stopAnalysis(session, request.id);
```

Limits are exactly one of `NodeLimit`, `DepthLimit`, `TimeLimit` (milliseconds), or
`InfiniteLimit`. Numeric limits must be positive. Nodes fit signed 64-bit UCI
values; depth fits a signed int; time fits `INT_MAX - 5000` milliseconds.
`searchMoves` optionally restricts legal root moves; an empty list, illegal move,
or duplicate is rejected. Requests use fresh non-null UUIDs. Accepted IDs cannot
be reused within the connection; reconnect after 65,536 accepted searches. Invalid
requests are rejected without invalidating the healthy engine. Competing requests
are rejected Busy rather than queued, and stale session/request IDs cannot stop
another search.

Before every request the worker sets advertised Threads to one, configures
MultiPV and optional `UCI_ShowWDL`, sends `ucinewgame`, and waits for `readyok`
after `isready`. Hash retains the advertised default. There are no profile
option overrides. If Threads cannot represent one, the request is rejected.
The position command retains the original start position/FEN and complete move
history; it is never reduced to the current FEN alone. Commands are bounded to
64 KiB and oversized histories are rejected without truncation.

MultiPV is reduced explicitly to the advertised maximum, the number of legal
restricted/unrestricted candidates, and a local maximum of 256. Engines without
MultiPV use one. The UCI configuration stays at least one even on terminal
positions, where achieved candidate width is zero. Results retain both requested
and effective width. WDL is optional: the worker enables it when advertised and
requested, and disables it for subsequent requests that do not request it.
Unsupported/missing WDL stays absent rather than being fabricated.

`searchChanged` reports Configuring, Analyzing, Stopping, and Synchronizing with
session/request identity. Progress is produced on a 100 ms worker timer, plus a
final snapshot, and delivered through `analysisUpdated`. While the caller is stalled, a
bounded mailbox retains only the latest progress value and one queued notification;
intermediate progress may be skipped. Terminal results always retain full evidence.
`analysisFinished`
provides Rejected, Completed, Stopped, or Failed. Await that terminal signal
before submitting another search. Completed means natural UCI completion and
successful final synchronization; it does not prove the requested node budget,
width, or evidence depth was achieved. Check actual statistics and coverage.
Stopped results retain partial evidence and any validated bestmove.

Raw centipawn/mate scores, bounds, and WDL use the root side-to-move perspective.
Each rank retains its latest evidence-bearing info line independently, with its
own depth and statistics. Missing fields remain absent; unrelated partial updates
never manufacture a score/PV pair. Global statistics accept partial updates, including depth-only progress, without
changing the depth attached to an older PV.
`achievedWidth` counts contiguous ranks with both score and PV and distinct root
moves; duplicate candidates never count as additional width. Temporary rank overlap
can occur while an engine updates its ordering and does not itself fail the search.
`commonDepth` is
present only when the entire effective width has that depth. Mixed-depth ranks
remain visible without implying a coherent set. WDL values use the 0–1000 scale
and sum to 1000. PVs are limited to 1,024 legal plies; bestmove and ponder are also
validated, including root restrictions. Truncated or overlong tokens beginning
with a square-like file/digit pair are rejected as malformed moves rather than
silently accepted as extensions. `0000`/`(none)` is accepted only when
there are no legal moves.

Auxiliary `info` fields are checked and discarded: `currmove` must be a legal
root move allowed by the request, `currmovenumber` must be positive, `cpuload`
uses 0–1000, and `sbhits` fits unsigned 64 bits. Move numbers and optional positive
`currline` CPU numbers fit a signed int. `refutation` requires at least one legal
move; `currline` may be empty. Both replay from the root, allow up to 1,024 plies,
and may include moves outside the request's root restrictions. Repeated refutations
and current lines are accepted; duplicate scalar fields and PVs fail. These fields
never create candidates, coverage, search statistics, or meaningful search activity.
An invalid field rejects the entire record before its valid prefix can be applied.

Unknown words end move sequences and are skipped while known fields afterward
are still parsed. Square-like malformed tokens remain errors. `string` makes the
rest of an info record opaque. `bestmove` accepts only a legal move with an optional
legal `ponder` move; extra tokens fail, and terminal null moves cannot have ponder.

Stop sends `stop` once and waits up to five seconds for `bestmove`. If stopped
during setup, no `go` is sent and no bestmove is required. Every completed search
requires a subsequent `isready`/`readyok` barrier before reuse. After ten seconds
without meaningful search activity, a readiness probe must be answered within
five seconds. A probe does not finish the search. Node/depth/infinite searches
have no total elapsed-time cap; timed searches allow five seconds after their
budget for bestmove. Configuration and final readiness also have five seconds.

Malformed recognized evidence, illegal moves, missing required replies, process
exit, or output violations fail the search and retire the connection. Reconnect
explicitly after cleanup; failed sessions are never reused. Disconnect/application
shutdown stops an active search, drains completion, then quits/reaps the child
using existing escalation. Failure results preserve the first error and are
published after worker retirement. Evidence is raw engine output validated for
structure and legality, not proof that the engine's evaluation is correct.


## Failure context and timeout recovery

Process-attempt errors include optional `Error::stage` context alongside the
existing error code and safe, actionable message. `FailureStage` distinguishes
executable checking (0), launch (1), handshake (2), integrity verification (3),
idle connection (4), search setup (5), analysis (6), readiness probe (7), timed
search completion (8), stop (9), final synchronization (10), and cleanup (11).
These numeric values are stable and independent of runtime phase codes. Errors
outside process execution, such as admission or invalid requests, may omit stage.

If a readiness probe or timed search times out during active analysis, the worker
records Timeout and sends `stop` once. It waits up to five seconds for a legal
`bestmove`, then quits; it also quits when that grace expires. The session always
fails and must be explicitly reconnected, even if the engine answers stop. Normal
quit is followed by one-second terminate/kill escalation when needed.

Evidence freezes when the timeout is recorded. Late info and bestmove do not
change the failed result. A pending probe's valid readyok may be consumed without
extending the stop deadline. Malformed terminal/control replies or excessive output
end the grace period immediately. Later crashes, protocol errors and forced cleanup
never replace the first error or its stage.

Search setup and final synchronization timeouts go directly to cleanup. An expired
user-stop deadline does not send another stop. Crashes and malformed recognized
output also trigger immediate cleanup. Earlier valid evidence and completed results
are retained, but failed evidence is never presented as completed analysis. Closing
or disconnecting during recovery keeps the bounded cleanup active without blocking
the GUI. No retry or expensive analysis starts automatically.

When an unexpected process exit is observed, its failure is recorded before final
buffered output is drained. Tail replies cannot complete analysis, change its
frozen evidence, or replace that failure's context. Both output channels are still
drained before retirement. Results completed before the exit was observed remain
completed.


## Scheduled analysis API

`EngineSupervisor::scheduleAnalysis(session, request, priority, optionalJobId)` queues
an immutable copy of a request on the established runtime connection. It launches
nothing without an explicit connection and submission. The scheduler shares the
existing engine thread and single process; no live analysis UI or persisted queue
is provided yet.

Priority order is `InteractiveAnalysis`, `RetryMove`, `VisibleReviewDeepening`,
`ReviewQuickScan`, `ReviewBackgroundDeepening`, `PuzzleVerification`, then
`Maintenance`. Equal priorities run FIFO. Interactive analysis and retry grading
may stop the three lowest classes; other priorities only select the next search.
Strict priority permits sustained interactive work to delay background work.

There is room for 256 pending requests and one active request. Overflow rejects
with Busy. A review producer should submit batches as capacity becomes available.
Malformed requests reject before they can interrupt a healthy search. Logical
request IDs are unique per session, including cancelled requests. The accepted-ID
and cancelled-job registries each hold at most 65,536 IDs; at exhaustion new
submissions reject with InvalidRequest until explicit reconnect.

Preemption uses the normal cooperative stop and final readiness barrier. It emits
`scheduledInterrupted` with a lossless partial snapshot, then requeues the logical
request at its original FIFO position. Resumption starts a fresh internal attempt
with the full original budget. Statistics, ranks, and budgets from different
attempts are never combined. This restarts work; it does not resume engine state.
The existing 65,536 worker search-ID limit also counts restarted attempts. If the
worker cannot admit a fresh attempt, that logical request fails explicitly.

`scheduledChanged` exposes Queued, Configuring, Analyzing, Stopping, and Synchronizing.
`scheduledUpdated` is provisional and latest-only: callers must tolerate missing
updates. State, interruption and terminal events are lossless queued deliveries on
the supervisor's owning thread. `scheduledFinished` reports Rejected, Completed,
Cancelled or Failed, with optional contextual error and available evidence.
A completed result still does not prove the requested depth, width, or strength;
inspect its actual evidence as with direct searches.

The outer identity contains the stable logical request, session, optional job, and
current attempt UUID. Queued requests have no current attempt. An evidence value
carries its own originating attempt identity; its inner `AnalysisSnapshot::request`
is that attempt UUID. A cancelled restart that produced no candidates can retain
an earlier interrupted attempt's useful evidence. Always use that evidence's
identity rather than assuming it came from the terminal result's current attempt.
Do not treat retained partial evidence as a completed search.

`cancelScheduledAnalysis(session, requestId)` removes a pending request or stops its
active attempt. Repeated or stale cancellation is harmless. A natural bestmove
already received wins over subsequent cancellation, provided final synchronization
succeeds. Cancelling an already preempted attempt prevents its restart even while
that stopped attempt is synchronizing. Protocol and cleanup failures remain failures.

`cancelAnalysisJob(session, jobId)` cancels unfinished members and blocks future
submissions with that ID until reconnect. Use a fresh non-null job UUID for a rerun;
omit the optional job for independent requests. Cancelling a job never retracts
completed results. Useful partial evidence is delivered to callers, but this
increment does not cache or persist it.

Failure finishes queued work as Failed after worker retirement. Explicit disconnect
or close cancels queued work and drains the active attempt; active cleanup errors
remain failures. Scheduled terminal delivery precedes `closed()`. Reconnect is
explicit and starts an empty queue with fresh registries; no pending work retries.

The direct `analyze`/`stopAnalysis` API retains its existing rejection and stop
semantics and legacy signals. Direct requests reject while the scheduler owns
capacity. Scheduled work waits for an active direct search and never preempts it.
Scheduled attempt events use only the scheduled signals; legacy stop calls cannot
cancel scheduled work. Settings remains the only application consumer and retains
its existing shutdown ownership until a future analysis UI consumer is integrated.
