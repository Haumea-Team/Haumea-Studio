# Building and testing Haumea Studio

## Build

The editor requires a C++17 compiler and builds with Premake and librw. librw is the
[Haumea-Team/librw](https://github.com/Haumea-Team/librw) fork (upstream librw with Haumea's
changes and the Dear ImGui docking build), pinned as the `librw` submodule. Fetch it with
`git submodule update --init` (or clone with `--recurse-submodules`), set `LIBRW` to it, and run
the commands from the Haumea checkout. See `librw/HAUMEA.md` for what the fork changes and how
to take upstream updates.

### Linux x86_64

```sh
git submodule update --init
export LIBRW="$PWD/librw"

(cd "$LIBRW" && premake5 gmake2)
make -C "$LIBRW/build" config=release_linux-amd64-gl3 librw

premake5 gmake2
make -C build -j2 config=release_linux-amd64-gl3 haumea
```

The program is `bin/linux-amd64-gl3/Release/haumea`.

### macOS

```sh
git submodule update --init
export LIBRW="$PWD/librw"

(cd "$LIBRW" && premake5 gmake2 --gfxlib=glfw)
make -C "$LIBRW/build" config=release_macos-arm64-gl3 librw  # Apple Silicon
# or: make -C "$LIBRW/build" config=release_macos-amd64-gl3 librw  # Intel

premake5 gmake2
make -C build config=release_macos-arm64-gl3 haumea  # Apple Silicon
# or: make -C build config=release_macos-amd64-gl3 haumea  # Intel
```

### Windows

The Windows build uses GLFW and OpenGL 3. Install Visual Studio, Premake 5 and the GLFW 3.3.4
Win64 binary package. Then build librw and Haumea from PowerShell:

```powershell
$Haumea = 'C:\path\to\haumea'
$Librw = "$Haumea\librw"
$Glfw = 'C:\path\to\glfw-3.3.4.bin.WIN64'
$env:LIBRW = $Librw
git -C $Haumea submodule update --init

Push-Location $Librw
premake5 vs2019 "--glfwdir64=$Glfw"
msbuild build\librw.sln /p:Configuration=Release /p:Platform=win-amd64-gl3 /t:librw /m
Pop-Location

Push-Location $Haumea
premake5 vs2019 "--glfwdir64=$Glfw"
msbuild build\librwgta.sln /p:Configuration=Release /p:Platform=win-amd64-gl3 '/t:librwgta;haumea' /m
Pop-Location
```

## Command line

```sh
haumea [--game /path/to/GTA\ San\ Andreas] [--workspace /path/to/server] [--open map.pwn]
haumea --game /path/to/GTA --pawn-roundtrip in.pwn out.pwn   # open, save and quit
```

`--game` and `--workspace` apply to that run only. `HAUMEA_GAME_DIR` is also read when no game
folder is saved yet.

## Tests

```sh
tools/haumea/tests/run_all.sh
```

This runs every test on your machine:

- the unit tests in `tools/haumea/tests`;
- the Pawn round trip: the editor opens and saves each map in `tests/pawn/maps`, and the output
  must match `tests/pawn/expected` and save again unchanged;
- a compile of every saved map as a gamemode, with no errors or warnings against the open.mp
  includes and, for maps without open.mp-only calls, the SA-MP 0.3.7 includes.

The compiler (open.mp's `pawncc`), the includes and Namaka's wheels are downloaded once, at fixed
versions. The game folder is `HAUMEA_GAME_DIR` or the one Haumea has saved. Without one, or with
`--fixture`, the tests use a synthetic game folder from `tests/fixture/make_game.py`: simple box
models with the game's model names and the data files the test maps need, and no game files at
all. A test that can't run says why. After an intended change to the exporter,
`run_all.sh --update-expected` updates `tests/pawn/expected`.

CI builds Linux, Windows and macOS for `main` and pull requests, and runs `run_all.sh --fixture`
on Linux under Xvfb.

## Package downloads

`tools/release/package_downloads.py` stages the editor, fonts, prefabs, translations,
licences and Namaka in one folder. Use a new staging directory:

```sh
python3 tools/release/package_downloads.py --platform linux \
  --engine bin/linux-amd64-gl3/Release/haumea \
  --namaka /path/to/Haumea-Namaka-linux-x86_64.zip \
  --stage /tmp/haumea-linux-stage --output /tmp/haumea-downloads
```

This makes the portable/update tarball and a self-extracting `.run` installer. The
installer uses Python 3.10+, installs for the current user and keeps settings in
the existing config folder. Re-running the installer updates its managed files;
`python3 ~/.local/share/haumea-studio/install-linux.py --uninstall` removes them
(use the printed path when `XDG_DATA_HOME` is set). Added files and settings stay.

For Windows, pass `--platform windows`, an editor `.exe`, `--glfw /path/to/glfw3.dll`
and the Windows Namaka ZIP. Put the app-local MSVC runtime DLLs beside the input
editor; the packager includes them. It writes one portable ZIP with a `Haumea Studio`
folder and the legacy updater tarball. Compile `tools/release/haumea.iss` with
Inno Setup 6 (`ISCC /DBundleDir=... /DOutputDir=... /DAppVersion=1.0.0.1 haumea.iss`)
to make the per-user Windows setup program. The `installer-windows` CI job builds
and exercises installation/uninstallation on every run. No release is published
unless the manual release workflow requests it.

For macOS, fetch the Apple Silicon wheels with `fetch_wheelhouse.sh macos`, then
use `package_agent.py` to make the internal Namaka companion. Pass `--platform macos`,
the Apple Silicon editor and `--namaka /path/to/Haumea-Namaka-macos-arm64.zip` to the
download packager (without `--glfw`). The single portable ZIP includes Namaka and
preserves the executable bit; GLFW must be installed on the target Mac. The
`agent-bundle-macos` CI job tests offline installation and repair with Python 3.14,
the MCP tool list, automatic listening and live camera/status/capture calls against
the native editor using a synthetic game, then uninstalls Namaka. Releases require
this job to pass.

`package_macos.py` wraps the complete Mac stage in `Haumea Studio.app`, bundles
GLFW in `Contents/Frameworks`, rewrites its load path and checks for unbundled
libraries. It preserves the editor's resource lookups through links into
`Contents/Resources`. The disk image contains the app and an Applications shortcut:

```sh
python3 tools/release/package_macos.py --bundle /path/to/macos-stage \
  --glfw "$(brew --prefix glfw)/lib/libglfw.3.dylib" --version 1.0.0.1 \
  --stage /tmp/haumea-dmg-stage --output /tmp/haumea-downloads
```

The DMG ships without Developer ID signing or notarization. The app and bundled
GLFW use ad-hoc signatures for Apple Silicon compatibility; no Apple account or
certificates are needed. The native Mac job mounts the DMG, copies the app into a
private test directory, verifies its ad-hoc signature and repeats the live Namaka
check there.
