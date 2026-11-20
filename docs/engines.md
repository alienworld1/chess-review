# Local UCI engines

## Add an engine

Open **Settings → Engines**, or **Engine Settings…** from Analyze. Choose a local
executable, check its resolved path, and select **Validate and add**. The app runs
it with your normal user permissions, so choose software you trust. It does not
download or sandbox engines.

The file must be a readable, executable regular file. **Validate and add** checks the
UCI handshake and saves the profile after successful validation. The first
engine becomes the default. The manager shows the reported engine name and author
when provided.

## Manage and test an engine

Select a saved profile. **Rename** changes its local label. **Set as default**
preselects it for new reviews. **Options** edits advertised UCI settings, including
CPU threads and hash memory where supported. Each override can be restored
individually. Disconnect first, then reconnect to apply saved options.

One engine worker runs at a time. Threads default to one. Hash memory defaults
to the engine's advertised value. The supported ranges appear in the options
dialog and are capped at 64 threads and 4,096 MiB. Higher values can consume
substantial CPU time or memory. Review candidate width and preferred win/draw/loss
estimates are in **Settings → Review preferences**. The engine and the number of
legal moves may further limit the width. Changing engine options can make earlier
analysis incompatible for reuse or for resuming the same saved review.

**Connect** checks the executable and its reported UCI identity and options
against its registration. **Ready** means the handshake succeeded. Choose
**Test analysis** to run a short search of the starting position and verify that
the engine returns a legal move. The manager displays active analysis and reports
engine failures inline. Reconnect explicitly after a failure.

**Remove** takes a profile out of the list while keeping its executable, games,
and review history. Set another engine as default first if removing the default
while more engines are registered. To replace an executable at its current
path, remove its profile and add the new executable. Re-registering the original
binary restores its profile ID with fresh validation and any saved options the engine still supports.
Adding a binary that is still registered returns its existing profile.

The connection stays open across pages and closes when you disconnect, the
engine fails, or the app exits. Disconnect before connecting or adding another
engine. Connections are opened on demand and after explicit requests.

Linux is verified. Windows release verification is pending. For protocol limits,
search APIs, scheduling, and cache rules, see
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
