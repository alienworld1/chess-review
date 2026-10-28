# Refinement, stability and summaries

Run Review first scans the entire original mainline, including its final position.
The results become readable while targeted refinement continues automatically.
Cancel retains completed work; Resume explicitly continues the original run with
its captured configuration and compatible engine. A new run uses current settings.

Refinement targets both players' major errors, expected-score losses within one
percentage point of a classification boundary, mate signals and contradictory
comparisons. Each target's adjacent positions receive the quality preset's
deepening and verification budgets. Shared positions are analyzed once per pass.
MultiPV defaults to three, constrained by the engine and legal move count. Width
one remains usable but cannot establish alternative equivalence.

A stable ordinary classification requires sufficient evidence from two distinct
stronger attempts, agreement on label and best move, and no more than two
percentage points of change in either expected score. Verification must have more
measured nodes and no shallower common depth. Reusing one attempt never provides
two confirmations. Equal custom budgets do not guarantee a stable result.
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
