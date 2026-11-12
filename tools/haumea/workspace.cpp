// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <shlobj.h>
#include <commdlg.h>
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#endif

// The settings that say where everything is live in the config folder, so
// they can be read before anything else.
static const char *WORKSPACE_SETTINGS = "workspace.cfg";
static const char *MIGRATED_MARKER = "migrated-from-game-folder";
static const int MAX_RECENT_WORKSPACES = 8;

static std::string gGameDir;
static std::string gSuggestedGameDir;
static std::string gWorkspaceDir;
// What workspace.cfg holds; --game and --workspace only last for the run.
static std::string gSavedGameDir, gSavedWorkspaceDir;
static std::string gConfigDir;
static std::vector<std::string> gRecentWorkspaces;
// When each was last opened and whether it is pinned: "recent_info" lines,
// which older builds skip.
struct RecentInfo { long long lastOpened; bool pinned; };
static std::map<std::string, RecentInfo> gRecentInfo;
static bool gOpenLastWorkspace;	// skip the start screen
static bool gSetupNeeded;
static bool gInitialized;

#ifdef _WIN32
static const char SEP = '\\';
#else
static const char SEP = '/';
#endif

static bool
isSeparator(char c)
{
	return c == '/' || c == '\\';
}

static bool
isAbsolute(const char *path)
{
	if(path == nil || path[0] == '\0')
		return false;
	if(isSeparator(path[0]))
		return true;
	return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static bool
isDirectory(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool
isFile(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

static bool
makeDirectories(const std::string &path)
{
	std::string partial;
	for(size_t i = 0; i < path.size(); i++){
		partial += path[i];
		if((isSeparator(path[i]) || i + 1 == path.size()) && partial.size() > 1 &&
		   !(partial.size() == 3 && partial[1] == ':')){
#ifdef _WIN32
			_mkdir(partial.c_str());
#else
			mkdir(partial.c_str(), 0777);
#endif
		}
	}
	return isDirectory(path);
}

static std::string
joinPath(const std::string &dir, const std::string &name)
{
	if(dir.empty())
		return name;
	if(name.empty())
		return dir;
	return isSeparator(dir.back()) ? dir + name : dir + SEP + name;
}

// Absolute, with native separators and no trailing separator.
static std::string
normalizeDir(const char *dir)
{
	if(dir == nil || dir[0] == '\0')
		return "";
	std::string path = dir;
	if(!isAbsolute(dir)){
		char cwd[1024];
#ifdef _WIN32
		if(_getcwd(cwd, sizeof(cwd)))
#else
		if(getcwd(cwd, sizeof(cwd)))
#endif
			path = joinPath(cwd, path);
	}
	for(char &c : path)
		if(isSeparator(c))
			c = SEP;
	// Drop "." parts: /games/gta/. is /games/gta.
	std::string dot = std::string(1, SEP) + ".";
	for(size_t at; (at = path.find(dot + SEP)) != std::string::npos; )
		path.erase(at, 2);
	if(path.size() > 2 && path.compare(path.size() - 2, 2, dot) == 0)
		path.resize(path.size() - 2);
	while(path.size() > 1 && path.back() == SEP && !(path.size() == 3 && path[1] == ':'))
		path.pop_back();
	return path;
}

// The entry of dir whose name matches name ignoring case, or name itself.
static std::string
matchCase(const std::string &dir, const std::string &name)
{
#ifdef _WIN32
	return name;	// the file system ignores case already
#else
	struct stat st;
	std::string exact = joinPath(dir, name);
	if(stat(exact.c_str(), &st) == 0)
		return name;
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return name;
	std::string found = name;
	while(dirent *ent = readdir(d))
		if(strcasecmp(ent->d_name, name.c_str()) == 0){
			found = ent->d_name;
			break;
		}
	closedir(d);
	return found;
#endif
}

// base/<relative>, each part matched against what exists ignoring case.
static std::string
resolveUnder(const std::string &base, const char *relative)
{
	std::string path = base, part;
	bool exists = isDirectory(base);
	for(const char *p = relative; ; p++){
		if(*p == '\0' || isSeparator(*p)){
			if(!part.empty() && part != "."){
				if(exists)
					part = matchCase(path, part);
				path = joinPath(path, part);
				exists = exists && isDirectory(path);
			}
			part.clear();
			if(*p == '\0')
				break;
		}else
			part += *p;
	}
	return path;
}

static std::string
dirOfExecutable(void)
{
	char dir[1024];
	return GetEditorRootDirectory(dir, sizeof(dir)) ? normalizeDir(dir) : "";
}

static std::string
currentDir(void)
{
	return normalizeDir(".");
}

// ---- Config folder ----

const char*
GetConfigDir(void)
{
	if(gConfigDir.empty()){
		std::string dir;
		// Tests and Wine runs: Wine sets APPDATA itself, so it can't be pointed elsewhere.
		const char *custom = getenv("HAUMEA_CONFIG_DIR");
		if(custom && custom[0])
			dir = custom;
#ifdef _WIN32
		const char *appData = getenv("APPDATA");
		if(dir.empty() && appData && appData[0])
			dir = joinPath(appData, "Haumea");
#elif defined(__APPLE__)
		const char *home = getenv("HOME");
		if(dir.empty() && home && home[0])
			dir = joinPath(home, "Library/Application Support/Haumea");
#else
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		if(!dir.empty())
			;
		else if(xdg && xdg[0])
			dir = joinPath(xdg, "haumea");
		else if(home && home[0])
			dir = joinPath(home, ".config/haumea");
#endif
		// No home: keep everything next to the binary.
		if(dir.empty())
			dir = joinPath(dirOfExecutable(), "haumea_data");
		gConfigDir = normalizeDir(dir.c_str());
	}
	makeDirectories(gConfigDir);
	return gConfigDir.c_str();
}

bool
IsBackgroundWorker(void)
{
	static int known = -1;
	if(known < 0){
		known = 0;
		for(int i = 1; i < sk::args.argc; i++)
			if(strcmp(sk::args.argv[i], "--background") == 0)
				known = 1;
	}
	return known == 1;
}

bool
SettingsAreReadOnly(void)
{
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], "--pawn-roundtrip") == 0 ||
		   strcmp(sk::args.argv[i], "--textdraw-roundtrip") == 0 ||
		   strcmp(sk::args.argv[i], "--material-text-png") == 0 ||
		   strcmp(sk::args.argv[i], "--textdraw-colours") == 0 ||
		   strcmp(sk::args.argv[i], "--dump-zone-settings") == 0 ||
		   strcmp(sk::args.argv[i], "--undo-selftest") == 0 ||
		   strcmp(sk::args.argv[i], "--dump-categories") == 0 ||
		   strcmp(sk::args.argv[i], "--dump-model-meta") == 0 ||
		   strcmp(sk::args.argv[i], "--profile") == 0 ||
		   strcmp(sk::args.argv[i], "--no-save-settings") == 0 ||
		   strcmp(sk::args.argv[i], "--background") == 0)
			return true;
	return false;
}

bool
IsAutomatedRun(void)
{
	if(SettingsAreReadOnly() || getenv("HAUMEA_TEST_XVFB") != nil)
		return true;
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], "--ui-script") == 0)
			return true;
	return false;
}

static void
saveSettings(void)
{
	if(SettingsAreReadOnly())
		return;
	std::string path = joinPath(GetConfigDir(), WORKSPACE_SETTINGS);
	FILE *f = fopen(path.c_str(), "w");
	if(f == nil){
		log("Workspace: could not write %s\n", path.c_str());
		return;
	}
	if(!gSavedGameDir.empty())
		fprintf(f, "game %s\n", gSavedGameDir.c_str());
	if(!gSavedWorkspaceDir.empty())
		fprintf(f, "workspace %s\n", gSavedWorkspaceDir.c_str());
	for(const std::string &recent : gRecentWorkspaces)
		fprintf(f, "recent %s\n", recent.c_str());
	for(const std::string &recent : gRecentWorkspaces){
		const RecentInfo &info = gRecentInfo[recent];
		fprintf(f, "recent_info %lld %d %s\n", info.lastOpened, info.pinned ? 1 : 0, recent.c_str());
	}
	fprintf(f, "skip_start_screen %d\n", gOpenLastWorkspace ? 1 : 0);
	fclose(f);
}

static void
loadSettings(std::string *game, std::string *workspace)
{
	std::string path = joinPath(GetConfigDir(), WORKSPACE_SETTINGS);
	FILE *f = fopen(path.c_str(), "r");
	if(f == nil)
		return;
	char line[1200];
	while(fgets(line, sizeof(line), f)){
		size_t n = strlen(line);
		while(n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
			line[--n] = '\0';
		if(strncmp(line, "game ", 5) == 0)
			*game = line + 5;
		else if(strncmp(line, "workspace ", 10) == 0)
			*workspace = line + 10;
		else if(strncmp(line, "recent ", 7) == 0 &&
		        (int)gRecentWorkspaces.size() < MAX_RECENT_WORKSPACES * 2)
			gRecentWorkspaces.push_back(line + 7);
		else if(strncmp(line, "recent_info ", 12) == 0){
			long long when = 0;
			int pinned = 0, used = 0;
			if(sscanf(line + 12, "%lld %d %n", &when, &pinned, &used) >= 2 && used > 0)
				gRecentInfo[line + 12 + used] = { when, pinned != 0 };
		}else if(strncmp(line, "skip_start_screen ", 18) == 0)
			gOpenLastWorkspace = atoi(line + 18) != 0;
	}
	fclose(f);
}

// Copies files that are not in dst yet; returns how many were copied.
static int
copyMissing(const std::string &src, const std::string &dst)
{
	int copied = 0;
	std::vector<std::pair<std::string, bool>> entries;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA(joinPath(src, "*").c_str(), &entry);
	if(handle != INVALID_HANDLE_VALUE){
		do{
			if(strcmp(entry.cFileName, ".") && strcmp(entry.cFileName, ".."))
				entries.push_back({ entry.cFileName,
					(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 });
		}while(FindNextFileA(handle, &entry));
		FindClose(handle);
	}
#else
	if(DIR *d = opendir(src.c_str())){
		while(dirent *ent = readdir(d))
			if(strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
				entries.push_back({ ent->d_name, isDirectory(joinPath(src, ent->d_name)) });
		closedir(d);
	}
#endif
	makeDirectories(dst);
	for(const auto &entry : entries){
		std::string from = joinPath(src, entry.first), to = joinPath(dst, entry.first);
		if(entry.second){
			copied += copyMissing(from, to);
			continue;
		}
		if(isFile(to))
			continue;
		FILE *in = fopen(from.c_str(), "rb");
		FILE *out = in ? fopen(to.c_str(), "wb") : nil;
		if(in && out){
			char buffer[65536];
			size_t n;
			while((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
				fwrite(buffer, 1, n, out);
			copied++;
		}
		if(in) fclose(in);
		if(out) fclose(out);
	}
	return copied;
}

// Editor data used to live in <GTA>/haumea_data (and imgui.ini beside the
// binary). Copy it once; the old folder is left alone.
static void
migrateOldData(void)
{
	std::string marker = joinPath(GetConfigDir(), MIGRATED_MARKER);
	if(isFile(marker) || SettingsAreReadOnly())
		return;
	// Only the user's own config folder takes over the old data: a private
	// one (tests, HAUMEA_NO_MIGRATE) starts empty instead of inheriting
	// someone's recent maps.
	const char *noMigrate = getenv("HAUMEA_NO_MIGRATE");
	if(noMigrate && noMigrate[0] && strcmp(noMigrate, "0") != 0)
		return;
	const char *custom = getenv("HAUMEA_CONFIG_DIR");
	if(custom && custom[0])
		return;
#if !defined(_WIN32) && !defined(__APPLE__)
	const char *home = getenv("HOME");
	if(home && home[0] && gConfigDir != normalizeDir(joinPath(home, ".config/haumea").c_str()))
		return;
#endif
	std::vector<std::string> roots;
	if(!gGameDir.empty())
		roots.push_back(gGameDir);
	std::string exe = dirOfExecutable();
	if(!exe.empty() && exe != gGameDir)
		roots.push_back(exe);
	int copied = 0;
	for(const std::string &root : roots){
		std::string old = joinPath(root, "haumea_data");
		if(isDirectory(old) && old != gConfigDir)
			copied += copyMissing(old, gConfigDir);
		std::string ini = joinPath(root, "imgui.ini"), newIni = joinPath(gConfigDir, "imgui.ini");
		if(isFile(ini) && !isFile(newIni)){
			FILE *in = fopen(ini.c_str(), "rb"), *out = in ? fopen(newIni.c_str(), "wb") : nil;
			if(in && out){
				char buffer[65536];
				size_t n;
				while((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
					fwrite(buffer, 1, n, out);
				copied++;
			}
			if(in) fclose(in);
			if(out) fclose(out);
		}
	}
	if(copied)
		log("Workspace: moved %d file(s) of editor data to %s\n", copied, gConfigDir.c_str());
	if(FILE *f = fopen(marker.c_str(), "w")){
		fputs("Editor data from the game folder was copied here; the old copy is unused.\n", f);
		fclose(f);
	}
}

// ---- Checks ----

bool
CheckGameDir(const char *dir, std::string *problem)
{
	std::string why;
	std::string base = normalizeDir(dir);
	if(base.empty())
		why = _("Choose the GTA San Andreas folder.");
	else if(!isDirectory(base))
		why = _("This folder does not exist.");
	else{
		// An executable alone is not enough: loading uses these files before
		// a scene exists. Keep missing installs on the folder chooser instead
		// of reaching FindVersion's fatal exit or null texture consumers.
		static const char *required[] = {
			"data/gta.dat", "data/default.dat", "models/gta3.img",
			"models/gta_int.img", "models/particle.txd", "data/timecyc.dat",
		};
		std::string missing;
		for(const char *relative : required){
			std::string path = resolveUnder(base, relative);
			FILE *f = fopen(path.c_str(), "rb");
			bool readable = f && fgetc(f) != EOF;
			if(f) fclose(f);
			if(!readable){
				if(!missing.empty()) missing += ", ";
				missing += relative;
			}
		}
		if(!missing.empty()){
			char message[1024];
			snprintf(message, sizeof(message), _("Missing or unreadable GTA files: %s. Restore them or choose another GTA San Andreas folder."), missing.c_str());
			why = message;
		}
	}
	if(problem)
		*problem = why;
	return why.empty();
}

bool
CheckWorkspaceDir(const char *dir, std::string *warning)
{
	std::string why;
	std::string base = normalizeDir(dir);
	static const char *const markers[] = {
		"gamemodes", "filterscripts", "scriptfiles", "npcmodes",
		"omp-server", "omp-server.exe", "samp03svr", "samp-server.exe", "config.json", "server.cfg",
	};
	if(base.empty())
		why = _("No workspace: maps open and save wherever you choose.");
	else if(!isDirectory(base))
		why = _("This folder does not exist.");
	else{
		bool server = false;
		for(const char *marker : markers)
			server |= isDirectory(resolveUnder(base, marker)) || isFile(resolveUnder(base, marker));
		if(!server)
			why = _("This does not look like a server folder (no gamemodes/, scriptfiles/ or server "
				"executable). It can still be the workspace.");
		else if(!gGameDir.empty() && IsInGameDir(base.c_str()))
			why = _("This folder is inside the game folder, which Haumea never writes to.");
	}
	if(warning)
		*warning = why;
	return why.empty();
}

// ---- Paths ----

const char*
GetGameDir(void)
{
	return gGameDir.c_str();
}

const char*
GetSuggestedGameDir(void)
{
	return gGameDir.empty() ? gSuggestedGameDir.c_str() : gGameDir.c_str();
}

const char*
GetWorkspaceDir(void)
{
	return gWorkspaceDir.c_str();
}

std::string
GamePath(const char *relative)
{
	if(relative == nil || relative[0] == '\0')
		return gGameDir;
	if(isAbsolute(relative) || gGameDir.empty()){
		std::string path = relative;
		for(char &c : path)
			if(isSeparator(c))
				c = SEP;
#ifndef _WIN32
		// Logical names are often lowercased: match each part from the root.
		struct stat st;
		if(path[0] == SEP && stat(path.c_str(), &st) != 0)
			path = resolveUnder("/", path.c_str() + 1);
#endif
		return path;
	}
	return resolveUnder(gGameDir, relative);
}

bool
GamePath(const char *relative, char *dst, size_t size)
{
	std::string path = GamePath(relative);
	return snprintf(dst, size, "%s", path.c_str()) < (int)size;
}

std::string
WorkspacePath(const char *relative)
{
	if(gWorkspaceDir.empty())
		return "";
	std::string path = gWorkspaceDir;
	if(relative && relative[0])
		path = resolveUnder(gWorkspaceDir, relative);
	return path;
}

std::string
WorkspaceDefaultDir(const char *relative)
{
	std::string path = WorkspacePath(relative);
	if(!path.empty() && isDirectory(path))
		return path;
	return isDirectory(gWorkspaceDir) ? gWorkspaceDir : "";
}

// The path with symlinks and ".." resolved, as far as it exists: a link to
// the game folder or "x/../GTA" is still the game folder.
static std::string
resolvedPath(const std::string &path)
{
#ifdef _WIN32
	char full[MAX_PATH * 2];
	DWORD n = GetFullPathNameA(path.c_str(), sizeof(full), full, nil);
	return n > 0 && n < sizeof(full) ? normalizeDir(full) : path;
#else
	// realpath() needs an existing path: resolve the deepest existing parent
	// and keep the rest.
	std::string head = path, tail;
	while(!head.empty()){
		char *real = realpath(head.c_str(), nil);
		if(real){
			std::string out = real;
			free(real);
			return normalizeDir((out + tail).c_str());
		}
		size_t slash = head.find_last_of(SEP);
		if(slash == std::string::npos || slash == 0)
			break;
		tail = head.substr(slash) + tail;
		head.resize(slash);
	}
	return path;
#endif
}

bool
IsInGameDir(const char *path)
{
	if(gGameDir.empty() || path == nil || path[0] == '\0')
		return false;
	std::string full = resolvedPath(normalizeDir(path)), game = resolvedPath(gGameDir);
#ifdef _WIN32
	std::transform(full.begin(), full.end(), full.begin(), ::tolower);
	std::transform(game.begin(), game.end(), game.begin(), ::tolower);
#endif
	if(full.compare(0, game.size(), game) != 0)
		return false;
	return full.size() == game.size() || full[game.size()] == SEP;
}

// ---- Settings ----

bool
SetGameDir(const char *dir, std::string *problem)
{
	if(!CheckGameDir(dir, problem))
		return false;
	gGameDir = gSavedGameDir = normalizeDir(dir);
	saveSettings();
	return true;
}

void
SetWorkspaceDir(const char *dir)
{
	gWorkspaceDir = gSavedWorkspaceDir = normalizeDir(dir);
	if(!gWorkspaceDir.empty()){
		gRecentWorkspaces.erase(std::remove(gRecentWorkspaces.begin(), gRecentWorkspaces.end(),
			gWorkspaceDir), gRecentWorkspaces.end());
		gRecentWorkspaces.insert(gRecentWorkspaces.begin(), gWorkspaceDir);
		gRecentInfo[gWorkspaceDir].lastOpened = (long long)time(nil);
		// Too many: the oldest unpinned one goes, so pinned ones stay, up to
		// twice as many (what is read back); past that the oldest pinned one.
		while((int)gRecentWorkspaces.size() > MAX_RECENT_WORKSPACES){
			int drop = -1;
			for(int i = (int)gRecentWorkspaces.size() - 1; i > 0 && drop < 0; i--)
				if(!gRecentInfo[gRecentWorkspaces[i]].pinned)
					drop = i;
			if(drop < 0 && (int)gRecentWorkspaces.size() > MAX_RECENT_WORKSPACES * 2)
				drop = (int)gRecentWorkspaces.size() - 1;
			if(drop < 0)
				break;
			gRecentInfo.erase(gRecentWorkspaces[drop]);
			gRecentWorkspaces.erase(gRecentWorkspaces.begin() + drop);
		}
	}
	saveSettings();
}

const std::vector<std::string>&
GetRecentWorkspaces(void)
{
	return gRecentWorkspaces;
}

std::vector<RecentWorkspace>
GetRecentWorkspaceList(void)
{
	std::vector<RecentWorkspace> list;
	for(const std::string &path : gRecentWorkspaces){
		const RecentInfo &info = gRecentInfo[path];
		list.push_back({ path, info.lastOpened, info.pinned });
	}
	// Pinned first; otherwise the order they were last opened in.
	std::stable_sort(list.begin(), list.end(), [](const RecentWorkspace &a, const RecentWorkspace &b){
		return a.pinned && !b.pinned;
	});
	return list;
}

void
PinRecentWorkspace(const char *path, bool pinned)
{
	gRecentInfo[normalizeDir(path)].pinned = pinned;
	saveSettings();
}

void
RemoveRecentWorkspace(const char *path)
{
	std::string dir = normalizeDir(path);
	gRecentWorkspaces.erase(std::remove(gRecentWorkspaces.begin(), gRecentWorkspaces.end(), dir),
		gRecentWorkspaces.end());
	gRecentInfo.erase(dir);
	saveSettings();
}

bool
GetOpenLastWorkspace(void)
{
	return gOpenLastWorkspace;
}

void
SetOpenLastWorkspace(bool open)
{
	gOpenLastWorkspace = open;
	saveSettings();
}

static const char*
argumentValue(const char *name)
{
	for(int i = 1; i + 1 < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], name) == 0)
			return sk::args.argv[i + 1];
	return nil;
}

void
InitWorkspace(void)
{
	if(gInitialized)
		return;
	gInitialized = true;
	std::string savedGame, savedWorkspace;
	loadSettings(&savedGame, &savedWorkspace);
	gSavedGameDir = normalizeDir(savedGame.c_str());
	gSavedWorkspaceDir = normalizeDir(savedWorkspace.c_str());

	// The game folder: --game, the saved one, $HAUMEA_GAME_DIR, then the old
	// drop-in layout (started from, or installed in, the game folder).
	std::vector<std::string> candidates;
	if(const char *arg = argumentValue("--game"))
		candidates.push_back(arg);
	candidates.push_back(savedGame);
	if(const char *env = getenv("HAUMEA_GAME_DIR"))
		candidates.push_back(env);
	// Keep the explicitly chosen/saved path visible when it is incomplete.
	// The user can repair it in place or browse to another installation.
	for(const std::string &candidate : candidates)
		if(!candidate.empty()){ gSuggestedGameDir = normalizeDir(candidate.c_str()); break; }
	candidates.push_back(currentDir());
	candidates.push_back(dirOfExecutable());
	for(const std::string &candidate : candidates)
		if(!candidate.empty() && CheckGameDir(candidate.c_str(), nil)){
			gGameDir = normalizeDir(candidate.c_str());
			break;
		}
	gSetupNeeded = gGameDir.empty();

	const char *argWorkspace = argumentValue("--workspace");
	gWorkspaceDir = normalizeDir(argWorkspace ? argWorkspace : savedWorkspace.c_str());

	if(!gGameDir.empty()){
		migrateOldData();
		// Remember a game folder found from the command line or the old
		// layout, so Haumea also starts from anywhere next time.
		if(gSavedGameDir.empty() && argumentValue("--game") == nil){
			gSavedGameDir = gGameDir;
			saveSettings();
		}
		log("Game folder: %s\n", gGameDir.c_str());
	}
	if(!gWorkspaceDir.empty())
		log("Workspace: %s\n", gWorkspaceDir.c_str());
}

// ---- Folder picker ----

bool
FileDialogAvailable(void)
{
#if defined(_WIN32) || defined(__APPLE__)
	return true;
#else
	// Checked again until found, so installing zenity needs no restart.
	static bool found = false;
	if(!found)
		found = system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") == 0;
	if(!found)
		ToastFor(TOAST_SAVE, 8.0f, _("File dialogs need zenity or kdialog. Install one "
			"(sudo apt install zenity) or type the path."));
	return found;
#endif
}

bool
PickFolderDialog(char *dst, size_t size, const char *title, const char *startDir)
{
	if(dst == nil || size == 0)
		return false;
#ifdef _WIN32
	BROWSEINFOA bi = {};
	char name[MAX_PATH] = "";
	bi.pszDisplayName = name;
	bi.lpszTitle = title;
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	LPITEMIDLIST item = SHBrowseForFolderA(&bi);
	if(item == nil)
		return false;
	char path[MAX_PATH];
	bool ok = SHGetPathFromIDListA(item, path) != FALSE;
	CoTaskMemFree(item);
	if(!ok)
		return false;
	snprintf(dst, size, "%s", path);
	return true;
#else
	auto quote = [](const char *text){
		std::string quoted = "'";
		for(const char *p = text ? text : ""; *p; p++)
			quoted += *p == '\'' ? std::string("'\\''") : std::string(1, *p);
		return quoted + "'";
	};
	std::string start = startDir && startDir[0] ? std::string(startDir) + "/" : "";
#ifdef __APPLE__
	std::string script = std::string("POSIX path of (choose folder with prompt \"") + (title ? title : "") + "\"" +
		(start.empty() ? "" : " default location (POSIX file \"" + start + "\")") + ")";
	std::string command = "osascript -e " + quote(script.c_str());
#else
	if(!FileDialogAvailable())
		return false;
	std::string script = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --directory --title=" +
		quote(title) + (start.empty() ? "" : " --filename=" + quote(start.c_str())) +
		"; elif command -v kdialog >/dev/null 2>&1; then kdialog --getexistingdirectory " +
		quote(start.empty() ? "." : start.c_str()) + "; fi";
	std::string command = "sh -c " + quote(script.c_str());
#endif
	FILE *pipe = popen(command.c_str(), "r");
	if(pipe == nil)
		return false;
	bool ok = fgets(dst, (int)size, pipe) != nil;
	pclose(pipe);
	if(!ok)
		return false;
	size_t n = strlen(dst);
	while(n > 0 && (dst[n-1] == '\n' || dst[n-1] == '\r' || (n > 1 && dst[n-1] == '/')))
		dst[--n] = '\0';
	return dst[0] != '\0';
#endif
}

bool
PickFileDialog(char *dst, size_t size, const char *title, const char *kind, const char *extensions)
{
	if(dst == nil || size == 0)
		return false;
	std::vector<std::string> exts;
	for(const char *p = extensions ? extensions : ""; *p; ){
		while(*p == ' ') p++;
		const char *e = p;
		while(*e && *e != ' ') e++;
		if(e > p)
			exts.push_back(std::string(p, e - p));
		p = e;
	}
#ifdef _WIN32
	// "Images (*.png;*.bmp)\0*.png;*.bmp\0All files\0*.*\0\0"
	std::string patterns;
	for(const std::string &e : exts)
		patterns += (patterns.empty() ? "*." : ";*.") + e;
	std::string filter = std::string(kind ? kind : "") + " (" + patterns + ")" + '\0' + patterns + '\0' +
		"*.*" + '\0' + "*.*" + '\0' + '\0';
	char filename[MAX_PATH] = "";
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = filter.c_str();
	ofn.lpstrFile = filename;
	ofn.nMaxFile = sizeof(filename);
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if(!GetOpenFileNameA(&ofn))
		return false;
	snprintf(dst, size, "%s", filename);
	return true;
#else
	auto quote = [](const std::string &text){
		std::string quoted = "'";
		for(char c : text)
			quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
		return quoted + "'";
	};
	std::string patterns;
	for(const std::string &e : exts)
		patterns += (patterns.empty() ? "*." : " *.") + e;
#ifdef __APPLE__
	std::string types;
	for(const std::string &e : exts)
		types += (types.empty() ? "\"" : ", \"") + e + "\"";
	std::string script = std::string("POSIX path of (choose file with prompt \"") + (title ? title : "") + "\"" +
		(types.empty() ? "" : " of type {" + types + "}") + ")";
	std::string command = "osascript -e " + quote(script);
#else
	if(!FileDialogAvailable())
		return false;
	std::string filter = std::string(kind ? kind : "") + " (" + patterns + ")";
	std::string script = "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title=" +
		quote(title ? title : "") + " --file-filter=" + quote(std::string(kind ? kind : "") + " | " + patterns) +
		"; elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename . " + quote(filter) + "; fi";
	std::string command = "sh -c " + quote(script);
#endif
	FILE *pipe = popen(command.c_str(), "r");
	if(pipe == nil)
		return false;
	bool ok = fgets(dst, (int)size, pipe) != nil;
	pclose(pipe);
	if(!ok)
		return false;
	size_t n = strlen(dst);
	while(n > 0 && (dst[n-1] == '\n' || dst[n-1] == '\r'))
		dst[--n] = '\0';
	return dst[0] != '\0';
#endif
}

// ---- UI ----

static void
folderField(const char *label, const char *id, char *buffer, size_t size, const char *dialogTitle)
{
	ImGui::TextUnformatted(label);
	ImGui::PushID(id);
	float browse = ImGui::CalcTextSize(_("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputText("##path", buffer, size);
	ImGui::SameLine();
	// The first-run window draws no toasts, so say it here too.
	ImGuiStorage *storage = ImGui::GetStateStorage();
	ImGuiID noDialog = ImGui::GetID("no dialog");
	if(ImGui::Button(_("Browse..."))){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), dialogTitle, buffer))
			snprintf(buffer, size, "%s", picked);
		storage->SetBool(noDialog, !FileDialogAvailable());
	}
	if(storage->GetBool(noDialog))
		ImGui::TextDisabled(_("Browse needs zenity or kdialog: type or paste the folder's path."));
	ImGui::PopID();
}

static void
statusLine(bool ok, const std::string &message, bool warningOnly)
{
	if(message.empty())
		return;
	ImVec4 colour = ok ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) :
		warningOnly ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Text, colour);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(message.c_str());
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

bool
WorkspaceSetupNeeded(void)
{
	return gSetupNeeded || !CheckGameDir(gGameDir.c_str(), nil);
}

// The first-run choice from the start screen: the game folder (checked
// by the caller) and the workspace, then loading can start.
void
FinishWorkspaceSetup(const char *game, const char *workspace)
{
	if(!SetGameDir(game, nil))
		return;
	SetWorkspaceDir(workspace);
	migrateOldData();
	gSetupNeeded = false;
}

bool
uiWorkspaceSettings(void)
{
	static char game[1024], workspace[1024];
	static std::string shownGame, shownWorkspace;
	static bool restartNeeded;
	// Follow changes made elsewhere (recent workspaces, the agent).
	if(shownGame != gGameDir && !restartNeeded){
		shownGame = gGameDir;
		snprintf(game, sizeof(game), "%s", gGameDir.c_str());
	}
	if(shownWorkspace != gWorkspaceDir){
		shownWorkspace = gWorkspaceDir;
		snprintf(workspace, sizeof(workspace), "%s", gWorkspaceDir.c_str());
	}
	bool changedGame = false;
	ImGui::PushID("WorkspaceSettings");

	folderField(_("GTA San Andreas folder (read only)"), "game", game, sizeof(game), _("GTA San Andreas folder"));
	std::string problem;
	bool gameOk = CheckGameDir(game, &problem);
	bool gameEdited = normalizeDir(game) != gGameDir;
	if(gameEdited || !gameOk)
		statusLine(gameOk, gameOk ? _("GTA San Andreas found.") : problem, false);
	if(gameEdited){
		ImGui::BeginDisabled(!gameOk);
		if(ImGui::Button(_("Use this game folder"))){
			SetGameDir(game, nil);
			restartNeeded = changedGame = true;
		}
		ImGui::EndDisabled();
	}
	if(restartNeeded){
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), _("The new game folder loads when Haumea restarts."));
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Restart now")))
			RestartHaumea();
	}
	ImGui::Spacing();

	folderField(_("Workspace (server folder)"), "workspace", workspace, sizeof(workspace), _("Server folder"));
	std::string warning;
	bool serverOk = CheckWorkspaceDir(workspace, &warning);
	if(!serverOk || workspace[0])
		statusLine(serverOk, serverOk ? _("Server folder found.") : warning, true);
	bool edited = normalizeDir(workspace) != gWorkspaceDir;
	if(edited && ImGui::Button(_("Use this workspace")))
		SetWorkspaceDir(workspace);
	if(!gWorkspaceDir.empty()){
		// Beside Use, not after the status line, where it ran off the window.
		if(edited)
			ImGui::SameLine();
		if(ImGui::Button(_("No workspace")))
			SetWorkspaceDir(nil);
	}
	ImGui::TextDisabled(_("Settings and editor data: %s"), GetConfigDir());
	ImGui::PopID();
	return changedGame;
}

// The user's Documents folder (wherever Windows keeps it, OneDrive included;
// XDG_DOCUMENTS_DIR or ~/Documents elsewhere), with a "Haumea Studio" folder
// in it for the exports, so they are easy to find afterwards.
bool
GetUserDocumentsPath(char *dst, size_t size, const char *name)
{
	std::string docs;
#ifdef _WIN32
	char personal[MAX_PATH] = {};
	if(SHGetFolderPathA(nil, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, nil, 0, personal) == S_OK && personal[0])
		docs = personal;
	else if(const char *profile = getenv("USERPROFILE"))
		docs = std::string(profile) + "\\Documents";
#else
	if(const char *xdg = getenv("XDG_DOCUMENTS_DIR"))
		docs = xdg;
	else if(const char *home = getenv("HOME"))
		docs = std::string(home) + "/Documents";
#endif
	if(docs.empty())
		return GetHaumeaDataPath(dst, size, name);
	char dir[1024], probe[1100];
	if(!BuildPath(dir, sizeof(dir), docs.c_str(), "Haumea Studio") ||
	   !BuildPath(probe, sizeof(probe), dir, "x") || !EnsureParentDirectoriesForPath(probe))
		return GetHaumeaDataPath(dst, size, name);
	if(name == nil || name[0] == '\0')
		return snprintf(dst, size, "%s", dir) < (int)size;
	return BuildPath(dst, size, dir, name);
}
