# Importing games

[Back to README](../README.md)

Use **File → Import PGN…** (`Ctrl+O`) or the Home/Games import button to select
one or more files. You can also drop PGN files on Games or the Home import
button. Drops copy their content into the library; they never move source files.
Files must have a `.pgn` extension (any letter case) and be regular local files.
Links to regular files are supported. Folders, special files, and remote URLs
are rejected. File URLs must be absolute and have no hostname, query, fragment,
or invalid UTF-8 path bytes; remote-share/UNC URLs are rejected before filesystem
inspection.

**File → Paste PGN…** (`Ctrl+Shift+V`, outside text editors) opens an editable
plain-text dialog containing the current clipboard text. Inputs above 1 MiB of
UTF-8 use a read-only preview of at most 4,096 characters; **Import full text**
explicitly imports all supplied text, including the undisplayed portion. Smaller
inputs remain editable. Use a UTF-8 PGN file to edit large inputs externally.
Nothing is imported
until you press **Import**. Normal `Ctrl+V` continues to paste into text editors.
Clipboard text is read only when you invoke Paste PGN. The unchanged clipboard
text is passed to the parser exactly as supplied by Qt. After editing, document
paragraph breaks use LF; no-break spaces and Unicode line separators within
blocks remain intact. File import preserves the original UTF-8 bytes. The editor
rejects oversized/invalid-Unicode clipboard insertions before inserting them;
the worker still enforces the 64 MiB UTF-8 import limit.

Earlier builds could change no-break spaces and Unicode separators during paste,
causing incorrect duplicate matches or rejecting otherwise valid PGN. Reimport
the original UTF-8 source if affected; discarded clipboard content cannot be
recovered or inferred from existing snapshots.

PGNs must use UTF-8, optionally with a leading BOM. Other encodings must be
converted explicitly before import; the app does not guess or replace invalid
characters. See [parser compatibility](pgn.md) for notation rules.

## Import a game link

Choose **Import game link…** on Home, Games, or in the **File** menu. Paste the
public link to a completed game and choose **Import game**. If your clipboard
already has a supported link, the dialog fills it in. The provider is contacted
only after you confirm the import.

- **Chess.com** supports public Live and Daily game links, including their
  analysis-page links. The app asks Chess.com for the game's details and picks
  the matching PGN from the player's public monthly archive. A recently finished
  game may take time to appear there. [Chess.com Published-Data API](https://www.chess.com/news/view/published-data-api)
- **Lichess** supports public links with an eight-character game ID, including
  links that open from White's or Black's side or at a particular move.
  [Lichess game export API](https://lichess.org/api#tag/Games/operation/gamePgn)

If the link fails or the game requires sign-in, use the provider's **Share → PGN**
option and import the downloaded file or use **Paste PGN…**. The Games page
shows delivery and PGN errors directly and keeps the details in **Import report**.
You can cancel a download from Games and continue using the app while it runs.
A successful import opens review setup when you're still in Games.

## Progress and results

Submitting an import opens Games to show progress. A completed single-game import
continues to review setup; multiple imported games offer a game chooser. You can navigate elsewhere and return while
it runs. Only one batch runs at a time; new import controls are disabled until it
finishes. The batch is limited to **32 files, 64 MiB read in total, 10,000 attempted
games, and 1,000 retained warnings**. Each game's existing parser/tree limits
still apply, including an 8 MiB game limit. Additional warnings are counted and
marked as suppressed.

Files run sequentially. An unreadable or oversized file is reported and other
files are attempted if they fit the remaining budget. Safely recoverable PGN
errors skip that game; an unrecoverable tail stops that file. A storage failure
stops the entire batch. Reaching the game limit leaves subsequent files visibly
unprocessed. Empty input reports **No games found**.

Choose **Import report** to open the results table. It lists each source and its
per-game outcomes. Select a saved game from the report to open it in Analyze or
review it. Select any row for copyable errors and warnings. Verbose diagnostics
also show source byte ranges and exact line and byte-column locations. The latest batch's report remains
available while navigating, until another batch is submitted or the app exits.
Reports are not saved to the database. Completion, completion with errors,
cancellation, game-limit stops, and terminal failures have distinct feedback;
unprocessed files identify why they were skipped.

**Cancel import** stops future reading/parsing and further saves. An already
submitted save finishes and its result remains in the report. Previously saved
games remain available after cancellation, failure, or orderly application exit.
There is no batch-wide rollback.

Duplicates keep the original saved snapshot, annotations, provenance, and import
time. Reimporting does not merge or replace a record. Imports never start engine
analysis automatically.

## Saved games and privacy

Games shows 50 rows per page. Search for either player with **Ctrl+F** or the
search field. Multiple words can match across both player names. **Filters**
offers result and saved-review filters and lets you sort by import order, played
date, or player name. Search and filters apply to the whole library and stay in
place when you leave Games. **Reset** clears them and restores newest-first
ordering. A completed import returns to the first page of the current view.

**Library Statistics…** shows totals for the whole library, including saved
games, games with at least one completed review, completed review runs, and
saved puzzles. Use **Refresh** to read the current totals again.

Select a row for the final mainline position and copyable game details, including
event, site, round, time control, source, and review accuracy when available.
Use **Details** to show the preview on a narrow window. Long metadata fields may
be shortened in the preview.

The **Opening** column shows the local opening name, variation, and ECO code
when the played mainline matches the bundled Lichess dataset. Common move-order
transpositions are supported. A blank cell means a lookup is pending or there
is no reliable match. Older games are looked up as you browse their pages.
Original PGN opening tags are preserved.

Imported snapshots are independent of their source files. Local-file provenance
stores the full absolute file URI; pasted games store only the source kind.
Link imports store the provider and the canonical public game URL. Tracking
parameters and move fragments are discarded. Requests use HTTPS without a
provider account, and the app saves only the requested game from a Chess.com
archive response. Logs exclude paths, game URLs, player names, comments, and
PGN content.
The source file is never reread to reopen a snapshot through the repository.

## Opening saved games

Select a saved row and choose **Open in Analyze**, double-click, or press Enter.
The complete validated snapshot opens with all moves and variations. The players,
result, and selected move's comments and NAGs remain visible. Loading errors are
shown without replacing the current workspace. No engine is required or started.
See [Analyze board](analysis-board.md#imported-games) for exploration and recovery.

## Deleting saved games

Select a row and choose **Delete game…**. Confirm to permanently remove the local
snapshot, opening lookup, saved reviews, and review recovery state. Your original
PGN file and already saved puzzles are kept. Finish or stop an active review or
puzzle generation for that game first. If the game is open in Analyze, export any
edits you want to keep and open another game or start a new board before deleting.
