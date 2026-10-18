# chess-review

Minimal native C++20 / Qt Widgets scaffold. The app currently opens an empty
`QMainWindow`; product features will be added in later tasks.

## Requirements

- C++20 compiler (GCC or Clang on Linux)
- Qt **6.8+** development SDK with Widgets
- CMake **3.24+**, Ninja, and GNU Make
- Developer tools: clang-format 15+, clang-tidy, and clangd for editor integration

## Everyday commands

From the repository root:

```sh
make run
```

This configures, builds with Ninja, and opens the window. Close the window to
return to the terminal. The Makefile is just a set of command shortcuts; CMake
and Ninja handle the actual build.

| Command | Action |
| --- | --- |
| `make` | Show command help |
| `make build` | Configure and build a debug binary |
| `make run` | Build and launch the app |
| `make run PRESET=release` | Build and launch an optimized binary |
| `make clean` | Clean the selected preset's compiled outputs |
| `make format` | Format C++ source |
| `make format-check` | Verify C++ formatting |
| `make lint` | Build and run the curated clang-tidy checks |

Builds are incremental. Debug and release outputs live in `build/dev` and
`build/release`. `build/dev/compile_commands.json` is available for C++ editors.

## Finding Qt

The presets look for Qt 6.12.0 in `.tools/Qt/6.12.0/gcc_64`, then CMake's normal
system locations. This workspace has its SDK in that ignored project directory.
To install the same minimal SDK on another Linux x86-64 machine (Python 3 and
venv support required):

```sh
python3 -m venv .tools/aqt
.tools/aqt/bin/pip install aqtinstall==3.3.0
.tools/aqt/bin/aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O .tools/Qt --archives qtbase icu
```

The matching ICU package is a runtime dependency of Qt's prebuilt SDK.
Linux also needs Qt's usual platform libraries. On Ubuntu/Mint, install
`libxcb-cursor0` if the X11 platform plugin reports it missing. This workspace
keeps a copy of that library in the local SDK because system installation needs
an administrator password.
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

There is no chess, engine, database, or networking code yet, and no additional
runtime dependency beyond Qt. Launching the window is the scaffold's smoke check;
focused automated tests belong with the first application behavior.
