# Review a game

On Home, choose **Review a game** to open a PGN file, or **Paste PGN** to paste
one. A single game opens its review setup. If you import several games, choose
one from the list. Your saved games remain available in **Games**.

Choose the side you played, then **Analyze game**. The app uses your default
engine and configured review quality. Open **Engine and review quality** to
change them. If no engine is available, use **Set up engine…** to add one installed
on your computer, such as Stockfish. Engines are never downloaded automatically.

## Your game at a glance

When the played mainline matches the bundled opening dataset, the game header
and summary show the opening name, variation, and ECO code. The header is
available before engine analysis. A saved review keeps the opening recorded
when it was created.

The summary shows your accuracy, analyzed-move coverage, move classifications,
and number of key moments. Choose **Start review** to walk through those moments.
You can begin while analysis continues. Unavailable accuracy is shown as a dash;
partial coverage and preliminary assessments are indicated explicitly.

The app only shows explanations and results supported by its analysis. A result
found in an analyzed continuation does not establish that every defence leads to
that outcome. See [Review methodology](review-summaries.md) for details.

## Guided review

The board shows the position after the selected move, with your side at the
bottom. Each lesson separates the move, classification, short explanation, and
stronger alternative when available.

- **Continue** advances through important moments in chronological order.
  **Finish review** returns to the summary.
- **Try Again** restores the position before your decision and hides the solution.
  It is available for your stable errors, including verified missed opportunities.
  Play any legal candidate: **Best** and objectively equivalent **Strong** moves
  receive credit, followed by a brief success state and a return to the lesson.
  **Okay** or **Try Again** gives feedback and resets the decision position.
  Uncertain analysis stays ungraded rather than marking your move wrong.
- **Hint** reveals a chess idea when supported, then the piece, then the destination
  and promotion choice. Hints remain visible across attempts. When no reliable
  tactical idea is available, the first hint offers a general thinking prompt.
- **Show Solution** ends practice and opens the supported best continuation at the
  decision position. Step through it with **Next**/**Previous**; it does not autoplay.
  **Return to Game** restores the lesson. Cancelling practice or completing it also
  restores your expanded details, reading position, and keyboard focus.
  **Escape** dismisses an active chooser, gesture, selection, or keyboard entry mode
  first. Press it again to cancel practice or return from a line preview.
- **Show Line** replays an analyzed continuation. Use the board navigation
  controls to step through it, then **Return to Game**.
- **Analyze Position** opens free analysis. **Back to Review** returns to the
  same move or continuation without losing your place.
- **More about this move** reveals expected-score impact and alternative moves.

Use the numbered White/Black timeline or **Start**, **Previous**, **Next**, and
**End** to explore every move, including moves that have not been analyzed.
Left/Right and Home/End also navigate. Hovering previews a move's board;
clicking selects it. Leaving restores the selected position.

Timeline symbols mark move classifications; a diamond marks a key moment.
Select or hover a move for its full description. Color is additional guidance,
not the only way to identify a classification.

The slim chess evaluation bar shows **White's expected score**, including draws.
It is not a win probability or pawn evaluation. Hover for its value and whether
it is approximate. Missing or unsupported evaluations leave the bar unavailable.

Retries use saved, comparable analysis first. An unknown candidate may need bounded
analysis and independent verification with the original review engine and settings.
If that engine is unavailable or the evidence remains inconclusive, you can try a
different move, reveal the solution, or cancel. Practice does not change your saved
game, classifications, accuracy, or review progress. Hints and attempts are not saved
between sessions.

## Settings, history, and unfinished reviews

**Review options** contains review settings, the chronological key-moment list,
and saved review runs. Select a saved run to inspect it without starting an engine.
**Resume selected run** continues an unfinished review using its original settings
and compatible engine. Work never resumes automatically after restarting.

Leaving an active review offers background continuation, staying, or cancelling.
**Cancel review** keeps completed, saved analysis. Home offers recent games,
resume, and unfinished reviews when available. See [Reopen and recovery](reopen-recovery.md).

## Technical inspection

Optional verbose mode reveals the technical inspector and engine protocol details:

```toml
[ui]
verbose = true
```

Add this to `config.toml` in the application's configuration directory, then
restart. It defaults to `false` and changes presentation only: analysis settings,
results, saved review identities, and cache reuse are unaffected.

In verbose mode, **Review options → Technical inspector** provides labeled review
and move fields, saved evidence, and search provenance. Useful chess details and
Analysis remain available in normal mode. **Help → Diagnostics** remains available
for troubleshooting in either mode. See [Configuration](review-evidence.md).
