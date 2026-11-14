# Local UCI engines

## Add an engine

Open **Settings → Engines**, or **Engine Settings…** from Analyze. Choose a local
executable, check its resolved path, and select **Validate and Add**. The app runs
it with your normal user permissions, so choose software you trust. It does not
download or sandbox engines.

The file must be a readable, executable regular file. Validation checks its UCI
handshake and detects observed changes to the file; it does not test the quality
of its analysis. Failed or cancelled validation does not create a profile.
The first engine becomes the default. Adding the same binary again returns its
existing profile; changing a registered file is rejected. Profile replacement,
removal, and option editing are not yet available.

## Connect an engine

Select a saved engine and choose **Connect**. The executable and its reported
identity and options must still match the registered profile. **Ready** means the
handshake succeeded; no search starts until you request one. Settings shows the
engine's advertised options, but does not change them.

The connection stays open across pages until you disconnect, the engine fails, or
the app exits. Only one engine process can be active at a time; disconnect it
before validating or connecting another. Connections are not restored automatically
after restart. If an engine fails or times out, reconnect explicitly.

Linux is verified; Windows release verification remains pending. For protocol
limits, search APIs, scheduling, and cache rules, see
[Engine infrastructure](engine-internals.md).

## Live Analysis panel

Connect an engine in Settings, return to Analyze, and click **Start**. The panel
analyzes the displayed position continuously with one principal variation. Legal
moves and history navigation retarget the search; annotations, selections, and
board flips do not. Previous work is cancelled before the latest position is
submitted, preserving the position's full original history.

Checkmate and stalemate positions use a depth-one search and finish as Completed,
with no principal variation. This accommodates engines that immediately return a
null bestmove for terminal positions, including Stockfish, and keeps the connection
ready for the next position.

**Stop**, leaving Analyze, or accepting New Analysis stops computation. Returning
to Analyze and reconnecting an engine both require another explicit Start. Manage
Engines opens the existing Settings page; no engine is automatically chosen or
connected. Starting requires a ready connection and a fully published board position.

The numeric score is always labeled **White perspective**: positive pawn scores
favor White, negative scores favor Black, and mate text names the mating side.
Bounds remain explicit and reverse correctly when converting Black-to-move scores.
Bounded mate evaluations use signed M notation (for example, ≤ +M3) instead of
claiming a certain winner; the light/dark bar remains unavailable for these bounds.
The adjacent evaluation bar uses a clamped −10 to +10 pawn visual scale, with mate
at the favored extreme. It is not a win probability. Missing and bounded scores
leave the bar unavailable. Flipping the board moves its light/dark ends without
changing numeric perspective.

The read-only, copyable PV uses numbered SAN; depth belongs to that exact line,
not a newer global statistics update. SAN replay runs off the GUI thread. The
panel distinguishes preparation, active analysis, stopping, disconnected and
failed states. Last usable evidence remains labeled after Stop or failure only
while its position remains displayed; moving to another position or connecting
a new engine session clears it. Errors appear inline and reconnect stays explicit.
An unsolicited bestmove during infinite search remains a protocol error under
the existing engine contract.

Live analysis stays in memory and does not read or write the analysis cache.
