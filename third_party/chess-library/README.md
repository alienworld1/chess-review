# Disservin/chess-library

Vendored without modifications from the official upstream repository:
https://github.com/Disservin/chess-library

- Commit: `53e6a841dcda7059a2af363d85f785ef1817304a`
- Header version: 0.9.4
- Source: `include/chess.hpp` at that commit
- License: MIT, retained in `LICENSE` and the header
- SHA-256 (`chess.hpp`): `f2c8e2e929641e2c71cbe9d8abd718cf3cac46c2a34531215ebd733905e98d7f`
- SHA-256 (`LICENSE`): `5860e7607afbf4c7e91bad5549b71d16fc4eceb90f0c671cd77a343ae7461a2a`

Only `src/chess/Position.cpp` includes this header. The include directory is a
private system include on `chess_review_chess`; clients use `chess/Position.h`.
No runtime or configure-time chess dependency download is required.

To update, review the upstream changes, replace the header/license from an exact
commit, update this provenance/checksum record, and run the chess suite, full
builds, sanitizers, and static analysis. Pay particular attention to enum mapping,
FEN validation/normalization, SAN check suffixes, legal en passant, undo allocation
ordering, and the upstream narrow counters. Never expose upstream hashes or
serialized representations as application identities.
