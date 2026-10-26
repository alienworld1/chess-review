# Analyze board

Open **Analyze** to explore standard chess from the initial position. The board
works without an engine. Moves, branches, and annotations last only until the app
closes; leaving Analyze for another page keeps the session. New Analysis and all
normal quit routes ask before discarding content. Cancel preserves the session.
Imported games can also be opened here and resume from their saved snapshots;
see [Imported games](#imported-games).

## Moves and history

Drag a piece to move, or choose **Click-click** in Move input. Illegal moves return
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
annotations and retains up to 64 annotation undo steps. Reset clears all nodes
and their annotation histories.

## Preferences and artwork

Input mode, coordinates, move animation, four annotation colors, manual board
orientation, and the analysis split are saved on orderly exit in the existing UI
preference file. Orientation stays fixed while navigating or starting New Analysis;
it defaults to White at bottom when no saved preference exists. Invalid board
preferences use the documented defaults with a nonmodal notice. No chess records or annotations enter the UI preference file.

Pieces are the twelve transparent Cburnett SVGs from Wikimedia Commons, bundled
under their per-file BSD license option. Sources, description revisions, file
timestamps, SHA-256 checksums, and license notices are in
[`assets/pieces/cburnett/`](../assets/pieces/cburnett/). Help → Chess Piece Artwork
License displays the bundled notices and provenance. The app never downloads art
at runtime.

Native screen-reader behavior and mixed-monitor rendering require platform QA;
the automated suite verifies the accessible square interfaces and actions,
including selected pieces, legal destinations, captures, check, and mate. Hidden
or disabled boards cannot be activated through square actions.

## Imported games

In Games, select a row and choose **Open in Analyze**, double-click the row, or
press Enter. Loading and chess-tree preparation run off the GUI thread. The board
shows the players and game result; the timeline and PGN tree expose every stored
move and variation. Comments and numeric annotation glyphs (`$n`) for the selected
move appear in a read-only plain-text pane and in move descriptions. Notes above
4,096 characters are marked and clipped for responsive display; complete imported
text remains in the saved snapshot. Comments before the first move appear when
that move is selected. Game result metadata
and current-board check/mate/stalemate status are displayed separately.

Navigation does not change the saved snapshot or require an engine. Playing an
existing continuation reuses it; new branches and visual annotations are temporary.
Reset, replacement, and quit ask before discarding temporary content. The saved
original stays available in Games. Saving edited analysis is a later feature.

After orderly exit, the last opened saved game and selected stored node resume
from SQLite, even if its source PGN has been moved or deleted. A temporary branch
resumes at the saved game's starting position. New Analysis clears the saved-game
resume pointer on the next orderly exit. Damaged or missing games show a load
error and leave the previous workspace intact; select another game to continue.


## Live engine evidence

Connect a local engine in Settings, then use Start in Analyze to follow the current
board with continuous analysis. Stop, leaving Analyze, and New Analysis stop the
search. The panel shows a White-perspective evaluation, adjacent bar, one numbered
read-only SAN line, its depth, and textual engine status. Stopped/failed evidence
remains visible for the same position; navigation clears it. See
[Local UCI engines](engines.md#live-analysis-panel) for score semantics and limits.
