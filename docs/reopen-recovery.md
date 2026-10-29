# Reopening reviews and recovering interrupted work

[Back to README](../README.md)

Opening a game in Review restores the last run you inspected for that game and
that run's last selected original-game moment. If you have not inspected a run,
the newest run opens at its first key moment, or the root when there are no key
moments. The run supplies its saved player perspective.

Review selections are saved asynchronously after 250 ms of inactivity. Normal
shutdown flushes the latest selection before closing storage. An unexpected exit
can lose selections made within that interval; completed evidence is checkpointed
separately. Hover previews never replace the saved moment.

When you last used Review or its linked Analysis workspace, startup returns to
Review. Candidate replay, Try Again attempts, and linked Analysis edits remain
session-only; reopening returns to their original game moment. Ordinary Analyze
saved-game restoration continues to use the existing workspace preference.

Saved evidence, classifications, summaries, and candidate lines remain readable
without an engine connection and independently of the disposable analysis cache.
An invalid current TOML configuration blocks new computation, but does not block
reading a supported saved review. Historical settings are identified in Review;
results continue to use the settings and supported models saved with that run.
Unsupported or damaged records are preserved and reported, never silently
reinterpreted or replaced with another run.

## Interrupted reviews

On startup, unfinished jobs become **Interrupted**. Home lists interrupted reviews
with game IDs, player sides, and saved position counts; the list is paginated.
Open a run to inspect its evidence, then choose **Resume selected run** explicitly.
Startup and opening a review do not launch an engine.

Resume requires valid current configuration, the original registered engine
profile, and a freshly validated matching engine fingerprint. It uses the run's
saved configuration and retains completed evidence, scheduling missing work.
Choose a new review to use changed settings or a different engine. New reviews
retain earlier runs in history.

## Working while a review runs

After choosing **Continue in background**, you can browse another game's saved
review. Only one review job runs at a time; starting or resuming a second waits
until the current job finishes or is cancelled. The global task button identifies
the running game's saved progress and returns directly to its run.

The library shows the selected run's status and chosen-player accuracy with
coverage. Partial and provisional values are explicitly marked. Missing or
unreadable accuracy is unavailable, never zero. Accuracy loads progressively from
validated saved evidence without starting analysis.

If a remembered run cannot load, the healthy game remains available with an
explanation and its run history. A missing/unreadable game returns to Games with
an error. No recovery action resets the library or discards saved evidence.
