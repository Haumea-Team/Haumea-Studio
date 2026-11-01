#include "haumea.h"
#include "discordpresence.h"
#include "i18n.h"
#include "crashreports.h"
#include "workspace.h"
#include "modelmeta.h"
#ifdef LIBRW_GLFW
#include <GLFW/glfw3.h>
#endif
#include "version.h"
#include "versionutil.h"
#include "haumea_icon.h"
#include "updater.h"
#include "telemetry.h"
#include "modloader.h"
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

//#define XINPUT
#ifdef XINPUT
#include <xinput.h>
#endif

float timeStep;
float avgTimeStep;

SceneGlobals Scene;
rw::EngineOpenParams engineOpenParams;
rw::Light *pAmbient, *pDirect;
rw::Texture *whiteTex;
static char gHotReloadTracePath[1024];
static char gImGuiIniPath[1024];

// Windows restarts pass it (the new process waits for the old one); a
// restart elsewhere never forwards it.
static const char *RESTART_PARENT_PID_ARG = "--haumea-restart-parent-pid";

static bool
ResolveEditorExecutablePath(char *path, size_t size)
{
	if(path == nil || size == 0)
		return false;

#ifdef _WIN32
	DWORD len = GetModuleFileNameA(nil, path, (DWORD)size);
	return len > 0 && len < size;
#elif defined(__APPLE__)
	uint32_t pathSize = (uint32_t)size;
	if(_NSGetExecutablePath(path, &pathSize) != 0)
		return false;
	char resolved[1024];
	if(realpath(path, resolved)){
		strncpy(path, resolved, size - 1);
		path[size - 1] = '\0';
	}
	return true;
#else
	ssize_t len = readlink("/proc/self/exe", path, size - 1);
	if(len <= 0 || (size_t)len >= size)
		return false;
	path[len] = '\0';
	return true;
#endif
}

// Keep the launch location before the updater moves the running executable.
// On Linux /proc/self/exe follows that move into .haumea-previous/files;
// resolving it again would relaunch the old version from the backup folder.
static bool
GetEditorExecutablePath(char *path, size_t size)
{
	static const std::string launchPath = []{
		char resolved[1024];
		return ResolveEditorExecutablePath(resolved, sizeof(resolved)) ? std::string(resolved) : std::string();
	}();
	if(path == nil || launchPath.empty() || size <= launchPath.size())
		return false;
	memcpy(path, launchPath.c_str(), launchPath.size() + 1);
	return true;
}

#ifdef _WIN32
static void
WaitForRestartParentIfRequested(void)
{
	for(int i = 1; i + 1 < sk::args.argc; i++){
		if(strcmp(sk::args.argv[i], RESTART_PARENT_PID_ARG) != 0)
			continue;

		char *end = nil;
		unsigned long pid = strtoul(sk::args.argv[i + 1], &end, 10);
		if(end == sk::args.argv[i + 1] || *end != '\0' || pid == 0)
			return;

		HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
		if(parent){
			WaitForSingleObject(parent, INFINITE);
			CloseHandle(parent);
		}
		return;
	}
}
#endif

bool
GetHaumeaExecutablePath(char *path, size_t size)
{
	return GetEditorExecutablePath(path, size);
}

// This run's arguments, without a restart's own --haumea-restart-parent-pid.
static std::vector<std::string>
forwardedArgs(void)
{
	std::vector<std::string> args;
	for(int i = 1; i < sk::args.argc; i++){
		if(strcmp(sk::args.argv[i], RESTART_PARENT_PID_ARG) == 0){
			i++;
			continue;
		}
		args.push_back(sk::args.argv[i]);
	}
	return args;
}

#ifdef _WIN32
// One argument for CreateProcess's command line, quoted the way the C runtime
// splits it again.
static void
appendQuotedArg(std::string &line, const std::string &arg)
{
	line += " \"";
	int slashes = 0;
	for(char c : arg){
		if(c == '\\'){ slashes++; continue; }
		line.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
		slashes = 0;
		line += c;
	}
	line.append(slashes * 2, '\\');
	line += '"';
}
#endif

static bool
restartHaumea(bool keepArgs)
{
	// The new process finds the game folder in the saved settings, so a
	// changed one takes effect; the working directory does not matter.
	char exePath[1024];
	if(!GetEditorExecutablePath(exePath, sizeof(exePath)))
		return false;
	std::vector<std::string> args;
	if(keepArgs)
		args = forwardedArgs();

#ifdef _WIN32
	std::string commandLine = std::string("\"") + exePath + "\" " + RESTART_PARENT_PID_ARG + " " +
		std::to_string((unsigned long)GetCurrentProcessId());
	for(const std::string &a : args)
		appendQuotedArg(commandLine, a);

	STARTUPINFOA startupInfo = {};
	PROCESS_INFORMATION processInfo = {};
	startupInfo.cb = sizeof(startupInfo);
	if(!CreateProcessA(exePath, &commandLine[0], nil, nil, FALSE, 0, nil, nil,
	                   &startupInfo, &processInfo))
		return false;

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
#else
	// argv is built before the fork: only async-signal-safe calls after it.
	std::vector<char*> argv;
	argv.push_back(exePath);
	for(std::string &a : args)
		argv.push_back(&a[0]);
	argv.push_back(nil);
	pid_t parentPid = getpid();
	pid_t childPid = fork();
	if(childPid < 0)
		return false;
	if(childPid == 0){
		// The child stays invisible until the old Haumea process has fully exited.
		while(getppid() == parentPid)
			usleep(50000);
		execv(exePath, argv.data());
		_exit(127);
	}
#endif

	sk::globals.quit = 1;
	return true;
}

bool
RestartHaumea(void)
{
	return restartHaumea(false);
}

bool
RestartHaumeaKeepingArgs(void)
{
	return restartHaumea(true);
}

void
ExecHaumeaInPlace(void)
{
	char exePath[1024];
	if(!GetEditorExecutablePath(exePath, sizeof(exePath)))
		return;
	std::vector<std::string> args = forwardedArgs();
#ifdef _WIN32
	std::string commandLine = std::string("\"") + exePath + "\"";
	for(const std::string &a : args)
		appendQuotedArg(commandLine, a);
	STARTUPINFOA startupInfo = {};
	PROCESS_INFORMATION processInfo = {};
	startupInfo.cb = sizeof(startupInfo);
	if(!CreateProcessA(exePath, &commandLine[0], nil, nil, FALSE, 0, nil, nil,
	                   &startupInfo, &processInfo))
		return;
	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	exit(0);
#else
	std::vector<char*> argv;
	argv.push_back(exePath);
	for(std::string &a : args)
		argv.push_back(&a[0]);
	argv.push_back(nil);
	execv(exePath, argv.data());
#endif
}

static bool
EnsureDirectoryTree(const char *path)
{
	if(path == nil || path[0] == '\0')
		return false;

	char dirpath[1024];
	strncpy(dirpath, path, sizeof(dirpath)-1);
	dirpath[sizeof(dirpath)-1] = '\0';

	for(char *p = dirpath; *p; p++){
		if(*p != '/' && *p != '\\')
			continue;
		char saved = *p;
		*p = '\0';
		if(dirpath[0] != '\0'){
#ifdef _WIN32
			_mkdir(dirpath);
#else
			mkdir(dirpath, 0777);
#endif
		}
		*p = saved;
	}
#ifdef _WIN32
	_mkdir(dirpath);
	DWORD attributes = GetFileAttributesA(dirpath);
	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	mkdir(dirpath, 0777);
	struct stat info;
	return stat(dirpath, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

// TODO: print to log as well
void
panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "error: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

void
debug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	addToLogWindow(fmt, ap);
	fflush(stdout);
	va_end(ap);
}

// TODO: do something fancy
void
log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char line[1024];
	va_list copy;
	va_copy(copy, ap);
	vsnprintf(line, sizeof(line), fmt, copy);
	va_end(copy);
	fputs(line, stdout);
	CrashReportsLog(line);	// the last lines go in a crash report
	addToLogWindow(fmt, ap);
	fflush(stdout);
	va_end(ap);
}

bool
GetEditorRootDirectory(char *dir, size_t size)
{
	if(size == 0)
		return false;

#ifdef _WIN32
	DWORD len = GetModuleFileNameA(nil, dir, (DWORD)size);
	if(len > 0 && len < size){
		for(int i = (int)len - 1; i >= 0; i--){
			if(dir[i] == '\\' || dir[i] == '/'){
				dir[i] = '\0';
				return true;
			}
		}
	}

	len = GetCurrentDirectoryA((DWORD)size, dir);
	return len > 0 && len < size;
#elif defined(__APPLE__)
	uint32_t bufsize = (uint32_t)size;
	if(_NSGetExecutablePath(dir, &bufsize) == 0){
		char *slash = strrchr(dir, '/');
		if(slash){ *slash = '\0'; return true; }
	}
	strncpy(dir, ".", size);
	dir[size - 1] = '\0';
	return true;
#else
	ssize_t len = readlink("/proc/self/exe", dir, size - 1);
	if(len > 0){
		dir[len] = '\0';
		char *slash = strrchr(dir, '/');
		if(slash){ *slash = '\0'; return true; }
	}
	strncpy(dir, ".", size);
	dir[size - 1] = '\0';
	return true;
#endif
}

bool
GetGameRootDirectory(char *dir, size_t size)
{
	if(size == 0)
		return false;

	// The game folder from the workspace settings, never the working
	// directory: Haumea runs from its own folder.
	const char *game = GetGameDir();
	if(game[0] == '\0')
		return false;
	return snprintf(dir, size, "%s", game) < (int)size;
}

bool
BuildPath(char *dst, size_t size, const char *dir, const char *name)
{
	if(size == 0)
		return false;
	if(dir == nil || dir[0] == '\0')
		return snprintf(dst, size, "%s", name) < (int)size;

	size_t len = strlen(dir);
#ifdef _WIN32
	const char *sep = (dir[len-1] == '\\' || dir[len-1] == '/') ? "" : "\\";
#else
	const char *sep = (dir[len-1] == '/') ? "" : "/";
#endif
	return snprintf(dst, size, "%s%s%s", dir, sep, name) < (int)size;
}

bool
EnsureParentDirectoriesForPath(const char *path)
{
	if(path == nil || path[0] == '\0')
		return false;
	// Every save goes through here: nothing is ever written to the game folder.
	if(IsInGameDir(path)){
		log("Refusing to write %s: Haumea never writes to the game folder\n", path);
		return false;
	}

	char dirpath[1024];
	strncpy(dirpath, path, sizeof(dirpath)-1);
	dirpath[sizeof(dirpath)-1] = '\0';
	char *slash = strrchr(dirpath, '/');
#ifdef _WIN32
	char *backslash = strrchr(dirpath, '\\');
	if(backslash && (slash == nil || backslash > slash))
		slash = backslash;
#endif
	if(slash == nil)
		return true;
	*slash = '\0';
	if(dirpath[0] == '\0')
		return true;
	return EnsureDirectoryTree(dirpath);
}

// Editor data lives in the config folder (~/.config/haumea, %APPDATA%\Haumea);
// InitWorkspace moved the old <GTA>/haumea_data there.
bool
GetHaumeaDataDirectory(char *dir, size_t size)
{
	return snprintf(dir, size, "%s", GetConfigDir()) < (int)size && EnsureDirectoryTree(dir);
}

bool
GetHaumeaDataPath(char *dst, size_t size, const char *name)
{
	char dir[1024];
	return GetHaumeaDataDirectory(dir, sizeof(dir)) &&
	       BuildPath(dst, size, dir, name);
}

FILE*
fopenHaumeaDataRead(const char *name, const char *legacyName)
{
	char path[1024];
	FILE *f = nil;

	if(name && GetHaumeaDataPath(path, sizeof(path), name))
		f = fopen(path, "r");
	if(f == nil && legacyName)
		f = fopen_ci(legacyName, "r");
	return f;
}

FILE*
fopenHaumeaDataWrite(const char *name)
{
	char path[1024];
	// Developer and test runs (--pawn-roundtrip) leave the user's settings alone.
	if(SettingsAreReadOnly())
		return nil;
	if(name == nil || !GetHaumeaDataPath(path, sizeof(path), name) ||
	   !EnsureParentDirectoriesForPath(path))
		return nil;
	return fopen(path, "w");
}

void
setHotReloadTracePath(const char *path)
{
	if(path == nil){
		gHotReloadTracePath[0] = '\0';
		return;
	}
	strncpy(gHotReloadTracePath, path, sizeof(gHotReloadTracePath));
	gHotReloadTracePath[sizeof(gHotReloadTracePath)-1] = '\0';
}

void
hotReloadTrace(const char *fmt, ...)
{
	char defaultPath[1024];
	const char *path = gHotReloadTracePath;
	if(path[0] == '\0'){
		if(GetHaumeaDataPath(defaultPath, sizeof(defaultPath), "haumea_hot_reload_log.txt"))
			path = defaultPath;
		else
			path = "haumea_hot_reload_log.txt";
	}
	FILE *f = fopen(path, "a");
	if(f == nil)
		return;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

char*
getPath(const char *path)
{
	// A game file: relative paths are in the game folder.
	static char cipath[1024];
	GamePath(path, cipath, sizeof(cipath));
	return cipath;
}

/* case insensitive fopen of a game file; relative paths are in the game folder */
FILE*
fopen_ci(const char *path, const char *mode)
{
	const char *redirect = ModloaderRedirectPath(path);
	if(redirect)
		return fopen(redirect, mode);
	return fopen(GamePath(path).c_str(), mode);
}

bool
doesFileExist(const char *path)
{
	FILE *f;
	f = fopen_ci(path, "r");
	if(f){
		fclose(f);
		return true;
	}
	return false;
}

float
clampFloat(float f, float min, float max)
{
	if(f < min) return min;
	if(f > max) return max;
	return f;
}

static void
ClearEditorInputState(void)
{
	memset(CPad::oldKeystates, 0, sizeof(CPad::oldKeystates));
	memset(CPad::newKeystates, 0, sizeof(CPad::newKeystates));
	memset(CPad::tempKeystates, 0, sizeof(CPad::tempKeystates));
	memset(&CPad::oldMouseState, 0, sizeof(CPad::oldMouseState));
	memset(&CPad::newMouseState, 0, sizeof(CPad::newMouseState));
	memset(&CPad::tempMouseState, 0, sizeof(CPad::tempMouseState));
	CPad::clickState = 0;
	CPad::clickx = 0;
	CPad::clicky = 0;
	CPad::clickbtn = 0;
	for(int i = 0; i < 2; i++){
		CPad::Pads[i].OldState.Clear();
		CPad::Pads[i].NewState.Clear();
	}

#if !defined(RW_D3D9) || !defined(_WIN32)
	if(ImGui::GetCurrentContext()){
		ImGuiIO &io = ImGui::GetIO();
		io.MouseDown[0] = false;
		io.MouseDown[1] = false;
		io.MouseDown[2] = false;
		if(ImGui::IsKeyDown(ImGuiKey_LeftShift)) io.AddKeyEvent(ImGuiKey_LeftShift, false);
		if(ImGui::IsKeyDown(ImGuiKey_RightShift)) io.AddKeyEvent(ImGuiKey_RightShift, false);
		if(ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
		if(ImGui::IsKeyDown(ImGuiKey_RightCtrl)) io.AddKeyEvent(ImGuiKey_RightCtrl, false);
		if(ImGui::IsKeyDown(ImGuiKey_LeftAlt)) io.AddKeyEvent(ImGuiKey_LeftAlt, false);
		if(ImGui::IsKeyDown(ImGuiKey_RightAlt)) io.AddKeyEvent(ImGuiKey_RightAlt, false);
		io.KeyShift = false;
		io.KeyCtrl = false;
		io.KeyAlt = false;
		io.KeySuper = false;
	}
#endif
}

#ifdef _WIN32
static bool
isEditorWindowActive(void)
{
#ifdef RW_D3D9
	HWND hwnd = (HWND)engineOpenParams.window;
	return hwnd && GetForegroundWindow() == hwnd;
#else
	return true;
#endif
}

static bool
isVirtualKeyDown(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void
SyncEditorInputState(void)
{
	static bool hadFocus = true;

	// A UI script's buttons and modifiers are the input: the physical ones
	// (up, under Wine and in tests) would undo every scripted press.
	if(UiScriptActive())
		return;

	if(!isEditorWindowActive()){
		if(hadFocus)
			ClearEditorInputState();
		hadFocus = false;
		return;
	}
	hadFocus = true;

	CPad::tempKeystates[KEY_LSHIFT] = isVirtualKeyDown(VK_LSHIFT);
	CPad::tempKeystates[KEY_RSHIFT] = isVirtualKeyDown(VK_RSHIFT);
	CPad::tempKeystates[KEY_LCTRL] = isVirtualKeyDown(VK_LCONTROL);
	CPad::tempKeystates[KEY_RCTRL] = isVirtualKeyDown(VK_RCONTROL);
	CPad::tempKeystates[KEY_LALT] = isVirtualKeyDown(VK_LMENU);
	CPad::tempKeystates[KEY_RALT] = isVirtualKeyDown(VK_RMENU);

	uint32 physicalMouseBtns =
		(isVirtualKeyDown(VK_LBUTTON) ? 1 : 0) |
		(isVirtualKeyDown(VK_MBUTTON) ? 2 : 0) |
		(isVirtualKeyDown(VK_RBUTTON) ? 4 : 0);
	CPad::tempMouseState.btns = physicalMouseBtns;

#ifdef RW_D3D9
	if(ImGui::GetCurrentContext() &&
	   (ImGui::GetIO().WantCaptureMouse || gGizmoHovered || gGizmoUsing))
		CPad::tempMouseState.btns = 0;
#else
	if(ImGui::GetCurrentContext()){
		ImGuiIO &io = ImGui::GetIO();
		io.MouseDown[0] = !!(physicalMouseBtns & 1);
		io.MouseDown[1] = !!(physicalMouseBtns & 4);
		io.MouseDown[2] = !!(physicalMouseBtns & 2);
		if(io.WantCaptureMouse || gGizmoHovered || gGizmoUsing)
			CPad::tempMouseState.btns = 0;
		if(ImGui::IsKeyDown(ImGuiKey_LeftShift) != (CPad::tempKeystates[KEY_LSHIFT] != 0))
			io.AddKeyEvent(ImGuiKey_LeftShift, CPad::tempKeystates[KEY_LSHIFT] != 0);
		if(ImGui::IsKeyDown(ImGuiKey_RightShift) != (CPad::tempKeystates[KEY_RSHIFT] != 0))
			io.AddKeyEvent(ImGuiKey_RightShift, CPad::tempKeystates[KEY_RSHIFT] != 0);
		if(ImGui::IsKeyDown(ImGuiKey_LeftCtrl) != (CPad::tempKeystates[KEY_LCTRL] != 0))
			io.AddKeyEvent(ImGuiKey_LeftCtrl, CPad::tempKeystates[KEY_LCTRL] != 0);
		if(ImGui::IsKeyDown(ImGuiKey_RightCtrl) != (CPad::tempKeystates[KEY_RCTRL] != 0))
			io.AddKeyEvent(ImGuiKey_RightCtrl, CPad::tempKeystates[KEY_RCTRL] != 0);
		if(ImGui::IsKeyDown(ImGuiKey_LeftAlt) != (CPad::tempKeystates[KEY_LALT] != 0))
			io.AddKeyEvent(ImGuiKey_LeftAlt, CPad::tempKeystates[KEY_LALT] != 0);
		if(ImGui::IsKeyDown(ImGuiKey_RightAlt) != (CPad::tempKeystates[KEY_RALT] != 0))
			io.AddKeyEvent(ImGuiKey_RightAlt, CPad::tempKeystates[KEY_RALT] != 0);
	}
#endif
}
#else
static void
SyncEditorInputState(void)
{
}
#endif

#ifdef XINPUT
int pads[4];
int numPads;
int currentPad;

void
plAttachInput(void)
{
	int i;
	XINPUT_STATE state;

	for(i = 0; i < 4; i++)
		if(XInputGetState(i, &state) == ERROR_SUCCESS)
			pads[numPads++] = i;
}

void
plCapturePad(int arg)
{
	currentPad = arg;
	return;
}

void
plUpdatePad(CControllerState *state)
{
	XINPUT_STATE xstate;
	int pad;

	pad = currentPad < numPads ? pads[currentPad] : -1;
	if(pad < 0 || XInputGetState(pad, &xstate) != ERROR_SUCCESS){
		memset(state, 0, sizeof(CControllerState));
		return;
	}

	state->leftX  = 0;
	state->leftY  = 0;
	state->rightX = 0;
	state->rightY = 0;
	if(xstate.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE || xstate.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
		state->leftX = xstate.Gamepad.sThumbLX;
	if(xstate.Gamepad.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE || xstate.Gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
		state->leftY = xstate.Gamepad.sThumbLY;
	if(xstate.Gamepad.sThumbRX > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE || xstate.Gamepad.sThumbRX < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
		state->rightX = xstate.Gamepad.sThumbRX;
	if(xstate.Gamepad.sThumbRY > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE || xstate.Gamepad.sThumbRY < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
		state->rightY = xstate.Gamepad.sThumbRY;

	state->triangle = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_Y);
	state->circle = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_B);
	state->cross = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_A);
	state->square = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_X);
	state->l1 = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER);
	state->l2 = xstate.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
	state->leftshock = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB);
	state->r1 = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
	state->r2 = xstate.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
	state->rightshock = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB);
	state->select = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_BACK);
	state->start = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_START);
	state->up = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
	state->right = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
	state->down = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
	state->left = !!(xstate.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);

}
#else
void
plAttachInput(void)
{
}
void
plCapturePad(int arg)
{
}
void
plUpdatePad(CControllerState *state)
{
}
#endif

void
Init(void)
{
	// --version: the version, commit and channel, before anything opens.
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], "--version") == 0){
			printf("Haumea Studio %s %s\n", HaumeaVersionDisplay(), HAUMEA_CHANNEL);
			fflush(stdout);
			exit(0);
		}
	// --update-check, --update-install, --update-rollback: the updater on
	// its own, for the tests.
	int updateExit = UpdaterCommandLine(sk::args.argc, sk::args.argv);
	if(updateExit >= 0){
		fflush(stdout);
		exit(updateExit);
	}
#ifdef _WIN32
	// A restarted copy is created before the current process exits, but waits
	// here before any window or RenderWare resources are initialized.
	WaitForRestartParentIfRequested();
#endif

	// Where the game, the server and the settings are, before anything loads.
	InitWorkspace();
	// From here a crash is reported at the next start.
	CrashReportsInstall();
	// haumea.log, from here on: a start that fails before the first frame still leaves one.
	OpenLogFile();
	log("Haumea Studio %s %s starting\n", HaumeaVersionDisplay(), HAUMEA_CHANNEL);
	// A downloaded update installs now, before any window, and starts in this
	// one's place.
	UpdaterInstallStaged();

	static char windowTitle[256];
	static bool saveHookRegistered;
	const char *channel = HAUMEA_CHANNEL_DISPLAY;	// empty for production
	snprintf(windowTitle, sizeof(windowTitle),
		"Haumea Studio %s%s%s%s - The all-in-one editor for open.mp and SA-MP",
		HaumeaVersionDisplay(), channel[0] ? " [" : "", channel, channel[0] ? "]" : "");
	sk::globals.windowtitle = windowTitle;
	sk::globals.width = 1280;
	sk::globals.height = 800;
	LoadInitialEditorWindowState(&sk::globals.width, &sk::globals.height);
#ifdef LIBRW_GLFW
	// Open at splash size; the window grows to the saved size once loaded.
	sk::globals.width = HAUMEA_SPLASH_WIDTH;
	sk::globals.height = HAUMEA_SPLASH_HEIGHT;
#endif
	sk::globals.quit = 0;
	if(!saveHookRegistered){
		atexit(SaveEditorSettingsNow);
		saveHookRegistered = true;
	}
}

bool
attachPlugins(void)
{
	gta::attachPlugins();
	RegisterTexStorePlugin();
	RegisterPipes();
	return true;
}

void
DefinedState(void)
{
	SetRenderState(rw::ZTESTENABLE, 1);
	SetRenderState(rw::ZWRITEENABLE, 1);
	SetRenderState(rw::VERTEXALPHA, 0);
	SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	SetRenderState(rw::FOGENABLE, 0);
	SetRenderState(rw::ALPHATESTREF, params.alphaRefDefault);
	SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
	SetRenderState(rw::GSALPHATEST, params.ps2AlphaTest);
	rw::RGBA fog;
	uint32 c;
	rw::convColor(&fog, &Timecycle::currentFogColour);
	c = RWRGBAINT(fog.red, fog.green, fog.blue, 255);
	SetRenderState(rw::FOGCOLOR, c);
	SetRenderState(rw::CULLMODE, rw::CULLBACK);
}

// Simple function to convert a raster to the current platform.
static rw::Raster*
CreateFallbackRaster(void)
{
	rw::Image *img = rw::Image::create(1, 1, 32);
	uint32 white = 0xFFFFFFFF;
	img->pixels = (uint8*)&white;
	rw::Raster *ras = rw::Raster::createFromImage(img);
	img->destroy();
	return ras;
}

static rw::Raster*
ConvertTexRaster(rw::Raster *ras, const char *textureName)
{
	using namespace rw;

	if(ras->platform == rw::platform)
		return ras;
	// compatible platforms
	if(ras->platform == PLATFORM_D3D8 && rw::platform == PLATFORM_D3D9 ||
	   ras->platform == PLATFORM_D3D9 && rw::platform == PLATFORM_D3D8)
		return ras;

#ifndef RW_GL3
	Image *img = ras->toImage();
	if(img == nil){
		log("warning: failed to convert raster from platform %d to %d, using fallback texture\n",
			ras->platform, rw::platform);
		CrashReportsError("raster conversion", "a raster from platform %d didn't convert to %d",
			ras->platform, rw::platform);
		ras->destroy();
		return CreateFallbackRaster();
	}
	ras->destroy();
	img->unpalettize();
	ras = Raster::createFromImage(img);
	img->destroy();
	if(ras == nil){
		log("warning: failed to create converted raster for platform %d, using fallback texture\n",
			rw::platform);
		return CreateFallbackRaster();
	}
	return ras;
#else
	Raster *converted = Raster::convertTexToCurrentPlatform(ras);
	if(converted == nil){
		log("warning: failed to convert texture %s from platform %d to %d, using fallback texture\n",
			textureName && textureName[0] ? textureName : "(unnamed)", ras->platform, rw::platform);
		CrashReportsError("texture conversion", "a texture from platform %d didn't convert to %d",
			ras->platform, rw::platform);
		ras->destroy();
		return CreateFallbackRaster();
	}
	return converted;
#endif
}

void
ConvertTxd(rw::TexDictionary *txd)
{
	rw::Texture *tex;
	if(txd == nil)
		return;
	FORLIST(lnk, txd->textures){
		tex = rw::Texture::fromDict(lnk);
		rw::Raster *ras = tex->raster;
		if(ras)
			tex->raster = ConvertTexRaster(ras, tex->name);
		tex->setFilter(rw::Texture::LINEAR);
	}
}

#ifdef LIBRW_GLFW
// The Haumea mark as the window icon. Wayland compositors take icons only
// from the desktop entry named by the app ID, so skip it there.
static void
setWindowIcon(void)
{
	GLFWwindow *window = engineOpenParams.window ? *engineOpenParams.window : nil;
	if(window == nil)
		return;
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
	if(glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
		return;
#endif
	GLFWimage images[IM_ARRAYSIZE(haumeaIcons)];
	for(int i = 0; i < (int)IM_ARRAYSIZE(haumeaIcons); i++){
		images[i].width = haumeaIcons[i].size;
		images[i].height = haumeaIcons[i].size;
		images[i].pixels = (unsigned char*)haumeaIcons[i].pixels;
	}
	glfwSetWindowIcon(window, IM_ARRAYSIZE(images), images);
}
#endif

bool
InitRW(void)
{
	LoadInitialAntialiasingSettings();
	sk::requestedMultiSamplingLevels = gRequestedAASamples;
#ifdef LIBRW_GLFW
	// Create the window hidden and borderless: the splash places it and
	// shows it with its first frame, instead of a framed empty window
	// appearing and then jumping. librw's own glfwInit is then a no-op.
	if(glfwInit()){
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
		// Taskbars find the icon through this name (haumea.desktop on Wayland).
#ifdef GLFW_WAYLAND_APP_ID
		glfwWindowHintString(GLFW_WAYLAND_APP_ID, "haumea");
#endif
#ifdef GLFW_X11_CLASS_NAME
		glfwWindowHintString(GLFW_X11_CLASS_NAME, "haumea");
		glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "haumea");
#endif
	}
#endif
	// The last line of haumea.log when the renderer is what fails.
	log("Starting the graphics\n");
	if(!sk::InitRW()){
		log("error: the graphics didn't start\n");
#ifdef _WIN32
		// Otherwise it just closes: no window yet, and the reason went to haumea.stderr.log.
		std::string text = std::string(_("Haumea Studio couldn't start its graphics. Update your graphics "
			"card's driver (Haumea needs OpenGL 3.3 or Direct3D 9; Windows' basic display driver, Remote "
			"Desktop and some virtual machines can't). The details are in haumea.log and haumea.stderr.log in:")) +
			"\n\n" + GetConfigDir();
		std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nil, 0), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], (int)wide.size());
		MessageBoxW(nil, wide.c_str(), L"Haumea Studio", MB_OK | MB_ICONERROR);
#endif
		return false;
	}
#ifdef LIBRW_GLFW
	glfwDefaultWindowHints();
	setWindowIcon();
#endif
	rw::d3d::isP8supported = false;

	rw::Image *img = rw::Image::create(1, 1, 32);
	uint32 white = 0xFFFFFFFF;
	img->pixels = (uint8*)&white;
	whiteTex = rw::Texture::create(rw::Raster::createFromImage(img));
	img->destroy();

	Scene.world = rw::World::create();

	pAmbient = rw::Light::create(rw::Light::AMBIENT);
	pAmbient->setColor(0.4f, 0.4f, 0.4f);
//	pAmbient->setColor(1.0f, 1.0f, 1.0f);
	Scene.world->addLight(pAmbient);

	pDirect = rw::Light::create(rw::Light::DIRECTIONAL);
	pDirect->setColor(0.8f, 0.8f, 0.8f);
	pDirect->setFrame(rw::Frame::create());
	{
		// SA's light direction (CTimeCycle::m_vecDirnLightToSun, fixed): the
		// light shines along -dir, as SetLightsWithTimeOfDayColour sets it.
		rw::V3d dir = normalize(rw::V3d{ -0.5f, -0.5f, 0.70710678f });
		rw::V3d right = normalize(cross(rw::V3d{ 0.0f, 0.0f, 1.0f }, dir));
		rw::V3d up = cross(right, dir);
		rw::Matrix m;
		m.setIdentity();
		m.right = up;
		m.up = right;
		m.at = scale(dir, -1.0f);
		m.optimize();
		pDirect->getFrame()->transform(&m, rw::COMBINEREPLACE);
	}
	Scene.world->addLight(pDirect);

	TheCamera.m_rwcam = sk::CameraCreate(sk::globals.width, sk::globals.height, 1);
	TheCamera.m_rwcam->setFarPlane(5000.0f);
	TheCamera.m_rwcam->setNearPlane(0.1f);
	// Create a second camera to use with a larger draw distance
	TheCamera.m_rwcam_viewer = rw::Camera::create();
	TheCamera.m_rwcam_viewer->setFrame(TheCamera.m_rwcam->getFrame());
	TheCamera.m_rwcam_viewer->frameBuffer = TheCamera.m_rwcam->frameBuffer;
	TheCamera.m_rwcam_viewer->zBuffer = TheCamera.m_rwcam->zBuffer;
	TheCamera.m_rwcam_viewer->setFarPlane(5000.0f);
	TheCamera.m_rwcam_viewer->setNearPlane(0.1f);

	Scene.camera = TheCamera.m_rwcam;
	TheCamera.m_aspectRatio = sk::globals.height > 0 ?
		(float)sk::globals.width/sk::globals.height : 4.0f/3.0f;

	TheCamera.m_LODmult = 1.0f;

	Scene.world->addCamera(TheCamera.m_rwcam);
	Scene.world->addCamera(TheCamera.m_rwcam_viewer);

	// The catalogs and the saved language, before the first frame and before
	// imgui.ini is read (L() moves old window titles to their ### IDs).
	I18nInit();
	if(!ImGui_ImplRW_Init())
		return false;
	if(SettingsAreReadOnly()){
		ImGui::GetIO().IniFilename = nil;
	}else if(BuildPath(gImGuiIniPath, sizeof(gImGuiIniPath), GetConfigDir(), "imgui.ini")){
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = gImGuiIniPath;
	}
	SetupFonts();
	SetupStyle();

	RenderInit();

	// A test that points the updater at its own release server
	// (HAUMEA_UPDATE_URL) still checks; nothing else does in an automated run.
	const char *testReleases = getenv("HAUMEA_UPDATE_URL");
	if(!IsAutomatedRun() || (testReleases && testReleases[0]))
		UpdaterCheckForUpdate();
	TelemetrySendPing();	// opt-in; never in automated runs (telemetry.h)
	CrashReportsNoteGraphics();
	CrashReportsStartup();	// last run's crash, and --crash-test

	return true;
}


sk::EventStatus AppEventHandler(sk::Event e, void *param);

// Wayland may apply a size set on a hidden window without a size callback
// (or report it late), which leaves the UI laid out for the old size and
// stretched over the window. Check the real size before drawing.
void
SyncEditorWindowSize(void)
{
#ifdef LIBRW_GLFW
	GLFWwindow *window = engineOpenParams.window ? *engineOpenParams.window : nil;
	if(window == nil)
		return;
	int width, height;
	glfwGetWindowSize(window, &width, &height);
	if(width <= 0 || height <= 0 ||
	   (width == sk::globals.width && height == sk::globals.height))
		return;
	sk::Rect r = { 0, 0, width, height };
	AppEventHandler(sk::RESIZE, &r);
#endif
}

// While the look button is held the cursor hides and stays where it was
// pressed: the view turns by the mouse's motion (raw where the system offers
// it), never stopped by the screen's edge, and the cursor is back on release.
void
SetLookCursorLocked(bool locked)
{
	static bool isLocked;
	static int savedX, savedY;
	if(locked == isLocked){
#if defined(_WIN32) && defined(RW_D3D9)
		// No disabled cursor mode here: put it back after every frame's turn.
		if(locked){
			sk::SetMousePosition(savedX, savedY);
			CPad::tempMouseState.x = CPad::newMouseState.x = savedX;
			CPad::tempMouseState.y = CPad::newMouseState.y = savedY;
		}
#endif
		return;
	}
	isLocked = locked;
	if(locked){
		savedX = CPad::newMouseState.x;
		savedY = CPad::newMouseState.y;
	}
#ifdef LIBRW_GLFW
	GLFWwindow *window = engineOpenParams.window ? *engineOpenParams.window : nil;
	if(window){
		// GLFW reports the motion as an unbounded virtual position and puts
		// the cursor back itself when it is enabled again.
		glfwSetInputMode(window, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#ifdef GLFW_RAW_MOUSE_MOTION
		if(glfwRawMouseMotionSupported())
			glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, locked ? GLFW_TRUE : GLFW_FALSE);
#endif
	}
#elif defined(_WIN32) && defined(RW_D3D9)
	HWND hwnd = (HWND)engineOpenParams.window;
	if(locked){
		RECT r;
		GetClientRect(hwnd, &r);
		MapWindowPoints(hwnd, nil, (POINT*)&r, 2);
		ClipCursor(&r);
		while(ShowCursor(FALSE) >= 0);
	}else{
		ClipCursor(nil);
		sk::SetMousePosition(savedX, savedY);
		while(ShowCursor(TRUE) < 0);
	}
#endif
	if(!locked){
		// Where it was pressed, also for ImGui and a script's mouse.
		sk::MouseState ms = {};
		ms.posx = savedX;
		ms.posy = savedY;
		AppEventHandler(sk::MOUSEMOVE, &ms);
	}
}

sk::EventStatus
AppEventHandler(sk::Event e, void *param)
{
	using namespace sk;
	Rect *r;
	MouseState *ms;

	if(UiScriptDropsEvent(e))
		return EVENTPROCESSED;	// a UI script is driving the input
	ImGuiEventHandler(e, param);

	ImGuiIO *input = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nil;
//	if(io.WantCaptureMouse || ImGuizmo::IsOver())
//		CPad::tempMouseState.btns = 0;

	switch(e){
	case INITIALIZE:
#if defined(LIBRW_GLFW) && defined(__linux__) && defined(GLFW_PLATFORM_X11)
		// Tests run in their own Xvfb: with Wayland available GLFW would
		// prefer it and open on the user's desktop instead.
		if(getenv("HAUMEA_TEST_XVFB") && strcmp(getenv("HAUMEA_TEST_XVFB"), "1") == 0)
			glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
/*
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
*/
#if defined(_WIN32) && defined(RW_D3D9)
		InitializeEditorWindowDpi();
#endif
		Init();
		plAttachInput();
		return EVENTPROCESSED;
	case RWINITIALIZE:
		if(!::InitRW())
			return EVENTERROR;
#if defined(LIBRW_GLFW) && defined(__linux__) && defined(GLFW_PLATFORM_X11)
		log("GLFW platform: %s\n", glfwGetPlatform() == GLFW_PLATFORM_X11 ? "X11" :
			glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "Wayland" : "other");
#endif
		if(!BeginSplashWindow(HAUMEA_SPLASH_WIDTH, HAUMEA_SPLASH_HEIGHT))
			ApplyInitialEditorWindowState();
		return EVENTPROCESSED;
	case RWTERMINATE:
		DiscordPresenceShutdown();
#if defined(_WIN32) && defined(RW_D3D9)
		ImGui_ImplRW_Shutdown();
#endif
		return EVENTNOTPROCESSED; // skeleton terminates librw after the GUI releases its device
	case PLUGINATTACH:
		return attachPlugins() ? EVENTPROCESSED : EVENTERROR;
	case KEYDOWN: {
		// A fly key can start looking after RMB was armed over the gizmo.
		// Otherwise the key was swallowed before the camera could start.
		int key = *(int*)param;
		int lookButton = gBrushMode && gCameraLookButton == 1 ? 3 : gCameraLookButton;
		bool flyKey = key == 'W' || key == 'A' || key == 'S' || key == 'D' || key == 'Q' || key == 'E';
		bool startFlying = lookButton != 1 && !gGizmoUsing && flyKey &&
			(CPad::tempMouseState.btns & (1u << (lookButton-1))) != 0 && input && !input->KeyCtrl;
		if(input && !input->WantCaptureKeyboard && !input->WantTextInput &&
		   (!gGizmoHovered || gCameraLooking || startFlying))
			CPad::tempKeystates[key] = 1;
		return EVENTPROCESSED;
	}
	case KEYUP:
		CPad::tempKeystates[*(int*)param] = 0;
		return EVENTPROCESSED;
	case MOUSEBTN:
		ms = (MouseState*)param;
		// The gizmo (which ImGui counts as capturing the mouse) takes only
		// the left button, so the right one still looks around on it. Over a
		// panel: no new presses, but a button held since a press in the view
		// stays held (looking around goes on).
		if(gGizmoHovered)
			CPad::tempMouseState.btns = (CPad::tempMouseState.btns & ms->buttons) | (ms->buttons & ~1u);
		else if(input && input->WantCaptureMouse)
			CPad::tempMouseState.btns &= ms->buttons;
		else
			CPad::tempMouseState.btns = ms->buttons;
		return EVENTPROCESSED;
	case MOUSEMOVE:
		ms = (MouseState*)param;
		CPad::tempMouseState.x = ms->posx;
		CPad::tempMouseState.y = ms->posy;
		return EVENTPROCESSED;
	case RESIZE:
		r = (Rect*)param;
		// TODO: register when we're minimized
		if(r->w == 0) r->w = 1;
		if(r->h == 0) r->h = 1;

		sk::globals.width = r->w;
		sk::globals.height = r->h;
		OnEditorWindowResized(r->w, r->h);
		if(TheCamera.m_rwcam){
			sk::CameraSize(TheCamera.m_rwcam, r);
			TheCamera.m_rwcam_viewer->frameBuffer = TheCamera.m_rwcam->frameBuffer;
			TheCamera.m_rwcam_viewer->zBuffer = TheCamera.m_rwcam->zBuffer;
			TheCamera.m_aspectRatio = (float)r->w/r->h;
		}
		break;
	case FILEDROP: {
		const char *path = (const char*)param;
		size_t len = strlen(path);
		bool pawnFile = len > 4 && rw::strncmp_ci(path + len - 4, ".pwn", 4) == 0;
		bool pawnInclude = len > 4 && rw::strncmp_ci(path + len - 4, ".inc", 4) == 0;
		bool textureStudioMap = (len > 3 && rw::strncmp_ci(path + len - 3, ".db", 3) == 0) ||
			IsForeignMapFile(path);	// MTA:SA .map, IPL
		bool haumeaPrefab = len > 7 && rw::strncmp_ci(path + len - 7, ".haumea", 7) == 0;
		if(haumeaPrefab){
			int imported = ImportPrefab(path);
			if(imported > 0)
				Toast(TOAST_SPAWN, _("Imported %d instance(s) from prefab"), imported);
			else
				Toast(TOAST_SPAWN, _("Failed to import prefab"));
		}else if(pawnFile || pawnInclude || textureStudioMap){
			HandleMapFileDrop(path);
		}else
			HandleCustomImportDrop(path);
		return EVENTPROCESSED;
	}
	case IDLE:
		UiScriptTick();	// --ui-script: this frame's scripted input
		ModelMetaTick();	// the browser filters' polygons and textures, a little a frame
		SyncEditorInputState();
		UpdateEditorWindowState();
		// A UI script counts frames, so it gets the same time per frame
		// whatever the load: ImGui's double-click (set_item), key repeat and
		// hover timers, toasts and camera speed stop depending on how fast
		// the machine is.
		timeStep = UiScriptActive() ? 1.0f/60.0f : *(float*)param;
		Idle();
		return EVENTPROCESSED;
	}
	return sk::EVENTNOTPROCESSED;
}
