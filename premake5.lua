Librw = os.getenv("LIBRW")

newoption {
	trigger		= "gfxlib",
	value       = "LIBRARY",
	description = "Choose a particular development library",
	default		= "glfw",
	allowed		= {
		{ "glfw",	"GLFW" },
		{ "sdl2",	"SDL2" },
	},
}

newoption {
	trigger     = "glfwdir64",
	value       = "PATH",
	description = "Directory of glfw",
	default     = "../glfw-3.3.4.bin.WIN64",
}

newoption {
	trigger     = "glfwdir32",
	value       = "PATH",
	description = "Directory of glfw",
	default     = "../glfw-3.3.4.bin.WIN32",
}

newoption {
	trigger     = "sdl2dir",
	value       = "PATH",
	description = "Directory of sdl2",
	default     = "../SDL2-2.0.14",
}

newoption {
	trigger     = "channel",
	value       = "NAME",
	description = "Release channel: production (default) or beta; CI uses dev for non-release builds",
	default     = "production",
}

-- gmake (gmake2 before premake 5.0 beta 8): objects built with other defines
-- are stale (make tracks files, not flags), so every object depends on
-- $(OBJDIR)/.defines, which is rewritten only when the defines change. Without it a define added later (like the
-- ImGui test engine hooks) never reaches objects built before.
for _, name in ipairs({ "gmake", "gmake2" }) do
	local loaded, gmake = pcall(require, name)
	if loaded and type(gmake) == "table" and gmake.cpp and gmake.cpp.pchRules then
		premake.override(gmake.cpp, "pchRules", function(base, cfg, toolset)
			base(cfg, toolset)
			_p('# Objects built with other defines are stale: they depend on $(OBJDIR)/.defines.')
			_p('ifeq (posix,$(SHELLTYPE))')
			_p('DEFINES_STAMP := $(OBJDIR)/.defines')
			_p("$(shell mkdir -p $(OBJDIR); printf '%%s' '$(DEFINES)' | cmp -s - $(DEFINES_STAMP) || printf '%%s' '$(DEFINES)' > $(DEFINES_STAMP))")
			_p('$(OBJECTS): $(DEFINES_STAMP)')
			_p('endif')
			_p('')
		end)
		break
	end
end

Zlibdir = "C:/Users/aap/src/zlib-1.2.11"
luadir = "/usr/include/lua5.4"

workspace "librwgta"
	location "build"
	language "C++"
	cppdialect "C++14"

	configurations { "Release", "Debug" }
	filter { "system:windows" }
		configurations { "ReleaseStatic" }
		platforms { "win-x86-null", "win-x86-gl3", "win-x86-d3d9",
			"win-amd64-null", "win-amd64-gl3", "win-amd64-d3d9" }
	filter { "system:linux" }
		platforms { "linux-x86-null", "linux-x86-gl3",
			"linux-amd64-null", "linux-amd64-gl3",
			"linux-arm-null", "linux-arm-gl3" }
	-- TODO: ps2
		if _OPTIONS["gfxlib"] == "sdl2" then
			includedirs { "/usr/include/SDL2" }
		end
	filter { "system:macosx" }
		platforms { "macos-amd64-null", "macos-amd64-gl3",
			"macos-arm64-null", "macos-arm64-gl3" }
	filter {}

	filter "configurations:Debug"
		defines { "DEBUG" }
		symbols "On"
	filter "configurations:Release*"
		defines { "NDEBUG" }
		optimize "On"
	filter "configurations:ReleaseStatic"
		staticruntime("On")

	filter { "platforms:*null" }
		defines { "RW_NULL" }
	filter { "platforms:*gl3" }
		defines { "RW_GL3" }
		if _OPTIONS["gfxlib"] == "glfw" then
			defines { "LIBRW_GLFW" }
		else
			defines { "LIBRW_SDL2" }
		end
	filter { "platforms:*d3d9" }
		defines { "RW_D3D9" }
	filter { "platforms:*ps2" }
		defines { "RW_PS2" }
		toolset "gcc"
		gccprefix 'ee-'
		buildoptions { "-nostdlib", "-fno-common" }
		includedirs { "$(PS2SDK)/ee/include", "$(PS2SDK)/common/include" }
		optimize "Off"

	filter { "platforms:*amd64*" }
		architecture "x86_64"
	filter { "platforms:*arm64*" }
		architecture "ARM64"
	filter { "platforms:*x86*" }
		architecture "x86"
	filter { "platforms:*-arm-*" }
		architecture "ARM"

	filter { "platforms:win*" }
		system "windows"
	filter { "platforms:linux*" }
		system "linux"
	filter { "platforms:macos*" }
		system "macosx"

	filter { "platforms:win*gl3" }
		if _OPTIONS["gfxlib"] == "sdl2" then
			includedirs { path.join(_OPTIONS["sdl2dir"], "include") }
		end
	filter { "platforms:win-x86-gl3" }
		includedirs { path.join(_OPTIONS["glfwdir32"], "include") }
	filter { "platforms:win-amd64-gl3" }
		includedirs { path.join(_OPTIONS["glfwdir64"], "include") }

	filter "action:vs*"
		buildoptions { "/wd4996", "/wd4244" }
		
	filter { "platforms:win*gl3", "action:not vs*" }
		if _OPTIONS["gfxlib"] == "sdl2" then
			includedirs { "/mingw/include/SDL2" } -- TODO: Detect this properly
		end

	filter { "platforms:macos*gl3" }
		includedirs { "/opt/homebrew/include", "/usr/local/include" }

	filter {}

	Libdir = "lib/%{cfg.platform}/%{cfg.buildcfg}"
	Bindir = "bin/%{cfg.platform}/%{cfg.buildcfg}"

	libdirs { path.join(Librw, Libdir) }
	includedirs { Librw }
	includedirs { "src" }

project "librwgta"
	kind "StaticLib"
	targetname "rwgta"
	targetdir (Libdir)
	files { "src/*.*" }

function tool(dir)
	targetdir (Bindir)
	files { path.join("tools", dir, "*") }
	libdirs { Libdir }
	links { "rw", "librwgta" }
	findlibs()
end

function findlibs()
	filter { "platforms:linux*gl3" }
		links { "GL" }
		if _OPTIONS["gfxlib"] == "glfw" then
			links { "glfw" }
		else
			links { "SDL2" }
		end
	filter { "platforms:win-amd64-gl3" }
		libdirs { path.join(_OPTIONS["glfwdir64"], "lib-vc2015") }
		libdirs { path.join(_OPTIONS["sdl2dir"], "lib/x64") }
	filter { "platforms:win-x86-gl3" }
		libdirs { path.join(_OPTIONS["glfwdir32"], "lib-vc2015") }
		libdirs { path.join(_OPTIONS["sdl2dir"], "lib/x86") }
	filter { "platforms:win*gl3" }
		links { "opengl32" }
		if _OPTIONS["gfxlib"] == "glfw" then
			links { "glfw3" }
		else
			links { "SDL2" }
		end
	filter { "platforms:macos*gl3" }
		linkoptions { "-framework OpenGL", "-framework Cocoa", "-framework IOKit", "-framework CoreVideo" }
		libdirs { "/opt/homebrew/lib", "/usr/local/lib" }
		if _OPTIONS["gfxlib"] == "glfw" then
			links { "glfw" }
		else
			links { "SDL2" }
		end
	filter { "platforms:*d3d9" }
		links { "gdi32", "d3d9" }
	filter { "platforms:*d3d9", "action:vs*" }
		links { "Xinput9_1_0" }
	filter {}
end

function skeleton()
	files { path.join(Librw, "skeleton/*.cpp"), path.join(Librw, "skeleton/*.h") }
	files { path.join(Librw, "skeleton/imgui/*.cpp"), path.join(Librw, "skeleton/imgui/*.h") }
	includedirs { path.join(Librw, "skeleton") }
end

function skeltool(dir)
	targetdir (Bindir)
	files { path.join("tools", dir, "*.cpp"),
	        path.join("tools", dir, "*.h") }
	vpaths {
		{["src"] = { path.join("tools", dir, "*") }},
		{["skeleton"] = { "skeleton/*" }},
	}
	skeleton()
	debugdir ( path.join("tools", dir) )
	includedirs { "." }
	libdirs { Libdir }
	links { "rw", "librwgta" }
	findlibs()
end

project "selanna"
	tool("selanna")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }
	includedirs { luadir }
	links { "lua5.4" }

project "convdff"
	tool("convdff")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }

project "convtxd"
	tool("convtxd")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }

project "lcsconv"
	tool("storiesconv")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }
	defines { "LCS" }

project "vcsconv"
	tool("storiesconv")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }
	defines { "VCS" }

project "lcsview"
	kind "WindowedApp"
	characterset ("MBCS")
	entrypoint("WinMainCRTStartup")
	skeltool("storiesview")
	includedirs { "tools/storiesconv" }
	files { "tools/storiesconv/relocchunk.cpp" }
	files { "tools/storiesconv/rsl.cpp" }
	files { "tools/storiesconv/rslconv.cpp" }
	files { "tools/storiesconv/hash.cpp" }
	includedirs { Zlibdir }
	libdirs { Zlibdir }
	links { "zlib" }
	removeplatforms { "*null" }
	removeplatforms { "*amd64*" }
	removeplatforms { "ps2" } -- for now
	defines { "LCS" }
	debugdir "L:/."

project "vcsview"
	kind "WindowedApp"
	characterset ("MBCS")
	entrypoint("WinMainCRTStartup")
	skeltool("storiesview")
	includedirs { "tools/storiesconv" }
	files { "tools/storiesconv/relocchunk.cpp" }
	files { "tools/storiesconv/rsl.cpp" }
	files { "tools/storiesconv/rslconv.cpp" }
	files { "tools/storiesconv/hash.cpp" }
	includedirs { Zlibdir }
	libdirs { Zlibdir }
	links { "zlib" }
	removeplatforms { "*null" }
	removeplatforms { "*amd64*" }
	removeplatforms { "ps2" } -- for now
	defines { "VCS" }
	debugdir "V:/."

project "haumea"
	cppdialect "C++17"
	files { "vendor/imgui-texteditor/TextEditor.cpp", "vendor/imgui-texteditor/TextEditor.h" }
	includedirs { "vendor/imgui-texteditor", path.join(Librw, "skeleton/imgui") }
	kind "WindowedApp"
	targetname "haumea"
	characterset ("MBCS")
	defines { 'HAUMEA_CHANNEL="' .. _OPTIONS["channel"] .. '"' }
	-- ImGui's item hooks, used only by --ui-script tests (off at run time otherwise)
	defines { "IMGUI_ENABLE_TEST_ENGINE" }
	filter { "system:windows" }
		entrypoint("WinMainCRTStartup")
		-- UTF-8 as the process code page: paths with any characters work.
		files { "tools/haumea/haumea.exe.manifest" }
	links { "winhttp", "ws2_32" }
	-- An 8 MB main stack, as Linux gives it: Windows' 1 MB default made code
	-- that works there overflow (placing a prefab crashed with 0xC00000FD).
	filter { "system:windows", "toolset:msc*" }
		linkoptions { "/STACK:8388608" }
	filter { "system:windows", "toolset:not msc*" }
		linkoptions { "-Wl,--stack,8388608" }
	filter { "system:macosx" }
		kind "ConsoleApp"
	filter {}
	skeltool("haumea")
	filter { "system:windows", "platforms:*d3d9" }
		removefiles { path.join(Librw, "skeleton/imgui/imgui_impl_rw.cpp") }
		files { path.join(Librw, "skeleton/imgui/backends/imgui_impl_win32.cpp"),
		        path.join(Librw, "skeleton/imgui/backends/imgui_impl_dx9.cpp") }
		includedirs { path.join(Librw, "skeleton/imgui") }
		links { "dwmapi" }
	filter {}
	-- Debug information in release builds too, for crash reports: CI splits it
	-- out (haumea.debug, haumea.dSYM, haumea.pdb) into haumea-symbols-<version>.zip
	-- and ships the editor stripped.
	filter "configurations:Release*"
		symbols "On"
	filter {}
	includedirs { "tools/haumea" }
	files { "tools/haumea/minilzo/minilzo.c", "tools/haumea/monocypher/monocypher.c" }
	-- The build number and commit, as open.mp does it (tools/release/gen_buildinfo.py):
	-- remade every build, rewritten only when they change, so only versionutil follows.
	-- These lines come before the configs set OBJDIR, so a rule on
	-- $(OBJDIR)/versionutil.o would be /versionutil.o; prebuild runs before every
	-- object, and versionutil.d (-MD) has versionutil.o follow the header.
	filter { "action:gmake*" }
		makesettings {
			".DEFAULT_GOAL := all",
			"prebuild: ../tools/haumea/haumea_buildinfo.h",
			"../tools/haumea/haumea_buildinfo.h: FORCE",
			"\t$(SILENT) python3 ../tools/release/gen_buildinfo.py --header $@",
			".PHONY: FORCE",
			"FORCE:",
		}
	filter { "action:vs*" }
		prebuildcommands { 'python "$(SolutionDir)..\\tools\\release\\gen_buildinfo.py" --header "$(SolutionDir)..\\tools\\haumea\\haumea_buildinfo.h"' }
		files { "tools/haumea/haumea.rc" }
	filter {}
	filter { "system:linux" }
		links { "pthread" }
	filter {}
	removeplatforms { "*null" }
	removeplatforms { "ps2" }

-- A multiplayer session on its own (tools/haumea/mprelay.h): no game, no
-- window, nothing of librw, so it runs on a bare server.
project "haumea-server"
	kind "ConsoleApp"
	cppdialect "C++14"
	targetdir (Bindir)
	files { "tools/haumea-server/*.cpp", "tools/haumea/mprelay.cpp", "tools/haumea/mprelay.h",
	        "tools/haumea/mpprotocol.h", "tools/haumea/minilzo/minilzo.c", "tools/haumea/monocypher/monocypher.c" }
	includedirs { "tools/haumea" }
	filter { "system:windows" }
		links { "ws2_32" }
	filter {}
	removeplatforms { "ps2" }

project "gtaclumpview"
	kind "WindowedApp"
	characterset ("MBCS")
	entrypoint("WinMainCRTStartup")
	skeltool("gtaclumpview")
	includedirs { "tools/gtaclumpview" }
	removeplatforms { "*null" }
	removeplatforms { "ps2" } -- for now

project "convifp"
	tool("convifp")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }

project "coltest"
	tool("coltest")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }

project "d3d9test"
	tool("d3d9test")
	kind "WindowedApp"
	characterset ("MBCS")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*gl3", "*null" }
	links { "winmm" }

project "IIItest"
	tool("IIItest")
	removelinks { "librwgta" }
	kind "WindowedApp"
	characterset ("MBCS")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	files { path.join("tools/IIItest/*") }
	debugdir "C:/Users/aap/games/gta3"

project "radaredit"
	tool("radaredit")
	kind "ConsoleApp"

project "txdbuild"
	tool("txdbuild")
	kind "ConsoleApp"
	removeplatforms { "*gl3", "*d3d9" }

project "convcdimage"
	tool("convcdimage")
	kind "ConsoleApp"
	files { "tools/haumea/minilzo/minilzo.c" }
	removeplatforms { "*gl3", "*d3d9" }
