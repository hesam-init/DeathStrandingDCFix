# Building on Arch Linux

DeathStrandingDCFix is a Windows x64 DLL built as `winhttp.dll`: the game imports `WINHTTP.dll`, so it
loads the fix directly, and every winhttp export is forwarded to the system `winhttp.dll`
(`src/proxy/`). No ASI loader is needed. On Linux we
cross-compile it with **CMake** and the **MinGW-w64** GCC toolchain, and test it through Proton/Wine.

The upstream Windows build uses xmake + MSVC (`xmake.lua`, `create_release.ps1`). That flow is
unchanged. The CMake build below is the Linux equivalent.

## Project layout

```
CMakeLists.txt                      # target, compile/link flags, release folder, install, CPack (zip)
cmake/
  toolchains/mingw-w64-x86_64.cmake # MinGW-w64 cross toolchain
  Dependencies.cmake                # spdlog, inipp, imgui (submodules) and safetyhook (vendored)
Makefile                            # short commands that run cmake / cpack for you
```

CMake does the actual build. The `Makefile` only wraps commands, so you can type `make`,
`make install`, `make dist`, ... instead of long `cmake` command lines.

## 1. Install packages

Required:

```sh
sudo pacman -S --needed base-devel git cmake ninja mingw-w64-gcc
```

| Package          | Why                                                                 |
|------------------|---------------------------------------------------------------------|
| `base-devel`     | `make` and the basic build tools                                    |
| `git`            | Cloning the repo and its submodules                                 |
| `cmake`          | The build system (3.24 or newer). Also provides `cpack` for `make dist` |
| `ninja`          | Fast CMake backend. Optional: the Makefile falls back to `Unix Makefiles` |
| `mingw-w64-gcc`  | The Windows x64 cross compiler                                      |

`mingw-w64-gcc` pulls in `mingw-w64-binutils`, `mingw-w64-crt`, `mingw-w64-headers` and
`mingw-w64-winpthreads`. It provides `x86_64-w64-mingw32-gcc` / `x86_64-w64-mingw32-g++`.

Optional:

```sh
sudo pacman -S --needed wine   # quick load tests outside Steam
sudo pacman -S --needed clang  # clangd for your editor
```

Check the toolchain:

```sh
x86_64-w64-mingw32-g++ --version
cmake --version
```

## 2. Get the source

The project uses git submodules for `spdlog` and `inipp`:

```sh
git clone --recurse-submodules <repo-url>
cd DeathStrandingDCFix
```

In an existing clone:

```sh
make submodules   # same as: git submodule update --init --recursive
```

## 3. Build

```sh
make                   # release build (default): configure on first run, then build
make debug             # debug build: -O0 -g, debug-level logging
make configure         # re-run CMake configure for the current MODE
make help              # list targets and variables
make dist VERSION=1.0  # release zip: build/DeathStrandingDCFix_1.0.zip
make clean             # remove build/mingw
make distclean         # clean + remove release zips
```

The build is incremental and uses all CPU cores (`JOBS` defaults to `nproc`; e.g. `make JOBS=4`). After the first configure, CMake also
reconfigures by itself whenever `CMakeLists.txt` or `cmake/*.cmake` change.

Each build has a `bin/` folder that is a complete, ready-to-copy release:

| File                       | What it is                                          |
|----------------------------|-----------------------------------------------------|
| `winhttp.dll`              | The fix (forwards winhttp calls to the system DLL)  |
| `DeathStrandingDCFix.ini`  | Default settings (copied from the repo root)        |

The folder is `build/mingw/release/bin/` or `build/mingw/debug/bin/`, depending on `MODE`.

`make install` also deletes a `DeathStrandingDCFix.asi` left over from older versions in the game folder.

The DLL is linked fully static (`-static -static-libgcc -static-libstdc++`), so it does not
need `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll` next to the game. To verify:

```sh
file build/mingw/release/bin/winhttp.dll
# PE32+ executable for MS Windows ... (DLL), x86-64

x86_64-w64-mingw32-objdump -p build/mingw/release/bin/winhttp.dll | grep 'DLL Name'
# Only system DLLs (KERNEL32, USER32, GDI32, SHELL32, dwmapi, D3DCOMPILER_47) and api-ms-win-crt-*.dll.
# dxgi.dll/d3d12.dll must not be listed (they would break dxgi.dll proxies such as OptiScaler).
```

Makefile variables (override on the command line):

| Variable        | Default                | Purpose                                          |
|-----------------|------------------------|--------------------------------------------------|
| `MODE`          | `release`              | `release` (CMake `MinSizeRel`) or `debug` (`Debug`) |
| `GAME_DIR`      | *(empty)*              | Game folder used by `make install`               |
| `VERSION`       | `dev`                  | Version string in the `make dist` zip name       |
| `MINGW_TRIPLET` | `x86_64-w64-mingw32`   | Toolchain prefix                                 |
| `GENERATOR`     | `Ninja` if installed   | CMake generator                                  |
| `JOBS`          | `nproc` (all cores)    | Parallel compile jobs                            |

### Using CMake directly

If you'd rather not use make, this is what it runs:

```sh
cmake -S . -B build/mingw/release -G Ninja \
      --toolchain cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build/mingw/release
cmake --install build/mingw/release --component game --prefix "<game dir>"
cpack --config build/mingw/release/CPackConfig.cmake -B build/mingw/release/package
```

## 4. Install into the game

The game folder for Steam is usually:

```
~/.local/share/Steam/steamapps/common/DEATH STRANDING DIRECTORS CUT
```

Build and install in one step:

```sh
make install GAME_DIR="$HOME/.local/share/Steam/steamapps/common/DEATH STRANDING DIRECTORS CUT"
```

`make install` copies `DeathStrandingDCFix.asi` and `winhttp.dll`. It only copies
`DeathStrandingDCFix.ini` if there isn't one already, so your settings are kept.

The first time, also open the game's **Properties → General → Launch Options** in Steam and set:

```
WINEDLLOVERRIDES="winhttp=n,b" %command%
```

Without this override, Wine uses its built-in `winhttp.dll` and the fix is never loaded.

Tip: put `GAME_DIR` in your shell profile so a plain `make install` works:

```sh
# fish
set -Ux GAME_DIR "$HOME/.local/share/Steam/steamapps/common/DEATH STRANDING DIRECTORS CUT"
# bash/zsh
export GAME_DIR="$HOME/.local/share/Steam/steamapps/common/DEATH STRANDING DIRECTORS CUT"
```

## 5. Debugging

- `make install MODE=debug GAME_DIR=...` builds and installs a build with debug symbols and
  debug-level logging (`_DEBUG` is defined).
- The fix writes `DeathStrandingDCFix.log` next to the game executable. Check it for
  "loaded" and for pattern-scan failures.
- For Proton-side issues, add `PROTON_LOG=1` to the launch options:
  `PROTON_LOG=1 WINEDLLOVERRIDES="winhttp=n,b" %command%`. The log goes to
  `~/steam-<appid>.log`.

## 6. Editor setup (clangd)

CMake writes `compile_commands.json` into the build folder. Link it into the repo root
(it's gitignored):

```sh
make compdb             # release build dir
make compdb MODE=debug  # or the debug one
```

clangd also needs to know that this is a Windows target. Create a `.clangd` file in the repo root:

```yaml
CompileFlags:
  Add: [--target=x86_64-w64-mingw32]
  Compiler: x86_64-w64-mingw32-g++
```

and allow clangd to query the cross compiler for its system headers. For example, in VS Code
`settings.json`:

```json
"clangd.arguments": ["--query-driver=/usr/bin/x86_64-w64-mingw32-*"]
```

## 7. Notes on compiler differences

- MSVC implicitly converts function pointers to `void*`, but GCC rejects this, so calls like
  `Memory::HookIAT(...)` need explicit `(void*)` casts. These casts also compile under MSVC.
- `-flto` is not used for release builds. `mingw-w64-gcc` 16.2 hits an internal compiler
  error (`binds_to_current_def_p`) with LTO on this codebase. `-ffunction-sections
  -fdata-sections -Wl,--gc-sections` is used instead.
- Warnings from the vendored safetyhook sources are silenced (`-w`). Warnings from `src/` are not.

## 8. Troubleshooting

| Problem | Fix |
| --------- | ----- |
| `make: cmake: No such file or directory` / `cmake: command not found` | `sudo pacman -S cmake` |
| CMake: `CMake 3.24 or higher is required` | Update your system: `sudo pacman -Syu cmake` |
| CMake: `x86_64-w64-mingw32-gcc ... is not a full path and was not found` | `sudo pacman -S mingw-w64-gcc` |
| CMake: `The git submodules are not initialized` | `make submodules` |
| `make install` says `set GAME_DIR` | Pass `GAME_DIR="..."` or export it (see above) |
| No `DeathStrandingDCFix.log` after launching | `winhttp.dll` (the ASI loader) is missing, or the `WINEDLLOVERRIDES` launch option isn't set |
| CMake: `Failed to download the ASI loader` | GitHub is blocked or unreachable. Download `Ultimate-ASI-Loader_x64.zip` manually, extract `dinput8.dll`, and either save it as `build/asi-loader/winhttp.dll` or run `cmake -B build/mingw/release -DASI_LOADER_DLL=/path/to/dinput8.dll` |
| Changed `GENERATOR` and CMake complains about it | `make clean` first. A build folder can't switch generators |
