# Local UCI engines

## Add an engine

Open **Settings → Engines**, or **Engine Settings…** from Analyze. Choose a local
executable, check its resolved path, and select **Add engine**. The app runs
it with your normal user permissions, so choose software you trust. It does not
download or sandbox engines.

The file must be a readable, executable regular file. Validation checks its UCI
handshake and detects observed changes to the file; it does not test the quality
of its analysis. Failed or cancelled validation does not create a profile.
The first engine becomes the default. Adding the same binary again returns its
existing profile; changing a registered file is rejected. Profile replacement,
removal, and option editing are not yet available.

Protocol options and full saved paths are available through **Technical inspector**
when `[ui] verbose = true` is set in `config.toml`. Restart after changing it.

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

Connect an engine in Settings, return to Analyze, and click **Start** to
follow the board. **Lines** sets the preferred MultiPV width and starts at
three. The engine and the number of legal moves cap the effective width. Engines
without MultiPV show one candidate. The preference survives a restart.

**Pause** releases the engine without leaving continuous-analysis mode. You
can navigate and explicitly **Resume** at the new position. **Stop**, leaving
Analyze, and New Analysis end the live session. **Deepen** replaces any current
search with a finite search at the current position. Its target starts at depth
18 or four plies beyond the last visible best-line depth, up to depth 64.
Navigation cancels that position's Deepen request. The panel distinguishes
finishing the search from reaching the target with full MultiPV evidence.

Checkmate and stalemate use a depth-one search and finish with no continuation.
Engines… opens Settings. Start and Deepen require an engine connection and a
fully displayed board.

The numeric score is always labeled **White perspective**: positive pawn scores
favor White, negative scores favor Black, and mate text names the mating side.
Bounds remain explicit and reverse correctly when converting Black-to-move scores.
Bounded mate evaluations use signed M notation (for example, ≤ +M3) instead of
claiming a certain winner; the light/dark bar remains unavailable for these bounds.
The adjacent evaluation bar uses a clamped −10 to +10 pawn visual scale, with mate
at the favored extreme. It is not a win probability. Missing and bounded scores
leave the bar unavailable. Flipping the board moves its light/dark ends without
changing numeric perspective.

Candidate lines use numbered SAN and their own depth. Select a row to preview
the line. The evaluation bar represents the analyzed position and is unavailable
during a preview. Engine details shows reported nodes, speed, and elapsed search
time when available.

The workspace distinguishes setup, analyzing, paused, stopped, completed,
disconnected, and failed states. The last usable live evidence stays visible
after Pause, Stop, or failure while its position remains displayed. Navigating
or replacing the engine session clears it. Opening a position from Review may
show labeled saved evidence for the exact position history. Live searches run
separately and keep their results in memory.

An unsolicited bestmove during infinite search remains a protocol error under
the existing engine contract.
