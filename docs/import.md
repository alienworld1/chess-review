# Importing games

[Back to README](../README.md)

Use **File → Import PGN…** (`Ctrl+O`) or the Home/Games import button to select
one or more files. You can also drop PGN files on Games or the Home import
button. Drops copy their content into the library; they never move source files.
Files must have a `.pgn` extension (any letter case) and be regular local files.
Links to regular files are supported. Folders, special files, and remote URLs
are rejected. File URLs must be absolute and have no hostname, query, fragment,
or invalid UTF-8 path bytes; remote-share/UNC URLs are rejected before filesystem
inspection. Public game URL import is not implemented yet.

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

## Progress and results

Submitting an import opens Games. You can navigate elsewhere and return while
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

The results table lists each source and its per-game outcomes. Select a row for
copyable diagnostic details, including source byte ranges, line/byte-column
locations, warnings, and suppression counts. The latest batch's report remains
available while navigating, until another batch is submitted or the app exits.
Reports are not saved to the database. Progress updates preserve the selected
diagnostic text and the results viewport. Completion, completion with errors,
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

Games shows a newest-first table with 50 rows per page, Previous/Next, and
Refresh. Import completion returns the table to its newest page. Damaged metadata
rows remain identifiable, and truncated fields are marked. Search, selectable
sorting, previews, and opening saved games on the board are not implemented yet.

Imported snapshots are independent of their source files. Local-file provenance
stores the full absolute file URI; pasted games store only the source kind.
These paths, player names, comments, and PGN content are not written to logs.
The source file is never reread to reopen a snapshot through the repository.
