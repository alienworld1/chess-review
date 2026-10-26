# toml++

The configuration boundary uses toml++ 3.4.0 (MIT license), fetched at configure
time from the upstream release archive:

- Source: https://github.com/marzer/tomlplusplus/tree/v3.4.0
- Archive: https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
- SHA-256: `8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155`

CMake verifies the archive checksum. Only `chess_review_config` includes the
parser, with `TOML_EXCEPTIONS=0` for explicit parse results and
`TOML_MAX_NESTED_VALUES=16` for bounded array/inline-table recursion. The product
also caps raw input dots at 256 before parsing: upstream's nested-value limit does
not bound dotted-key table depth or recursive table destruction. Parser types do not
cross the configuration API. No engine or network activity occurs at runtime.

For offline builds, extract the verified archive and configure with
`-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=/absolute/path/to/tomlplusplus-3.4.0`.
The source override is trusted local input; verify it against the pinned archive.
See LICENSE for the upstream license and notices.
