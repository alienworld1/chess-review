# Settings

Choose **System**, **Light**, or **Dark** under **Appearance**. Changes take
effect immediately. The app remembers the choice when you close it normally.

## Review preferences

Choose the quality to preselect when starting a review. You can change it in
the review setup for a particular game.

| Quality | Analysis |
| --- | --- |
| Fast | Scans the full game and looks deeper at up to two key moves. |
| Standard | Scans the full game and looks deeper at up to four key moves. |
| Thorough | Performs deeper searches and broader verification. |

Review time depends on the game, the engine, and the computer. You can tune the
search budgets of each preset in Advanced. New reviews capture their configuration
when they start, so later edits do not change the meaning of saved results.

Enable **Include key moments from both players** to guide new reviews through
important decisions from either side. The summary displays accuracy and move
counts separately by color. The selected player still controls puzzle generation.

Under **Move classifications**, change the expected-score loss at which
Inaccuracy, Mistake, and Blunder begin. Enter increasing values between 0 and 1.
**Use standard thresholds** restores 0.05, 0.10, and 0.20. These cutoffs affect
move classifications. Accuracy continues to use its own calculation.

The same page controls candidate lines for review, local opening references,
win/draw/loss preference, automatic puzzle generation, technical details, and
the starting line count in Analyze. The live line count can also be changed
during a session. With **Use last choice**, Analyze remembers that selection.

Choose **Save preferences** to apply edits, or **Discard changes** to restore the
last loaded values. Saving basic preferences may reformat the TOML file.
For per-engine CPU threads and hash memory, see [Local UCI engines](engines.md).

## Advanced TOML

**Settings → Advanced** shows the location and contents of your local
`config.toml`. A missing or empty file uses built-in defaults.
**Configuration reference** shows the [annotated example](config.example.toml),
including search budgets, classification thresholds, and puzzle and opening rules.

- **Validate TOML** checks the draft and reports any errors with the field and
  source location where available. It does not write or apply the draft.
- **Save TOML** validates and saves valid text, including comments. A failed
  save leaves the draft available for correction.
- **Reload from disk** picks up changes made in another editor. A draft is
  discarded only when you explicitly choose the discard-and-reload action.
  If the file changed on disk before you saved, copy any changes you want to
  keep before reloading and merging them.

You can repair invalid TOML from the Advanced editor. New review computation
requires valid configuration. Saved reviews and manual analysis remain available.
