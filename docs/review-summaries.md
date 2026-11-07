# Refinement, stability and summaries

Run Review first scans the entire original mainline, including its final position.
The results become readable while targeted refinement continues automatically.
Cancel retains completed work; Resume explicitly continues the original run with
its captured configuration and compatible engine. A new run uses current settings.

New reviews refine both players' Inaccuracy-level expected-score losses (5% by
default), replies immediately following those errors, losses near applicable
classification boundaries (including Excellent/Good for non-best moves), mate
signals, contradictory comparisons and repairable evidence gaps.
Each target's adjacent positions receive the quality preset's
deepening and verification budgets. Shared positions are analyzed once per pass.
MultiPV defaults to three, constrained by the engine and legal move count. Width
one remains usable but cannot establish alternative equivalence.

A stable ordinary classification requires sufficient evidence from two distinct
stronger attempts, agreement on label and best move, and no more than two
percentage points of change in either expected score. Verification must have more
measured nodes and no shallower common depth. Reusing one attempt never provides
two confirmations. Equal custom budgets do not guarantee a stable result.

New runs capture pipeline policy 9 and stability model 3. Both compared losses must
also be farther than the captured `boundaryMargin` from each applicable label
cutoff. The default margin is one percentage point; equality remains provisional.
A zero margin still protects an exact cutoff. Best moves do not use the
Excellent/Good boundary, and an Excellent cutoff at or above Inaccuracy is inactive
under the classifier's precedence rules. Native WDL/terminal losses retain exact
half-permille differences; fallback losses retain their original conversion values.

Threshold uncertainty can use the existing two bounded extra rounds. The evidence
inspector shows both observed losses, affected cutoffs, required clearance and why
work stopped. Agreement alone does not confirm a nearby label. These checks are a
conservative rule, not a statistical confidence interval or a guarantee that deeper
search will agree. With the default 1% Excellent cutoff and one-point margin,
non-best Excellent labels remain provisional even after exhausting the rounds.
Custom non-grid cutoffs and fallback losses use canonical double subtraction, so
new-policy decisions do not depend on the platform's extended floating-point precision.
Older saved runs retain their captured stability and scheduling rules, including
when resumed. Start a new run to use the new policy. Historical policy-8 arithmetic
can differ across platforms with different extended precision; start a new run to
reconfirm its labels after moving the database between such platforms. Retry
exercises retain their existing grading rules; provisional original moves cannot
start an exercise.

Moves with only quick-scan evidence and unresolved results remain provisional.
Finishing the planned work does not mean every move is stable. There are no unbounded refinement retries.

Engine mate claims stay separate from numeric interpretation. When both stronger
passes report mate at the same position for the same side, the inspector reports
a persistent signal. It does not turn that claim into an ordinary label or accuracy
score. Local checkmate and stalemate proofs retain their existing semantics.

The Summary tab shows separate White and Black accuracy, evaluated/total and
stable/total move coverage, classification counts, withheld and unprocessed moves,
and chronological key moments centered on the selected player. Key moments include
Mistakes, Blunders and mate signals; an immediately preceding opponent error is
attached as context. Select a moment to inspect its evidence. Counts and moments
can change during refinement. An older checkpoint is explicitly identified.

Summaries use owned review evidence and remain available without an engine or the
disposable analysis cache. Existing completed quick-scan runs retain historical
classifications; start a new run to use refinement. Refresh summary retries a
failed summary read without silently repairing saved records. If a refresh fails,
the error appears above any retained numbers, which are explicitly marked as the
last successfully loaded summary. Invalid stage mappings are rejected even for
games with no moves. During deepening, visible move interpretations coalesce
checkpoint updates over one second; manual paging and final results remain prompt.

## Accuracy model 1

This app's metric adapts the open [Lichess move accuracy formula](https://lichess.org/page/accuracy)
to its native-WDL-first expected-score evidence; it does not claim to reproduce
Lichess game accuracy. It measures available move-quality evidence, not playing
strength or decision difficulty.

For valid mover-perspective loss `L` in **percentage points**:

- Zero loss gives move accuracy 100.
- Otherwise use `clamp(103.1668 * exp(-0.04354 * L) - 3.1669, 0, 100)`.
- Each side's game accuracy averages the arithmetic and harmonic means of its
  included move accuracies. A zero value makes the harmonic mean zero.
- No included moves means unavailable, never 0%.

Each move uses the strongest valid complete adjacent pair available. Missing,
bounded, contradictory and unsupported mate evidence is excluded. Native WDL and
terminal differences are computed from integer half-permille counts before
conversion. This calculation is independent of classification labels, historical
classification arithmetic and configurable label thresholds. Summaries display
whole-number accuracy alongside coverage and provisional status.

Advanced margins and MultiPV belong in the validated TOML configuration; see
[the example](config.example.toml). Changes apply after restart and are captured by
new runs. Accuracy model constants are not classification configuration.

## Quick-scan evidence map

New runs retain the existing quick-scan node budgets: Fast 50,000, Standard
100,000 and Thorough 250,000 nodes per position, with MultiPV=1. The scan covers
both players, the root and the final position before any automatic deepening.
Saved evidence becomes readable progressively while the scan runs.

**Positions saved** measures durable processing progress. **Original quick-scan coverage**
counts moves separately for each side: usable provisional evaluations, retained
gaps and moves awaiting adjacent evidence. Neither saved coverage nor a usable
quick estimate establishes stability. An engine mate signal remains nonnumeric.
The evidence inspector explains the original scan's gaps and refinement reasons.
These scan facts remain tied to the original quick attempts even after stronger
results update the displayed label, accuracy or stability.

Alongside its large-loss, threshold, mate and conflicting-search targets,
policy 2 introduced valid completed attempts with insufficient measured strength,
missing evaluations, bounds or incompatible WDL/centipawn conversions. Under policy 2,
shared before/after positions enter one chronological queue. Each receives at most the
existing deepening and verification cycle; verification is a fresh search.
Unresolved results remain provisional or unclassified when processing finishes.
Damaged records, unsupported policies and incompatible evidence identities are
errors, not permission to overwrite evidence or retry indefinitely.

The pipeline policy is captured in each run's checksummed settings. Existing
policy-1 and policy-2 runs retain their original targeting and explicit-resume behavior;
opening a saved review does not upgrade it. Quick-scan facts can be inspected
without an engine or the disposable cache. Start a new run to use the current policy.

Original scan gaps are historical facts, not a count of currently unresolved moves.
Use the current evaluated/stable coverage and per-move stability details to inspect
stronger results. If more evidence is required, this does not promise automatic
work after cancellation, interruption or application exit; resume remains explicit.

## Adaptive candidate selection (pipeline policies 3 and 4)

New runs select usable quick-scan losses at or above the captured Inaccuracy
threshold, plus losses within the configured margin of Inaccuracy, Mistake and
Blunder boundaries. The Excellent boundary alone no longer selects routine moves.
An immediate reply after either player's qualifying loss is also selected to
investigate the opportunity, even if that reply loses no expected score. Missing
or unusable numeric evidence cannot establish this opportunity signal.

Refinement order is mate signals, conflicting estimates, qualifying losses from
largest down, opportunity replies ordered by the preceding loss, repairable gaps,
then threshold-only ambiguity. The strongest applicable reason determines priority;
remaining ties use game order. Each move expands to its before/after positions,
with shared positions analyzed once per pass at their earliest scheduled occurrence.
Every selected target is processed; there is no candidate cap. All deepening searches
finish before the separate fresh-verification pass uses the same order.

The existing inspector explains original selection reasons and preceding-move
context. An opportunity signal does not establish a verified missed chance or a
special label. Stronger results do not change the original selection queue.
Policies 1, 2 and 3 retain their historical rules and ordering when resumed. Budgets,
score conversion, classification and stability requirements are unchanged.

Policy 4 introduced selection that compares the canonical expected-score loss directly
to the captured Inaccuracy cutoff. This corrects rounding at extremely close custom
cutoffs without changing the score-conversion or classification model. Saved policy-3
runs retain their rounded cutoff decisions; start a new review to use the corrected
policy. The evidence inspector identifies the captured selection policy, and labels
policy-3 cutoff reasons as historical rounded comparisons. Reply selection is
described as investigating the preceding loss, independently of the final label.

## Bounded deeper searches (pipeline policy 5)

New reviews keep the same quick scan, candidate selection, ranked deepening and
fresh verification. Selected moves that remain unresolved can receive two further
rounds. Each round searches the before/after positions freshly, using twice then
four times the captured verification node budget, with the same MultiPV constraints.
Positions shared by targets are searched once per round. All targets in one round
finish before the next round is planned in the original risk order.

A complete newer pair is compared with the preceding complete pair using the same
independent-attempt, measured-strength, label/best-move agreement and score-drift
rules. A larger requested budget does not itself prove stronger evidence. Confirmed
moves and supported persistent mate signals need no further automatic searches.
Persistent mate signals remain separate from numeric labels and accuracy.

A shared-position search can reopen a neighbor's uncertainty. Until a newer complete
pair is available, its last usable label stays visible as provisional. The inspector
shows the investigation reason, compared stages, label/best-move changes, expected
scores, and supporting evidence IDs. It also distinguishes confirmation from exhausted
rounds or the node-budget ceiling. Finishing a review still does not guarantee stability.

The ceiling is one trillion requested nodes per search; repeated capped budgets are
omitted. There is no third round. Difficult reviews can cost more than policy 4;
already confirmed targets receive no extra searches. Cancellation retains acknowledged
attempts, and explicit resume reconstructs the same round queues and skips saved work.
Saved policies 1–4 retain their historical behavior. Reopening never upgrades a review.

A best-move comparison is shown only when both compared passes provide that
witness. Missing witnesses are reported as **not comparable**, rather than an
engine changing or retaining its choice. Cross-engine or different-position
witnesses cannot establish a stable result.

## Alternative move comparison (pipeline policy 6)

Guided Review compares the played move with the saved engine candidates. Rows show
expected-score loss relative to the best supported move, whether the move is
within the captured Excellent threshold, and independent comparison confidence.
These are expected-score **percentage points**, not the probability of winning a
particular game. Centipawn-based conversions are identified as approximate.

Comparison confirmation is separate from classification stability. Two independent,
measurably stronger passes must support the latest candidates by move identity,
including promotion. Candidate expected scores and gaps must agree within the
captured maximum drift, and equivalent/weaker assessments must agree. Equivalent
moves can exchange ranks without invalidating comparison confirmation. Classification
still follows its existing label and best-move agreement rules.

The played move appears first. If it is absent from root candidates, its saved
after-move evaluation supplies its comparison and its reply continuation. This
source is identified. Contradictory root/child values withhold the played move's
numerical comparison. Show Line previews each saved continuation without editing
the game; its evaluation belongs to the search root, not every replayed position.
Try Again hides all comparison solutions. Analyze Position and Back to Review
preserve the selected continuation and step.

Policy 6 retains the original quick-scan targets, ordering, captured MultiPV width
and budgets. After verification, unresolved comparisons for those targets may use
the same two bounded escalation rounds as unresolved classifications. It adds no
separate comparison pass or arbitrary-move searches. Shared positions remain
searched once per round. A changing candidate set can remain unresolved at the
round or node ceiling; the UI reports uncertainty instead of claiming confirmation.
Supported persistent mate signals retain their existing stopping behavior.

Width-one searches do not assess alternative equivalence and do not cause extra
comparison work. A sole legal move is identified explicitly. Even confirmed
comparisons cover only the captured candidates, not every legal move.

Historical reviews gain comparisons where their retained evidence supports them.
Policies 1–5 preserve their original scheduling and resume decisions; opening an
old review does not upgrade it. Comparison facts are derived from owned review
evidence and remain available without the engine or disposable analysis cache.

## Comparison confidence hardening (pipeline policy 7)

New reviews capture policy 7. Comparisons use exact native-WDL arithmetic when
checking a root candidate against a locally proven terminal child, including the
configured drift boundary. If root and child evidence disagree about whether the
played move is equivalent to best, its numerical comparison is withheld even
when the difference falls within the drift allowance. Classification confidence
remains separate.

Older policy-6 runs keep their original automatic work plans, including resume
and completion decisions. Their displayed comparisons use the corrected
interpretation. If that exposes unresolved quality that the saved policy does
not investigate, the UI explains that a new review can collect current-policy
evidence. Opening a saved run does not silently schedule extra work.

The comparison table follows the displayed continuation when switching lines or
returning from Analysis. Played-move replay explicitly identifies its saved
after-move evaluation; it does not assign that evaluation to the pre-move board.

Review sizing follows the active workspace's minimum size. Hidden workspaces do
not push board navigation below the laptop viewport; the outer shell still
scrolls when the active content or enlarged fonts require it.
