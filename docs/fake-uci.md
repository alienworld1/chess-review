# Deterministic fake UCI engine

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

## Scenario format (version 1)

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

## Included scenarios

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

