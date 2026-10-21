# Analyze board

Open **Analyze** to explore standard chess from the initial position. The board
works without an engine. Moves, branches, and annotations last only until the app
closes; leaving Analyze for another page keeps the session. New Analysis and all
normal quit routes ask before discarding content. Cancel preserves the session.
There is no save or recovery for this workspace yet.

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

Beginning, Previous, Next, and End navigate the session. History selects an
ancestor; Continue with selects an existing branch. A different move from an
older position adds a variation without deleting the existing continuation.
History and continuation labels include move numbers and side-to-move punctuation.
Forward navigation follows the continuation last chosen at each fork; selecting a
node also selects its complete path through earlier forks.

When the board or history buttons have focus, Left/Right navigate history and
Home/End go to its beginning/end. On the board, Enter enables square navigation:
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

Input mode, coordinates, move animation, and four annotation colors are saved on
orderly exit in the existing UI preference file. Orientation belongs to the live
workspace. Invalid board preferences use the documented defaults with a nonmodal
notice. No chess records or annotations enter the UI preference file.

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
