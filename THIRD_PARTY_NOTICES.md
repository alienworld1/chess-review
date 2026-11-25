# Third-Party Notices and Asset Attribution

`chess-review` incorporates or uses the following third-party software components and graphic assets.

## Chess Piece Artwork (Cburnett SVG Set)

- **Author and attribution:** Colin M. L. Burnett and Wikimedia Commons contributors.
- **License:** Distributed here under the BSD 3-Clause License option recorded in `assets/pieces/cburnett/LICENSE.txt` (bundled in the application as `:/pieces/LICENSE.txt`). Upstream Wikimedia Commons provenance, authors, tri-license notes, and SHA-256 verification checksums are recorded in `assets/pieces/cburnett/provenance.json` (bundled as `:/pieces/provenance.json`).
- **Included files (12 SVG assets under `assets/pieces/cburnett/`):**
  - `Chess_bdt45.svg` (black bishop)
  - `Chess_blt45.svg` (white bishop)
  - `Chess_kdt45.svg` (black king)
  - `Chess_klt45.svg` (white king)
  - `Chess_ndt45.svg` (black knight)
  - `Chess_nlt45.svg` (white knight)
  - `Chess_pdt45.svg` (black pawn)
  - `Chess_plt45.svg` (white pawn)
  - `Chess_qdt45.svg` (black queen)
  - `Chess_qlt45.svg` (white queen)
  - `Chess_rdt45.svg` (black rook)
  - `Chess_rlt45.svg` (white rook)

## Application Interface Icons

- **Location:** `assets/icons/` (documented in `assets/icons/README.md`).
- **Attribution:** Original 24x24 single-color SVG interface icons authored for `chess-review`.

## Runtime Libraries and Data Dependencies

### Disservin `chess-library`
- **Location:** `third_party/chess-library`
- **License:** MIT License (`third_party/chess-library/LICENSE`)

### Lichess `chess-openings`
- **Location:** `third_party/chess-openings`
- **License:** CC0 1.0 Universal Public Domain Dedication (`third_party/chess-openings/COPYING.txt`)

### `toml++` (`tomlplusplus`) 3.4.0
- **Location:** `third_party/tomlplusplus`
- **License:** MIT License (`third_party/tomlplusplus/LICENSE`)

### Qt 6.12 Framework
- **Author:** The Qt Company Ltd. and contributors
- **License:** GNU Lesser General Public License v3.0 (LGPLv3) or GNU General Public License v3.0 (GPLv3)
- **Notes:** Dynamic Qt shared libraries and runtime plugins bundled in Linux AppImage releases remain licensed under LGPLv3 / GPLv3.

## Test-Only Dependencies

### Catch2 3.8.1
- **License:** Boost Software License 1.0 (BSL-1.0)
- **Notes:** Used only by the test suite; not compiled into or distributed with the `chess-review` application binary.

## External UCI Chess Engines

`chess-review` does not bundle or execute an embedded chess engine binary. Any external UCI engine configured by the user (for example Stockfish, GNU GPL v3.0) is obtained and governed under its own license.
