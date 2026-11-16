# Curated review-intelligence corpus

`historical.pgn` contains unannotated game scores, transcribed from the
[Opera Game](https://en.wikipedia.org/wiki/Opera_Game#The_game) and
[Evergreen Game](https://en.wikipedia.org/wiki/Evergreen_Game#The_game) records
(accessed 2026-11-16). Dates retain unknown components rather than choosing a
disputed day. Game scores are historical facts; no source commentary is copied.

`ReviewCorpusTest.cpp` loads the checked-in PGN through the production parser and
replays every position. No engine or network is needed for these exact outcomes:

| Game / root | Required reasoning witnesses |
| --- | --- |
| Morphy–consultants, before 13.Rxd7 | Rook on d7 concedes two pawn units after ...Rxd7; the saved nine-ply continuation ends in White's locally verified checkmate. |
| Anderssen–Dufresne, before 21.Qxd7+ | Queen on d7 concedes eight net pawn units after ...Kxd7; the saved seven-ply continuation ends in White's locally verified checkmate. |
| Both opening roots | No supported sacrifice, checking fork or checkmate in the bounded continuation. |
| Both sacrifice roots, truncated after acceptance | Neither sacrifice soundness nor checkmate is established by these two plies. |
| Both sacrifice roots, without analysis evidence | No special label; `special.numeric_evidence_required.v3`. Historical reputation cannot establish analysis stability or move quality. |

Every returned fact must reference a legal prefix of the actual continuation.
Checkmate witnesses must end with check and no legal reply. The complete games
must also end in checkmate. These are line facts, not proof against every defence
or engine evaluation goldens; famous moves do not automatically deserve Brilliant.

The companion curated tactical positions in `IntelligenceTest.cpp` cover exact
rule outcomes with controlled numeric evidence: a Greek-gift sacrifice, a missed
queen capture, checking and pinned forks, promotion forks, recaptured promotions,
en passant, exchange versus pawn gains, an x-ray recapture, and unresolved PV
horizons. They check witness type, beneficiary, material, actor, rule ID,
independent attempts, disagreement and withheld labels. Controlled WDL values
exercise the rules and do not purport to be measured chess evaluations.

Run `ctest --preset dev -L corpus` for the historical corpus, or
`ctest --preset dev -L review` for the domain intelligence/classification suite.
Persistence and Qt suites additionally cover reproduction, stale revisions,
retained explanations, cancellation, resume and verified cache authorization.
