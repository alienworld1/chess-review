# Review a game

On Home, choose **Review a game** to open a PGN file, or **Paste PGN** to paste
one. A single game opens its review setup. If you import several games, choose
one from the list. **Recent games** shows up to five games by import or new
review activity. Select one and press Enter or **Open selected game** to open its
review or setup. **Open Games** takes you to the full library.

Home also shows the default engine and its connection state. Use **Engine
Settings** to add an engine or check a connection failure. You can import games,
open saved reviews, and access saved puzzles while the engine is disconnected.
**Open Puzzles** opens the full puzzle library.

Choose the side you played, then **Analyze game**. The app uses your default
engine and configured review quality. Open **Engine and review options** to
change them or turn Book classifications on or off for this run. If no engine
is available, use **Set up engine…** to add one installed on your computer,
such as Stockfish. Engines are never downloaded automatically.

All three presets scan the full game. **Fast** checks up to two ranked moves
more closely, and **Standard** checks up to four before finishing automatically.
**Thorough** can spend additional time checking more moves and resolving
uncertainty. Use **Analyze Position** to explore a specific move in more depth.
Review completion and optional puzzle generation have separate progress.

## Your game at a glance

When the played mainline matches the bundled opening dataset, the game header
and summary show the opening name, variation, and ECO code. The header is
available before engine analysis. The review summary shows where the local
book ends. Moves in the verified book prefix receive **Book** once the review has
usable analysis for the move. The first move outside the book receives the usual
engine-based assessment and a departure note. An error at that boundary can
appear among the guided moments.

Book lookup runs offline and follows the played mainline. The default policy
requires each move from its current position to appear in at least two distinct
bundled reference lines, up to 24 half-moves. Shared positions reached by
transposition can use the same continuations. The book ends at the first
unsupported move or when the depth limit is reached. You can tune these cutoffs
in the `[openings]` section of the [example configuration](config.example.toml).
A saved review keeps its opening, book policy, and boundary. Book labels leave
the engine-based accuracy calculation independent.

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
  It is available for your error moves—including preliminary results graded against
  the move's best saved analysis stage and verified missed opportunities—and for both
  sides when reviewing both sides.
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
- **Show Line** opens an analyzed continuation in the coach panel with a next-move
  board arrow, clickable move list, and inline playback controls (**Play Line** /
  **Pause Line** / **Replay Line**). Use the inline buttons, clickable moves,
  board navigation controls, or Left/Right/Home/End keys to step through it, then
  **Return to Game**.
- **Analyze Position** opens the selected position or continuation step with its
  move history and any matching saved evaluation. Review can keep refining the
  game while you explore. Use **Back to Review** or **Alt+Left** to restore the
  same board orientation, step, expanded details, scroll position, and keyboard
  focus. Open **Analyze Position** again from that spot to resume your variations
  and any live search you started there. If you open engine settings from this
  workspace, **Back to Analysis** brings you back to the same analysis session.
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

## Export an annotated PGN

Open the review run you want to share and select **Export PGN…** in the Review
header. Choose where to save the file. You can export a completed review or an
unfinished run with at least two saved positions. An unfinished export includes
the full game and marks its review coverage in the opening comment.

The file includes the recorded result, existing variations and annotations,
review labels, and short suggested lines at selected moves. Evaluations use
White’s perspective, so a positive value favors White. The export labels
preliminary classifications and keeps your original move-quality glyphs.
Opening tags from the game take precedence over recognized opening information.

Open the PGN in another chess application or import it through Games. Exporting
keeps the saved game and review intact. To include working variations you added
in Analyze, use **Save PGN** there.

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
