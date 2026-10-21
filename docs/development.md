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
