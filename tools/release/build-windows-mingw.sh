#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compiles haumea.exe for Windows x64 with MinGW-w64, on Linux.
#
#   tools/release/build-windows-mingw.sh [gl3|d3d9]
#
# Needs x86_64-w64-mingw32-g++ and python3. It prepares its own librw
# (cloned from $LIBRW_GIT, patched with patches/librw-haumea.patch and
# tools/release/patch_librw_haumea.py) and, for gl3, GLFW's official
# Windows binaries, all under $WORK (default build/windows-mingw), then
# builds librw, librwgta and haumea there. The result is
# $WORK/win-amd64-<gfx>/haumea.exe, a single static exe (GLFW, libstdc++ and
# libgcc are linked in). The source lists come from build/librwgta.make and
# build/haumea.make, so files added there are built here too; premake5
# makes them when they are missing (build/ is generated, not in git).
#
# d3d9 uses ImGui's Win32 and DX9 backends from vendor/imgui-docking/backends
# (installed by patch_librw_haumea.py) and librw's releaseDeviceResources
# hook (patches/librw-haumea.patch).
#
# Running it under Wine on Linux, with its own prefix and config folder:
#   WINEPREFIX=/tmp/haumea-wine HAUMEA_CONFIG_DIR='Z:\tmp\haumea-cfg' \
#     wine build/windows-mingw/win-amd64-gl3/haumea.exe --game 'Z:\path\to\GTA San Andreas'
# (Wine sets APPDATA itself, hence HAUMEA_CONFIG_DIR.)
set -eu

GFX=${1:-gl3}
case "$GFX" in gl3|d3d9) ;; *) echo "usage: $0 [gl3|d3d9]" >&2; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$ROOT/build/windows-mingw}
LIBRW_GIT=${LIBRW_GIT:-https://github.com/aap/librw.git}
GLFW_ZIP_URL=https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip
GLFW_ZIP_SHA256=54efa829400f2a0537f742b2b3bdd74e437bb4f2f048e4b7d3c5557d11a611e6
CHANNEL=${HAUMEA_CHANNEL:-production}
JOBS=${JOBS:-$(nproc)}
CC=${CC:-x86_64-w64-mingw32-gcc}
CXX=${CXX:-x86_64-w64-mingw32-g++}
AR=${AR:-x86_64-w64-mingw32-ar}
OUT=$WORK/win-amd64-$GFX
mkdir -p "$WORK" "$OUT"

# --- librw, patched like the CI build ---
# The marker holds a checksum of what patches it (the patch, the patch script
# and the ImGui it installs): when that changes, librw is cloned and patched again.
LIBRW=$WORK/librw
PATCHED=$( { cat "$ROOT/patches/librw-haumea.patch" "$ROOT/tools/release/patch_librw_haumea.py"
	find "$ROOT/vendor/imgui-docking" -type f -print0 | LC_ALL=C sort -z | xargs -0 cat; } | sha256sum | cut -d' ' -f1)
if [ "$(cat "$LIBRW/.haumea-patched" 2>/dev/null)" != "$PATCHED" ]; then
	rm -rf "$LIBRW"
	git clone -q --depth=1 "$LIBRW_GIT" "$LIBRW"
	(cd "$LIBRW" && git apply "$ROOT/patches/librw-haumea.patch")
	python3 "$ROOT/tools/release/patch_librw_haumea.py" "$LIBRW"
	echo "$PATCHED" > "$LIBRW/.haumea-patched"
	# Objects built against the old librw headers are stale too.
	rm -rf "$WORK"/win-amd64-*/obj
fi

# --- GLFW (gl3 only) ---
GLFW=$WORK/glfw-3.4.bin.WIN64
if [ "$GFX" = gl3 ] && [ ! -d "$GLFW" ]; then
	curl -sSL -o "$WORK/glfw.zip" "$GLFW_ZIP_URL"
	echo "$GLFW_ZIP_SHA256  $WORK/glfw.zip" | sha256sum -c -
	python3 -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$WORK/glfw.zip" "$WORK"
fi

if [ "$GFX" = gl3 ]; then
	GFXDEFS="-DRW_GL3 -DLIBRW_GLFW"
	GFXINC="-I$GLFW/include"
	GFXLIBS="$GLFW/lib-mingw-w64/libglfw3.a -lopengl32"
else
	GFXDEFS="-DRW_D3D9"
	GFXINC="-I$LIBRW/skeleton/imgui"
	GFXLIBS="-ld3d9 -ldwmapi"
fi
COMMON="-O2 -DNDEBUG $GFXDEFS -DLODEPNG_NO_COMPILE_CPP $GFXINC"

# compile <objdir> <extra flags> <sources...>: in parallel, only what changed.
compile() {
	objdir=$1; flags=$2; shift 2
	cppstd=c++14
	case "$objdir" in */haumea) cppstd=c++17 ;; esac
	stampflags="$flags -std=$cppstd"
	# Objects built with other flags (a define added to a makefile) are stale.
	if [ "$(cat "$objdir/.flags" 2>/dev/null)" != "$stampflags" ]; then
		rm -rf "$objdir"
	fi
	mkdir -p "$objdir"
	printf '%s' "$stampflags" > "$objdir/.flags"
	for src in "$@"; do
		# The folder goes into the name: librw's skeleton has a camera.cpp too.
		obj="$objdir/$(echo "$src" | sed 's|\.[^.]*$||; s|.*/\([^/]*/[^/]*\)$|\1|; s|/|_|g').o"
		if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
			echo "$src|$obj"
		fi
	done | xargs -r -P "$JOBS" -I{} sh -c '
		src=${1%%|*}; obj=${1#*|}
		case "$src" in *.c) comp="$2" ;; *) comp="$3 -std=$5" ;; esac
		echo "  $(basename "$src")"
		$comp $4 -c "$src" -o "$obj"' _ {} "$CC" "$CXX" "$flags" "$cppstd"
	# Objects of sources no longer listed (a removed file, or a header an older
	# script took for a source) would be archived or linked by the *.o globs.
	keep=$(for src in "$@"; do
		echo "$objdir/$(echo "$src" | sed 's|\.[^.]*$||; s|.*/\([^/]*/[^/]*\)$|\1|; s|/|_|g').o"
	done)
	for obj in "$objdir"/*.o; do
		[ -e "$obj" ] || continue
		printf '%s\n' "$keep" | grep -qxF "$obj" || rm -f "$obj"
	done
}

# Sources a makefile builds, with /opt/librw pointing at our librw.
make_sources() {
	python3 -I -c '
import re, sys
s = open(sys.argv[1]).read()
# Object rules name their C/C++ source; other prerequisites (a generated
# header) are not sources.
for src in re.findall(r"^\$\(OBJDIR\)/\S+\.o: (\S+\.(?:c|cc|cpp))$", s, re.M):
    src = src.replace("/opt/librw", sys.argv[2])
    print(src if src.startswith("/") else sys.argv[3] + "/build/" + src)
' "$1" "$LIBRW" "$ROOT"
}

if [ ! -f "$ROOT/build/haumea.make" ] || [ ! -f "$ROOT/build/librwgta.make" ]; then
	if ! command -v premake5 >/dev/null; then
		echo "build-windows-mingw.sh: needs premake5 for build/haumea.make (or run premake5 gmake2 first)" >&2
		exit 1
	fi
	(cd "$ROOT" && LIBRW="$LIBRW" premake5 gmake2 >/dev/null)
fi

echo "librw ($GFX)"
compile "$OUT/obj/librw" "$COMMON -I$LIBRW/src" \
	$(find "$LIBRW/src" -name '*.cpp' -o -name '*.c' | sort)
$AR rcs "$OUT/librw.a" "$OUT"/obj/librw/*.o

echo "librwgta"
compile "$OUT/obj/librwgta" "$COMMON -I$LIBRW -I$ROOT/src" $(make_sources "$ROOT/build/librwgta.make")
$AR rcs "$OUT/librwgta.a" "$OUT"/obj/librwgta/*.o

echo "haumea"
HAUMEA_SOURCES=$(make_sources "$ROOT/build/haumea.make")
# haumea.make's own defines beyond the graphics ones (e.g. ImGui's test
# engine hooks for --ui-script).
HAUMEA_DEFINES=$(python3 -I -c '
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r"^DEFINES \+= (.*)$", s, re.M)
skip = ("-DNDEBUG", "-DDEBUG", "-DRW_GL3", "-DRW_D3D9", "-DLIBRW_GLFW")
print(" ".join(d for d in m.group(1).split() if d not in skip and not d.startswith("-DHAUMEA_CHANNEL")))
' "$ROOT/build/haumea.make")
if [ "$GFX" = d3d9 ]; then
	# As premake5.lua does for d3d9: ImGui's own Win32 and DX9 backends.
	HAUMEA_SOURCES=$(echo "$HAUMEA_SOURCES" | grep -v 'imgui_impl_rw.cpp')
	HAUMEA_SOURCES="$HAUMEA_SOURCES $LIBRW/skeleton/imgui/backends/imgui_impl_win32.cpp $LIBRW/skeleton/imgui/backends/imgui_impl_dx9.cpp"
fi
# The build number and commit (versionutil.cpp, haumea.rc), as the makefile does.
python3 "$ROOT/tools/release/gen_buildinfo.py" --header "$ROOT/tools/haumea/haumea_buildinfo.h"
# compile() follows sources, not headers: a new build number rebuilds versionutil.
[ "$ROOT/tools/haumea/haumea_buildinfo.h" -nt "$OUT/obj/haumea/haumea_versionutil.o" ] &&
	rm -f "$OUT/obj/haumea/haumea_versionutil.o"
compile "$OUT/obj/haumea" "$COMMON -I$LIBRW -I$ROOT/src -I$LIBRW/skeleton -I$ROOT -I$ROOT/tools/haumea -I$ROOT/vendor/imgui-texteditor -I$ROOT/vendor/libvterm/include -I$LIBRW/skeleton/imgui -DHAUMEA_CHANNEL=\"$CHANNEL\" -D_MBCS $HAUMEA_DEFINES" \
	$HAUMEA_SOURCES "$ROOT/tools/haumea/minilzo/minilzo.c" "$ROOT/tools/haumea/monocypher/monocypher.c"
# The manifest (UTF-8 code page) goes in as a resource, as MSVC embeds it
# itself, and the version info (haumea.rc).
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
# haumea.rc by its full path: a relative include would find a file of that
# name in $OUT first (an older build wrote its own haumea.rc there).
printf '1 24 "%s"\n#include "%s"\n' "$ROOT/tools/haumea/haumea.exe.manifest" "$ROOT/tools/haumea/haumea.rc" \
	> "$OUT/haumea_res.rc"
$WINDRES -I "$ROOT/tools/haumea" "$OUT/haumea_res.rc" -O coff -o "$OUT/haumea_res.o"
$CXX -o "$OUT/haumea.exe" "$OUT"/obj/haumea/*.o "$OUT/haumea_res.o" "$OUT/librwgta.a" "$OUT/librw.a" \
	$GFXLIBS -lwinhttp -lws2_32 -lgdi32 -lcomdlg32 -lshell32 -lole32 -luuid -lpsapi -ldwmapi -lshlwapi \
	-static -static-libgcc -static-libstdc++ -mwindows -Wl,--stack,8388608
echo "built $OUT/haumea.exe"

