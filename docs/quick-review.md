# Quick review

Select a saved game in **Games** and choose **Review…**. Choose **White** or
**Black**, a registered engine, and **Fast**, **Standard**, or **Thorough**, then
choose **Run Review**. Scrolling over the setup controls leaves their selections
unchanged; open a selector or use the keyboard to change a choice. The quality defaults to the review configuration; changing
it here affects only this run. Each new run keeps its own evidence and history.

Run Review connects the chosen local engine when necessary. If another engine is
connected, disconnect it in Settings first. The scan includes both players' moves,
the initial position and the final position. Your player choice is retained as the
review perspective. Checkmate and stalemate positions use local board proof.

The progress count includes only positions whose evidence has been saved. Valid
completed engine results can still contain weak or otherwise insufficient evidence.
New runs automatically refine selected moments after the quick scan. Processing
completion does not certify every classification. The compact summary shows
accuracy with coverage; expand **Summary and evidence details** for counts,
provenance and the full inspector. See [refinement and summaries](review-summaries.md).

## Board-first navigation

Review places the original game board beside the selected move's explanation.
The board shows the position **after** the selected move, with the reviewed player
at the bottom. **Flip Board** changes the current Review session's orientation.
The first available summary opens the first chronological key moment, or the
initial position if there are none. Navigating yourself prevents a late summary
from moving you elsewhere.

Use **Start**, **Previous**, **Next**, **End**, or the numbered White/Black timeline
to explore every original mainline move, including moves without analysis. Left/Right
and Home/End work in the timeline and move-navigation controls. Hovering a move
temporarily previews its board and evaluation; the selected explanation stays in
place. Leaving the timeline restores the selected position. Clicking commits the
preview. The preview indicator distinguishes temporary and pending positions.

**Previous key moment** and **Next key moment** follow the reviewed player's
important moments chronologically, across evidence-page boundaries. Expand
**Chronological key moments** for the list and opponent-error context.
Classifications have textual markers and provisional/stable labels as well as
color. Missing evidence is explicitly unavailable.

The slim bar displays **White's expected score**, including draws; it is neither
win probability nor a pawn evaluation. Approximate conversion is labeled. Missing,
bounded, or unsupported numeric evidence leaves the bar unavailable.

Drag the divider to resize the workspace; its Review-specific share survives an
orderly exit. Explanations scroll independently. **Review setup** and **Saved
review runs** expand on demand. Returning within the session retains the selected
moment. Interactive better-move lines, Try Again, and Analyze/Back transitions are
not yet implemented.

## Background work and cancellation

Leaving an active Review workspace offers **Continue in background**, **Stay** or
**Cancel task**. Once background continuation is chosen, navigation does not ask
again for that run. A persistent task button shows saved coverage and returns to
Review. Only one review runs at a time; games and manual analysis remain usable.
Manual engine analysis can delay a quick scan on the shared engine worker.

**Cancel review** stops further work and retains completed, committed position
evidence. It waits for engine cancellation and outstanding saves before reporting
Cancelled. Orderly app shutdown also cancels unfinished scans and drains their
accepted writes. A forced exit leaves startup recovery to mark unfinished jobs
Interrupted. Work never resumes automatically.

## Saved runs and resume

The Review workspace’s **Saved review runs** section lists runs for the selected
game, with pagination. The **Evidence** tab under **Summary and evidence details**
shows the inspected run’s paged evidence and full rationale.
Select a run to inspect its status and coverage without starting an engine.
Incomplete runs offer **Resume selected run**. Resume keeps the original player,
quality, complete configuration and supported evidence-model versions, even if the
current valid configuration uses a different quality.

Resume freshly reconnects the original engine and compares its full effective
configuration fingerprint. If the original engine is already disconnecting, resume
waits for that disconnect before reconnecting. A changed executable or configuration blocks continuation;
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

## Move classifications and evidence

The timeline includes both players and all mainline moves. Evidence is loaded in
bounded pages of 50 plies as needed; the expanded inspector also offers explicit
page controls. Select a move with the mouse or keyboard to inspect its rationale. Labels appear
only after both adjacent positions and the classification have been committed.
Cancelled and partial runs retain useful classifications; unprocessed moves remain
visible as **Awaiting evidence**. **Unclassified** means evidence was retained but
does not support a normal label. Details explain the reason.

Quick-scan labels are **provisional**. Targeted refinement can establish stability
when two stronger passes agree; unselected or unresolved moves remain provisional. The rules use expected-score loss from the mover's perspective:

| Rule, evaluated in order | Result |
| --- | --- |
| Verified rank 1, but loss reaches Inaccuracy | Withhold: searches disagree |
| Loss ≥ Blunder threshold (default 20 percentage points) | Blunder |
| Loss ≥ Mistake threshold (default 10 points) | Mistake |
| Loss ≥ Inaccuracy threshold (default 5 points) | Inaccuracy |
| Verified rank 1 below Inaccuracy | Best |
| Loss ≤ Excellent maximum (default 1 point) | Excellent |
| Remaining valid loss below Inaccuracy | Good |

Error bands take priority even if the configured Excellent maximum exceeds
Inaccuracy. Best means the played move matches the earlier search's verified
first candidate, with paired loss below Inaccuracy; quick-scan evidence remains provisional.
A missing played-move rank remains unknown. Approximate centipawn conversion is
identified explicitly. Display values are rounded; rules compare the unrounded
stored values.

The inspector shows before/after expected scores, loss in percentage points,
thresholds, rule/model version, verified best move and played rank, measured
candidate gaps, raw scores/WDL with their original perspective and bounds,
measured analysis strength, engine identity, and evidence/session/attempt IDs.
Nearly equivalent alternatives require comparable candidates from the same search
within the configured Excellent maximum. MultiPV=1 says **not assessed**; no
alternative rank or equivalence is inferred from the later-position score.

Bounds, engine mate claims, insufficient strength, incompatible conversions,
contradictory candidates, and apparent negative loss withhold normal labels.
Locally proven terminal outcomes retain the existing conservative pairing rules.
No Brilliant, Great, Miss or Book labels are produced by this increment.

Opening an older run derives and saves classifications in background batches from
its owned evidence, without an engine or the disposable cache. It retains the
original thresholds and conversion settings and records a 1% Excellent maximum.
Backfill can continue when the run is opened again after interruption; it does
not alter scan coverage or job status. Changed current TOML settings apply to new
runs, not existing runs. Unsupported or damaged saved records produce explicit
errors instead of silent replacement.

### Classification model versions and retry

New classifications use model 2. Native WDL values are exact multiples of 1/2000:
model 2 subtracts the integer win/draw counts before converting loss or candidate
gaps to a number. An exact 5-percentage-point loss therefore reaches the 5-point
Inaccuracy threshold; an exact 1-point loss stays within Excellent. This avoids
floating-point cancellation at those boundaries. Approximate centipawn conversion
and conservative evidence validation are unchanged; negative loss is not clamped.

Saved model-1 classifications retain their original numeric rules, rule IDs and
rationale, with an explanatory note in the inspector. Opening them does not
silently rewrite historical labels. Missing classifications and genuinely replaced
evidence use model 2. The underlying evidence/conversion models, saved thresholds,
and configuration identities are unchanged.

A failed move-evidence load offers **Retry move evidence**. Retry rechecks the
visible page and restarts pending backfill without starting an engine; damaged or
unsupported records remain explicit errors. Late replies from a deselected run or
superseded page cannot replace the current view or its error state. While scanning,
move-list refreshes coalesce over 200 ms; new classifications still commit with
each evidence checkpoint. Background backfill continues after the scan stops.

Retrying preserves the selected move within its evidence page. Scrolling the
timeline keeps that move's explanation available within the bounded page cache.
While a hover preview is loading, its status identifies the requested position
and the position still displayed on the board.
At the initial position the evidence inspector has no selected move row. Long
game and player labels stay within the workspace; hover over clipped labels for
their displayed-name text, which also remains available to accessibility tools.


## A finished run has no useful evaluations

**Processing complete** means the scheduled searches and saves finished. It does
not guarantee that every result supports a move classification. The summary now
explicitly identifies a run with no usable move evaluations; evidence details
explain bounds, mate claims, inconsistent estimates, and other withheld results.

Node-limited Stockfish searches can stop during an unfinished iteration and finish
with an upper/lower bound. New runs retain the last coherent exact iteration as
separate evidence, rather than losing it when that partial output arrives. The
original latest output and final bestmove remain stored. No score bound is
relabelled as an exact score, and missing/inconsistent evidence stays unclassified.

Older runs that saved only the final bounded output cannot reconstruct earlier
iterations. After updating and restarting the app, expand **Review setup** and
choose **Run Review** to collect a new run. Existing runs and the imported game
remain intact; reopening an old run does not silently rewrite its evidence.
