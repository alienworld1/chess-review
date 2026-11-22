# Analyze board

Open **Analyze** to explore standard chess from the initial position. The board
works without an engine. **Save PGN** writes a standalone copy of the current
tree and annotations. Unsaved edits stay in the session when you switch pages.
New Analysis, replacing the session, and quitting offer to save or discard
unsaved work.

Use **Board Options → Set Position from FEN…** to start from a custom position.
The dialog starts with the current board's FEN. Edit it or paste a complete
six-field FEN. Invalid input leaves the current session intact and the dialog
open for correction. A custom position can be kept with **Save PGN**.

## Moves and history

Drag a piece to move, or choose **Click-click** in **Board Options → Move input**. Illegal moves return
the piece without changing history. A dragged piece stays at the drop square during
validation and settles immediately when accepted; click/keyboard moves use the
optional animation. Promotion offers queen, rook, bishop, and
knight; Escape cancels the chooser. White/Black labels, coordinates, last move,
selection, legal destinations, capture shapes, and check highlight aid orientation.
Flip Board is also in View (`Ctrl+F` while Analyze is active).

Check is announced in the position status. Checkmate displays the winner and a
`#` badge beside the mated king; stalemate displays a draw without a mate badge.
Terminal positions remain available for history navigation and annotations.
Board Options → Board Gestures and Keyboard Help opens a scrollable reference
with separate sections for moves, history, annotations, and color modifiers.

Beginning, Previous, Next, and End navigate the session. The timeline beneath the
board shows the entire chosen line in numbered White/Black cells, including moves
after the current position. Starting position is selectable, including for FEN
roots. Click any move in the timeline or PGN variation pane to navigate; hovering
shows details without changing the board. The current move is filled; a pending
navigation target has a distinct outline/underline.

The PGN pane displays bold mainline moves and indented, collapsible variations.
Long lines wrap in bounded segments. Disclosure controls and plus/minus expand or
collapse variations. Up/Down browse lines; Ctrl+Left/Right focus individual moves
within a line and Enter selects. An outline marks keyboard focus independently
of the current-position fill. Wrapped lines can scroll within a short pane to
keep the focused move visible. Left/Right and Home/End navigate chess history
when the board, navigation controls, timeline, or PGN pane has focus. Ordinary
editors retain their keys. Wheel input scrolls the hovered view.

A different move from an older position adds a variation without deleting the
existing continuation. Forward navigation follows the continuation last chosen
at each fork; selecting a node also selects its complete path through earlier
forks. Rapid history commands coalesce to the latest requested position, including
repeated steps. Move entry and annotation editing wait until that position is
displayed. Single-move history steps animate when Animate Moves is enabled,
including backward steps. Jumps across multiple moves remain instant; rapid
commands cancel any active animation and resolve the latest requested position.

The board and timeline sit left of a resizable PGN pane at wider widths. At narrow
widths the PGN pane stacks below them; the shell retains its scrolling fallback.
The split preference survives orderly exit. Neither view changes chess data merely
by focusing, hovering, scrolling, or expanding a branch.

On the board, Enter enables square navigation:
arrows move focus in the displayed orientation and Enter or Space selects a piece
or attempts its move. Escape cancels a gesture, then selection, then square entry.
Tab reaches ordinary controls; shortcuts do not take arrows from their editors.

## Edit and save variations

Play a legal move from any position to extend a line. Select a move and open
**Variations**, or right-click it in the PGN pane, to promote it to the main
continuation at that fork or delete it and its descendants. Deleting the line
you are viewing returns the board to the position before it.

**Edit Note** opens the position's comments and numeric PGN glyphs. For a move,
you can edit the text before and after it separately. A short note appears
beside its move in the tree, and the selected position shows a longer excerpt
below the tree. The editor opens the full text.

Use **Save PGN** to keep an edited copy. If the played line has changed, the
copy is marked unfinished (`*`). Import the copy from Games to continue editing
it later. An analysis copy with different annotations or variations can
coexist with the original game in your library.

## Annotations

Right-click a square to toggle its highlight; right-drag between squares to toggle
an arrow. The toolbar selects the default color using numbered slots and current-color
swatches. Shift uses color 2 (red by
default), Ctrl color 3 (yellow), and Ctrl+Shift color 4 (blue). Color 1 is green.
Board Options customizes the selected color and provides help, clear-current,
annotation undo, and redo. Undo/redo apply to annotations, not moves.

In keyboard square navigation, H toggles a highlight. A marks an arrow's start;
navigate to its endpoint and press A again. The same color modifiers apply.
Annotations belong to tree nodes and return when that node is revisited; identical
positions in separate branches do not share them. Each node allows 256 active
annotations and retains up to 64 drawing undo steps. PGN copies include arrows
and square highlights as `%cal` and `%csl` drawing directives. Chess Review
restores them when the copy is reopened. The saved PGN carries the four color
slots, which follow the drawing preferences of the viewer.

## Preferences and artwork

Input mode, coordinates, move animation, four annotation colors, manual board
orientation, both analysis splitters, and the engine line count are saved on orderly exit in the existing UI
preference file. Orientation stays fixed while navigating or starting New Analysis;
it defaults to White at bottom when no saved preference exists. Invalid board
preferences use the documented defaults with a nonmodal notice. No chess records or annotations enter the UI preference file.

Pieces are the twelve transparent Cburnett SVGs from Wikimedia Commons, bundled
under their per-file BSD license option. Sources, description revisions, file
timestamps, SHA-256 checksums, and license notices are in
[`assets/pieces/cburnett/`](../assets/pieces/cburnett/). Help → Chess Piece Artwork
License displays the bundled notices and provenance. The app never downloads art
at runtime.

## Imported games

In Games, select a row and choose **Open in Analyze**, double-click the row, or
press Enter. The board shows the players and game result; the timeline and PGN
tree expose every stored move and variation. Comments and numeric annotation glyphs (`$n`) for the selected
move appear below the tree and in move descriptions. The excerpt is limited to
4,096 characters for responsive display. Game result metadata
and current-board check/mate/stalemate status are displayed separately.

Playing an existing continuation reuses that move. The imported snapshot and its
associated Review remain available in Games. Edits are made in the Analyze
session and saved through **Save PGN**.

After orderly exit, the last opened saved game and selected stored node resume
from SQLite, even if its source PGN has been moved or deleted. A temporary branch
resumes at the saved game's starting position. New Analysis clears the saved-game
resume pointer on the next orderly exit. Damaged or missing games show a load
error and leave the previous workspace intact; select another game to continue.


## Engine analysis

Connect a local engine in Settings. **Start** follows the current board with
continuous analysis. **Lines** chooses how many candidates to request, starting
at three when the engine and position allow. Select a candidate to preview its
first move, then use Previous and Next to explore the line. **Add Line** inserts
the full legal continuation at the original position and opens its end.
**Exit Preview** or Escape returns to the editable board.

**Pause** releases the engine and stays paused through board navigation.
**Resume** starts following from the position you are viewing. **Stop** ends
the session. **Deepen** runs a finite search on the current position and shows
its target depth. See [Local UCI engines](engines.md#live-analysis-panel) for
score semantics, evidence retention, and the depth policy.
