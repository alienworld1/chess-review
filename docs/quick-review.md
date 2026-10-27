# Quick review

Select a saved game in **Games** and choose **Review…**. Choose **White** or
**Black**, a registered engine, and **Fast**, **Standard**, or **Thorough**, then
choose **Run Review**. The quality defaults to the review configuration; changing
it here affects only this run. Each new run keeps its own evidence and history.

Run Review connects the chosen local engine when necessary. If another engine is
connected, disconnect it in Settings first. The scan includes both players' moves,
the initial position and the final position. Your player choice is retained as the
review perspective. Checkmate and stalemate positions use local board proof.

The progress count includes only positions whose evidence has been saved. Valid
completed engine results can still contain weak or otherwise insufficient evidence.
**Quick scan complete** means every position has been processed, not that every
position supports an interpretation. Classifications, accuracy, explanations and
the guided board review are not available yet.

## Background work and cancellation

Leaving an active Review workspace offers **Continue in background**, **Stay** or
**Cancel task**. Once background continuation is chosen, navigation does not ask
again for that run. A persistent task button shows saved coverage and returns to
Review. Only one review runs at a time; games and manual analysis remain usable.
Manual engine analysis can delay a quick scan on the shared engine worker.

**Cancel scan** stops further work and retains completed, committed position
evidence. It waits for engine cancellation and outstanding saves before reporting
Cancelled. Orderly app shutdown also cancels unfinished scans and drains their
accepted writes. A forced exit leaves startup recovery to mark unfinished jobs
Interrupted. Work never resumes automatically.

## Saved runs and resume

The Review workspace lists saved runs for the selected game, with pagination.
Select a run to inspect its status and coverage without starting an engine.
Incomplete runs offer **Resume selected run**. Resume keeps the original player,
quality, complete configuration and supported evidence-model versions, even if the
current valid configuration uses a different quality.

Resume freshly reconnects the original engine and compares its full effective
configuration fingerprint. A changed executable or configuration blocks continuation;
use the original engine or start a new independent run. If the original profile is
on another engine page, load that page in Settings first. Invalid current TOML
configuration blocks starting and resuming until corrected and the app restarted.

Resume validates retained evidence and processes only missing positions. Damaged
saved records produce an error rather than being silently replaced. Revision
conflicts reload the newer saved state without overwriting it; another active owner
must finish or stop before continuation. Saved complete runs remain inspectable
without an engine.

Completed attempts also populate the disposable analysis cache. Reuse follows the
existing strict history, search-strength, configuration and verified-resource rules;
currently only the reviewed pinned Stockfish configuration qualifies. Other engines
can collect durable review evidence but safely miss cache reuse. Clearing or failing
to update that cache does not erase the review's own saved evidence.
