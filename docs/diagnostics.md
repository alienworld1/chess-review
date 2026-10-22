# Diagnostics and troubleshooting

[Back to README](../README.md)

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
location override described in [Storage](storage.md#application-storage-paths). Files `events-0.jsonl` (current) through
`events-4.jsonl` hold newline-delimited JSON, capped at 2 MiB each (10 MiB total).
Logs are disposable, rotate by size, and have no age-based expiry. A cooperating
second process uses memory-only diagnostics while the log directory is locked.
Only known log filenames rotate; unrelated cache and library files are preserved.

Normal records cover session/storage lifecycle, schema version, backup/migration
outcomes, elapsed time, and stable error codes. Engine registration adds validation
start/success/failure and registration-completion events. Registration counts
separate new entries (one) from duplicates (zero); failures carry typed numeric error
codes. Engine identity, options, paths, and raw stdout/stderr remain excluded. For extra safe intermediate events
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
