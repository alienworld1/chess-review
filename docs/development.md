# Development setup

[Back to README](../README.md)

## Finding Qt

The presets look for Qt 6.12.0 in `.tools/Qt/6.12.0/gcc_64`, then CMake's normal
system locations. To install a minimal SDK on Linux x86-64 (Python 3 and venv
support required):

```sh
python3 -m venv .tools/aqt
.tools/aqt/bin/pip install aqtinstall==3.3.0
.tools/aqt/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O .tools/Qt --archives qtbase icu qtsvg
```

The matching ICU package is a runtime dependency of Qt's prebuilt SDK.
Linux also needs Qt's usual platform libraries. On Ubuntu/Mint, install
`libxcb-cursor0` if the X11 platform plugin reports it missing.

For another SDK location, pass its prefix when configuring:

```sh
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/gcc_64
cmake --build --preset dev --target run
```

For repeated use of a different SDK with the Make shortcuts, set the `Qt6_DIR`
cache entry to `/path/to/Qt/6.12.0/gcc_64/lib/cmake/Qt6`, or keep your own presets
in ignored `CMakeUserPresets.json`. `Qt6_DIR` stays in the local build cache.

## Formatting and static analysis

`.clang-format` defines the shared C++ style. VS Code workspace settings enable
format-on-save through the recommended clangd extension. Other editors can use
the same file with their clang-format integration. `make format` formats all
source files, and `make format-check` reports drift without editing files.

`.clang-tidy` enables a small set of analyzer, lifetime, modernization, and copy
checks. `make lint` runs them against the actual build compilation database;
clangd also shows these diagnostics in the editor.

## Direct CMake commands

Without the Make shortcuts, the equivalent commands are:

```sh
cmake --preset dev
cmake --build --preset dev
cmake --build --preset dev --target run
```

Debug and release outputs live in `build/dev` and `build/release`.
`build/dev/compile_commands.json` is available for C++ editors. Use
`CMakeUserPresets.json` for local overrides. See [Testing](testing.md) for test
presets, offline dependency setup, sanitizers, and CI.

## Building without tests

To build only the desktop application binary (`1.0.0`):

```sh
cmake -S . -B build/app-only -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_PREFIX_PATH=.tools/Qt/6.12.0/gcc_64
cmake --build build/app-only
```

Pass `-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=/path/to/tomlplusplus-3.4.0` when
configuring offline.

## Linux AppImage packaging

Packaging targets produce a standalone x86-64 AppImage (`dist/chess-review-1.0.0-x86_64.AppImage`)
and verify it under a scrubbed environment:

```sh
make package-linux
make package-verify
```

`packaging/linux/build-appimage.sh` builds a Release `BUILD_TESTING=OFF` install
with `INSTALL_RPATH "$ORIGIN/../lib"`, bundles required Qt 6.12 shared libraries,
ICU runtime libraries, Qt plugins (including `qxcb`, `qoffscreen`, `qwayland`,
`qsqlite`, `qsvg`, and `qopensslbackend`), and `libxcb-cursor.so.0` (when
present in `.tools/xcb-cursor/` or the host library directories), strips debug
symbols, checks all ELF binaries with `ldd`, validates desktop and AppStream
metadata, and packages the AppDir with `appimagetool`. It rejects any AppDir
containing engine binaries or unresolved shared libraries.

Override defaults when needed with environment variables:

- `QT_PREFIX` (defaults to `.tools/Qt/6.12.0/gcc_64`)
- `APPIMAGETOOL` (defaults to `appimagetool` on `PATH` or `.tools/appimagetool-x86_64.AppImage`)
- `APPIMAGE_RUNTIME` (defaults to `.tools/runtime-x86_64` when present for offline builds)

`packaging/linux/verify-appimage.sh` checks the `.sha256` file, extracts the
AppImage, confirms zero `.tools/` path leaks or missing shared libraries under
`env -i`, runs `AppRun --version`, `AppRun --help`, and
`--appimage-extract-and-run --version`, and launches `AppRun` under
`QT_QPA_PLATFORM=offscreen` in isolated temporary XDG directories until the
schema v16 SQLite database initializes cleanly.
