#include "haumea.h"
#include "checkpoints.h"
#include "particles.h"
#include "discordpresence.h"
#include "coordinatepin.h"
#include "layoutpaths.h"
#include "documentxml.h"
#include "crashreports.h"
#include "telemetry.h"
#include "editoricons.h"
#include "trains.h"
#include "textdraws.h"
#include "namaka.h"
#include "multiplayer.h"
#include "terminal.h"
#include "samp_loader.h"
#include "autocol.h"
#include "modloader.h"
#include "imgui/imgui_internal.h"
#include "categories.h"
#include "updater.h"
#include "icons.h"
#include "modelmeta.h"
#include "objfilters.h"
#include "browserdiscovery.h"
#include "modelquery.h"
#include "rec.h"
#include "anim.h"
#include "npcpreview.h"
#include "testserver.h"
#include "interiors.h"
#include "maptools.h"
#include "areas.h"
#include "radarart.h"
#include "encoding.h"
#include "pawneditor.h"
#include "shortcuts.h"
#include "version.h"
#include "versionutil.h"
#include "mapcheck.h"
#include "playerview.h"
#include "selftest.h"
#include "i18n.h"
#include "keycast.h"
#include "radar.h"
#include <chrono>
#include <deque>
#include <mutex>
#include <time.h>
#ifdef LIBRW_GLFW
#include <GLFW/glfw3.h>
#endif
#include "materialtext.h"
#include "workspace.h"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <cmath>
#include <ctime>

#include <string>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#else
#include <dirent.h>
#endif

// SA-MP and open.mp take one weather ID (SetWeather); 0-20 are the
// game's own, higher IDs only give glitched colours.
static const int SAMP_NUM_WEATHERS = 21;

static bool showEditorWindow;
static bool showInstanceWindow = true;
static bool showLogWindow;
static bool showHelpWindow;
static bool showShortcutsWindow;

static bool showTimeWeatherWindow;
static bool showViewWindow;
static bool showBrowserWindow = true;
static bool showDiffWindow;
static bool showMapObjectsWindow = true;	// the outliner, on the left
static bool showHistoryWindow;
static bool showTextureBrowserWindow;
static bool showPawnCodeWindow;
static bool showToolsWindow;
static bool showLicensesWindow;
static bool showPreferencesWindow;
static bool showMapCheckWindow;
static bool showNodesWindow;
static bool showTestServerWindow;
static bool showInteriorsWindow;
static bool showNamakaWindow;
static bool showMultiplayerWindow;
static bool showTerminalWindow;
static bool gPanelSnap = true;
static bool gResetPanelLayout;
static bool gSaNodeJustSelected;
static bool gBrowserIdeListDirty = true;
static char gEditorCameraName[256] = "default";

// Autosave of the open map, separate from the map file itself.
static bool gAutosaveEnabled = true;
static int gAutosaveIntervalSeconds = 180;
static int gAutosaveKeepCount = 10;
static float gAutosaveSecondsSinceLastRun = 0.0f;
static float gAutosaveSecondsSinceLastChange = 0.0f;
static uint32 gAutosaveLastSeenSeq = 0;
static uint32 gAutosaveLastSavedSeq = 0;
static int gCustomImportPreferredStartId = 18631;

static ImGuiTextFilter gEditorModelFilter;
static ImGuiTextFilter gEditorTxdFilter;
static bool gEditorHighlightMatches;
static std::vector<ObjectInst*> gEditorFilteredInstances;
static std::string gEditorCachedModelFilter;
static std::string gEditorCachedTxdFilter;
static uint32 gEditorCachedInstanceChangeSeq = (uint32)-1;
static CPtrNode *gEditorCachedInstancesFirst;

static ImGuiTextFilter gBrowserCategoryFilter;
static ImGuiTextFilter gBrowserIdeFilter;
static ImGuiTextFilter gBrowserSearchFilter;
static ImGuiTextFilter gBrowserFavFilter;
static ImGuiTextFilter gMaterialTextureFilter;
static ImGuiTextFilter gMaterialTxdFilter;
static ImGuiTextFilter gBrowserPrefabFilter;
// Edits in the Object Info panel go to the whole selection.
static bool gBatchEditSelection = true;
// Labels, tags and gang zone outlines in the viewport.
static bool gRenderSampElements = true;
static void uiSelectedInstances(std::vector<ObjectInst*> &out);
static void uiBatchEditHeader(ObjectInst *inst);

enum { BROWSER_CATEGORY_RECENT = -2, BROWSER_CATEGORY_ALL = -1 };
static int gBrowserSelectedCategory = BROWSER_CATEGORY_RECENT;
// What the Objects shelf lists: map objects, skins or vehicles, never mixed.
enum { BROWSER_KIND_OBJECTS, BROWSER_KIND_SKINS, BROWSER_KIND_VEHICLES, BROWSER_NUM_KINDS };
static int gBrowserKind = BROWSER_KIND_OBJECTS;
static char gBrowserSelectedIde[256];
static bool gBrowserThumbnailView = true;
static float gBrowserTileSize = 7.0f;	// minimum tile width in font heights
static bool gBrowserTabRestorePending;
static bool gBrowserPrefabListDirty = true;
static int gBrowserSelectedPrefab = -1;	// in gBrowserPrefabs
// The prefab library (see refreshBrowserPrefabList).
static std::vector<PrefabListing> gBrowserPrefabs;
static std::string gBrowserPrefabFolder;	// shown folder and its subfolders; "" all
static void refreshBrowserPrefabList(void);
static int gDiffFilter;
static int gRenderMode;
static char gPawnSavePath[1024];	// the open map; empty while untitled
enum { MAX_RECENT_MAPS = 10 };
static char gRecentMaps[MAX_RECENT_MAPS][1024];
static int gNumRecentMaps;
static bool gReopenLastMap = true;
static bool gShowStartGuide = true;
static bool gShowTextDrawsHint = true;
static bool gShowCodeHint = true;
static ImVec2 gTextDrawsHintAnchor, gCodeHintAnchor;
static float gUiScale = 1.0f;
static int gPawnSaveFormat = PAWN_CODE_NATIVE;
static uint32 gPawnLastSavedChangeSeq;
static bool gOpenSavePawnAs;
static bool gShowSavePawnAs;

struct MaterialTextureChoice
{
	int txdSlot;
	int sourceModel;
	int textureDictionarySlot;
	int textureIndex;
	char txdName[MODELNAMELEN];
	char textureName[PAWN_TEXTURE_NAME_LEN];
};
static std::vector<MaterialTextureChoice> gMaterialTextureChoices;
static ObjectInst *gMaterialTextureBrowserInst;
static int gMaterialTextureBrowserSlot = -1;
static bool gOpenMaterialTexturePopup;
static int gMaterialTextureBrowserTxd = -1;
static ObjectMaterialOverride gMaterialTextureBrowserInitial;

static const int MAX_INTERIOR_AREA_CODE = 255;


enum BrowserTabId
{
	BROWSER_TAB_CATEGORIES,
	BROWSER_TAB_IDE,
	BROWSER_TAB_SEARCH,
	BROWSER_TAB_FAVOURITES,
	BROWSER_TAB_PREFABS,
	BROWSER_TAB_MAP,
	BROWSER_TAB_ELEMENTS
};
static int gBrowserActiveTab = BROWSER_TAB_CATEGORIES;
enum BrowserElementFeature {
	BROWSER_CHARACTERS, BROWSER_PATHS, BROWSER_MARKERS, BROWSER_TEXT_AUDIO, BROWSER_ZONES
};
static int gBrowserElementFeature = BROWSER_CHARACTERS;
static int gBrowserElementKind = SAMP_ELEMENT_ACTOR;
static bool gCopyableTextCopiedThisFrame;


static bool gSavedWindowStateLoaded;
static bool gSavedWindowPlacementValid;
static bool gSavedWindowPlacementApplied;
static int gSavedWindowX;
static int gSavedWindowY;
static int gSavedWindowWidth = 1280;
static int gSavedWindowHeight = 800;
static bool gSavedWindowMaximized;
static float gSettingsAutosaveSeconds;
static bool gPersistentSettingsLoaded;
// Off: Reload Game Files asks only when there are unsaved changes.
static bool gConfirmReloadGame = false;
static const int SAVE_SETTINGS_VERSION = 9;

static void loadSaveSettings(void);
static void saveSaveSettings(void);
static void normalizePersistentSettings(void);
static void finishTransformPanelEdit(void);
enum MapAction { MAP_ACTION_NONE, MAP_ACTION_NEW, MAP_ACTION_OPEN, MAP_ACTION_QUIT };
static void requestMapAction(int action, const char *path = nil);
static bool mapHasUnsavedChanges(void);
static const char *currentMapName(void);
static void countMapContents(int *objects, int *removed);
static void uiMapObjectsList(void);
static bool splitSettingLine(const char *line, char *key, size_t keySize, const char **value);
static bool parseBoolSetting(const char *value, bool *out);
static bool parseIntSetting(const char *value, int *out);
static bool savePawnMap(void);
static void uiRunShortcut(int action);
static void uiOpenObjectDiscovery(int model, bool similar, const char *ids = nil);
static bool gBrowserDiscoveryFocusPending;
static void uiWorkspaceNavigation(void);
static void uiWorkspaceNewHints(void);
static int uiSelectionCount(void);
// --profile: frame times are measured for a while after the map opens.
static FILE *gProfileReport;
static int gProfileFrames = -1;
static void uiMapCheckBefore(const char *action);
// A shortcut's keys for hint text, from the shortcut table.
static const char*
uiKey(int action)
{
	const char *label = ShortcutLabel(action);
	return label[0] ? label : _("no key");
}
static void uiSavePawnAsPopup(void);

static const char*
cameraMouseButtonName(int button)
{
	switch(button){
	case 1: return "LMB";
	case 2: return "MMB";
	case 3: return "RMB";
	default: return "?";
	}
}

static int
cameraBrushLookButton(void)
{
	// Brush painting owns LMB, so an LMB camera preference uses RMB while
	// brushing. This mirrors the camera input path in camera.cpp.
	return gCameraLookButton == 1 ? 3 : gCameraLookButton;
}

static int
cameraActiveLookButton(void)
{
	return gBrushMode ? cameraBrushLookButton() : gCameraLookButton;
}
static const char *pathFilename(const char *path);


static uint32
sanitizeAASamples(uint32 samples, uint32 maxSamples = 0)
{
	if(samples <= 1)
		return 1;

	switch(samples){
	case 2:
	case 4:
	case 8:
	case 16:
		break;
	default:
		return 1;
	}

	if(maxSamples > 1){
		while(samples > maxSamples && samples > 1)
			samples >>= 1;
		if(samples < 2)
			return 1;
	}

	return samples;
}

static const char*
getAASamplesLabel(uint32 samples)
{
	switch(samples){
	case 1: return _("Off");
	case 2: return "2x MSAA";
	case 4: return "4x MSAA";
	case 8: return "8x MSAA";
	case 16: return "16x MSAA";
	default: return _("Custom");
	}
}

void
LoadInitialAntialiasingSettings(void)
{
	FILE *f;
	char line[1024];
	char key[128];
	const char *value;
	int intValue;

	gRequestedAASamples = 1;

	f = fopenHaumeaDataRead("savesettings.txt", "savesettings.txt");
	if(f == nil)
		return;

	while(fgets(line, sizeof(line), f)){
		if(!splitSettingLine(line, key, sizeof(key), &value))
			continue;
		if(strcmp(key, "aa_samples") == 0 && parseIntSetting(value, &intValue)){
			gRequestedAASamples = sanitizeAASamples((uint32)max(intValue, 1));
			break;
		}
	}

	fclose(f);
}

int gRendererChoice = RENDERER_AUTO;
bool gRendererGlFailed;

static const char *rendererNames[] = { "auto", "opengl", "d3d9" };

static void
parseRendererSetting(const char *key, const char *value)
{
	bool boolValue;
	char name[16];
	if(strcmp(key, "renderer") == 0){
		if(sscanf(value, "%15s", name) == 1)
			for(int i = 0; i < IM_ARRAYSIZE(rendererNames); i++)
				if(strcmp(name, rendererNames[i]) == 0)
					gRendererChoice = i;
	}else if(strcmp(key, "renderer_gl_failed") == 0){
		// Only ever set here: a start told it OpenGL failed keeps that.
		if(parseBoolSetting(value, &boolValue) && boolValue)
			gRendererGlFailed = true;
	}
}

// Before any window: main.cpp picks the exe from these.
void
LoadInitialRendererSettings(void)
{
	FILE *f = fopenHaumeaDataRead("savesettings.txt", "savesettings.txt");
	if(f == nil)
		return;
	char line[1024], key[128];
	const char *value;
	while(fgets(line, sizeof(line), f))
		if(splitSettingLine(line, key, sizeof(key), &value))
			parseRendererSetting(key, value);
	fclose(f);
}

static int
getDefaultCustomImportStartId(void)
{
	return isSA() ? 18631 : 0;
}

static void
sanitizeCustomImportSettings(void)
{
	if(gCustomImportPreferredStartId < 0 || gCustomImportPreferredStartId >= NUMOBJECTDEFS)
		gCustomImportPreferredStartId = getDefaultCustomImportStartId();
}

static const char*
skipSpaces(const char *p)
{
	while(*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static bool
splitSettingLine(const char *line, char *key, size_t keySize, const char **value)
{
	size_t len;
	const char *p = skipSpaces(line);
	if(*p == '\0' || *p == '#')
		return false;

	len = 0;
	while(p[len] && !isspace((unsigned char)p[len]))
		len++;
	if(len == 0 || len >= keySize)
		return false;

	memcpy(key, p, len);
	key[len] = '\0';
	*value = skipSpaces(p + len);
	return true;
}

static bool
parseIntSetting(const char *value, int *out)
{
	return sscanf(skipSpaces(value), "%d", out) == 1;
}

static bool
parseFloatSetting(const char *value, float *out)
{
	return sscanf(skipSpaces(value), "%f", out) == 1;
}

static bool
parseBoolSetting(const char *value, bool *out)
{
	int i;
	if(!parseIntSetting(value, &i))
		return false;
	*out = i != 0;
	return true;
}

static bool
parseVec3Setting(const char *value, rw::V3d *out)
{
	return sscanf(skipSpaces(value), "%f %f %f", &out->x, &out->y, &out->z) == 3;
}

static bool
parseQuotedStringValue(const char *value, char *out, size_t outSize, const char **after = nil)
{
	size_t len = 0;
	const char *p = skipSpaces(value);
	if(*p != '"' || outSize == 0)
		return false;
	p++;

	while(*p && *p != '"'){
		char c = *p++;
		if(c == '\\'){
			c = *p++;
			if(c == '\0')
				return false;
			switch(c){
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case '\\':
			case '"':
				break;
			default:
				break;
			}
		}
		if(len + 1 < outSize)
			out[len++] = c;
	}
	if(*p != '"')
		return false;

	out[len] = '\0';
	if(after)
		*after = p + 1;
	return true;
}

static void
writeQuotedSetting(FILE *f, const char *key, const char *value)
{
	fprintf(f, "%s \"", key);
	if(value){
		for(const char *p = value; *p; p++){
			switch(*p){
			case '\\':
			case '"':
				fputc('\\', f);
				fputc(*p, f);
				break;
			case '\n':
				fputs("\\n", f);
				break;
			case '\r':
				fputs("\\r", f);
				break;
			case '\t':
				fputs("\\t", f);
				break;
			default:
				fputc(*p, f);
				break;
			}
		}
	}
	fputs("\"\n", f);
}

static void
setTextFilterValue(ImGuiTextFilter &filter, const char *value)
{
	if(value == nil || value[0] == '\0'){
		filter.Clear();
		return;
	}
	strncpy(filter.InputBuf, value, IM_ARRAYSIZE(filter.InputBuf)-1);
	filter.InputBuf[IM_ARRAYSIZE(filter.InputBuf)-1] = '\0';
	filter.Build();
}

static void
loadWindowStateFromSettingsFile(void)
{
	FILE *f;
	char line[512];
	char key[128];
	const char *value;
	bool haveWindowX = false;
	bool haveWindowY = false;

	if(gSavedWindowStateLoaded)
		return;
	gSavedWindowStateLoaded = true;

	f = fopenHaumeaDataRead("savesettings.txt", "savesettings.txt");
	if(f == nil)
		return;

	while(fgets(line, sizeof(line), f)){
		if(!splitSettingLine(line, key, sizeof(key), &value))
			continue;
		if(strcmp(key, "window_width") == 0){
			parseIntSetting(value, &gSavedWindowWidth);
		}else if(strcmp(key, "window_height") == 0){
			parseIntSetting(value, &gSavedWindowHeight);
		}else if(strcmp(key, "window_x") == 0){
			haveWindowX = parseIntSetting(value, &gSavedWindowX);
		}else if(strcmp(key, "window_y") == 0){
			haveWindowY = parseIntSetting(value, &gSavedWindowY);
		}else if(strcmp(key, "window_maximized") == 0){
			parseBoolSetting(value, &gSavedWindowMaximized);
		}
	}
	fclose(f);

	gSavedWindowWidth = clamp(gSavedWindowWidth, 640, 8192);
	gSavedWindowHeight = clamp(gSavedWindowHeight, 480, 8192);
	gSavedWindowPlacementValid = haveWindowX && haveWindowY;
}

void
LoadInitialEditorWindowState(int *width, int *height)
{
	loadWindowStateFromSettingsFile();
	if(width)
		*width = gSavedWindowWidth;
	if(height)
		*height = gSavedWindowHeight;
}

void
OnEditorWindowResized(int width, int height)
{
#ifdef LIBRW_GLFW
	// UpdateEditorWindowState keeps the normal (unmaximized) size instead;
	// saving every resize would store a maximized window's size as normal.
	(void)width;
	(void)height;
#else
	gSavedWindowWidth = clamp(width, 1, 8192);
	gSavedWindowHeight = clamp(height, 1, 8192);
#endif
}

#ifdef LIBRW_GLFW
static GLFWwindow*
getEditorGlfwWindow(void)
{
	return engineOpenParams.window ? *engineOpenParams.window : nil;
}

// Wayland does not let applications read or set window positions; the
// compositor places windows.
static bool
glfwCanPositionWindows(void)
{
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
	return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
#else
	return true;
#endif
}

// Work area of the monitor whose work area contains the point, or of the
// primary monitor.
static void
glfwWorkAreaAt(int x, int y, bool usePoint, int *ax, int *ay, int *aw, int *ah)
{
	int count = 0;
	GLFWmonitor **monitors = glfwGetMonitors(&count);
	*ax = *ay = 0;
	*aw = 1280;
	*ah = 800;
	if(count <= 0)
		return;
	GLFWmonitor *chosen = monitors[0];
	glfwGetMonitorWorkarea(monitors[0], ax, ay, aw, ah);
	if(usePoint)
		for(int i = 0; i < count; i++){
			int mx, my, mw, mh;
			glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
			if(x >= mx && x < mx + mw && y >= my && y < my + mh){
				*ax = mx; *ay = my; *aw = mw; *ah = mh;
				chosen = monitors[i];
				break;
			}
		}
	// Wayland gives the monitor in pixels and windows in scaled units: with
	// display scaling the window would open bigger than the screen.
	if(!glfwCanPositionWindows()){
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(chosen, &sx, &sy);
		if(sx > 1.0f) *aw = (int)(*aw / sx);
		if(sy > 1.0f) *ah = (int)(*ah / sy);
	}
}

// The title bar and borders round the window: GLFW sizes and places its
// inside, so they have to fit on the monitor as well. Before the window has
// a frame (the borderless splash, or not shown yet) a usual one.
static void
glfwFrameExtents(GLFWwindow *window, int *l, int *t, int *r, int *b)
{
	*l = *t = *r = *b = 0;
	if(glfwGetWindowAttrib(window, GLFW_DECORATED) && glfwGetWindowAttrib(window, GLFW_VISIBLE))
		glfwGetWindowFrameSize(window, l, t, r, b);
	if(*l == 0 && *t == 0 && *r == 0 && *b == 0){
		*l = *r = *b = 8;
		*t = 32;
	}
}

// Where the editor window opens on the monitor, its frame included: the
// saved size if it fits, else the work area less the frame; the saved place
// kept on the monitor, else centred. Logged, for reports of windows that
// open too big or off the screen.
static void
fitEditorWindow(GLFWwindow *window, bool usePosition, int *x, int *y, int *width, int *height)
{
	int ax, ay, aw, ah, l, t, r, b;
	glfwWorkAreaAt(gSavedWindowX + 40, gSavedWindowY + 40, usePosition, &ax, &ay, &aw, &ah);
	glfwFrameExtents(window, &l, &t, &r, &b);
	int roomW = aw - l - r, roomH = ah - t - b;
	*width = max(min(gSavedWindowWidth, roomW), min(640, roomW));
	*height = max(min(gSavedWindowHeight, roomH), min(480, roomH));
	*x = ax + l + (roomW - *width) / 2;
	*y = ay + t + (roomH - *height) / 2;
	if(usePosition){
		*x = clamp(gSavedWindowX, ax + l, max(ax + l, ax + aw - r - *width));
		*y = clamp(gSavedWindowY, ay + t, max(ay + t, ay + ah - b - *height));
	}
	static bool logged;
	if(!logged){
		logged = true;
		log("Window: work area %d,%d %dx%d, frame %d %d %d %d; opens at %d,%d %dx%d%s\n", ax, ay, aw, ah,
			l, t, r, b, *x, *y, *width, *height, gSavedWindowMaximized ? ", maximized" : "");
	}
}
#endif

#ifdef _WIN32
static HWND
getEditorWindowHandle(void)
{
#ifdef RW_D3D9
	return (HWND)engineOpenParams.window;
#else
	return nil;
#endif
}
#endif

void
ApplyInitialEditorWindowState(void)
{
	loadWindowStateFromSettingsFile();
	if(gSavedWindowPlacementApplied)
		return;
	gSavedWindowPlacementApplied = true;

#if defined(LIBRW_GLFW)
	// The window was created at the saved size. Fit it to the monitor it
	// opens on (the size may come from a bigger screen or another scale),
	// put it back where it was or centre it, and restore maximized.
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil)
		return;
	bool canPosition = glfwCanPositionWindows();
	bool usePosition = canPosition && gSavedWindowPlacementValid;
	int x, y, width, height;
	fitEditorWindow(window, usePosition, &x, &y, &width, &height);
	glfwSetWindowSize(window, width, height);
	if(canPosition)
		glfwSetWindowPos(window, x, y);
	gSavedWindowWidth = width;
	gSavedWindowHeight = height;
	if(gSavedWindowMaximized)
		glfwMaximizeWindow(window);
#elif defined(_WIN32)
	HWND hwnd = getEditorWindowHandle();
	if(hwnd && gSavedWindowPlacementValid){
		SetWindowPos(hwnd, nil, gSavedWindowX, gSavedWindowY,
			gSavedWindowWidth, gSavedWindowHeight,
			SWP_NOZORDER | SWP_NOACTIVATE);
		if(gSavedWindowMaximized)
			ShowWindow(hwnd, SW_MAXIMIZE);
	}
#endif
}

// The splash is a small borderless window in the middle of the screen; once
// the game is loaded it grows into the editor window (ExpandSplashWindow).
bool
BeginSplashWindow(int width, int height)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil)
		return false;
	loadWindowStateFromSettingsFile();
	glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
	glfwSetWindowSize(window, width, height);
	if(glfwCanPositionWindows()){
		int ax, ay, aw, ah;
		bool usePosition = gSavedWindowPlacementValid;
		glfwWorkAreaAt(gSavedWindowX + 40, gSavedWindowY + 40, usePosition, &ax, &ay, &aw, &ah);
		glfwSetWindowPos(window, ax + (aw - width) / 2, ay + (ah - height) / 2);
	}
	return true;
#else
	(void)width;
	(void)height;
	return false;
#endif
}

// Where the editor window opens: the saved size and place, fitted to the
// monitor (as ApplyInitialEditorWindowState does). False without GLFW.
bool
GetInitialEditorWindowRect(int *x, int *y, int *width, int *height, bool *canPosition)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil)
		return false;
	*canPosition = glfwCanPositionWindows();
	bool usePosition = *canPosition && gSavedWindowPlacementValid;
	fitEditorWindow(window, usePosition, x, y, width, height);
	if(gSavedWindowMaximized){
		// Grow to the whole work area, less the frame; maximizing afterwards is then seamless.
		int ax, ay, aw, ah, l, t, r, b;
		glfwWorkAreaAt(gSavedWindowX + 40, gSavedWindowY + 40, usePosition, &ax, &ay, &aw, &ah);
		glfwFrameExtents(window, &l, &t, &r, &b);
		*x = ax + l; *y = ay + t; *width = aw - l - r; *height = ah - t - b;
	}
	return true;
#else
	return false;
#endif
}

void
GetEditorWindowRect(int *x, int *y, int *width, int *height)
{
	*x = *y = 0;
	*width = sk::globals.width;
	*height = sk::globals.height;
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil)
		return;
	glfwGetWindowSize(window, width, height);
	if(glfwCanPositionWindows())
		glfwGetWindowPos(window, x, y);
#endif
}

void
SetEditorWindowRect(int x, int y, int width, int height, bool setPosition)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil)
		return;
	glfwSetWindowSize(window, width, height);
	if(setPosition)
		glfwSetWindowPos(window, x, y);
#else
	(void)x; (void)y; (void)width; (void)height; (void)setPosition;
#endif
}

void
HideEditorWindow(void)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window)
		glfwHideWindow(window);
#endif
}

// The window is created hidden; the splash shows it once it has a frame.
void
ShowEditorWindow(void)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window && !IsBackgroundWorker() && !glfwGetWindowAttrib(window, GLFW_VISIBLE))
		glfwShowWindow(window);
#endif
}

void
SetEditorWindowDecorated(bool decorated)
{
#if defined(LIBRW_GLFW)
	GLFWwindow *window = getEditorGlfwWindow();
	if(window)
		glfwSetWindowAttrib(window, GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
#else
	(void)decorated;
#endif
}

static void
sampleEditorWindowState(void)
{
#if defined(LIBRW_GLFW)
	// Remember the normal size and position, and whether it is maximized;
	// a maximized or minimized window keeps the last normal size.
	GLFWwindow *window = getEditorGlfwWindow();
	if(window == nil || !gSavedWindowPlacementApplied)
		return;
	if(glfwGetWindowAttrib(window, GLFW_ICONIFIED))
		return;
	gSavedWindowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
	if(gSavedWindowMaximized)
		return;
	int width, height;
	glfwGetWindowSize(window, &width, &height);
	if(width > 0 && height > 0){
		gSavedWindowWidth = width;
		gSavedWindowHeight = height;
	}
	if(glfwCanPositionWindows()){
		glfwGetWindowPos(window, &gSavedWindowX, &gSavedWindowY);
		gSavedWindowPlacementValid = true;
	}
#elif defined(_WIN32)
	HWND hwnd = getEditorWindowHandle();
	if(hwnd){
		WINDOWPLACEMENT placement;
		placement.length = sizeof(placement);
		if(GetWindowPlacement(hwnd, &placement)){
			RECT rect = placement.rcNormalPosition;
			if(placement.showCmd == SW_SHOWMAXIMIZED){
				gSavedWindowMaximized = true;
			}else if(placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE ||
			          placement.showCmd == SW_SHOWMINNOACTIVE){
				gSavedWindowMaximized = (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
			}else{
				GetWindowRect(hwnd, &rect);
				gSavedWindowMaximized = false;
			}

			gSavedWindowX = rect.left;
			gSavedWindowY = rect.top;
			gSavedWindowWidth = max((int)(rect.right - rect.left), 1);
			gSavedWindowHeight = max((int)(rect.bottom - rect.top), 1);
			gSavedWindowPlacementValid = true;
		}
	}
#endif
}

// Called every frame. Besides sampling where the window is, it writes the
// settings file a moment after the window stopped moving or resizing, so the
// next start opens it in the same place and size even if nothing else in the
// settings ever changed (and after a crash or a kill).
void
UpdateEditorWindowState(void)
{
	static bool haveLast;
	static int lastX, lastY, lastW, lastH;
	static bool lastMax, lastValid, pending;
	static std::chrono::steady_clock::time_point changedAt;
	sampleEditorWindowState();
	if(!gSavedWindowPlacementApplied)
		return;
	bool same = haveLast && lastX == gSavedWindowX && lastY == gSavedWindowY &&
		lastW == gSavedWindowWidth && lastH == gSavedWindowHeight &&
		lastMax == gSavedWindowMaximized && lastValid == gSavedWindowPlacementValid;
	if(!haveLast){
		// The first sample is what was just loaded: nothing to write yet.
		haveLast = true;
		same = false;
		pending = false;
	}else if(!same){
		pending = true;
		changedAt = std::chrono::steady_clock::now();
	}
	lastX = gSavedWindowX; lastY = gSavedWindowY;
	lastW = gSavedWindowWidth; lastH = gSavedWindowHeight;
	lastMax = gSavedWindowMaximized; lastValid = gSavedWindowPlacementValid;
	if(pending && std::chrono::steady_clock::now() - changedAt > std::chrono::milliseconds(800)){
		pending = false;
		saveSaveSettings();
	}
}

static void
normalizePersistentSettings(void)
{
	currentHour = ((currentHour % 24) + 24) % 24;
	currentMinute = ((currentMinute % 60) + 60) % 60;
	if(params.numAreas > 0)
		currentArea = clamp(currentArea, 0, MAX_INTERIOR_AREA_CODE);
	else
		currentArea = 0;
	if(params.numWeathers > 0){
		Weather::oldWeather = clamp(Weather::oldWeather, 0, params.numWeathers-1);
		Weather::newWeather = clamp(Weather::newWeather, 0, params.numWeathers-1);
	}else{
		Weather::oldWeather = 0;
		Weather::newWeather = 0;
	}
	if(params.timecycle != GAME_III && params.numExtraColours > 0 && params.numHours > 0)
		extraColours = clamp(extraColours, -1, params.numExtraColours*params.numHours - 1);
	else
		extraColours = -1;
	Weather::interpolation = clamp(Weather::interpolation, 0.0f, 1.0f);
	TheCamera.m_fov = ClampCameraFov(TheCamera.m_fov);
	TheCamera.m_LODmult = clamp(TheCamera.m_LODmult, 0.5f, 3.0f);
	gFogMode = clamp(gFogMode, (int)FOG_GAME, (int)FOG_OFF);
	gStreamDistance = clamp(gStreamDistance, 50.0f, 5000.0f);
	gFrameLimit = gFrameLimit <= 0 ? 0 : clamp(gFrameLimit, 30, 360);
	gFlySpeed = clamp(gFlySpeed, 0.1f, 70.0f);
	gFlyFastMul = clamp(gFlyFastMul, 1.0f, 10.0f);
	gFlySlowMul = clamp(gFlySlowMul, 0.05f, 1.0f);
	gFovWheelStep = clamp(gFovWheelStep, 0.1f, 15.0f);
	gCameraLookButton = clamp(gCameraLookButton, 1, 3);
	gNeoLightMapStrength = clamp(gNeoLightMapStrength, 0.0f, 1.0f);
	gDayNightBalance = clamp(gDayNightBalance, 0.0f, 1.0f);
	gWetRoadEffect = clamp(gWetRoadEffect, 0.0f, 1.0f);
	gSaPedPathWalkerCount = clamp(gSaPedPathWalkerCount, 1, 32);
	gSaCarPathTrafficCount = clamp(gSaCarPathTrafficCount, 1, 32);
	gSaCarPathTrafficSpeedScale = clamp(gSaCarPathTrafficSpeedScale, 0.25f, 3.0f);
	gSaCarPathParkedCarCount = clamp(gSaCarPathParkedCarCount, 1, 24);
	gRenderMode = clamp(gRenderMode, 0, 2);
	gRenderOnlyHD = gRenderMode == 1;
	gRenderOnlyLod = gRenderMode == 2;
	gSelectionHighlightOpacity = clamp(gSelectionHighlightOpacity, 0, 100);
	gGizmoMode = gGizmoMode == GIZMO_ROTATE ? GIZMO_ROTATE : GIZMO_TRANSLATE;
	gObjectSnapDistance = clamp(gObjectSnapDistance, 0.01f, 10.0f);
	if(gGizmoSpace != GIZMO_LOCAL && gGizmoSpace != GIZMO_WORLD)
		gGizmoSpace = GIZMO_WORLD;
	gBrowserActiveTab = clamp(gBrowserActiveTab, (int)BROWSER_TAB_CATEGORIES, (int)BROWSER_TAB_ELEMENTS);
	gBrowserElementFeature = clamp(gBrowserElementFeature, (int)BROWSER_CHARACTERS, (int)BROWSER_ZONES);
	// The Search and In-this-map tabs were folded into Objects and the Map
	// Objects window; carry an old search over to the one search box.
	if(gBrowserActiveTab == BROWSER_TAB_SEARCH || gBrowserActiveTab == BROWSER_TAB_MAP){
		if(gBrowserActiveTab == BROWSER_TAB_SEARCH && gBrowserSearchFilter.IsActive())
			setTextFilterValue(gBrowserCategoryFilter, gBrowserSearchFilter.InputBuf);
		gBrowserActiveTab = BROWSER_TAB_CATEGORIES;
	}
	gBrowserSelectedCategory = clamp(gBrowserSelectedCategory, (int)BROWSER_CATEGORY_RECENT, NUM_OBJ_CATEGORIES-1);
	gBrowserKind = clamp(gBrowserKind, (int)BROWSER_KIND_OBJECTS, (int)BROWSER_NUM_KINDS-1);
	gDiffFilter = max(gDiffFilter, 0);
	params.alphaRef = clamp(params.alphaRef, 0, 255);

	switch(params.timecycle){
	case GAME_SA:
		if(gColourFilter == PLATFORM_XBOX)
			gColourFilter = PLATFORM_PC;
		if(gColourFilter != 0 && gColourFilter != PLATFORM_PS2 &&
		   gColourFilter != PLATFORM_PC)
			gColourFilter = PLATFORM_PC;
		break;
	case GAME_LCS:
		gRadiosity = false;
		if(gColourFilter != 0 && gColourFilter != PLATFORM_PS2 &&
		   gColourFilter != PLATFORM_PSP)
			gColourFilter = 0;
		break;
	case GAME_VCS:
		if(gColourFilter != 0 && gColourFilter != PLATFORM_PS2 &&
		   gColourFilter != PLATFORM_PSP)
			gColourFilter = 0;
		break;
	default:
		gColourFilter = 0;
		gRadiosity = false;
		break;
	}

	if(params.daynightPipe){
		if(gBuildingPipeSwitch != PLATFORM_PS2 &&
		   gBuildingPipeSwitch != PLATFORM_PC &&
		   gBuildingPipeSwitch != PLATFORM_XBOX)
			gBuildingPipeSwitch = PLATFORM_PS2;
	}else if(params.leedsPipe){
		if(gBuildingPipeSwitch != PLATFORM_NULL &&
		   gBuildingPipeSwitch != PLATFORM_PSP &&
		   gBuildingPipeSwitch != PLATFORM_PS2 &&
		   gBuildingPipeSwitch != PLATFORM_PC)
			gBuildingPipeSwitch = PLATFORM_PS2;
	}else
		gBuildingPipeSwitch = PLATFORM_PS2;

	// SA-MP and open.mp run the PC game: one weather at a time (SetWeather),
	// the PC building pipe and no PS2 radiosity. Overlays from the other
	// games' viewers are not offered any more, so old settings drop them.
	if(params.timecycle == GAME_SA){
		Weather::oldWeather = clamp(Weather::oldWeather, 0, SAMP_NUM_WEATHERS-1);
		Weather::newWeather = Weather::oldWeather;
		Weather::interpolation = 0.0f;
		extraColours = -1;
		gBuildingPipeSwitch = PLATFORM_PC;
		if(gColourFilter != 0)
			gColourFilter = PLATFORM_PC;
		gRadiosity = false;
		params.ps2AlphaTest = false;
	}
	gRenderZones = false;
	gRenderTimecycleBoxes = false;
	gRenderEffects = false;
	gRenderLegacyCarPaths = false;
	gRenderLegacyPedPaths = false;
	gRenderSaCarPathTraffic = false;
	gRenderSaCarPathParkedCars = false;
	gRenderSaPedPathWalkers = false;
	gRenderMode = 0;
	gRenderOnlyHD = false;
	gRenderOnlyLod = false;
}

static bool pathHasExtensionCi(const char *path, const char *ext);
static const char *pathFilename(const char *path);

static void
sanitizeAutosaveSettings(void)
{
	gAutosaveIntervalSeconds = clamp(gAutosaveIntervalSeconds, 30, 60*60);
	gAutosaveKeepCount = clamp(gAutosaveKeepCount, 1, 100);
}

static bool
getAutosaveDirectory(char *dst, size_t size)
{
	return GetHaumeaDataPath(dst, size, "autosave");
}

// Autosaves of every map, newest first.
static void
listAutosaves(std::vector<std::string> &files)
{
	files.clear();
	char dir[1024];
	if(!getAutosaveDirectory(dir, sizeof(dir)))
		return;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((std::string(dir) + "\\*.pwn").c_str(), &entry);
	if(handle != INVALID_HANDLE_VALUE){
		do
			files.push_back(std::string(dir) + "\\" + entry.cFileName);
		while(FindNextFileA(handle, &entry));
		FindClose(handle);
	}
#else
	DIR *d = opendir(dir);
	if(d){
		dirent *ent;
		while((ent = readdir(d)) != nil)
			if(pathHasExtensionCi(ent->d_name, ".pwn"))
				files.push_back(std::string(dir) + "/" + ent->d_name);
		closedir(d);
	}
#endif
	// Names end in a sortable timestamp.
	std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b){
		return a.substr(a.size() > 19 ? a.size() - 19 : 0) >
		       b.substr(b.size() > 19 ? b.size() - 19 : 0);
	});
}

static void
writeAutosave(void)
{
	char dir[1024];
	if(SettingsAreReadOnly() || !getAutosaveDirectory(dir, sizeof(dir)))
		return;
	char stem[256];
	const char *name = currentMapName();
	size_t n = 0;
	for(const char *p = name; *p && *p != '.' && n + 1 < sizeof(stem); p++)
		stem[n++] = isalnum((unsigned char)*p) || *p == '_' || *p == '-' ? *p : '_';
	stem[n] = '\0';
	time_t now = time(nil);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	char path[1300];
	snprintf(path, sizeof(path), "%s/%s-%s.pwn", dir, stem, stamp);
	int skipped = 0;
	if(ExportPawnCode(path, (PawnCodeFormat)gPawnSaveFormat, false, &skipped) < 0){
		log("Autosave: failed to write %s\n", path);
		return;
	}
	log("Autosave: wrote %s\n", path);
	std::vector<std::string> files;
	listAutosaves(files);
	for(size_t i = gAutosaveKeepCount; i < files.size(); i++){
		remove(files[i].c_str());
		remove(EditorXML::Sidecar(files[i]).c_str());
		remove((EditorXML::Sidecar(files[i])+".previous").c_str());
	}
}

static void
autosaveTick(void)
{
	uint32 seq = GetLatestChangeSeq();
	if(seq != gAutosaveLastSeenSeq){
		gAutosaveLastSeenSeq = seq;
		gAutosaveSecondsSinceLastChange = 0.0f;
	}else
		gAutosaveSecondsSinceLastChange += timeStep;
	gAutosaveSecondsSinceLastRun += timeStep;
	// Only unsaved, not-yet-autosaved work, and only once editing pauses.
	if(!gAutosaveEnabled || !isSA() || !mapHasUnsavedChanges() || seq == gAutosaveLastSavedSeq ||
	   gAutosaveSecondsSinceLastRun < gAutosaveIntervalSeconds ||
	   gAutosaveSecondsSinceLastChange < 3.0f)
		return;
	gAutosaveSecondsSinceLastRun = 0.0f;
	gAutosaveLastSavedSeq = seq;
	writeAutosave();
}

#ifdef _WIN32
static bool
findFileRecursive(const char *dir, const char *name)
{
	char searchPath[2048];
	if(!BuildPath(searchPath, sizeof(searchPath), dir, "*"))
		return false;

	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA(searchPath, &entry);
	if(handle == INVALID_HANDLE_VALUE)
		return false;

	bool found = false;
	do{
		if(strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0)
			continue;

		if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
			if(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				continue;

			char childDir[2048];
			if(BuildPath(childDir, sizeof(childDir), dir, entry.cFileName) &&
			   findFileRecursive(childDir, name)){
				found = true;
				break;
			}
		}else if(_stricmp(entry.cFileName, name) == 0){
			found = true;
			break;
		}
	}while(FindNextFileA(handle, &entry));

	FindClose(handle);
	return found;
}
#endif

#ifdef _WIN32
static bool
findTeleportAsi(const char *rootDir)
{
	return findFileRecursive(rootDir, "haumea.asi") ||
		findFileRecursive(rootDir, "haumea.III.asi") ||
		findFileRecursive(rootDir, "haumea.VC.asi") ||
		findFileRecursive(rootDir, "haumea.SA.asi");
}
#endif

#ifdef _WIN32
static bool
isProcessRunningByName(const char *exeName)
{
	if(exeName == nil || exeName[0] == '\0')
		return false;

	DWORD processIds[2048];
	DWORD bytesReturned = 0;
	if(!EnumProcesses(processIds, sizeof(processIds), &bytesReturned))
		return false;

	const DWORD processCount = bytesReturned / sizeof(DWORD);
	for(DWORD i = 0; i < processCount; i++){
		const DWORD pid = processIds[i];
		if(pid == 0)
			continue;

		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if(process == nil)
			continue;

		char imagePath[MAX_PATH];
		DWORD imagePathSize = sizeof(imagePath);
		bool match = false;
		if(QueryFullProcessImageNameA(process, 0, imagePath, &imagePathSize)){
			const char *baseName = strrchr(imagePath, '\\');
			if(baseName == nil)
				baseName = strrchr(imagePath, '/');
			baseName = baseName ? baseName + 1 : imagePath;
			match = _stricmp(baseName, exeName) == 0;
		}
		CloseHandle(process);

		if(match)
			return true;
	}
	return false;
}
#endif

// Toast notification system
#define TOAST_MAX 5
#define TOAST_DURATION 2.0f
#define TOAST_FADE_IN 0.15f
#define TOAST_FADE_OUT 0.4f

struct ToastEntry {
	char text[256];
	float timer;		// time remaining (counts down)
	float totalTime;	// total lifetime
	ToastCategory category;
};

static ToastEntry toasts[TOAST_MAX];
static int numToasts;
static bool toastEnabled = true;
static bool toastCategoryEnabled[TOAST_NUM_CATEGORIES] = { true, true, true, true, true, true };
static const char *toastCategoryNames[TOAST_NUM_CATEGORIES] = {
	N_("Undo / Redo"), N_("Delete"), N_("Copy / Paste"), N_("Save"), N_("Selection"), N_("Spawn")
};

static void
addToast(ToastCategory cat, float duration, const char *fmt, va_list args)
{
	char text[sizeof(toasts[0].text)];
	vsnprintf(text, sizeof(text), fmt, args);
	log("%s\n", text);	// what the user is told stays in the Log, shown or not
	if(!toastEnabled || !toastCategoryEnabled[cat])
		return;

	// Shift existing toasts down if full
	if(numToasts >= TOAST_MAX){
		memmove(&toasts[0], &toasts[1], (TOAST_MAX-1)*sizeof(ToastEntry));
		numToasts = TOAST_MAX - 1;
	}

	ToastEntry *t = &toasts[numToasts++];
	memcpy(t->text, text, sizeof(t->text));
	UiScriptNoteToast(t->text);
	t->totalTime = duration + TOAST_FADE_IN + TOAST_FADE_OUT;
	t->timer = t->totalTime;
	t->category = cat;
}

void
Toast(ToastCategory cat, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	addToast(cat, TOAST_DURATION, fmt, args);
	va_end(args);
}

void
ToastFor(ToastCategory cat, float duration, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	addToast(cat, duration, fmt, args);
	va_end(args);
}

static void
uiToasts(void)
{
	if(numToasts == 0) return;

	float dt = ImGui::GetIO().DeltaTime;
	float screenW = ImGui::GetIO().DisplaySize.x;
	float screenH = ImGui::GetIO().DisplaySize.y;

	// Update timers and remove expired
	for(int i = 0; i < numToasts; ){
		toasts[i].timer -= dt;
		if(toasts[i].timer <= 0.0f){
			memmove(&toasts[i], &toasts[i+1], (numToasts-i-1)*sizeof(ToastEntry));
			numToasts--;
		}else{
			i++;
		}
	}

	// Render from bottom up, centered horizontally
	float yBase = screenH - 60.0f;
	float spacing = 32.0f;

	for(int i = numToasts - 1; i >= 0; i--){
		ToastEntry *t = &toasts[i];
		float elapsed = t->totalTime - t->timer;

		// Compute alpha: fade in -> hold -> fade out
		float alpha;
		if(elapsed < TOAST_FADE_IN)
			alpha = elapsed / TOAST_FADE_IN;
		else if(t->timer < TOAST_FADE_OUT)
			alpha = t->timer / TOAST_FADE_OUT;
		else
			alpha = 1.0f;

		// Slide up slightly on appear
		float slideOffset = 0.0f;
		if(elapsed < TOAST_FADE_IN)
			slideOffset = (1.0f - elapsed / TOAST_FADE_IN) * 10.0f;

		ImVec2 textSize = ImGui::CalcTextSize(t->text);
		float padX = 16.0f, padY = 8.0f;
		float boxW = textSize.x + padX * 2;
		float boxH = textSize.y + padY * 2;
		float x = (screenW - boxW) * 0.5f;
		float y = yBase - (numToasts - 1 - i) * spacing + slideOffset;

		KeepNextEditorOverlayOnMainWindow();
		ImGui::SetNextWindowPos(EditorUiPoint(x, y));
		ImGui::SetNextWindowSize(ImVec2(boxW, boxH));
		ImGui::SetNextWindowBgAlpha(0.0f);

		char winId[32];
		snprintf(winId, sizeof(winId), "##toast%d", i);
		ImGui::Begin(winId, nil,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoBringToFrontOnFocus);

		ImDrawList *dl = ImGui::GetWindowDrawList();

		// Rounded rect background
		ImU32 bgCol = IM_COL32(20, 20, 20, (int)(200 * alpha));
		ImU32 borderCol = IM_COL32(80, 80, 80, (int)(150 * alpha));
		ImVec2 p0 = EditorUiPoint(x, y);
		ImVec2 p1 = EditorUiPoint(x + boxW, y + boxH);
		dl->AddRectFilled(p0, p1, bgCol, 6.0f);
		dl->AddRect(p0, p1, borderCol, 6.0f);

		// Text
		ImU32 textCol = IM_COL32(240, 240, 240, (int)(255 * alpha));
		dl->AddText(EditorUiPoint(x + padX, y + padY), textCol, t->text);

		ImGui::End();
	}
}

static void
uiNotificationSettings(void)
{
	ImGui::Checkbox(_("Enable Notifications"), &toastEnabled);
	if(toastEnabled){
		ImGui::Indent();
		for(int i = 0; i < TOAST_NUM_CATEGORIES; i++)
			ImGui::Checkbox(_(toastCategoryNames[i]), &toastCategoryEnabled[i]);
		ImGui::Unindent();
	}
}

// From the demo, slightly changed
// The Log window: everything log() prints, from any thread, with when it
// happened. Lines that read as errors or warnings stand out and are counted
// in the status bar. The same lines go to haumea.log in the config folder
// (the last run's to haumea.previous.log), so a user can send it.
struct LogLine {
	char time[12];
	int level;
	std::string text;
};
enum { LOG_INFO, LOG_WARNING, LOG_ERROR };
static std::mutex gLogMutex;
static std::deque<LogLine> gLogLines;
static const size_t MAX_LOG_LINES = 20000;
static int gLogErrors, gLogWarnings;	// since the last Clear
static int gLogSeenErrors;	// errors when the Log window was last open
static FILE *gLogFile;
static bool gLogFileTried;
static std::string gLogFilePath;

static int
logLevel(const std::string &text)
{
	std::string low = text;
	for(char &c : low)
		c = (char)tolower((unsigned char)c);
	static const char *errors[] = { "error", "failed", "fail to", "could not", "couldn't", "can't", "cannot",
		"unable to", "permission denied", "not found",
		// The pt-BR interface's toasts.
		"erro", "falh", "não foi possível", "não pôde", "não é possível", "não dá" };
	for(const char *e : errors)
		if(low.find(e) != std::string::npos)
			return LOG_ERROR;
	if(low.find("warning") != std::string::npos || low.find("missing") != std::string::npos)
		return LOG_WARNING;
	return LOG_INFO;
}

// "error: " or "warning: " for the file and Copy, unless the line says so itself.
static const char*
logTag(const LogLine &l)
{
	if(l.level == LOG_INFO)
		return "";
	const char *tag = l.level == LOG_ERROR ? "error" : "warning";
	for(size_t i = 0; tag[i]; i++)
		if(tolower((unsigned char)l.text[i]) != tag[i])	// text ends in '\0', which stops it
			return l.level == LOG_ERROR ? "error: " : "warning: ";
	return "";
}

static void
writeLogLine(const LogLine &l)
{
	if(gLogFile){
		fprintf(gLogFile, "%s %s%s\n", l.time, logTag(l), l.text.c_str());
		fflush(gLogFile);
	}
}

void
addToLogWindow(const char *fmt, va_list args)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, args);
	time_t now = time(nil);
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	std::lock_guard<std::mutex> lock(gLogMutex);
	for(const char *p = buf; *p; ){
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if(len > 0){
			LogLine l;
			strftime(l.time, sizeof(l.time), "%H:%M:%S", &local);
			l.text.assign(p, len);
			l.level = logLevel(l.text);
			if(l.level == LOG_ERROR) gLogErrors++;
			if(l.level == LOG_WARNING) gLogWarnings++;
			writeLogLine(l);
			gLogLines.push_back(l);
			if(gLogLines.size() > MAX_LOG_LINES)
				gLogLines.pop_front();
		}
		p += len + (eol ? 1 : 0);
	}
}

// The log file opens at start, as soon as the settings folder is known, so a
// start that fails before the first frame still leaves one; what was logged
// before goes in first. Command-line runs keep none.
void
OpenLogFile(bool append)
{
	if(gLogFileTried)
		return;
	gLogFileTried = true;
	if(SettingsAreReadOnly())
		return;
	char path[1024], previous[1024];
	if(!BuildPath(path, sizeof(path), GetConfigDir(), "haumea.log") ||
	   !BuildPath(previous, sizeof(previous), GetConfigDir(), "haumea.previous.log"))
		return;
	if(!append){
		remove(previous);
		rename(path, previous);
	}
	std::lock_guard<std::mutex> lock(gLogMutex);
	gLogFile = fopen(path, append ? "a" : "w");
	if(gLogFile == nil)
		return;
	gLogFilePath = path;
	for(const LogLine &l : gLogLines)
		writeLogLine(l);
#ifdef _WIN32
	// Started without a console, stderr goes nowhere, and librw prints its
	// errors (a renderer that won't start, say) there: into haumea.stderr.log.
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	char errPath[1024];
	if((err == nil || err == INVALID_HANDLE_VALUE) &&
	   BuildPath(errPath, sizeof(errPath), GetConfigDir(), "haumea.stderr.log") &&
	   freopen(errPath, "w", stderr))
		setvbuf(stderr, nil, _IONBF, 0);
#endif
}

static void
drawLogWindow(bool *open)
{
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40.0f, ImGui::GetFontSize() * 20.0f), ImGuiCond_FirstUseEver);
	// ###Log: the ID the window always had, so saved layouts stay.
	char title[64];
	snprintf(title, sizeof(title), ICON_FA_LIST " %s###Log", _("Log"));
	if(!ImGui::Begin(title, open)){
		ImGui::End();
		return;
	}
	static bool showInfo = true, showWarnings = true, showErrors = true, follow = true;
	static ImGuiTextFilter filter;
	int errors, warnings;
	{
		std::lock_guard<std::mutex> lock(gLogMutex);
		errors = gLogErrors;
		warnings = gLogWarnings;
		gLogSeenErrors = errors;
	}
	char label[64];
	ImGui::Checkbox(L("Info"), &showInfo);
	ImGui::SameLine();
	snprintf(label, sizeof(label), "%s (%d)###warnings", _("Warnings"), warnings);
	ImGui::Checkbox(label, &showWarnings);
	ImGui::SameLine();
	snprintf(label, sizeof(label), "%s (%d)###errors", _("Errors"), errors);
	ImGui::Checkbox(label, &showErrors);
	ImGui::SameLine();
	filter.Draw("##filter", ImGui::GetFontSize() * 10.0f);
	ImGui::SetItemTooltip("%s", _("Filter: words to find, -word to leave out"));
	ImGui::SameLine();
	bool copy = ImGui::Button(LI(ICON_FA_COPY, "Copy"));
	ImGui::SetItemTooltip("%s", _("Copies the lines shown, to paste in a bug report"));
	ImGui::SameLine();
	if(ImGui::Button(LI(ICON_FA_TRASH, "Clear"))){
		std::lock_guard<std::mutex> lock(gLogMutex);
		gLogLines.clear();
		gLogErrors = gLogWarnings = gLogSeenErrors = 0;
	}
	ImGui::SameLine();
	ImGui::Checkbox(L("Follow"), &follow);
	ImGui::SetItemTooltip("%s", _("Keep the newest line in view"));
	if(!gLogFilePath.empty()){
		ImGui::TextDisabled("%s", gLogFilePath.c_str());
		ImGui::SetItemTooltip("%s", _("This run's log. The one before is haumea.previous.log."));
	}
	ImGui::Separator();

	// The lines to show, copied out of the lock.
	std::vector<LogLine> shown;
	{
		std::lock_guard<std::mutex> lock(gLogMutex);
		for(const LogLine &l : gLogLines){
			if((l.level == LOG_INFO && !showInfo) || (l.level == LOG_WARNING && !showWarnings) ||
			   (l.level == LOG_ERROR && !showErrors))
				continue;
			if(filter.IsActive() && !filter.PassFilter(l.text.c_str()))
				continue;
			shown.push_back(l);
		}
	}
	if(copy){
		std::string text;
		for(const LogLine &l : shown)
			text += std::string(l.time) + " " + logTag(l) + l.text + "\n";
		ImGui::SetClipboardText(text.c_str());
		Toast(TOAST_COPY_PASTE, _("Copied %d log lines"), (int)shown.size());
	}
	ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImGuiListClipper clipper;
	clipper.Begin((int)shown.size());
	while(clipper.Step())
		for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
			const LogLine &l = shown[i];
			ImGui::TextDisabled("%s", l.time);
			ImGui::SameLine();
			if(l.level == LOG_ERROR)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), "%s", l.text.c_str());
			else if(l.level == LOG_WARNING)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", l.text.c_str());
			else
				ImGui::TextUnformatted(l.text.c_str());
		}
	if(follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetFontSize() * 4.0f)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
	ImGui::End();
}

static ImVec4 mkColor(rw::RGBA &c) { return ImVec4(c.red/255.0f, c.green/255.0f, c.blue/255.0f, c.alpha/255.0f); }


static bool gOpenExportPrefab = false;
static bool gOpenImportPrefab = false;
static bool gOpenExportPawn = false;
static bool gOpenImportPawn = false;
static bool gOpenCustomImport = false;
static bool gOpenReloadGame = false;
static bool gShowExportPrefab = false;
static bool gShowImportPrefab = false;
static bool gShowExportPawn = false;
static bool gShowImportPawn = false;
static bool gShowReloadGame = false;
static int gPawnExportFormat = PAWN_CODE_NATIVE;
static bool gPawnExportSelectionOnly = true;
static bool gPawnExportCentered;	// coordinates relative to the export's centre
static void uiExportPrefabPopup(void);
static void uiImportPrefabPopup(void);
static void uiExportPawnPopup(void);
static void uiImportPawnPopup(void);
static void uiOpenReportWindow(void);
static void uiCustomImportPopup(void);
static void uiReloadGamePopup(void);
static void trimLineEnding(char *line);
static int findSuggestedCustomImportId(void);

static void uiNextPanelSizeConstraints(ImVec2 minSize = ImVec2(0.0f, 0.0f));
static void uiObjectResults(int *filtered, int numFiltered, int selId);

// Widgets stretch with the panel and keep a column on the right for their
// labels, instead of ImGui's fixed 65% of the window. Call after Begin.
static void
uiFitItemsToPanel(void)
{
	ImGuiWindow *window = ImGui::GetCurrentWindow();
	float font = ImGui::GetFontSize();
	float avail = ImGui::GetContentRegionAvail().x;
	float label = clamp(avail * 0.4f, font * 5.0f, font * 11.0f);
	// Labels sit to the right of their fields. When last frame's content ran
	// past the panel's edge (a long label, a narrow panel), give the labels
	// that much more room by narrowing the fields. Kept until the panel's
	// width changes, so it settles instead of flickering.
	struct Fit { float width, extra; };
	static std::map<ImGuiID, Fit> fits;
	Fit &fit = fits[window->ID];
	if(fabsf(fit.width - avail) > 0.5f){
		fit.width = avail;
		fit.extra = 0.0f;
	}
	float overflow = window->ContentSizeIdeal.x - avail;
	if(overflow > 1.0f && overflow < avail * 0.5f)
		fit.extra = min(fit.extra + overflow, avail * 0.6f - label);
	label = clamp(label + max(fit.extra, 0.0f), label, avail * 0.6f);
	window->DC.ItemWidth = -label;
	// Plain text wraps at the panel's edge instead of running out of it.
	window->DC.TextWrapPos = 0.0f;
}

void
UiFitItemsToPanel(void)
{
	uiFitItemsToPanel();
}

// A small pill for what only some servers have, e.g. "open.mp only" or
// "0.3.DL". The same look everywhere; hover it for the reason.
void
UiBadge(const char *text, const char *tooltip)
{
	ImVec2 pad(ImGui::GetFontSize() * 0.35f, 1.0f);
	ImVec2 size = ImGui::CalcTextSize(text);
	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImVec2 end(pos.x + size.x + pad.x * 2.0f, pos.y + size.y + pad.y * 2.0f);
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(pos, end, IM_COL32(40, 85, 130, 255), size.y * 0.5f);
	drawList->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), IM_COL32(205, 230, 255, 255), text);
	ImGui::Dummy(ImVec2(end.x - pos.x, end.y - pos.y));
	if(tooltip)
		ImGui::SetItemTooltip("%s", tooltip);
}

static bool
BeginEditorDialog(const char *name, bool *open, ImGuiWindowFlags flags = 0)
{
	if(open && !*open)
		return false;

	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	flags |= ImGuiWindowFlags_AlwaysAutoResize |
	         ImGuiWindowFlags_NoCollapse;
	if(!ImGui::Begin(name, open, flags)){
		ImGui::End();
		return false;
	}
	return true;
}

static void
EndEditorDialog(void)
{
	ImGui::End();
}

static void
SetEditorClipboardText(const char *text)
{
	if(text == nil)
		text = "";
	ImGui::SetClipboardText(text);
#ifdef __APPLE__
	FILE *pipe = popen("/usr/bin/pbcopy", "w");
	if(pipe){
		fwrite(text, 1, strlen(text), pipe);
		pclose(pipe);
	}
#endif
}

static bool
IsCopyShortcutPressed(void)
{
	bool imguiCopy = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) ||
		ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_C);
	bool padCommand = CPad::IsCtrlDown() ||
		CPad::IsKeyDown(KEY_LSUPER) ||
		CPad::IsKeyDown(KEY_RSUPER);
	return imguiCopy || (padCommand && CPad::IsKeyJustDown('C'));
}

template <size_t N>
static void
InputTextReadonly(const char *label, const char *value)
{
	char buf[N];
	if(value == nil)
		value = "";
	strncpy(buf, value, N - 1);
	buf[N - 1] = '\0';

	const char *labelEnd = strstr(label, "##");
	if(labelEnd == nil)
		labelEnd = label + strlen(label);

	ImGui::PushID(label);
	if(labelEnd > label){
		ImGui::TextUnformatted(label, labelEnd);
		ImGui::SameLine(120.0f);
	}
	float copyButtonWidth = ImGui::GetFrameHeight();
	float fieldWidth = ImGui::GetContentRegionAvail().x - copyButtonWidth - ImGui::GetStyle().ItemSpacing.x;
	if(fieldWidth < 80.0f)
		fieldWidth = 80.0f;
	ImGui::SetNextItemWidth(fieldWidth);
	ImGui::InputText("##value", buf, N,
		ImGuiInputTextFlags_ReadOnly |
		ImGuiInputTextFlags_AutoSelectAll);
	if((ImGui::IsItemActive() || ImGui::IsItemFocused()) &&
	   IsCopyShortcutPressed()){
		SetEditorClipboardText(value);
		gCopyableTextCopiedThisFrame = true;
	}
	ImGui::SameLine();
	if(ImGui::SmallButton(ICON_FA_COPY))
		SetEditorClipboardText(value);
	ImGui::PopID();
	ImGui::SetItemTooltip(_("Copy"));
}

static const char *CUSTOM_IMPORT_MANIFEST_LOGICAL_PATH = "haumea_custom.txt";
static const char *CUSTOM_IMPORT_IDE_LOGICAL_PATH = "data/maps/haumea/custom.ide";
static const char *CUSTOM_IMPORT_IPL_LOGICAL_PATH = "data/maps/haumea/custom.ipl";

struct CustomImportState
{
	bool active;
	char sourceBase[MODELNAMELEN];
	char modelName[MODELNAMELEN];
	char txdName[MODELNAMELEN];
	char sourceDir[1024];
	char dffSource[1024];
	char txdSource[1024];
	char colSource[1024];
	bool hasCol;
	bool preferAutoCol;
	int objectId;
	float drawDist;
	ObjectDef previewObj;
	char error[512];
	char warning[512];
};
static CustomImportState gCustomImport = {};
static GameFile *gCustomImportIdeFile = nil;
static GameFile *gCustomImportIplFile = nil;

static void
restartHaumeaFromUi(void)
{
	if(!RestartHaumea())
	ToastFor(TOAST_SAVE, 6.0f, _("Could not restart Haumea. No files or editor changes were modified."));
}

static void
uiReloadGamePopup(void)
{
	if(gOpenReloadGame){
		gShowReloadGame = true;
		gOpenReloadGame = false;
		ImGui::SetNextWindowFocus();
	}

	if(!BeginEditorDialog(L("Reload Game Files"), &gShowReloadGame,
	                      ImGuiWindowFlags_NoSavedSettings))
		return;

	bool unsaved = mapHasUnsavedChanges();
	ImGui::TextWrapped(_("Haumea will close, start again, and reload the game files from disk."));
	ImGui::Spacing();
	if(unsaved){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
		                   _("Unsaved editor changes will be discarded."));
		ImGui::TextWrapped(_("Save anything you want to keep before restarting."));
	}else
		ImGui::TextDisabled(_("No unsaved map, water, or SA path changes were detected."));

	ImGui::Spacing();
	bool dontShowAgain = !gConfirmReloadGame;
	if(ImGui::Checkbox(_("Don't show again when there are no unsaved changes"), &dontShowAgain)){
		gConfirmReloadGame = !dontShowAgain;
		saveSaveSettings();
	}
	ImGui::SetItemTooltip(_("Unsaved editor changes will always require confirmation.\nYou can re-enable this prompt in the Editor window under General."));

	ImGui::Spacing();
	const char *restartLabel = unsaved ? _("Restart and Discard Changes") : _("Restart Haumea Studio");
	if(ImGui::Button(restartLabel, ImVec2(220, 0))){
		finishTransformPanelEdit();
		restartHaumeaFromUi();
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
		gShowReloadGame = false;
	EndEditorDialog();
}

struct FileRollbackEntry
{
	std::string path;
	bool existed;
	std::vector<char> data;
};

static const char*
pathFilename(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	if(backslash && (slash == nil || backslash > slash))
		slash = backslash;
	return slash ? slash + 1 : path;
}

static bool
pathsEqualCiNormalized(const char *a, const char *b)
{
	if(a == nil || b == nil)
		return false;
	while(*a || *b){
		char ca = *a++;
		char cb = *b++;
		if(ca == '\\') ca = '/';
		if(cb == '\\') cb = '/';
		if(ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
		if(cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
		if(ca != cb)
			return false;
	}
	return true;
}

static bool
pathContainsModloaderDir(const char *path)
{
	if(path == nil || path[0] == '\0')
		return false;
	char normalized[1024];
	size_t i = 0;
	for(; path[i] && i < sizeof(normalized)-1; i++){
		char c = path[i];
		if(c == '\\') c = '/';
		if(c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
		normalized[i] = c;
	}
	normalized[i] = '\0';
	return strstr(normalized, "/modloader/") != nil ||
	       strcmp(normalized, "modloader") == 0 ||
	       strncmp(normalized, "modloader/", 10) == 0;
}

static bool
pickFileDialog(char *dst, size_t size, const char *expectedExt)
{
	if(dst == nil || size == 0)
		return false;

#ifdef _WIN32
	char filename[MAX_PATH] = "";
	char previousDirectory[2048];
	DWORD previousDirectoryLen = GetCurrentDirectoryA(
		(DWORD)sizeof(previousDirectory), previousDirectory);
	bool restoreDirectory = previousDirectoryLen > 0 &&
		previousDirectoryLen < sizeof(previousDirectory);
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFile = filename;
	ofn.nMaxFile = sizeof(filename);
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	BOOL picked = GetOpenFileNameA(&ofn);
	// Keep the process environment stable even on dialog implementations that
	// ignore OFN_NOCHANGEDIR. Rooted exports no longer depend on cwd, but other
	// legacy relative-path callers still do.
	if(restoreDirectory)
		SetCurrentDirectoryA(previousDirectory);
	if(!picked)
		return false;
	strncpy(dst, filename, size-1);
	dst[size-1] = '\0';
	return true;
#else
	if(!FileDialogAvailable())
		return false;
	const char *command =
#ifdef __APPLE__
		"osascript -e 'POSIX path of (choose file)'";
#else
		"sh -lc 'if command -v zenity >/dev/null 2>&1; then zenity --file-selection; "
		"elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename; fi'";
#endif
	FILE *pipe = popen(command, "r");
	if(pipe == nil)
		return false;
	bool ok = fgets(dst, (int)size, pipe) != nil;
	pclose(pipe);
	if(!ok)
		return false;
	trimLineEnding(dst);
	return dst[0] != '\0';
#endif
}

static bool
pathHasExtensionCi(const char *path, const char *ext)
{
	size_t pathLen = strlen(path);
	size_t extLen = strlen(ext);
	if(pathLen < extLen)
		return false;
	return rw::strncmp_ci(path + pathLen - extLen, ext, (int)extLen) == 0;
}

static void
stripExtensionCopy(const char *path, char *dst, size_t size)
{
	const char *filename = pathFilename(path);
	const char *dot = strrchr(filename, '.');
	size_t len = dot && dot > filename ? (size_t)(dot - filename) : strlen(filename);
	if(len >= size)
		len = size - 1;
	memcpy(dst, filename, len);
	dst[len] = '\0';
}

static bool
getDirectoryCopy(const char *path, char *dst, size_t size)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	if(backslash && (slash == nil || backslash > slash))
		slash = backslash;
	if(slash == nil){
		if(size == 0) return false;
		dst[0] = '\0';
		return true;
	}
	size_t len = (size_t)(slash - path);
	if(len >= size)
		return false;
	memcpy(dst, path, len);
	dst[len] = '\0';
	return true;
}

static bool
buildSiblingPath(char *dst, size_t size, const char *dir, const char *base, const char *ext)
{
	if(dir == nil || dir[0] == '\0')
		return snprintf(dst, size, "%s%s", base, ext) < (int)size;
	return snprintf(dst, size, "%s/%s%s", dir, base, ext) < (int)size;
}

static bool
buildCustomImportModelLogicalPath(char *dst, size_t size, const char *name, const char *ext)
{
	return snprintf(dst, size, "models/gta3.img/%s.%s", name, ext) < (int)size;
}

static bool
buildCustomImportColLogicalPath(char *dst, size_t size, const char *name)
{
	return snprintf(dst, size, "data/maps/haumea/cols/%s.col", name) < (int)size;
}

static bool
copyFileExact(const char *src, const char *dst)
{
	// Avoid truncating the source when a user selects a file that is already
	// at Haumea's export path.
	if(pathsEqualCiNormalized(src, dst))
		return doesFileExist(src);

	FILE *in = fopen(src, "rb");
	if(in == nil)
		return false;
	if(!EnsureParentDirectoriesForPath(dst)){
		fclose(in);
		return false;
	}
	FILE *out = fopen(dst, "wb");
	if(out == nil){
		fclose(in);
		return false;
	}

	char buffer[64*1024];
	bool ok = true;
	while(!feof(in)){
		size_t n = fread(buffer, 1, sizeof(buffer), in);
		if(n == 0)
			break;
		if(fwrite(buffer, 1, n, out) != n){
			ok = false;
			break;
		}
	}
	if(ferror(in))
		ok = false;
	fclose(in);
	fclose(out);
	return ok;
}

static bool
filesHaveSameContents(const char *aPath, const char *bPath)
{
	FILE *a = fopen(aPath, "rb");
	if(a == nil)
		return false;
	FILE *b = fopen(bPath, "rb");
	if(b == nil){
		fclose(a);
		return false;
	}

	bool same = false;
	long aSize;
	long bSize;
	if(fseek(a, 0, SEEK_END) != 0 || fseek(b, 0, SEEK_END) != 0)
		goto done;
	aSize = ftell(a);
	bSize = ftell(b);
	if(aSize < 0 || bSize < 0 || aSize != bSize)
		goto done;
	if(fseek(a, 0, SEEK_SET) != 0 || fseek(b, 0, SEEK_SET) != 0)
		goto done;

	{
		char aBuffer[64*1024];
		char bBuffer[64*1024];
		same = true;
		while(true){
			size_t aRead = fread(aBuffer, 1, sizeof(aBuffer), a);
			size_t bRead = fread(bBuffer, 1, sizeof(bBuffer), b);
			if(aRead != bRead || (aRead > 0 && memcmp(aBuffer, bBuffer, aRead) != 0)){
				same = false;
				break;
			}
			if(aRead == 0){
				if(ferror(a) || ferror(b))
					same = false;
				break;
			}
		}
	}

done:
	fclose(a);
	fclose(b);
	return same;
}

static bool
writeFileExact(const char *path, const char *data, size_t size)
{
	if(!EnsureParentDirectoriesForPath(path))
		return false;
	FILE *f = fopen(path, "wb");
	if(f == nil)
		return false;
	bool ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	return ok;
}

static bool
isValidColFourcc(uint32 fourcc)
{
	return fourcc == 0x4C4C4F43 || fourcc == 0x324C4F43 ||
	       fourcc == 0x334C4F43 || fourcc == 0x344C4F43;
}

static bool
readFileExact(const char *path, std::vector<char> &data)
{
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return false;

	if(fseek(f, 0, SEEK_END) != 0){
		fclose(f);
		return false;
	}
	long size = ftell(f);
	if(size < 0){
		fclose(f);
		return false;
	}
	if(fseek(f, 0, SEEK_SET) != 0){
		fclose(f);
		return false;
	}
	data.resize((size_t)size);
	bool ok = size == 0 || fread(data.data(), 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	return ok;
}

static bool
forEachColEntry(std::vector<char> &data, bool (*cb)(ColFileHeader *header, size_t offset, void *ctx), void *ctx)
{
	size_t offset = 0;
	bool sawEntry = false;
	while(offset + 32 <= data.size()){
		ColFileHeader *header = (ColFileHeader*)&data[offset];
		if(!isValidColFourcc(header->fourcc) || header->modelsize < 24)
			return sawEntry;
		sawEntry = true;
		if(!cb(header, offset, ctx))
			return false;

		size_t nextOffset = offset + 8 + (size_t)header->modelsize;
		if(nextOffset <= offset)
			return false;
		if(nextOffset > data.size())
			nextOffset = data.size();
		offset = nextOffset;
	}
	return sawEntry;
}

struct ColInspectContext
{
	const char *modelName;
	int count;
	bool allMatch;
	char firstName[25];
};

static bool
inspectColEntry(ColFileHeader *header, size_t, void *ctx)
{
	ColInspectContext *inspect = (ColInspectContext*)ctx;
	char entryName[25];
	memcpy(entryName, header->name, sizeof(header->name));
	entryName[sizeof(header->name)] = '\0';
	if(inspect->count == 0){
		strncpy(inspect->firstName, entryName, sizeof(inspect->firstName)-1);
		inspect->firstName[sizeof(inspect->firstName)-1] = '\0';
	}
	if(rw::strncmp_ci(entryName, inspect->modelName, MODELNAMELEN) != 0)
		inspect->allMatch = false;
	inspect->count++;
	return true;
}

static bool
inspectColFileForImport(const char *path, int *entryCount, bool *allEntriesMatchModel,
                        bool *singleEntryNeedsRename, const char *modelName)
{
	std::vector<char> data;
	if(!readFileExact(path, data) || data.size() < 32)
		return false;

	ColInspectContext ctx = {};
	ctx.modelName = modelName;
	ctx.allMatch = true;
	ctx.firstName[0] = '\0';
	if(!forEachColEntry(data, inspectColEntry, &ctx) || ctx.count == 0)
		return false;

	if(entryCount) *entryCount = ctx.count;
	if(allEntriesMatchModel) *allEntriesMatchModel = ctx.allMatch;
	if(singleEntryNeedsRename) *singleEntryNeedsRename = ctx.count == 1 &&
		rw::strncmp_ci(ctx.firstName, modelName, MODELNAMELEN) != 0;
	return true;
}

static bool
copyColWithInternalRename(const char *src, const char *dst, const char *modelName)
{
	std::vector<char> data;
	if(!readFileExact(src, data) || data.size() < 32)
		return false;
	size_t offset = 0;
	bool sawEntry = false;
	while(offset + 32 <= data.size()){
		ColFileHeader *header = (ColFileHeader*)&data[offset];
		if(!isValidColFourcc(header->fourcc) || header->modelsize < 24)
			break;
		sawEntry = true;
		if(offset + 8 + (size_t)header->modelsize > data.size())
			header->modelsize = (uint32)(data.size() - offset - 8);
		size_t modelLen = strlen(modelName);
		if(modelLen >= sizeof(header->name))
			return false;
		memcpy(header->name, modelName, modelLen);
		header->name[modelLen] = '\0';

		size_t nextOffset = offset + 8 + (size_t)header->modelsize;
		if(nextOffset <= offset)
			return false;
		offset = nextOffset;
	}
	if(!sawEntry)
		return false;
	return writeFileExact(dst, data.data(), data.size());
}

static bool
ensureTextFileExists(const char *path, const char *contents)
{
	if(doesFileExist(path))
		return true;
	if(!EnsureParentDirectoriesForPath(path))
		return false;
	FILE *f = fopen(path, "w");
	if(f == nil)
		return false;
	fputs(contents, f);
	fclose(f);
	return true;
}

static bool
captureRollbackEntry(std::vector<FileRollbackEntry> &entries, const char *path)
{
	for(size_t i = 0; i < entries.size(); i++)
		if(entries[i].path == path)
			return true;

	FileRollbackEntry entry;
	entry.path = path;
	entry.existed = doesFileExist(path);
	if(entry.existed){
		FILE *f = fopen(path, "rb");
		if(f == nil)
			return false;
		if(fseek(f, 0, SEEK_END) != 0){
			fclose(f);
			return false;
		}
		long size = ftell(f);
		if(size < 0){
			fclose(f);
			return false;
		}
		if(fseek(f, 0, SEEK_SET) != 0){
			fclose(f);
			return false;
		}
		entry.data.resize((size_t)size);
		if(size > 0 && fread(entry.data.data(), 1, (size_t)size, f) != (size_t)size){
			fclose(f);
			return false;
		}
		fclose(f);
	}
	entries.push_back(entry);
	return true;
}

static void
rollbackTouchedFiles(const std::vector<FileRollbackEntry> &entries)
{
	for(size_t i = entries.size(); i > 0; i--){
		const FileRollbackEntry &entry = entries[i-1];
		if(entry.existed)
			writeFileExact(entry.path.c_str(), entry.data.data(), entry.data.size());
		else
			remove(entry.path.c_str());
	}
}

static void
trimLineEnding(char *line)
{
	char *p = line + strlen(line);
	while(p > line && (p[-1] == '\n' || p[-1] == '\r'))
		*--p = '\0';
}

static bool
appendUniqueLine(const char *path, const char *line)
{
	char existing[1024];
	FILE *f = fopen(path, "r");
	if(f){
		while(fgets(existing, sizeof(existing), f)){
			trimLineEnding(existing);
			if(strcmp(existing, line) == 0){
				fclose(f);
				return true;
			}
		}
		fclose(f);
	}
	if(!EnsureParentDirectoriesForPath(path))
		return false;
	f = fopen(path, "a");
	if(f == nil)
		return false;
	bool needNewline = false;
	if(fseek(f, 0, SEEK_END) == 0 && ftell(f) > 0){
		if(fseek(f, -1, SEEK_END) == 0)
			needNewline = fgetc(f) != '\n';
		fseek(f, 0, SEEK_END);
	}
	if(needNewline)
		fputc('\n', f);
	fprintf(f, "%s\n", line);
	fclose(f);
	return true;
}

static void
resetCustomImportPreview(void)
{
	memset(&gCustomImport.previewObj, 0, sizeof(gCustomImport.previewObj));
	gCustomImport.previewObj.m_type = ObjectDef::ATOMIC;
	gCustomImport.previewObj.m_numAtomics = 1;
	gCustomImport.previewObj.m_drawDist[0] = gCustomImport.drawDist;
}

static void
resetCustomImportState(void)
{
	memset(&gCustomImport, 0, sizeof(gCustomImport));
	gCustomImport.active = true;
	gCustomImport.objectId = findSuggestedCustomImportId();
	gCustomImport.drawDist = 300.0f;
	resetCustomImportPreview();
}

static void
beginEmptyCustomImport(void)
{
	resetCustomImportState();
	gOpenCustomImport = true;
}

static void
clearCustomImportMessages(void)
{
	gCustomImport.error[0] = '\0';
	gCustomImport.warning[0] = '\0';
}

static void
clearCustomImportColSelection(void)
{
	gCustomImport.colSource[0] = '\0';
	gCustomImport.hasCol = false;
	gCustomImport.preferAutoCol = true;
	clearCustomImportMessages();
}

static void
setCustomImportColPath(const char *path)
{
	if(path == nil || path[0] == '\0'){
		clearCustomImportColSelection();
		return;
	}
	strncpy(gCustomImport.colSource, path, sizeof(gCustomImport.colSource)-1);
	gCustomImport.colSource[sizeof(gCustomImport.colSource)-1] = '\0';
	gCustomImport.hasCol = true;
	gCustomImport.preferAutoCol = false;
	clearCustomImportMessages();
}

static void
autofillCustomImportSiblingPaths(const char *baseName)
{
	char txdPath[1024];
	char colPath[1024];
	if(gCustomImport.sourceDir[0] == '\0' || baseName == nil || baseName[0] == '\0')
		return;
	if(buildSiblingPath(txdPath, sizeof(txdPath), gCustomImport.sourceDir, baseName, ".txd") &&
	   doesFileExist(txdPath)){
		strncpy(gCustomImport.txdSource, txdPath, sizeof(gCustomImport.txdSource)-1);
		gCustomImport.txdSource[sizeof(gCustomImport.txdSource)-1] = '\0';
	}
	if(!gCustomImport.preferAutoCol &&
	   buildSiblingPath(colPath, sizeof(colPath), gCustomImport.sourceDir, baseName, ".col") &&
	   doesFileExist(colPath))
		setCustomImportColPath(colPath);
}

static void
setCustomImportDffPath(const char *path)
{
	char detectedBase[MODELNAMELEN];
	if(path == nil || path[0] == '\0')
		return;
	strncpy(gCustomImport.dffSource, path, sizeof(gCustomImport.dffSource)-1);
	gCustomImport.dffSource[sizeof(gCustomImport.dffSource)-1] = '\0';
	if(getDirectoryCopy(path, gCustomImport.sourceDir, sizeof(gCustomImport.sourceDir))){
		stripExtensionCopy(path, detectedBase, sizeof(detectedBase));
		strncpy(gCustomImport.sourceBase, detectedBase, sizeof(gCustomImport.sourceBase)-1);
		gCustomImport.sourceBase[sizeof(gCustomImport.sourceBase)-1] = '\0';
		strncpy(gCustomImport.modelName, detectedBase, sizeof(gCustomImport.modelName)-1);
		gCustomImport.modelName[sizeof(gCustomImport.modelName)-1] = '\0';
		gCustomImport.txdSource[0] = '\0';
		strncpy(gCustomImport.txdName, detectedBase, sizeof(gCustomImport.txdName)-1);
		gCustomImport.txdName[sizeof(gCustomImport.txdName)-1] = '\0';
		if(!gCustomImport.preferAutoCol){
			gCustomImport.colSource[0] = '\0';
			gCustomImport.hasCol = false;
		}
		autofillCustomImportSiblingPaths(detectedBase);
	}
	clearCustomImportMessages();
}

static void
setCustomImportTxdPath(const char *path)
{
	char detectedBase[MODELNAMELEN];
	if(path == nil || path[0] == '\0')
		return;
	strncpy(gCustomImport.txdSource, path, sizeof(gCustomImport.txdSource)-1);
	gCustomImport.txdSource[sizeof(gCustomImport.txdSource)-1] = '\0';
	stripExtensionCopy(path, detectedBase, sizeof(detectedBase));
	strncpy(gCustomImport.txdName, detectedBase, sizeof(gCustomImport.txdName)-1);
	gCustomImport.txdName[sizeof(gCustomImport.txdName)-1] = '\0';
	clearCustomImportMessages();
}

static bool
chooseCustomImportFile(const char *expectedExt, char *pickedPath, size_t pickedPathSize)
{
	char path[1024];
	path[0] = '\0';
	if(!pickFileDialog(path, sizeof(path), expectedExt))
		return false;
	if(!pathHasExtensionCi(path, expectedExt)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Please choose a %s file."), expectedExt);
		return false;
	}
	strncpy(pickedPath, path, pickedPathSize-1);
	pickedPath[pickedPathSize-1] = '\0';
	return true;
}

static int
computeFlagsFromObjectDef(const ObjectDef *obj)
{
	int flags = 0;
	switch(params.objFlagset){
	case GAME_III:
		if(obj->m_normalCull) flags |= 1;
		if(obj->m_noFade) flags |= 2;
		if(obj->m_drawLast) flags |= obj->m_additive ? 8 : 4;
		if(obj->m_isSubway) flags |= 0x10;
		if(obj->m_ignoreLight) flags |= 0x20;
		if(obj->m_noZwrite) flags |= 0x40;
		break;
	case GAME_VC:
		if(obj->m_wetRoadReflection) flags |= 1;
		if(obj->m_noFade) flags |= 2;
		if(obj->m_drawLast) flags |= obj->m_additive ? 8 : 4;
		if(obj->m_isSubway) flags |= 0x10;
		if(obj->m_ignoreLight) flags |= 0x20;
		if(obj->m_noZwrite) flags |= 0x40;
		if(obj->m_noShadows) flags |= 0x80;
		if(obj->m_ignoreDrawDist) flags |= 0x100;
		if(obj->m_isCodeGlass) flags |= 0x200;
		if(obj->m_isArtistGlass) flags |= 0x400;
		break;
	case GAME_SA:
		if(obj->m_drawLast) flags |= obj->m_additive ? 8 : 4;
		if(obj->m_noZwrite) flags |= 0x40;
		if(obj->m_noShadows) flags |= 0x80;
		if(obj->m_noBackfaceCulling) flags |= 0x200000;
		if(obj->m_type == ObjectDef::ATOMIC){
			if(obj->m_wetRoadReflection) flags |= 1;
			if(obj->m_dontCollideWithFlyer) flags |= 0x8000;
			if(obj->m_isCodeGlass) flags |= 0x200;
			if(obj->m_isArtistGlass) flags |= 0x400;
			if(obj->m_isGarageDoor) flags |= 0x800;
			if(obj->m_isDamageable && !obj->m_isTimed) flags |= 0x1000;
			if(obj->m_isTree) flags |= 0x2000;
			if(obj->m_isPalmTree) flags |= 0x4000;
			if(obj->m_isTag) flags |= 0x100000;
			if(obj->m_noCover) flags |= 0x400000;
			if(obj->m_wetOnly) flags |= 0x800000;
		}else if(obj->m_isDoor)
			flags |= 0x20;
		break;
	}
	return flags;
}

static void
uiObjectFlagsEditor(ObjectDef *obj)
{
	switch(params.objFlagset){
	case GAME_III:
		ImGui::Checkbox(_("Normal cull"), &obj->m_normalCull);
		ImGui::Checkbox(_("No Fade"), &obj->m_noFade);
		ImGui::Checkbox(_("Draw Last"), &obj->m_drawLast);
		ImGui::Checkbox(_("Additive Blend"), &obj->m_additive);
		if(obj->m_additive) obj->m_drawLast = true;
		ImGui::Checkbox(_("Is Subway"), &obj->m_isSubway);
		ImGui::Checkbox(_("Ignore Light"), &obj->m_ignoreLight);
		ImGui::Checkbox(_("No Z-write"), &obj->m_noZwrite);
		break;

	case GAME_VC:
		ImGui::Checkbox(_("Wet Road Effect"), &obj->m_wetRoadReflection);
		ImGui::Checkbox(_("No Fade"), &obj->m_noFade);
		ImGui::Checkbox(_("Draw Last"), &obj->m_drawLast);
		ImGui::Checkbox(_("Additive Blend"), &obj->m_additive);
		if(obj->m_additive) obj->m_drawLast = true;
		ImGui::Checkbox(_("Ignore Light"), &obj->m_ignoreLight);
		ImGui::Checkbox(_("No Z-write"), &obj->m_noZwrite);
		ImGui::Checkbox(_("No shadows"), &obj->m_noShadows);
		ImGui::Checkbox(_("Ignore Draw Dist"), &obj->m_ignoreDrawDist);
		ImGui::Checkbox(_("Code Glass"), &obj->m_isCodeGlass);
		ImGui::Checkbox(_("Artist Glass"), &obj->m_isArtistGlass);
		break;

	case GAME_SA: {
		ImGui::Checkbox(_("Draw Last"), &obj->m_drawLast);
		ImGui::Checkbox(_("Additive Blend"), &obj->m_additive);
		if(obj->m_additive) obj->m_drawLast = true;
		ImGui::Checkbox(_("No Z-write"), &obj->m_noZwrite);
		ImGui::Checkbox(_("No shadows"), &obj->m_noShadows);
		ImGui::Checkbox(_("No Backface Culling"), &obj->m_noBackfaceCulling);
		if(obj->m_type == ObjectDef::ATOMIC){
			ImGui::Checkbox(_("Wet Road Effect"), &obj->m_wetRoadReflection);
			ImGui::Checkbox(_("Don't collide with Flyer"), &obj->m_dontCollideWithFlyer);

			int flag = (int)obj->m_isCodeGlass |
				(int)obj->m_isArtistGlass<<1 |
				(int)obj->m_isGarageDoor<<2 |
				(int)obj->m_isDamageable<<3 |
				(int)obj->m_isTree<<4 |
				(int)obj->m_isPalmTree<<5 |
				(int)obj->m_isTag<<6 |
				(int)obj->m_noCover<<7 |
				(int)obj->m_wetOnly<<8;
			ImGui::RadioButton(_("None"), &flag, 0);
			ImGui::RadioButton(_("Code Glass"), &flag, 1);
			ImGui::RadioButton(_("Artist Glass"), &flag, 2);
			ImGui::RadioButton(_("Garage Door"), &flag, 4);
			if(!obj->m_isTimed)
				ImGui::RadioButton(_("Damageable"), &flag, 8);
			ImGui::RadioButton(_("Tree"), &flag, 0x10);
			ImGui::RadioButton(_("Palm Tree"), &flag, 0x20);
			ImGui::RadioButton(_("Tag"), &flag, 0x40);
			ImGui::RadioButton(_("No Cover"), &flag, 0x80);
			ImGui::RadioButton(_("Wet Only"), &flag, 0x100);
			obj->m_isCodeGlass = !!(flag & 1);
			obj->m_isArtistGlass = !!(flag & 2);
			obj->m_isGarageDoor = !!(flag & 4);
			obj->m_isDamageable = !!(flag & 8);
			obj->m_isTree = !!(flag & 0x10);
			obj->m_isPalmTree = !!(flag & 0x20);
			obj->m_isTag = !!(flag & 0x40);
			obj->m_noCover = !!(flag & 0x80);
			obj->m_wetOnly = !!(flag & 0x100);
		}else if(obj->m_type == ObjectDef::CLUMP)
			ImGui::Checkbox(_("Door"), &obj->m_isDoor);
		break;
	}
	}
}

static int
findSuggestedCustomImportId(void)
{
	int limit = NUMOBJECTDEFS;
	int start = gCustomImportPreferredStartId;
	if(start < 0)
		start = 0;
	if(start >= limit)
		start = limit - 1;
	int maxExisting = start - 1;
	for(int i = start; i < limit; i++)
		if(GetObjectDef(i))
			maxExisting = i;
	for(int i = maxExisting + 1; i < limit; i++)
		if(GetObjectDef(i) == nil)
			return i;
	for(int i = start; i < limit; i++)
		if(GetObjectDef(i) == nil)
			return i;
	return -1;
}

static void
beginCustomImportFromPath(const char *path)
{
	char detectedBase[MODELNAMELEN];
	char dffPath[1024];
	char txdPath[1024];
	char colPath[1024];

	resetCustomImportState();
	stripExtensionCopy(path, detectedBase, sizeof(detectedBase));
	if(!getDirectoryCopy(path, gCustomImport.sourceDir, sizeof(gCustomImport.sourceDir)))
		return;
	if(!buildSiblingPath(dffPath, sizeof(dffPath), gCustomImport.sourceDir, detectedBase, ".dff") ||
	   !buildSiblingPath(txdPath, sizeof(txdPath), gCustomImport.sourceDir, detectedBase, ".txd") ||
	   !buildSiblingPath(colPath, sizeof(colPath), gCustomImport.sourceDir, detectedBase, ".col"))
		return;
	if(!doesFileExist(dffPath) || !doesFileExist(txdPath)){
		Toast(TOAST_SPAWN, _("Custom import needs matching .dff and .txd in the same folder"));
		return;
	}

	strncpy(gCustomImport.sourceBase, detectedBase, sizeof(gCustomImport.sourceBase)-1);
	strncpy(gCustomImport.modelName, detectedBase, sizeof(gCustomImport.modelName)-1);
	strncpy(gCustomImport.txdName, detectedBase, sizeof(gCustomImport.txdName)-1);
	strncpy(gCustomImport.dffSource, dffPath, sizeof(gCustomImport.dffSource)-1);
	strncpy(gCustomImport.txdSource, txdPath, sizeof(gCustomImport.txdSource)-1);
	if(doesFileExist(colPath)){
		strncpy(gCustomImport.colSource, colPath, sizeof(gCustomImport.colSource)-1);
		gCustomImport.hasCol = true;
	}
	gOpenCustomImport = true;
}

static bool
hasInvalidModelTokenChars(const char *s)
{
	if(s == nil || s[0] == '\0')
		return true;
	for(const char *p = s; *p; p++)
		if(!isalnum((unsigned char)*p) && *p != '_')
			return true;
	return false;
}

static GameFile*
getOrCreateCustomImportGameFile(GameFile **cache, const char *logicalPath)
{
	if(*cache){
		if((*cache)->sourcePath == nil){
			const char *src = ModloaderGetSourcePath(logicalPath);
			if(src)
				(*cache)->sourcePath = strdup(src);
			else{
				char exportPath[1024];
				if(BuildModloaderLogicalExportPath(logicalPath, exportPath, sizeof(exportPath)))
					(*cache)->sourcePath = strdup(exportPath);
			}
		}
		return *cache;
	}
	char mutablePath[256];
	strncpy(mutablePath, logicalPath, sizeof(mutablePath)-1);
	mutablePath[sizeof(mutablePath)-1] = '\0';
	*cache = NewGameFile(mutablePath);
	return *cache;
}

static bool
spawnCustomImportedObject(int objectId)
{
	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil)
		return false;

	rw::V3d position = GetPlacementPosition();
	GameFile *file = IsIplMapDocumentOpen() ?
		GetOrCreateCurrentPlacementIplFile() :
		getOrCreateCustomImportGameFile(&gCustomImportIplFile, CUSTOM_IMPORT_IPL_LOGICAL_PATH);
	int maxIplIndex = -1;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other->m_file && file &&
		   LogicalPathEquals(other->m_file->name, file->name) &&
		   other->m_imageIndex < 0 && other->m_iplIndex > maxIplIndex)
			maxIplIndex = other->m_iplIndex;
	}

	ObjectInst *inst = AddInstance();
	inst->m_objectId = objectId;
	inst->m_area = currentArea;
	inst->m_rotation.x = 0.0f;
	inst->m_rotation.y = 0.0f;
	inst->m_rotation.z = 0.0f;
	inst->m_rotation.w = 1.0f;
	inst->m_translation = position;
	inst->m_lodId = -1;
	inst->m_lod = nil;
	inst->m_numChildren = 0;
	inst->m_file = file;
	inst->m_imageIndex = -1;
	inst->m_binInstIndex = -1;
	inst->m_iplIndex = maxIplIndex + 1;
	SetInstIplFilterKey(inst, file ? file->name : nil);
	inst->m_isAdded = true;
	inst->m_iplRotation = IsIplMapDocumentOpen();
	inst->m_isDirty = true;
	inst->m_savedStateValid = false;
	inst->m_wasSavedDeleted = false;
	inst->m_gameEntityExists = false;
	StampChangeSeq(inst);

	if(obj->m_isBigBuilding)
		inst->SetupBigBuilding();
	inst->UpdateMatrix();
	if(!obj->IsLoaded()){
		RequestObject(objectId);
		LoadAllRequestedObjects();
	}

	inst->CreateRwObject();
	if(obj->m_colModel)
		InsertInstIntoSectors(inst);
	else{
		CPtrList *list = inst->m_isBigBuilding
			? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
		list->InsertItem(inst);
	}

	ClearSelection();
	inst->Select();
	ObjectInst *pasted[1] = { inst };
	UndoRecordPaste(pasted, 1);
	return true;
}

void
HandleCustomImportDrop(const char *path)
{
	if(path == nil)
		return;
	// The old import wrote into the game's modloader/ folder, which SA-MP
	// players don't have.
	if(InTextDrawMode() && (pathHasExtensionCi(path, ".png") || pathHasExtensionCi(path, ".bmp") ||
	   pathHasExtensionCi(path, ".tga"))){
		AddTextDrawImageLayer(path);
		return;
	}
	if(pathHasExtensionCi(path, ".dff") ||
	   pathHasExtensionCi(path, ".txd") ||
	   pathHasExtensionCi(path, ".col"))
		OpenAddCustomModelDialog(path);
}

static void uiResetPanelLayout(void);

static void
uiUndo(void)
{
	if(InPawnIdeMode()){ PawnEditorRequest(PAWN_EDITOR_UNDO); return; }
	if(TextDrawModeShortcut(SC_UNDO))
		return;
	finishTransformPanelEdit();
	Undo();
	Toast(TOAST_UNDO_REDO, _("Undo"));
}

static void
uiRedo(void)
{
	if(InPawnIdeMode()){ PawnEditorRequest(PAWN_EDITOR_REDO); return; }
	if(TextDrawModeShortcut(SC_REDO))
		return;
	finishTransformPanelEdit();
	Redo();
	Toast(TOAST_UNDO_REDO, _("Redo"));
}

// Flat icon button for the bars; a tool or option that is on is filled
// with the accent, like the tools of a creative suite.
static bool
uiToolbarButton(const char *label, bool active, const char *tooltip, bool enabled = true, ImVec2 size = ImVec2(0, 0))
{
	ImVec4 fill = UiAccentFill();
	ImVec4 hovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
	ImGui::PushStyleColor(ImGuiCol_Button, active ? fill : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ImVec4(fill.x, fill.y, fill.z, 0.85f) : hovered);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? fill : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
	if(active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
	ImGui::BeginDisabled(!enabled);
	bool pressed = ImGui::Button(label, size);
	ImGui::EndDisabled();
	if(active) ImGui::PopStyleColor();
	ImGui::PopStyleColor(4);
	if(tooltip && tooltip[0])
		ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}

// A thin upright line between groups on a horizontal bar.
static void
uiBarDivider(void)
{
	ImGui::SameLine();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetFrameHeight();
	// A clear but quiet line between the groups, with room on both sides.
	ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	c.w *= 0.35f;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 6.0f, p.y + h*0.2f), ImVec2(p.x + 6.0f, p.y + h*0.8f),
		ImGui::GetColorU32(c));
	ImGui::Dummy(ImVec2(13.0f, h));
	ImGui::SameLine();
}

// Two or more buttons that work as one choice, in a sunken track.
static bool
uiSegment(const char *label, bool selected)
{
	ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
	if(!selected) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	bool pressed = ImGui::Button(label);
	if(!selected) ImGui::PopStyleColor();
	ImGui::PopStyleColor(2);
	return pressed;
}

static void
uiToolbarSnapSteps(void)
{
	ImGui::TextDisabled(_("Move step (hold Shift)"));
	static const float intervals[] = { 0.25f, 0.5f, 1, 2, 5, 10, 25, 50 };
	for(float step : intervals){
		char buf[16];
		snprintf(buf, sizeof(buf), "%g m", step);
		if(ImGui::MenuItem(buf, nil, gGizmoSnapTranslate == step))
			gGizmoSnapTranslate = step;
	}
	ImGui::Separator();
	ImGui::TextDisabled(_("Rotate step (hold Shift)"));
	static const float angles[] = { 5, 10, 15, 30, 45, 90 };
	for(float step : angles){
		char buf[16];
		snprintf(buf, sizeof(buf), "%g\xC2\xB0", step);
		if(ImGui::MenuItem(buf, nil, gGizmoSnapAngle == step))
			gGizmoSnapAngle = step;
	}
}

// A small drop-down of fixed steps, for the options bar.
static void
uiStepCombo(const char *id, float *value, const float *steps, int numSteps, const char *unit, const char *tooltip)
{
	char preview[24];
	snprintf(preview, sizeof(preview), "%g%s", *value, unit);
	ImGui::SetNextItemWidth(ImGui::CalcTextSize("000.00 m").x + ImGui::GetFrameHeight());
	if(ImGui::BeginCombo(id, preview)){
		for(int i = 0; i < numSteps; i++){
			char buf[24];
			snprintf(buf, sizeof(buf), "%g%s", steps[i], unit);
			if(ImGui::Selectable(buf, *value == steps[i]))
				*value = steps[i];
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("%s", tooltip);
}

static ImRect uiPanelWorkArea(void);

// ---- Main toolbar and 3D view overlay (layout after Unreal Engine 5's editor) ----
//
// The bar under the menus holds the big commands, each an icon over its name;
// the settings of the tool in use float over the 3D view in rounded pills.

static float
uiMainToolbarHeight(void)
{
	return floorf(ImGui::GetFontSize() * 3.3f);
}

// An icon over a short name. active: a tool that is on; accent: the call to
// action (Test in game). dot: a small amber mark (unsaved changes).
static bool
uiBigToolButton(const char *id, const char *icon, const char *label, bool active, const char *tip,
	bool enabled = true, bool accent = false, bool dot = false)
{
	float fs = ImGui::GetFontSize();
	float labelSize = fs * 0.78f;
	ImFont *font = ImGui::GetFont();
	ImVec2 ls = font->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label);
	ImVec2 is = ImGui::CalcTextSize(icon);
	float w = std::max(std::max(ls.x, is.x) + fs*1.2f, fs*3.3f);
	float h = uiMainToolbarHeight() - fs*0.5f;
	ImGui::PushID(id);
	ImGui::BeginDisabled(!enabled);
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool pressed = ImGui::InvisibleButton("##big", ImVec2(w, h));
	bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
	ImVec4 fill = UiAccentFill();
	ImU32 bg = 0;
	if(accent){
		float k = held ? 0.88f : hovered ? 1.15f : 1.0f;
		bg = ImGui::GetColorU32(ImVec4(fill.x*k, fill.y*k, fill.z*k, 1.0f));
	}else if(active)
		bg = ImGui::GetColorU32(ImVec4(a.x, a.y, a.z, hovered ? 0.34f : 0.24f));
	else if(hovered)
		bg = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
	ImVec2 q(p.x + w, p.y + h);
	if(bg)
		dl->AddRectFilled(p, q, bg, 7.0f);
	if(active && !accent)
		dl->AddRectFilled(ImVec2(p.x + w*0.28f, q.y - 3.0f), ImVec2(q.x - w*0.28f, q.y - 1.0f), ImGui::GetColorU32(a), 1.0f);
	ImU32 iconCol = accent ? IM_COL32_WHITE : active ? ImGui::GetColorU32(a) : ImGui::GetColorU32(ImGuiCol_Text);
	ImU32 textCol = accent ? IM_COL32_WHITE : ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	if(hovered && !accent)
		textCol = ImGui::GetColorU32(ImGuiCol_Text);
	float top = p.y + (h - is.y - ls.y - fs*0.25f) * 0.5f;
	ImVec2 iconAt(p.x + (w - is.x)*0.5f, top);
	dl->AddText(iconAt, iconCol, icon);
	dl->AddText(font, labelSize, ImVec2(p.x + (w - ls.x)*0.5f, top + is.y + fs*0.25f), textCol, label);
	if(dot)
		dl->AddCircleFilled(ImVec2(iconAt.x + is.x + 2.0f, iconAt.y + 2.0f), fs*0.2f, IM_COL32(240, 160, 40, 255));
	ImGui::EndDisabled();
	ImGui::PopID();
	if(tip && tip[0])
		ImGui::SetItemTooltip("%s", tip);
	return pressed;
}

// A tall divider between groups of big buttons.
static void
uiBigDivider(void)
{
	ImGui::SameLine(0.0f, ImGui::GetFontSize()*0.5f);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = uiMainToolbarHeight() - ImGui::GetFontSize()*0.5f;
	ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	c.w *= 0.3f;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h*0.2f), ImVec2(p.x, p.y + h*0.8f), ImGui::GetColorU32(c));
	ImGui::Dummy(ImVec2(1.0f, h));
	ImGui::SameLine(0.0f, ImGui::GetFontSize()*0.5f);
}

static void
uiMainToolbar(void)
{
	char tip[320];
	bool selected = selection.first != nil;
	float gap = 2.0f;

	snprintf(tip, sizeof(tip), "%s (%s)", _("Save"), ShortcutLabel(SC_SAVE_MAP));
	bool dirty = mapHasUnsavedChanges();
	if(uiBigToolButton("save", ICON_FA_FLOPPY_DISK, _("Save"), false, tip, isSA(), false, dirty))
		savePawnMap();
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Undo"), ShortcutLabel(SC_UNDO));
	if(uiBigToolButton("undo", ICON_FA_ROTATE_LEFT, _("Undo"), false, tip))
		uiUndo();
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Redo"), ShortcutLabel(SC_REDO));
	if(uiBigToolButton("redo", ICON_FA_ROTATE_RIGHT, _("Redo"), false, tip))
		uiRedo();

	uiBigDivider();
	if(uiBigToolButton("select", ICON_FA_ARROW_POINTER, _("Select"), !gGizmoEnabled,
	   _("Select\nClick objects to select them, drag them to move along the ground.")))
		gGizmoEnabled = false;
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), _("Move (%s)\nDrag the arrows on the selected object."), ShortcutLabel(SC_GIZMO_MOVE));
	if(uiBigToolButton("move", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, _("Move"), gGizmoEnabled && gGizmoMode == GIZMO_TRANSLATE, tip))
		uiRunShortcut(SC_GIZMO_MOVE);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), _("Rotate (%s)\nDrag the rings on the selected object."), ShortcutLabel(SC_GIZMO_ROTATE));
	if(uiBigToolButton("rotate", ICON_FA_ROTATE, _("Rotate"), gGizmoEnabled && gGizmoMode == GIZMO_ROTATE, tip))
		uiRunShortcut(SC_GIZMO_ROTATE);

	uiBigDivider();
	snprintf(tip, sizeof(tip), "%s (%s)", _("Object Browser"), ShortcutLabel(SC_WINDOW_BROWSER));
	if(uiBigToolButton("objects", ICON_FA_CUBE, _("Objects"), showBrowserWindow, tip))
		uiRunShortcut(SC_WINDOW_BROWSER);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Texture Browser"), ShortcutLabel(SC_WINDOW_TEXTURES));
	if(uiBigToolButton("textures", ICON_FA_IMAGE, _("Textures"), showTextureBrowserWindow, tip))
		uiRunShortcut(SC_WINDOW_TEXTURES);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Tools"), ShortcutLabel(SC_WINDOW_TOOLS));
	if(uiBigToolButton("tools", ICON_FA_WRENCH, _("Tools"), showToolsWindow, tip))
		uiRunShortcut(SC_WINDOW_TOOLS);

	uiBigDivider();
	snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(SC_SNAP_TO_GROUND)->name), ShortcutLabel(SC_SNAP_TO_GROUND));
	if(uiBigToolButton("drop", ICON_FA_ARROW_DOWN, _("To ground"), false, tip, selected))
		uiRunShortcut(SC_SNAP_TO_GROUND);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(SC_FOCUS_SELECTION)->name), ShortcutLabel(SC_FOCUS_SELECTION));
	if(uiBigToolButton("focus", ICON_FA_CROSSHAIRS, _("Focus"), false, tip, selected))
		uiRunShortcut(SC_FOCUS_SELECTION);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(SC_PLAYER_VIEW)->name), ShortcutLabel(SC_PLAYER_VIEW));
	if(uiBigToolButton("player", ICON_FA_USER, _("Player view"), gPlayerView, tip))
		uiRunShortcut(SC_PLAYER_VIEW);
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(SC_OUTLINE_OBJECTS)->name), ShortcutLabel(SC_OUTLINE_OBJECTS));
	if(uiBigToolButton("outline", ICON_FA_VECTOR_SQUARE, _("Outlines"), gOutlineMapObjects, tip))
		uiRunShortcut(SC_OUTLINE_OBJECTS);

	// Right end: the session (multiplayer) and the call to action, Test in game.
	if(!isSA())
		return;
	float fs = ImGui::GetFontSize();
	ImFont *font = ImGui::GetFont();
	auto widthOf = [&](const char *icon, const char *label){
		return std::max(std::max(font->CalcTextSizeA(fs*0.78f, FLT_MAX, 0.0f, label).x, ImGui::CalcTextSize(icon).x) + fs*1.2f, fs*3.3f);
	};
	float right = widthOf(ICON_FA_USERS, _("Multiplayer")) + gap + widthOf(ICON_FA_PLAY, _("Test in game"));
	float x = ImGui::GetWindowContentRegionMax().x - right;
	ImGui::SameLine();
	if(x > ImGui::GetCursorPosX())
		ImGui::SetCursorPosX(x);
	if(uiBigToolButton("multiplayer", ICON_FA_USERS, _("Multiplayer"), showMultiplayerWindow,
	   _("Edit this map together with others: host it, or join someone's.")))
		showMultiplayerWindow ^= 1;
	ImGui::SameLine(0.0f, gap);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Test in game"), ShortcutLabel(SC_TEST_IN_GAME));
	if(uiBigToolButton("test", ICON_FA_PLAY, _("Test in game"), false, tip, true, true))
		uiRunShortcut(SC_TEST_IN_GAME);
}

// A rounded, see-through pill over the 3D view.
static bool
uiBeginOverlayPill(const char *id, ImVec2 pos, ImVec2 pivot)
{
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 5.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, UiThemeIsLight() ? 0.0f : 0.07f));
	return ImGui::Begin(id, nil, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
}

static void
uiEndOverlayPill(void)
{
	ImGui::End();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(5);
}

static void
uiViewportOverlay(void)
{
	if(InTextDrawMode() || InPawnIdeMode())
		return;
	ImRect view = uiPanelWorkArea();
#ifdef IMGUI_HAS_DOCK
	if(ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaDockSpace")))
		view = ImRect(central->Pos, ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y));
#endif
	float margin = ImGui::GetFontSize()*0.6f;
	char tip[320];

	// Left: how the gizmo moves things.
	if(uiBeginOverlayPill("##overlay_transform", ImVec2(view.Min.x + margin, view.Min.y + margin), ImVec2(0, 0))){
		bool world = gGizmoSpace == GIZMO_WORLD;
		snprintf(tip, sizeof(tip), world ? _("Gizmo follows the map axes. Click or press %s for the object's own axes.") :
			_("Gizmo follows the object's own axes. Click or press %s for the map axes."), ShortcutLabel(SC_GIZMO_SPACE));
		ImGui::BeginDisabled(!gGizmoEnabled);
		if(uiSegment(TI(ICON_FA_GLOBE, "World"), world)) gGizmoSpace = GIZMO_WORLD;
		ImGui::SetItemTooltip("%s", tip);
		ImGui::SameLine(0.0f, 1.0f);
		if(uiSegment(TI(ICON_FA_CUBE, "Local"), !world)) gGizmoSpace = GIZMO_LOCAL;
		ImGui::SetItemTooltip("%s", tip);
		ImGui::EndDisabled();
		uiBarDivider();
		snprintf(tip, sizeof(tip), _("Step snapping: hold Shift while dragging the gizmo\n"
			"to move in %g m steps or rotate in %g\xC2\xB0 steps.\nRight-click to change the steps."),
			gGizmoSnapTranslate, gGizmoSnapAngle);
		if(uiToolbarButton(ICON_FA_MAGNET "##overlay_snap", gGizmoSnap, tip))
			gGizmoSnap = !gGizmoSnap;
		if(ImGui::BeginPopupContextItem("##overlay_snap_steps")){
			uiToolbarSnapSteps();
			ImGui::EndPopup();
		}
		ImGui::SameLine(0.0f, 2.0f);
		static const float intervals[] = { 0.25f, 0.5f, 1, 2, 5, 10, 25, 50 };
		static const float angles[] = { 5, 10, 15, 30, 45, 90 };
		if(gGizmoMode == GIZMO_ROTATE)
			uiStepCombo("##overlay_rotate_step", &gGizmoSnapAngle, angles, IM_ARRAYSIZE(angles), "\xC2\xB0", _("Rotate step (hold Shift)"));
		else
			uiStepCombo("##overlay_move_step", &gGizmoSnapTranslate, intervals, IM_ARRAYSIZE(intervals), " m", _("Move step (hold Shift)"));
		uiBarDivider();
		if(isSA()){
			snprintf(tip, sizeof(tip), "%s\n%s", _("Object snap"), _("Line up edges and faces with nearby objects while moving."));
			if(uiToolbarButton(ICON_FA_LOCATION_CROSSHAIRS "##overlay_object_snap", gObjectSnapMode, tip))
				gObjectSnapMode = !gObjectSnapMode;
			ImGui::SameLine(0.0f, 2.0f);
		}
		snprintf(tip, sizeof(tip), "%s\n%s", _("Follow ground"), _("Keep dragged objects on the ground surface."));
		if(uiToolbarButton(ICON_FA_MOUNTAIN "##overlay_follow_ground", gDragFollowGround, tip))
			gDragFollowGround = !gDragFollowGround;
		ImGui::SameLine(0.0f, 2.0f);
		if(uiToolbarButton(ICON_FA_GEAR "##overlay_options", false, _("More display and snapping options")))
			ImGui::OpenPopup("##overlay_options_popup");
		if(ImGui::BeginPopup("##overlay_options_popup")){
			ImGui::MenuItem(TI(ICON_FA_VECTOR_SQUARE, "Outline map objects"), ShortcutLabel(SC_OUTLINE_OBJECTS), &gOutlineMapObjects);
			ImGui::SetItemTooltip(_("Show object edges, including invisible and see-through objects."));
			ImGui::MenuItem(_("Outline selected and hovered objects"), nil, &gOutlineSelection);
			ImGui::EndPopup();
		}
	}
	uiEndOverlayPill();

	// Right: which world and interior the view shows.
	if(!isSA())
		return;
	if(uiBeginOverlayPill("##overlay_world", ImVec2(view.Max.x - margin, view.Min.y + margin), ImVec2(1, 0))){
		float field = ImGui::GetFontSize() * 2.8f;
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(ICON_FA_LAYER_GROUP);
		ImGui::SetItemTooltip("%s", _("Virtual world"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(field);
		if(ImGui::InputInt("##overlay_world_value", &gPlayerViewWorld, 0, 0))
			gPlayerViewWorld = std::max(0, gPlayerViewWorld);
		ImGui::SetItemTooltip("%s", _("Virtual world"));
		ImGui::SameLine(0.0f, ImGui::GetFontSize()*0.6f);
		ImGui::TextDisabled(ICON_FA_MAP_PIN);
		ImGui::SetItemTooltip("%s", _("Interior"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(field);
		if(ImGui::InputInt("##overlay_interior_value", &currentArea, 0, 0))
			currentArea = clamp(currentArea, 0, MAX_INTERIOR_AREA_CODE);
		ImGui::SetItemTooltip("%s", _("Interior"));
	}
	uiEndOverlayPill();
}

// The options bar: the settings of the tool in use, from left to right,
// and the world the view shows on the right.
[[maybe_unused]] static void
uiToolbar(void)
{
	char tip[256];
	const char *toolName = !gGizmoEnabled ? TI(ICON_FA_ARROW_POINTER, "Select") :
		gGizmoMode == GIZMO_ROTATE ? TI(ICON_FA_ROTATE, "Rotate") : TI(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Move");
	const char *toolKey = !gGizmoEnabled ? "" : gGizmoMode == GIZMO_ROTATE ? ShortcutLabel(SC_GIZMO_ROTATE) : ShortcutLabel(SC_GIZMO_MOVE);
	// The current tool and its key.
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(toolName);
	if(toolKey[0]){
		ImGui::SameLine(0.0f, 4.0f);
		ImGui::TextDisabled("%s", toolKey);
	}

	if(gGizmoEnabled){
		uiBarDivider();
		bool world = gGizmoSpace == GIZMO_WORLD;
		snprintf(tip, sizeof(tip), world ? _("Gizmo follows the map axes. Click or press %s for the object's own axes.") :
			_("Gizmo follows the object's own axes. Click or press %s for the map axes."), ShortcutLabel(SC_GIZMO_SPACE));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, ImGui::GetStyle().ItemSpacing.y));
		if(uiSegment(TI(ICON_FA_GLOBE, "World"), world)) gGizmoSpace = GIZMO_WORLD;
		ImGui::SetItemTooltip("%s", tip);
		ImGui::SameLine();
		if(uiSegment(TI(ICON_FA_CUBE, "Local"), !world)) gGizmoSpace = GIZMO_LOCAL;
		ImGui::SetItemTooltip("%s", tip);
		ImGui::PopStyleVar();

		uiBarDivider();
		snprintf(tip, sizeof(tip), _("Step snapping: hold Shift while dragging the gizmo\n"
			"to move in %g m steps or rotate in %g\xC2\xB0 steps.\nRight-click to change the steps."),
			gGizmoSnapTranslate, gGizmoSnapAngle);
		ImGui::Checkbox(_("Step snapping"), &gGizmoSnap);
		ImGui::SetItemTooltip("%s", tip);
		if(ImGui::BeginPopupContextItem("##snap_steps")){
			uiToolbarSnapSteps();
			ImGui::EndPopup();
		}
		static const float intervals[] = { 0.25f, 0.5f, 1, 2, 5, 10, 25, 50 };
		static const float angles[] = { 5, 10, 15, 30, 45, 90 };
		if(gGizmoMode == GIZMO_ROTATE)
			uiStepCombo("##rotate_step", &gGizmoSnapAngle, angles, IM_ARRAYSIZE(angles), "\xC2\xB0", _("Rotate step (hold Shift)"));
		else
			uiStepCombo("##move_step", &gGizmoSnapTranslate, intervals, IM_ARRAYSIZE(intervals), " m", _("Move step (hold Shift)"));
	}

	uiBarDivider();
	if(isSA()){
		ImGui::Checkbox(_("Object snap"), &gObjectSnapMode);
		ImGui::SetItemTooltip(_("Line up edges and faces with nearby objects while moving."));
		ImGui::SameLine();
	}
	ImGui::Checkbox(_("Follow ground"), &gDragFollowGround);
	ImGui::SetItemTooltip(_("Keep dragged objects on the ground surface."));

	ImGui::SameLine();
	if(uiToolbarButton(ICON_FA_GEAR "##toolbar_options", false,
	   _("More display and snapping options")))
		ImGui::OpenPopup("##toolbar_options");
	if(ImGui::BeginPopup("##toolbar_options")){
		ImGui::MenuItem(TI(ICON_FA_VECTOR_SQUARE, "Outline map objects"), ShortcutLabel(SC_OUTLINE_OBJECTS), &gOutlineMapObjects);
		ImGui::SetItemTooltip(_("Show object edges, including invisible and see-through objects."));
		ImGui::MenuItem(_("Outline selected and hovered objects"), nil, &gOutlineSelection);
		ImGui::EndPopup();
	}

	// The world and interior the view shows, at the right end.
	if(isSA()){
		// Compact: an icon and a small field each, the name in the tooltip.
		float field = ImGui::GetFontSize() * 2.8f;
		float icon = ImGui::GetFontSize() * 1.2f;
		float need = icon*2.0f + field*2.0f + ImGui::GetStyle().ItemSpacing.x*5.0f;
		float x = ImGui::GetWindowContentRegionMax().x - need;
		ImGui::SameLine();
		if(x > ImGui::GetCursorPosX())
			ImGui::SetCursorPosX(x);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(ICON_FA_LAYER_GROUP);
		ImGui::SetItemTooltip("%s", _("Virtual world"));
		ImGui::SameLine(0.0f, 4.0f);
		ImGui::SetNextItemWidth(field);
		if(ImGui::InputInt("##viewport_world", &gPlayerViewWorld, 0, 0))
			gPlayerViewWorld = std::max(0, gPlayerViewWorld);
		ImGui::SetItemTooltip("%s", _("Virtual world"));
		ImGui::SameLine();
		ImGui::TextDisabled(ICON_FA_MAP_PIN);
		ImGui::SetItemTooltip("%s", _("Interior"));
		ImGui::SameLine(0.0f, 4.0f);
		ImGui::SetNextItemWidth(field);
		if(ImGui::InputInt("##viewport_interior", &currentArea, 0, 0))
			currentArea = clamp(currentArea, 0, MAX_INTERIOR_AREA_CODE);
		ImGui::SetItemTooltip("%s", _("Interior"));
	}
}

// The tools down the left side: how a click in the world works, what
// happens to the selection, and how the view draws it.
[[maybe_unused]] static void
uiToolStrip(void)
{
	if(InTextDrawMode() || InPawnIdeMode())
		return;
	float button = floorf(ImGui::GetFrameHeight() + 8.0f);
	float pad = 5.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 3.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::BeginViewportSideBar("##ToolStrip", ImGui::GetMainViewport(), ImGuiDir_Left, button + pad*2.0f,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoScrollWithMouse);
	ImVec2 size(button, button);
	char tip[256];
	bool selected = selection.first != nil;

	if(uiToolbarButton(ICON_FA_ARROW_POINTER "##tool_select", !gGizmoEnabled,
	   _("Select\nClick objects to select them, drag them to move along the ground."), true, size))
		gGizmoEnabled = false;
	snprintf(tip, sizeof(tip), _("Move (%s)\nDrag the arrows on the selected object."), ShortcutLabel(SC_GIZMO_MOVE));
	if(uiToolbarButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "##tool_move", gGizmoEnabled && gGizmoMode == GIZMO_TRANSLATE, tip, true, size))
		uiRunShortcut(SC_GIZMO_MOVE);
	snprintf(tip, sizeof(tip), _("Rotate (%s)\nDrag the rings on the selected object."), ShortcutLabel(SC_GIZMO_ROTATE));
	if(uiToolbarButton(ICON_FA_ROTATE "##tool_rotate", gGizmoEnabled && gGizmoMode == GIZMO_ROTATE, tip, true, size))
		uiRunShortcut(SC_GIZMO_ROTATE);

	struct StripAction { const char *icon; int action; const bool *on; bool needsSelection; };
	const StripAction actions[] = {
		{ nil, -1, nil, false },
		{ ICON_FA_ARROW_DOWN "##tool_drop", SC_SNAP_TO_GROUND, nil, true },
		{ ICON_FA_CROSSHAIRS "##tool_focus", SC_FOCUS_SELECTION, nil, true },
		{ nil, -1, nil, false },
		{ ICON_FA_USER "##tool_player_view", SC_PLAYER_VIEW, &gPlayerView, false },
		{ ICON_FA_VECTOR_SQUARE "##tool_outline", SC_OUTLINE_OBJECTS, &gOutlineMapObjects, false },
		{ ICON_FA_LOCATION_CROSSHAIRS "##tool_object_snap", SC_OBJECT_SNAP, &gObjectSnapMode, false },
	};
	for(const StripAction &a : actions){
		if(a.icon == nil){
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + button*0.2f, p.y + 3.0f), ImVec2(p.x + button*0.8f, p.y + 3.0f),
				ImGui::GetColorU32(ImGuiCol_Border));
			ImGui::Dummy(ImVec2(button, 6.0f));
			continue;
		}
		if(a.action == SC_OBJECT_SNAP && !isSA())
			continue;
		const char *key = ShortcutLabel(a.action);
		if(key[0]) snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(a.action)->name), key);
		else snprintf(tip, sizeof(tip), "%s", _(GetShortcutInfo(a.action)->name));
		if(uiToolbarButton(a.icon, a.on && *a.on, tip, !a.needsSelection || selected, size))
			uiRunShortcut(a.action);
	}

	// Undo and redo at the foot of the strip.
	float foot = button*2.0f + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y;
	if(ImGui::GetWindowHeight() - foot > ImGui::GetCursorPosY())
		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - foot);
	snprintf(tip, sizeof(tip), _("Undo (%s)"), ShortcutLabel(SC_UNDO));
	if(uiToolbarButton(ICON_FA_ROTATE_LEFT "##tool_undo", false, tip, true, size))
		uiUndo();
	snprintf(tip, sizeof(tip), _("Redo (%s)"), ShortcutLabel(SC_REDO));
	if(uiToolbarButton(ICON_FA_ROTATE_RIGHT "##tool_redo", false, tip, true, size))
		uiRedo();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(4);
}

// Collapsed panels, as a column of icons on the right edge: a click opens
// or closes one, like the panel icons of a creative suite.
static bool*
uiPanelVisibility(int action)
{
	switch(action){
	case SC_WINDOW_BROWSER: return &showBrowserWindow;
	case SC_WINDOW_OBJECT_INFO: return &showInstanceWindow;
	case SC_WINDOW_MAP_OBJECTS: return &showMapObjectsWindow;
	case SC_WINDOW_TOOLS: return &showToolsWindow;
	case SC_WINDOW_TEXTURES: return &showTextureBrowserWindow;
	case SC_WINDOW_TIME_WEATHER: return &showTimeWeatherWindow;
	case SC_WINDOW_VIEW: return &showViewWindow;
	case SC_WINDOW_CAMERA: return &showEditorWindow;
	case SC_WINDOW_MAP_CHECK: return &showMapCheckWindow;
	case SC_WINDOW_CHANGES: return &showDiffWindow;
	case SC_WINDOW_LOG: return &showLogWindow;
	case SC_WINDOW_TERMINAL: return &showTerminalWindow;
	}
	return nil;
}

static void
uiPanelStrip(void)
{
	if(InTextDrawMode() || InPawnIdeMode())
		return;
	float button = floorf(ImGui::GetFrameHeight() + 4.0f);
	float pad = 4.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 6.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::BeginViewportSideBar("##PanelStrip", ImGui::GetMainViewport(), ImGuiDir_Right, button + pad*2.0f,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoScrollWithMouse);
	// action -2: the History panel, -3: Multiplayer; neither has a shortcut.
	struct PanelIcon { const char *icon; int action; };
	const PanelIcon panels[] = {
		{ ICON_FA_MAGNIFYING_GLASS "##panel_browser", SC_WINDOW_BROWSER },
		{ ICON_FA_CIRCLE_INFO "##panel_info", SC_WINDOW_OBJECT_INFO },
		{ ICON_FA_LAYER_GROUP "##panel_map_objects", SC_WINDOW_MAP_OBJECTS },
		{ ICON_FA_WRENCH "##panel_tools", SC_WINDOW_TOOLS },
		{ nil, -1 },
		{ ICON_FA_IMAGE "##panel_textures", SC_WINDOW_TEXTURES },
		{ ICON_FA_CLOUD_SUN "##panel_weather", SC_WINDOW_TIME_WEATHER },
		{ ICON_FA_EYE "##panel_view", SC_WINDOW_VIEW },
		{ ICON_FA_VIDEO "##panel_camera", SC_WINDOW_CAMERA },
		{ nil, -1 },
		{ ICON_FA_LIST_CHECK "##panel_map_check", SC_WINDOW_MAP_CHECK },
		{ ICON_FA_FILE "##panel_changes", SC_WINDOW_CHANGES },
		{ ICON_FA_CLOCK_ROTATE_LEFT "##panel_history", -2 },
		{ ICON_FA_TERMINAL "##panel_log", SC_WINDOW_LOG },
		{ nil, -1 },
		{ ICON_FA_USERS "##panel_multiplayer", -3 },
		{ ">_##panel_terminal", SC_WINDOW_TERMINAL },
	};
	char tip[256];
	for(const PanelIcon &p : panels){
		if(p.icon == nil){
			ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + button*0.2f, at.y + 3.0f), ImVec2(at.x + button*0.8f, at.y + 3.0f),
				ImGui::GetColorU32(ImGuiCol_Border));
			ImGui::Dummy(ImVec2(button, 6.0f));
			continue;
		}
		bool *open = p.action == -2 ? &showHistoryWindow : p.action == -3 ? &showMultiplayerWindow : uiPanelVisibility(p.action);
		const char *key = p.action < 0 ? "" : ShortcutLabel(p.action);
		const char *name = p.action == -2 ? _("History") : p.action == -3 ? _("Multiplayer") : _(GetShortcutInfo(p.action)->name);
		if(key[0]) snprintf(tip, sizeof(tip), "%s (%s)", name, key);
		else snprintf(tip, sizeof(tip), "%s", name);
		bool on = open && *open;
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if(uiToolbarButton(p.icon, false, tip, true, ImVec2(button, button))){
			if(p.action == -2) showHistoryWindow ^= 1;
			else if(p.action == -3) showMultiplayerWindow ^= 1;
			else uiRunShortcut(p.action);
		}
		ImGui::PopStyleColor();
		if(on){
			// A bar on the inner edge marks the open ones.
			ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(lo.x - pad, lo.y + 4.0f), ImVec2(lo.x - pad + 2.0f, hi.y - 4.0f),
				ImGui::GetColorU32(ImGuiCol_CheckMark));
		}
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(4);
}

static bool gShowWorkspaceWindow;

// File > Workspace: the server being mapped, and recent ones.
static void
uiWorkspaceMenu(void)
{
	if(!ImGui::BeginMenu(LI(ICON_FA_GLOBE, "Workspace")))
		return;
	const char *current = GetWorkspaceDir();
	ImGui::TextDisabled("%s", current[0] ? current : _("No workspace"));
	const std::vector<std::string> &recent = GetRecentWorkspaces();
	if(!recent.empty())
		ImGui::Separator();
	for(size_t i = 0; i < recent.size(); i++){
		ImGui::PushID((int)i);
		if(ImGui::MenuItem(pathFilename(recent[i].c_str()), nil, recent[i] == current))
			SetWorkspaceDir(recent[i].c_str());
		ImGui::SetItemTooltip("%s", recent[i].c_str());
		ImGui::PopID();
	}
	ImGui::Separator();
	if(ImGui::MenuItem(_("Workspace Settings...")))
		gShowWorkspaceWindow = true;
	ImGui::EndMenu();
}

static void
uiWorkspaceWindow(void)
{
	if(!gShowWorkspaceWindow)
		return;
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f), ImGuiCond_FirstUseEver);
	if(ImGui::Begin(LI(ICON_FA_GLOBE, "Workspace"), &gShowWorkspaceWindow)){
		uiFitItemsToPanel();
		uiWorkspaceSettings();
	}
	ImGui::End();
}

static void uiMaterialTexturePopup(void);
static void uiViewportObjectMenu(void);

// History: every step Undo can take, oldest first. Click one to go back
// (or forward) to it, like a creative suite's History panel.
static const char*
uiUndoStepName(int type)
{
	switch(type){
	case UNDO_MOVE: return _("Move");
	case UNDO_ROTATE: return _("Rotate");
	case UNDO_DELETE: return _("Delete");
	case UNDO_PASTE: return _("Paste");
	case UNDO_TRANSFORM_BATCH: return _("Transform");
	case UNDO_MATERIAL: return _("Materials");
	case UNDO_PROPERTIES: return _("Edit");
	case UNDO_WORLD_CONVERSION: return _("Convert to map object");
	case UNDO_VEHICLE: return _("Vehicle");
	case UNDO_MODEL_ID: return _("Model ID");
	case UNDO_NODES: return _("Nodes");
	case UNDO_RADAR_ART: return _("Radar art from objects");
	case UNDO_LAYOUT_PATHS: return _("Paths & curves");
	}
	return "?";
}

static void
uiHistoryWindow(void)
{
	uiNextPanelSizeConstraints();
	if(!ImGui::Begin(LI(ICON_FA_CLOCK_ROTATE_LEFT, "History"), &showHistoryWindow)){
		ImGui::End();
		return;
	}
	uiFitItemsToPanel();
	std::vector<UndoHistoryStep> steps;
	int done = GetUndoHistory(steps);
	char tip[64];
	ImGui::BeginDisabled(done == 0);
	snprintf(tip, sizeof(tip), "%s (%s)", _("Undo"), ShortcutLabel(SC_UNDO));
	if(uiToolbarButton(ICON_FA_ROTATE_LEFT "##history_undo", false, tip)) uiUndo();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(done >= (int)steps.size());
	snprintf(tip, sizeof(tip), "%s (%s)", _("Redo"), ShortcutLabel(SC_REDO));
	if(uiToolbarButton(ICON_FA_ROTATE_RIGHT "##history_redo", false, tip)) uiRedo();
	ImGui::EndDisabled();
	ImGui::Separator();
	ImGui::BeginChild("##history_steps");
	static int lastDone = -1;
	for(int i = 0; i <= (int)steps.size(); i++){
		// Row 0 is the map as it was before the first step.
		char label[128];
		if(i == 0)
			snprintf(label, sizeof(label), "%s##step0", _("Original"));
		else if(steps[i-1].objects > 1)
			snprintf(label, sizeof(label), "%s (%d)##step%d", uiUndoStepName(steps[i-1].type), steps[i-1].objects, i);
		else
			snprintf(label, sizeof(label), "%s##step%d", uiUndoStepName(steps[i-1].type), i);
		bool undone = i > done;
		if(undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if(ImGui::Selectable(label, i == done) && i != done){
			finishTransformPanelEdit();
			GoToUndoStep(i);
		}
		if(undone) ImGui::PopStyleColor();
		if(i == done && lastDone != done)
			ImGui::SetScrollHereY(0.5f);
	}
	lastDone = done;
	ImGui::EndChild();
	ImGui::End();
}

// The right end of the menu bar: which editor (Map, TextDraws or Code) in a
// switcher, then Save and Test in game.
static void
uiMenuBarRight(void)
{
	// Save and Test in game are on the main toolbar now; the TextDraw and
	// Code editors have their own.
	bool mapActions = false;
	ImGuiStyle &style = ImGui::GetStyle();
	const char *labels[] = { L("Map"), L("TextDraws"), L("Code") };
	float switcher = style.FramePadding.x*6.0f + style.ItemSpacing.x*2.0f + 8.0f;
	for(const char *l : labels)
		switcher += ImGui::CalcTextSize(l, nil, true).x;
	float actions = 0.0f;
	if(mapActions)
		actions = ImGui::CalcTextSize(TI(ICON_FA_FLOPPY_DISK, "Save"), nil, true).x + style.FramePadding.x*2.0f +
			ImGui::CalcTextSize(TI(ICON_FA_PLAY, "Test in game"), nil, true).x +
			style.FramePadding.x*2.0f + style.ItemSpacing.x*3.0f + 6.0f;
	float x = ImGui::GetWindowContentRegionMax().x - switcher - actions - style.ItemSpacing.x;
	if(x > ImGui::GetCursorPosX())
		ImGui::SetCursorPosX(x);
	// The switcher: a sunken track with the current editor raised.
	ImVec2 p = ImGui::GetCursorScreenPos();
	float h = ImGui::GetFrameHeight();
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 2.0f, p.y + 1.0f), ImVec2(p.x + switcher - 6.0f, p.y + h - 1.0f),
		ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, style.ItemSpacing.y));
	uiWorkspaceNavigation();
	ImGui::PopStyleVar();
	if(!mapActions)
		return;
	ImGui::Dummy(ImVec2(4.0f, 0.0f));
	char tip[96];
	snprintf(tip, sizeof(tip), "%s (%s)", _(GetShortcutInfo(SC_SAVE_MAP)->name), ShortcutLabel(SC_SAVE_MAP));
	// A labelled Save, lit amber while there is something unsaved, so it is
	// plain how to keep the work.
	bool dirty = mapHasUnsavedChanges();
	if(dirty){
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.55f, 0.10f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.63f, 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.47f, 0.06f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.02f, 1.0f));
	}
	if(ImGui::Button(TI(ICON_FA_FLOPPY_DISK, "Save")))
		savePawnMap();
	if(dirty)
		ImGui::PopStyleColor(4);
	ImGui::SetItemTooltip("%s\n%s", tip, dirty ? _("This map has unsaved changes.") : _("All changes are saved."));
	ImGui::SameLine();
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	if(ImGui::Button(TI(ICON_FA_PLAY, "Test in game")))
		uiRunShortcut(SC_TEST_IN_GAME);
	ImGui::PopStyleColor(5);
	ImGui::SetItemTooltip("%s", ShortcutLabel(SC_TEST_IN_GAME));
}

static void
uiMainmenu(void)
{
	if(ImGui::BeginMainMenuBar()){
		if(ImGui::BeginMenu(LI(ICON_FA_FOLDER_OPEN, "File"))){
			if(ImGui::MenuItem(InPawnIdeMode() ? TI(ICON_FA_FILE, "New script") : TI(ICON_FA_FILE, "New Map"), ShortcutLabel(SC_NEW_MAP), false, isSA()))
				requestMapAction(MAP_ACTION_NEW);
			if(ImGui::MenuItem(InPawnIdeMode() ? TI(ICON_FA_FOLDER_OPEN, "Open script...") : TI(ICON_FA_FOLDER_OPEN, "Open Map..."), ShortcutLabel(SC_OPEN_MAP), false, isSA()))
				requestMapAction(MAP_ACTION_OPEN);
			if(!InPawnIdeMode()) ImGui::SetItemTooltip(_("Open a .pwn map. The current scene is replaced by its objects,\nremoved buildings and materials."));
			if(ImGui::BeginMenu(LI(ICON_FA_CLOCK_ROTATE_LEFT, "Open Recent"), isSA() && !InPawnIdeMode() && gNumRecentMaps > 0)){
				for(int i = 0; i < gNumRecentMaps; i++){
					ImGui::PushID(i);
					if(ImGui::MenuItem(pathFilename(gRecentMaps[i]))){
						char path[1024];
						strncpy(path, gRecentMaps[i], sizeof(path));
						requestMapAction(MAP_ACTION_OPEN, path);
					}
					ImGui::SetItemTooltip("%s", gRecentMaps[i]);
					ImGui::PopID();
				}
				ImGui::Separator();
				if(ImGui::MenuItem(_("Reopen last map on startup"), nil, gReopenLastMap)){
					gReopenLastMap = !gReopenLastMap;
					saveSaveSettings();
				}
				if(ImGui::MenuItem(_("Clear list"))){
					gNumRecentMaps = 0;
					saveSaveSettings();
				}
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(LI(ICON_FA_CLOCK, "Recover Autosave"), isSA() && !InPawnIdeMode())){
				std::vector<std::string> autosaves;
				listAutosaves(autosaves);
				for(size_t i = 0; i < autosaves.size(); i++){
					ImGui::PushID((int)i);
					if(ImGui::MenuItem(pathFilename(autosaves[i].c_str())))
						requestMapAction(MAP_ACTION_OPEN, autosaves[i].c_str());
					ImGui::PopID();
				}
				if(autosaves.empty())
					ImGui::TextDisabled(_("No autosaves yet"));
				ImGui::EndMenu();
			}
			ImGui::SetItemTooltip(_("Opens an autosaved copy. Use Save As to keep it as your map."));
			if(ImGui::MenuItem(TI(ICON_FA_GLOBE, "Switch Workspace...")))
				ShowStartScreen();
			ImGui::SetItemTooltip(_("The start screen: workspaces, their maps and the game folder."));
			uiWorkspaceMenu();
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_FLOPPY_DISK, "Save"), ShortcutLabel(SC_SAVE_MAP), false, isSA()))
				if(InPawnIdeMode()) PawnEditorRequest(PAWN_EDITOR_SAVE); else savePawnMap();
			if(ImGui::MenuItem(TI(ICON_FA_FLOPPY_DISK, "Save As..."), ShortcutLabel(SC_SAVE_MAP_AS), false, isSA()))
				if(InPawnIdeMode()) PawnEditorRequest(PAWN_EDITOR_SAVE_AS); else gOpenSavePawnAs = true;
			if(isSA() && !InPawnIdeMode()){
				ImGui::TextDisabled(mapHasUnsavedChanges() ? _("Unsaved changes - %s") : _("Saved - %s"),
					currentMapName());
				if(gPawnSavePath[0])
					ImGui::SetItemTooltip(gPawnSaveFormat == PAWN_CODE_STREAMER ? _("%s\nStreamer code, %s text") :
						_("%s\nNative code, %s text"), gPawnSavePath,
						PawnTextEncodingName(GetPawnTextEncoding()));
			}
			ImGui::Separator();
			if(ImGui::BeginMenu(LI(ICON_FA_FILE_IMPORT, "Import"))){
				if(ImGui::MenuItem(_("Merge Pawn File..."), nil, false, isSA()))
					gOpenImportPawn = true;
				ImGui::SetItemTooltip(_("Adds the objects from another .pwn/.inc file, a Texture Studio .db,\nan MTA:SA .map or an IPL file to this map."));
				if(ImGui::MenuItem(_("Prefab..."), ShortcutLabel(SC_IMPORT_PREFAB)))
					gOpenImportPrefab = true;
				ImGui::SetItemTooltip(_("Places the objects of a prefab file (.haumea) in this map."));
				if(ImGui::MenuItem(_("Custom Model... (0.3.DL)"), nil, false, isSA()))
					OpenAddCustomModelDialog(nil);
				ImGui::SetItemTooltip(_("Puts a DFF and TXD in the server's models folder and artconfig.txt,\nso SA-MP 0.3.DL and open.mp players get it. The game folder isn't touched."));
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(LI(ICON_FA_FILE_EXPORT, "Export"), selection.first != nil)){
				if(ImGui::MenuItem(_("Selection as Pawn..."), nil, false, isSA()))
					gOpenExportPawn = true;
				ImGui::SetItemTooltip(_("Writes the selection to a separate Pawn file, e.g. to paste into\nanother script. Save writes the whole map."));
				if(ImGui::MenuItem(_("Selection as Prefab..."), ShortcutLabel(SC_EXPORT_PREFAB)))
					gOpenExportPrefab = true;
				ImGui::SetItemTooltip(_("Saves the selection as a prefab file (.haumea) to place again or share."));
				ImGui::EndMenu();
			}
			ImGui::SetItemTooltip(_("Export needs a selection."));
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_PLAY, "Test in game"), ShortcutLabel(SC_TEST_IN_GAME), false, isSA() && !InPawnIdeMode())){
				uiMapCheckBefore("testing");
				TestServerRunMap(gPawnSaveFormat);
				showTestServerWindow = true;
			}
			ImGui::SetItemTooltip(_("Runs the map as it is now on a private open.mp test server.\nYour map file and your own server are not touched."));
			if(ImGui::MenuItem(TI(ICON_FA_USER, "Multiplayer..."), nil, showMultiplayerWindow, isSA() && !InPawnIdeMode()))
				showMultiplayerWindow ^= 1;
			ImGui::SetItemTooltip(_("Edit this map together with others: host it, or join someone's."));
			if(ImGui::MenuItem(TI(ICON_FA_GAMEPAD, "Launch game"), nil, false, isSA()))
				TestServerLaunchGame();
			ImGui::SetItemTooltip(_("Starts the game (open.mp's launcher, or SA-MP) and joins the test server.\n"
				"Which one: Preferences > Test server."));
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_ROTATE, "Reload Game Files..."))){
				finishTransformPanelEdit();
				if(!gConfirmReloadGame && !mapHasUnsavedChanges())
					restartHaumeaFromUi();
				else
					gOpenReloadGame = true;
			}
			ImGui::SetItemTooltip(_("Restarts Haumea and reloads the game files from disk."));
			if(ImGui::MenuItem(TI(ICON_FA_RIGHT_FROM_BRACKET, "Exit"), "Alt+F4")) requestMapAction(MAP_ACTION_QUIT);
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu(LI(ICON_FA_PEN, "Edit"))){
   if(InPawnIdeMode()){
    if(ImGui::MenuItem(_("Undo"), "Ctrl+Z")) PawnEditorRequest(PAWN_EDITOR_UNDO);
    if(ImGui::MenuItem(_("Redo"), "Ctrl+Y")) PawnEditorRequest(PAWN_EDITOR_REDO);
    ImGui::Separator();
    if(ImGui::MenuItem(_("Cut"), "Ctrl+X")) PawnEditorRequest(PAWN_EDITOR_CUT);
    if(ImGui::MenuItem(_("Copy"), "Ctrl+C")) PawnEditorRequest(PAWN_EDITOR_COPY);
    if(ImGui::MenuItem(_("Paste"), "Ctrl+V")) PawnEditorRequest(PAWN_EDITOR_PASTE);
    if(ImGui::MenuItem(_("Select all"), "Ctrl+A")) PawnEditorRequest(PAWN_EDITOR_SELECT_ALL);
   }else{
   bool hasSelection = selection.first != nil;
			if(ImGui::MenuItem(TI(ICON_FA_ROTATE_LEFT, "Undo"), ShortcutLabel(SC_UNDO))) uiUndo();
			if(ImGui::MenuItem(TI(ICON_FA_ROTATE_RIGHT, "Redo"), ShortcutLabel(SC_REDO))) uiRedo();
			ImGui::Separator();
			if(ImGui::MenuItem(_("Copy"), ShortcutLabel(SC_COPY), false, hasSelection)) uiRunShortcut(SC_COPY);
			if(ImGui::MenuItem(_("Cut"), ShortcutLabel(SC_CUT), false, hasSelection)) uiRunShortcut(SC_CUT);
			if(ImGui::MenuItem(_("Paste"), ShortcutLabel(SC_PASTE))) uiRunShortcut(SC_PASTE);
			if(ImGui::MenuItem(_("Paste in Place"), ShortcutLabel(SC_PASTE_IN_PLACE))) uiRunShortcut(SC_PASTE_IN_PLACE);
			if(ImGui::MenuItem(_("Duplicate"), ShortcutLabel(SC_DUPLICATE), false, hasSelection)) uiRunShortcut(SC_DUPLICATE);
			if(ImGui::MenuItem(_("Delete"), ShortcutLabel(SC_DELETE), false, hasSelection)) uiRunShortcut(SC_DELETE);
			ImGui::Separator();
			if(ImGui::MenuItem(_("Focus on Selection"), ShortcutLabel(SC_FOCUS_SELECTION), false, hasSelection)) uiRunShortcut(SC_FOCUS_SELECTION);
			if(ImGui::MenuItem(_("Go to Coordinates..."), ShortcutLabel(SC_GO_TO_COORDINATES))) uiRunShortcut(SC_GO_TO_COORDINATES);
			ImGui::SetItemTooltip(_("Moves the camera to look at a point: paste x, y, z or a SetPlayerPos line."));
			if(ImGui::MenuItem(_("Copy Camera Position"), ShortcutLabel(SC_COPY_CAMERA_POSITION))) uiRunShortcut(SC_COPY_CAMERA_POSITION);
			ImGui::SetItemTooltip(_("Copies where the camera is as \"x, y, z\", ready for SetPlayerPos."));
			if(ImGui::MenuItem(_("Drop to Ground"), ShortcutLabel(SC_SNAP_TO_GROUND), false, hasSelection)) uiRunShortcut(SC_SNAP_TO_GROUND);
			if(ImGui::MenuItem(_("Drop and Align to Ground"), ShortcutLabel(SC_ALIGN_TO_GROUND), false, hasSelection)) uiRunShortcut(SC_ALIGN_TO_GROUND);
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_EYE_SLASH, "Hide Selection"), ShortcutLabel(SC_HIDE_SELECTION), false, hasSelection)) uiRunShortcut(SC_HIDE_SELECTION);
			if(ImGui::MenuItem(_("Isolate Selection"), ShortcutLabel(SC_ISOLATE_SELECTION), false, hasSelection)) uiRunShortcut(SC_ISOLATE_SELECTION);
			if(ImGui::MenuItem(TI(ICON_FA_EYE, "Show All"), ShortcutLabel(SC_SHOW_ALL), false, CountHiddenInstances() > 0)) uiRunShortcut(SC_SHOW_ALL);
			ImGui::SetItemTooltip(_("Hiding is only for editing: hidden objects stay in the map and are saved as usual."));
   }
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu(LI(ICON_FA_WINDOW_MAXIMIZE, "Windows"))){
			if(ImGui::BeginMenu(L("Editor panels"))){
				if(ImGui::MenuItem(TI(ICON_FA_MAGNIFYING_GLASS, "Object Browser"), ShortcutLabel(SC_WINDOW_BROWSER), showBrowserWindow)) { showBrowserWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_CIRCLE_INFO, "Object Info"), ShortcutLabel(SC_WINDOW_OBJECT_INFO), showInstanceWindow)) { showInstanceWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_LAYER_GROUP, "Map Objects"), ShortcutLabel(SC_WINDOW_MAP_OBJECTS), showMapObjectsWindow)) { showMapObjectsWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_CLOCK_ROTATE_LEFT, "History"), nil, showHistoryWindow)) { showHistoryWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_WRENCH, "Tools"), ShortcutLabel(SC_WINDOW_TOOLS), showToolsWindow)) { showToolsWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_PAINTBRUSH, "Texture Browser"), ShortcutLabel(SC_WINDOW_TEXTURES), showTextureBrowserWindow)) { showTextureBrowserWindow ^= 1; }
				ImGui::EndMenu();
			}
			if(isSA() && ImGui::MenuItem(L("Attachment Workspace"), nil, IsAttachmentWorkspaceActive())){
				if(IsAttachmentWorkspaceActive())
					CloseAttachmentWorkspace();
				else{
					ObjectInst *target = selection.first ? (ObjectInst*)selection.first->item : nil;
					if(target && !target->m_isVehicle && !IsPlayerAttachmentTarget(target))
						target = GetAttachParent(target);
					OpenAttachmentWorkspace(target, false);
				}
			}
			if(ImGui::MenuItem(L("Terminal"), ShortcutLabel(SC_WINDOW_TERMINAL), showTerminalWindow))
				showTerminalWindow ^= 1;
			ImGui::SetItemTooltip(_("Your own shell, inside Haumea: run any AI assistant, a server or anything else."));
			if(isSA() && ImGui::BeginMenu(L("Game tools"))){
				if(isSA() && ImGui::MenuItem(TI(ICON_FA_LOCATION_DOT, "Nodes (open.mp only)"), ShortcutLabel(SC_WINDOW_NODES), showNodesWindow)) { showNodesWindow ^= 1; }
				if(isSA() && ImGui::MenuItem(TI(ICON_FA_GAMEPAD, "Test server"), nil, showTestServerWindow)) { showTestServerWindow ^= 1; }
				if(isSA() && ImGui::MenuItem(TI(ICON_FA_MAP_PIN, "Interiors"), nil, showInteriorsWindow)) { showInteriorsWindow ^= 1; }
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(L("Viewport"))){
				if(ImGui::MenuItem(TI(ICON_FA_CLOUD_SUN, "Time & Weather"), ShortcutLabel(SC_WINDOW_TIME_WEATHER), showTimeWeatherWindow)) { showTimeWeatherWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_EYE, "View"), ShortcutLabel(SC_WINDOW_VIEW), showViewWindow)) { showViewWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_VIDEO, "Camera"), ShortcutLabel(SC_WINDOW_CAMERA), showEditorWindow)) { showEditorWindow ^= 1; }
				if(ImGui::MenuItem(L("Show Pressed Keys"), nil, KeycastEnabled())){
					KeycastSetEnabled(!KeycastEnabled());
					saveSaveSettings();
				}
				ImGui::SetItemTooltip(_("Shows the keys and mouse buttons you press over the 3D view, for tutorials and recordings."));
				if(ImGui::MenuItem(L("Show Radar"), nil, RadarEnabled())){
					RadarSetEnabled(!RadarEnabled());
					saveSaveSettings();
				}
				ImGui::SetItemTooltip(_("The game's radar in the corner of the 3D view, with the map's gang zones.\nClick it for the full map."));
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(L("Review"))){
				if(ImGui::MenuItem(TI(ICON_FA_CODE_COMPARE, "Changes"), ShortcutLabel(SC_WINDOW_CHANGES), showDiffWindow)) { showDiffWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_LIST, "Map Check"), ShortcutLabel(SC_WINDOW_MAP_CHECK), showMapCheckWindow)) { showMapCheckWindow ^= 1; }
				if(ImGui::MenuItem(TI(ICON_FA_LIST, "Log"), ShortcutLabel(SC_WINDOW_LOG), showLogWindow)) { showLogWindow ^= 1; }
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(L("Extensions"))){
				if(ImGui::MenuItem(TI(ICON_FA_BOLT, "Namaka"), nil, showNamakaWindow)) { showNamakaWindow ^= 1; }
				ImGui::SetItemTooltip(_("Namaka lets an AI assistant (Claude, Codex…) work in Haumea: install it and connect one."));
				ImGui::EndMenu();
			}
			if(ImGui::BeginMenu(L("Panel layout"))){
				if(ImGui::MenuItem(TI(ICON_FA_TABLE_COLUMNS, "Reset Panel Layout")))
					uiResetPanelLayout();
				ImGui::SetItemTooltip(_("Restores the panels to one column on the right: the browser above Object Info."));
				if(ImGui::MenuItem(TI(ICON_FA_MAGNET, "Snap Panels"), nil, gPanelSnap)){
					gPanelSnap = !gPanelSnap;
					saveSaveSettings();
				}
				ImGui::SetItemTooltip(_("Dragged panels stick to the screen edges and to each other.\nHold Alt while dragging to place a panel freely."));
#if defined(_WIN32) && defined(RW_D3D9)
				if(ImGui::MenuItem(_("Bring Panels Back to Main Window")))
					BringEditorPanelsHome();
				ImGui::SetItemTooltip(_("Drag a panel by its title bar onto another screen.\nUse this command to recover panels after disconnecting a monitor."));
#endif
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_GEAR, "Preferences..."), ShortcutLabel(SC_WINDOW_PREFERENCES), showPreferencesWindow)) { showPreferencesWindow ^= 1; }
			ImGui::EndMenu();
		}
		if(ImGui::BeginMenu(LI(ICON_FA_CIRCLE_QUESTION, "Help"))){
			if(ImGui::MenuItem(_("Getting started"))){
				gShowStartGuide = true;
				saveSaveSettings();
			}
			ImGui::SetItemTooltip(_("Shows the first-steps card while the map is empty."));
			if(ImGui::MenuItem(TI(ICON_FA_LIST, "Keyboard Shortcuts"), ShortcutLabel(SC_WINDOW_SHORTCUTS), showShortcutsWindow)) { showShortcutsWindow ^= 1; }
			if(ImGui::MenuItem(TI(ICON_FA_CIRCLE_QUESTION, "Help"), nil, showHelpWindow)) { showHelpWindow ^= 1; }
			ImGui::Separator();
			if(ImGui::MenuItem(TI(ICON_FA_BOLT, "Namaka (AI assistant)..."))) showNamakaWindow = true;
			ImGui::SetItemTooltip(_("Install Namaka and connect an AI assistant (Claude, Codex…), like Windows > Namaka."));
			ImGui::Separator();
			UpdaterHelpMenuItems();
			ImGui::EndMenu();
		}

		uiMenuBarRight();
		ImGui::EndMainMenuBar();
	}
	uiExportPrefabPopup();
	uiImportPrefabPopup();
	uiSavePawnAsPopup();
	// Object Info's "Browse textures", whichever panels are open.
	uiMaterialTexturePopup();
	uiViewportObjectMenu();
	uiExportPawnPopup();
	uiImportPawnPopup();
	uiOpenReportWindow();
	uiCustomImportPopup();
	uiReloadGamePopup();
}

// The options bar under the menus: the settings of the tool in use (map)
// or the TextDraw tools. The Code editor has its own.
static void
uiCommandBar(void)
{
	if(InPawnIdeMode())
		return;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	bool mapBar = !InTextDrawMode();
	ImGui::BeginViewportSideBar("##EditorCommands", ImGui::GetMainViewport(), ImGuiDir_Up,
		mapBar ? uiMainToolbarHeight() : ImGui::GetFrameHeight() + 10.0f, ImGuiWindowFlags_NoDecoration |
		(mapBar ? 0 : ImGuiWindowFlags_MenuBar) | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	if(mapBar){
		// The big commands; the tool's settings float over the 3D view (uiViewportOverlay).
		ImGui::SetCursorPos(ImVec2(ImGui::GetFontSize()*0.5f, ImGui::GetFontSize()*0.25f));
		uiMainToolbar();
	}else if(ImGui::BeginMenuBar()){
		DrawTextDrawsToolbar();
		ImGui::EndMenuBar();
	}
	// A dark line under the bar, where the work area starts.
	ImVec2 lo = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, lo.y + sz.y - 1.0f), ImVec2(lo.x + sz.x, lo.y + sz.y - 1.0f),
		ImGui::GetColorU32(ImGuiCol_Separator));
	ImGui::End();
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(4);
}

// Save selection as prefab: a name, where (Mine or the workspace, and a
// subfolder), tags, and the point it is held by when placed.
static void
uiExportPrefabPopup(void)
{
	static char prefabName[128] = "";
	static char folder[128] = "";
	static char tags[160] = "";
	static int origin = PREFAB_ORIGIN_LOWEST;
	static int where = 0;	// 0 Mine, 1 the workspace

	if(gOpenExportPrefab){
		gShowExportPrefab = true;
		gOpenExportPrefab = false;
		prefabName[0] = '\0';
		ImGui::SetNextWindowFocus();
	}
	if(!BeginEditorDialog(L("Save Prefab"), &gShowExportPrefab))
		return;
	int selected = uiSelectionCount();
	ImGui::Text(_("Save %d selected as a prefab"), selected);
	if(ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint(_("Name"), _("e.g. bus stop"), prefabName, sizeof(prefabName));
	bool workspace = GetWorkspaceDir()[0] != '\0';
	if(!workspace)
		where = 0;
	ImGui::BeginDisabled(!workspace);
	const char *places[] = { _("Mine (Haumea's config folder)"), _("The workspace's prefabs folder") };
	ImGui::Combo(_("Save in"), &where, places, IM_ARRAYSIZE(places));
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(workspace ? _("The workspace's prefabs are shared with everyone who maps for that server.") :
		_("Set a workspace (File > Workspace) to save prefabs with the server."));
	// The subfolders already there, or a new one.
	const char *root = where == 1 ? "Workspace" : "Mine";
	ImGui::InputTextWithHint("##folder", _("no folder"), folder, sizeof(folder));
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::BeginCombo("##folders", "", ImGuiComboFlags_NoPreview)){
		if(ImGui::Selectable(_("(no folder)"), folder[0] == '\0'))
			folder[0] = '\0';
		std::set<std::string> folders;
		for(const PrefabListing &prefab : gBrowserPrefabs)
			if(prefab.folder.compare(0, strlen(root) + 1, std::string(root) + "/") == 0)
				folders.insert(prefab.folder.substr(strlen(root) + 1));
		for(const std::string &f : folders)
			if(ImGui::Selectable(f.c_str(), f == folder))
				snprintf(folder, sizeof(folder), "%s", f.c_str());
		ImGui::EndCombo();
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextUnformatted(_("Folder"));
	ImGui::InputTextWithHint(_("Tags"), _("e.g. street, LS"), tags, sizeof(tags));
	ImGui::SetItemTooltip(_("Comma-separated words the prefab search finds it by."));
	ImGui::TextUnformatted(_("Held by"));
	ImGui::SameLine();
	ImGui::RadioButton(_("Lowest point"), &origin, PREFAB_ORIGIN_LOWEST);
	ImGui::SetItemTooltip(_("The middle, at the bottom: it stands on the ground where you click."));
	ImGui::SameLine();
	ImGui::RadioButton(_("Centre"), &origin, PREFAB_ORIGIN_CENTRE);
	ImGui::SameLine();
	ImGui::RadioButton(_("First object"), &origin, PREFAB_ORIGIN_FIRST);
	ImGui::SetItemTooltip(_("The first selected object goes where you click."));
	if(selected > 256)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Only the first 256 are saved."));

	bool validName = prefabName[0] != '\0' && strpbrk(prefabName, "/\\:*?\"<>|") == nil &&
		strstr(folder, "..") == nil && selected > 0;
	ImGui::BeginDisabled(!validName);
	bool save = ImGui::Button(_("Save"), ImVec2(120, 0));
	ImGui::EndDisabled();
	if(save){
		std::string dir = PrefabSaveDir(where == 1, folder);
		std::string path = dir + "/" + prefabName + ".haumea";
		int exported = dir.empty() ? 0 : ExportPrefabEx(path.c_str(), origin, tags);
		if(exported > 0){
			Toast(TOAST_SAVE, _("Saved %d instance(s) as %s"), exported, prefabName);
			refreshBrowserPrefabList();
		}else
			Toast(TOAST_SAVE, _("Couldn't save the prefab"));
		gShowExportPrefab = false;
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
		gShowExportPrefab = false;
	EndEditorDialog();
}

static void
scanPrefabDir(const char *dir, char files[][256], char fullPaths[][512], int *numFiles, int maxFiles)
{
#ifdef _WIN32
	{
		char pattern[512];
		snprintf(pattern, sizeof(pattern), "%s\\*.haumea", dir);
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(pattern, &fd);
		if(h == INVALID_HANDLE_VALUE)
			return;
		do {
			if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && *numFiles < maxFiles){
				bool duplicate = false;
				for(int i = 0; i < *numFiles; i++)
					if(strcmp(files[i], fd.cFileName) == 0){
						duplicate = true;
						break;
					}
				if(duplicate)
					continue;
				strncpy(files[*numFiles], fd.cFileName, 255);
				files[*numFiles][255] = '\0';
				snprintf(fullPaths[*numFiles], 512, "%s/%s", dir, fd.cFileName);
				fullPaths[*numFiles][511] = '\0';
				(*numFiles)++;
			}
		} while(FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR *d = opendir(dir);
	if(!d) return;
	struct dirent *ent;
	while((ent = readdir(d)) != nil){
		if(ent->d_name[0] == '.') continue;
		if(pathHasExtensionCi(ent->d_name, ".haumea")){
			if(*numFiles < maxFiles){
				bool duplicate = false;
				for(int i = 0; i < *numFiles; i++)
					if(strcmp(files[i], ent->d_name) == 0){
						duplicate = true;
						break;
					}
				if(duplicate)
					continue;
				strncpy(files[*numFiles], ent->d_name, 255);
				files[*numFiles][255] = '\0';
				snprintf(fullPaths[*numFiles], 512, "%s/%s", dir, ent->d_name);
				fullPaths[*numFiles][511] = '\0';
				(*numFiles)++;
			}
		}
	}
	closedir(d);
#endif
}

static void
uiImportPrefabPopup(void)
{
	static char prefabFiles[128][256];
	static char prefabPaths[128][512];
	static int numPrefabFiles = 0;
	static char manualPath[512] = "";
	static int selectedFile = -1;

	if(gOpenImportPrefab){
		char prefabDir[512];
		gShowImportPrefab = true;
		gOpenImportPrefab = false;
		manualPath[0] = '\0';
		selectedFile = -1;
		numPrefabFiles = 0;
		if(GetHaumeaDataPath(prefabDir, sizeof(prefabDir), "prefabs"))
			scanPrefabDir(prefabDir, prefabFiles, prefabPaths, &numPrefabFiles, 128);
		scanPrefabDir(GamePath("prefabs").c_str(), prefabFiles, prefabPaths, &numPrefabFiles, 128);
		ImGui::SetNextWindowFocus();
	}

	if(BeginEditorDialog(L("Import Prefab"), &gShowImportPrefab)){
		ImGui::Text(_("Load prefab in front of camera"));

		if(numPrefabFiles > 0){
			ImGui::Text(_("Available prefabs:"));
			ImGui::BeginChild("PrefabList", ImVec2(350, 150), true);
			for(int i = 0; i < numPrefabFiles; i++)
				if(ImGui::Selectable(prefabFiles[i], selectedFile == i))
					selectedFile = i;
			ImGui::EndChild();
		}else
		ImGui::TextDisabled(_("No .haumea files found in the config folder's prefabs/"));

		ImGui::Separator();
		ImGui::InputText(_("Or path"), manualPath, sizeof(manualPath));

		bool canImport = selectedFile >= 0 || manualPath[0] != '\0';
		if(ImGui::Button(_("Import"), ImVec2(120, 0)) && canImport){
			char path[512];
			if(manualPath[0] != '\0')
				strncpy(path, manualPath, sizeof(path));
			else
				strncpy(path, prefabPaths[selectedFile], sizeof(path));
			path[sizeof(path)-1] = '\0';

			int imported = ImportPrefab(path);
			if(imported > 0)
				Toast(TOAST_SPAWN, _("Imported %d instance(s) from prefab"), imported);
			else
				Toast(TOAST_SPAWN, _("Failed to import prefab"));
			gShowImportPrefab = false;
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
			gShowImportPrefab = false;

		EndEditorDialog();
	}
}

static bool
pawnExportNameValid(const char *name)
{
	if(name == nil || name[0] == '\0')
		return false;
	for(const char *p = name; *p; p++)
		if(*p == '/' || *p == '\\')
			return false;
	return true;
}

static bool
pawnPathWithExtension(const char *path, char *out, size_t size)
{
	if(path == nil || path[0] == '\0' || out == nil || size == 0)
		return false;
	if(snprintf(out, size, "%s", path) >= (int)size)
		return false;
	if(pathHasExtensionCi(out, ".pwn") || pathHasExtensionCi(out, ".inc"))
		return true;
	size_t len = strlen(out);
	if(len + 5 >= size)
		return false;
	strcat(out, ".pwn");
	return true;
}

/*
 * Map document: the Pawn file is the map.
 */

static int gPendingMapAction = MAP_ACTION_NONE;
static char gPendingMapPath[1024];
static bool gOpenUnsavedMapPrompt;
static bool gTextDrawsAskedBeforeQuit;
static bool gContinueMapActionAfterSave;
static bool gWindowCloseRequested;
static bool gShowOpenReport;
static char gOpenReportName[256];

static bool
mapHasUnsavedChanges(void)
{
	return GetLatestChangeSeq() != gPawnLastSavedChangeSeq;
}

static const char*
currentMapName(void)
{
	return gPawnSavePath[0] ? pathFilename(gPawnSavePath) : _("Untitled map");
}

static void
addRecentMap(const char *path)
{
	if(path == nil || path[0] == '\0')
		return;
	int existing = -1;
	for(int i = 0; i < gNumRecentMaps; i++)
		if(strcmp(gRecentMaps[i], path) == 0)
			existing = i;
	if(existing < 0)
		existing = gNumRecentMaps < MAX_RECENT_MAPS ? gNumRecentMaps++ : MAX_RECENT_MAPS - 1;
	for(int i = existing; i > 0; i--)
		strcpy(gRecentMaps[i], gRecentMaps[i-1]);
	strncpy(gRecentMaps[0], path, sizeof(gRecentMaps[0])-1);
	gRecentMaps[0][sizeof(gRecentMaps[0])-1] = '\0';
}

static void
removeRecentMap(const char *path)
{
	for(int i = 0; i < gNumRecentMaps; i++)
		if(strcmp(gRecentMaps[i], path) == 0){
			for(int j = i; j + 1 < gNumRecentMaps; j++)
				strcpy(gRecentMaps[j], gRecentMaps[j+1]);
			gNumRecentMaps--;
			return;
		}
}

#ifndef _WIN32
// Single-quoted for sh: ' becomes '\''.
static std::string
shellQuote(const char *text)
{
	std::string out = "'";
	for(const char *p = text ? text : ""; *p; p++){
		if(*p == '\'')
			out += "'\\''";
		else
			out += *p;
	}
	return out + "'";
}
#endif

// Where the workspace keeps maps: scriptfiles/maps, else scriptfiles/, else
// gamemodes/ or filterscripts/ (the workspace itself when it has none).
static std::string
workspaceMapsDir(void)
{
	static const char *folders[] = { "scriptfiles/maps", "scriptfiles", "gamemodes", "filterscripts" };
	for(const char *folder : folders){
		std::string dir = WorkspaceDefaultDir(folder);
		if(dir != GetWorkspaceDir())
			return dir;
	}
	return WorkspaceDefaultDir("");
}

// Native open/save dialog for Pawn files.
// Where map dialogs start: the suggested file's folder, else the workspace's maps.
static std::string
mapDialogStartDir(const char *suggested)
{
	if(suggested && (strchr(suggested, '/') || strchr(suggested, '\\'))){
		std::string dir = suggested;
		dir.resize(dir.find_last_of("/\\"));
		if(!dir.empty())
			return dir;
	}
	return workspaceMapsDir();
}

static bool
pickPawnFileDialog(char *dst, size_t size, bool save, const char *suggested)
{
	if(dst == nil || size == 0)
		return false;
	std::string startDir = mapDialogStartDir(suggested);
	std::string suggestedPath = suggested && suggested[0] ? suggested : "";
	if(!suggestedPath.empty() && suggestedPath.find_first_of("/\\") == std::string::npos && !startDir.empty())
		suggestedPath = startDir + "/" + suggestedPath;
#ifdef _WIN32
	char filename[MAX_PATH] = "";
	if(!suggestedPath.empty())
		strncpy(filename, suggestedPath.c_str(), sizeof(filename)-1);
	char previousDirectory[2048];
	DWORD previousDirectoryLen = GetCurrentDirectoryA(
		(DWORD)sizeof(previousDirectory), previousDirectory);
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = save ? "Pawn source (*.pwn, *.inc)\0*.pwn;*.inc\0All files\0*.*\0"
		: "Maps (*.pwn, *.inc, Texture Studio *.db, MTA *.map, *.ipl)\0*.pwn;*.inc;*.db;*.map;*.ipl\0All files\0*.*\0";
	ofn.lpstrFile = filename;
	ofn.nMaxFile = sizeof(filename);
	ofn.lpstrDefExt = "pwn";
	ofn.lpstrInitialDir = startDir.empty() ? nil : startDir.c_str();
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
		(save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	BOOL picked = save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
	if(previousDirectoryLen > 0 && previousDirectoryLen < sizeof(previousDirectory))
		SetCurrentDirectoryA(previousDirectory);
	if(!picked)
		return false;
	strncpy(dst, filename, size-1);
	dst[size-1] = '\0';
	return true;
#else
	if(!FileDialogAvailable())
		return false;
	std::string command;
#ifdef __APPLE__
	if(save)
		command = std::string("osascript -e 'POSIX path of (choose file name default name \"") +
			(suggested ? pathFilename(suggested) : "map.pwn") + "\")'";
	else
		command = "osascript -e 'POSIX path of (choose file of type {\"pwn\", \"inc\", \"db\"})'";
#else
	std::string start = shellQuote(!suggestedPath.empty() ? suggestedPath.c_str() :
		(startDir.empty() ? std::string("map.pwn") : startDir + "/map.pwn").c_str());
	std::string openStart = shellQuote((startDir.empty() ? std::string(".") : startDir).c_str());
	if(save)
		command = "sh -lc " + shellQuote(("if command -v zenity >/dev/null 2>&1; then "
			"zenity --file-selection --save --confirm-overwrite --filename=" + start +
			" --file-filter='Pawn source | *.pwn *.inc'; "
			"elif command -v kdialog >/dev/null 2>&1; then kdialog --getsavefilename " + start +
			" '*.pwn *.inc'; fi").c_str());
	else
		command = "sh -lc " + shellQuote((std::string("if command -v zenity >/dev/null 2>&1; then "
			"zenity --file-selection --filename=") + openStart + "/ --file-filter='Maps | *.pwn *.inc *.db *.map *.ipl' "
			"--file-filter='All files | *'; "
			"elif command -v kdialog >/dev/null 2>&1; then kdialog --getopenfilename " + openStart +
			" '*.pwn *.inc *.db *.map *.ipl'; fi").c_str());
#endif
	FILE *pipe = popen(command.c_str(), "r");
	if(pipe == nil)
		return false;
	bool ok = fgets(dst, (int)size, pipe) != nil;
	pclose(pipe);
	if(!ok)
		return false;
	trimLineEnding(dst);
	return dst[0] != '\0';
#endif
}

static bool
isTextureStudioMap(const char *path)
{
	return path && pathHasExtensionCi(path, ".db");
}

// A Pawn map, or a Texture Studio (.db), MTA:SA (.map) or IPL map
// converted to Pawn calls.
static int
importMapFile(const char *path, int *skipped)
{
	if(isTextureStudioMap(path))
		return ImportTextureStudioMap(path, skipped);
	if(IsForeignMapFile(path))
		return ImportForeignMap(path, skipped);
	return ImportPawnCode(path, skipped);
}

static bool
openPawnMap(const char *path)
{
	FILE *probe = fopen(path, "rb");
	if(probe == nil){
		ToastFor(TOAST_SAVE, 5.0f, _("Could not open %s"), path);
		removeRecentMap(path);
		saveSaveSettings();
		return false;
	}
	// A streamer map keeps saving as streamer code.
	std::string head;
	char buffer[4096];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), probe)) > 0)
		head.append(buffer, n);
	fclose(probe);
	// Other tools' maps are never written to; Save writes a .pwn next to them.
	bool foreignMap = IsForeignMapFile(path);
	bool textureStudio = isTextureStudioMap(path) || foreignMap;
	// Texture Studio maps use the streamer, as does any CreateDynamic* call
	// (objects, actors, pickups, labels, map icons).
	bool streamer = isTextureStudioMap(path) ||
		(!foreignMap && head.find("CreateDynamic") != std::string::npos);

	finishTransformPanelEdit();
	ResetPawnMap();
	// The server's 0.3.DL / open.mp custom models, before their IDs are read.
	LoadCustomModelsForMap(path, head);
	int skipped = 0;
	int imported = importMapFile(path, &skipped);
	if(foreignMap)
		streamer = LastForeignMapUsesStreamer();
	ClearSelection();
	ResetUndoHistory();
	if(imported < 0){
		ToastFor(TOAST_SAVE, 5.0f, _("Could not read %s"), path);
		return false;
	}
	gPawnSaveFormat = streamer ? PAWN_CODE_STREAMER : PAWN_CODE_NATIVE;
	if(!foreignMap){
		int xmlFormat = RestoreMapXMLWorkspace(path);
		if(xmlFormat == PAWN_CODE_NATIVE || xmlFormat == PAWN_CODE_STREAMER)
			gPawnSaveFormat = xmlFormat;
	}
	// Its text keeps the encoding it came in; other tools' maps get the
	// client's Windows-1252.
	if(!foreignMap)
		SetPawnTextEncoding(GetLastImportTextEncoding());
	char autosaveDir[1024];
	bool fromAutosave = getAutosaveDirectory(autosaveDir, sizeof(autosaveDir)) &&
		strncmp(path, autosaveDir, strlen(autosaveDir)) == 0;
	if(fromAutosave){
		// A recovered copy is unsaved work; Save asks where to keep it.
		gPawnSavePath[0] = '\0';
		gPawnLastSavedChangeSeq = GetLatestChangeSeq() - 1;
	}else if(textureStudio){
		// Never write into the .db: Save writes a .pwn next to it, or asks
		// where when one already exists there.
		std::string pwn = std::string(path, strrchr(path, '.') - path) + ".pwn";
		FILE *existing = fopen(pwn.c_str(), "rb");
		if(existing)
			fclose(existing);
		strncpy(gPawnSavePath, existing ? "" : pwn.c_str(), sizeof(gPawnSavePath)-1);
		gPawnSavePath[sizeof(gPawnSavePath)-1] = '\0';
		gPawnLastSavedChangeSeq = GetLatestChangeSeq() - 1;
		addRecentMap(path);
	}else{
		strncpy(gPawnSavePath, path, sizeof(gPawnSavePath)-1);
		gPawnSavePath[sizeof(gPawnSavePath)-1] = '\0';
		gPawnLastSavedChangeSeq = GetLatestChangeSeq();
		addRecentMap(gPawnSavePath);
	}
	saveSaveSettings();
	// Recordings are looked for in this server's npcmodes/recordings.
	RecSetServerHint(path);
	ToastFor(TOAST_SAVE, 4.0f, _("Opened %s"), pathFilename(path));
	if(skipped > 0 || !GetLastPawnImportReport().notes.empty()){
		// Say what was left out instead of failing silently.
		strncpy(gOpenReportName, pathFilename(path), sizeof(gOpenReportName)-1);
		gOpenReportName[sizeof(gOpenReportName)-1] = '\0';
		gShowOpenReport = true;
	}
	return true;
}

static void
newPawnMap(void)
{
	finishTransformPanelEdit();
	ResetPawnMap();
	gPawnSavePath[0] = '\0';
	gPawnLastSavedChangeSeq = GetLatestChangeSeq();
	saveSaveSettings();
	Toast(TOAST_SAVE, _("New map"));
}

// Someone else's multiplayer session: opening or starting a map leaves it,
// rather than replacing everyone's map with this one. Joining starts a new
// map itself while connecting.
static void
leaveJoinedSession(bool connecting)
{
	int session = GetMultiplayerState();
	if(MultiplayerReplacingMap())
		return;	// this map becomes the session's
	if(session == MP_JOINED || (connecting && session == MP_CONNECTING)){
		MultiplayerLeave();
		Toast(TOAST_SAVE, _("Left the multiplayer session"));
	}
}

static void
performMapAction(int action, const char *path)
{
	switch(action){
	case MAP_ACTION_NEW:
		leaveJoinedSession(true);
		newPawnMap();
		break;
	case MAP_ACTION_OPEN: {
		char picked[1024];
		if(path == nil || path[0] == '\0'){
			if(!pickPawnFileDialog(picked, sizeof(picked), false, gPawnSavePath))
				return;
			path = picked;
		}
		leaveJoinedSession(true);
		if(openPawnMap(path))
			OfferMoveMapTextDraws(path);
		break;
	}
	case MAP_ACTION_QUIT:
		saveSaveSettings();	// remembers the window's place and size
		sk::globals.quit = 1;
		break;
	}
}

// Runs the action now, or after asking about unsaved changes.
static void
quitAfterTextDraws(void)
{
	gTextDrawsAskedBeforeQuit = true;
	requestMapAction(MAP_ACTION_QUIT);
}

static void
requestMapAction(int action, const char *path)
{
 if(action == MAP_ACTION_QUIT) PawnEditorFlushDraft();
 else if(InPawnIdeMode() && !path){
  PawnEditorRequest(action == MAP_ACTION_NEW ? PAWN_EDITOR_NEW : PAWN_EDITOR_OPEN);
  return;
 }
	// The textdraw file is asked about first, then the map.
	if(action == MAP_ACTION_QUIT && !gTextDrawsAskedBeforeQuit && TextDrawFileHasUnsavedChanges()){
		RequestTextDrawFileClose(quitAfterTextDraws);
		return;
	}
	gTextDrawsAskedBeforeQuit = false;
	if(!mapHasUnsavedChanges()){
		performMapAction(action, path);
		return;
	}
	gPendingMapAction = action;
	strncpy(gPendingMapPath, path ? path : "", sizeof(gPendingMapPath)-1);
	gPendingMapPath[sizeof(gPendingMapPath)-1] = '\0';
	gOpenUnsavedMapPrompt = true;
}

void
HandleMapFileDrop(const char *path)
{
	if(path && path[0] && isSA())
		requestMapAction(MAP_ACTION_OPEN, path);
}

static void
continuePendingMapAction(void)
{
	int action = gPendingMapAction;
	gPendingMapAction = MAP_ACTION_NONE;
	gContinueMapActionAfterSave = false;
	char path[1024];
	strncpy(path, gPendingMapPath, sizeof(path));
	performMapAction(action, path);
}

// The point in pasted text: "x, y, z", "x y z", with an angle after it, or a
// Pawn call such as SetPlayerPos(playerid, x, y, z). In a call, a leading
// whole number (a model or skin id) is skipped when more numbers follow, as in
// CreateObject(1634, x, y, z, ...). False without three numbers.
static bool
ParseCoordinates(const char *text, rw::V3d *out)
{
	bool call = false;
	if(const char *open = strchr(text, '(')){
		const char *p = text;
		while(*p == ' ' || *p == '\t') p++;
		call = p < open && (isalpha((unsigned char)*p) || *p == '_');
		if(call)
			text = open + 1;
	}
	float values[16];
	bool whole[16];
	int n = 0;
	for(const char *p = text; *p && n < 16; ){
		if(call && *p == ')')
			break;
		// Skip names, digits and all, such as playerid2.
		if(isalpha((unsigned char)*p) || *p == '_'){
			while(isalnum((unsigned char)*p) || *p == '_') p++;
			continue;
		}
		char *end;
		float v = strtof(p, &end);
		if(end == p || !(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')){
			p++;
			continue;
		}
		whole[n] = strcspn(p, ".eE") >= (size_t)(end - p);
		values[n++] = v;
		p = end;
		if(*p == 'f' || *p == 'F') p++;
	}
	int first = call && n >= 4 && whole[0] ? 1 : 0;
	if(n - first < 3)
		return false;
	*out = { values[first], values[first + 1], values[first + 2] };
	return true;
}

static bool gOpenGoToCoordinates;

static void
uiCopyCameraPosition(void)
{
	char text[96];
	snprintf(text, sizeof(text), "%.4f, %.4f, %.4f",
		TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z);
	SetEditorClipboardText(text);
	Toast(TOAST_COPY_PASTE, _("Copied the camera position: %s"), text);
}

// A view change only: nothing goes on the undo stack.
static void
uiGoToCoordinatesPopup(void)
{
	static char text[512];
	static bool bad;
	if(gOpenGoToCoordinates){
		ImGui::OpenPopup(L("Go to Coordinates"));
		gOpenGoToCoordinates = false;
		bad = false;
	}
	if(!ImGui::BeginPopupModal(L("Go to Coordinates"), nil, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::TextUnformatted(_("Paste a position: x, y, z (an angle after it is fine) or a SetPlayerPos line."));
	if(ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(420.0f);
	bool go = ImGui::InputText("##coordinates", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue);
	if(bad)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("That has no x, y and z in it."));
	ImGui::Spacing();
	go |= ImGui::Button(_("Go"), ImVec2(110, 0));
	ImGui::SameLine();
	if(ImGui::Button(_("Cancel"), ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		ImGui::CloseCurrentPopup();
	if(go){
		rw::V3d point;
		if(ParseCoordinates(text, &point)){
			TheCamera.setTarget(point);
			TheCamera.setDistanceFromTarget(25.0f);
			Toast(TOAST_SELECTION, _("Camera at %.2f, %.2f, %.2f"), point.x, point.y, point.z);
			ImGui::CloseCurrentPopup();
		}else
			bad = true;
	}
	ImGui::EndPopup();
}

static void
uiUnsavedMapPopup(void)
{
	if(gOpenUnsavedMapPrompt){
		ImGui::OpenPopup(L("Unsaved changes"));
		gOpenUnsavedMapPrompt = false;
	}
	if(!ImGui::BeginPopupModal(L("Unsaved changes"), nil, ImGuiWindowFlags_AlwaysAutoResize))
		return;
	ImGui::Text(_("Save changes to %s?"), currentMapName());
	ImGui::TextDisabled(_("Your changes are lost if you don't save them."));
	ImGui::Spacing();
	if(ImGui::Button(_("Save"), ImVec2(110, 0))){
		ImGui::CloseCurrentPopup();
		gContinueMapActionAfterSave = true;
		if(gPawnSavePath[0] == '\0')
			gOpenSavePawnAs = true;	// continues once the file is written
		else if(savePawnMap())
			;	// savePawnMap continues the pending action
		else
			gContinueMapActionAfterSave = false;
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Don't Save"), ImVec2(110, 0))){
		ImGui::CloseCurrentPopup();
		continuePendingMapAction();
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Cancel"), ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)){
		gPendingMapAction = MAP_ACTION_NONE;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

#ifdef LIBRW_GLFW
static void
onWindowCloseRequested(GLFWwindow *window)
{
 PawnEditorFlushDraft();
	if(!mapHasUnsavedChanges() && !TextDrawFileHasUnsavedChanges())
		return;
	glfwSetWindowShouldClose(window, GLFW_FALSE);
	gWindowCloseRequested = true;
}
#endif

static void
updateMapWindowTitle(void)
{
#ifdef LIBRW_GLFW
	static std::string current;
	char title[1200];
	const char *workspace = GetWorkspaceDir();
	if(workspace[0])
		snprintf(title, sizeof(title), "%s%s - %s - Haumea Studio", mapHasUnsavedChanges() ? "* " : "",
			currentMapName(), pathFilename(workspace));
	else
		snprintf(title, sizeof(title), "%s%s - Haumea Studio", mapHasUnsavedChanges() ? "* " : "",
			currentMapName());
	if(current == title)
		return;
	current = title;
	if(GLFWwindow *window = glfwGetCurrentContext()){
		static bool closeHookInstalled;
		if(!closeHookInstalled){
			glfwSetWindowCloseCallback(window, onWindowCloseRequested);
			closeHookInstalled = true;
		}
		glfwSetWindowTitle(window, title);
	}
#endif
}

static bool
savePawnMap(void)
{
	if(!isSA()){
		Toast(TOAST_SAVE, _("Pawn map saving is only supported for GTA San Andreas"));
		return false;
	}
	if(gPawnSavePath[0] == '\0'){
		// Untitled: ask where to save it first.
		gOpenSavePawnAs = true;
		return false;
	}
	char outputPath[1024];
	if(!pawnPathWithExtension(gPawnSavePath, outputPath, sizeof(outputPath))){
		Toast(TOAST_SAVE, _("Invalid or too long Pawn map path"));
		return false;
	}
	int skipped = 0;
	int saved = ExportPawnCode(outputPath, (PawnCodeFormat)gPawnSaveFormat, false, &skipped);
	if(saved < 0){
		Toast(TOAST_SAVE, _("Failed to save Pawn map"));
		return false;
	}
	strncpy(gPawnSavePath, outputPath, sizeof(gPawnSavePath)-1);
	gPawnSavePath[sizeof(gPawnSavePath)-1] = '\0';
	gPawnLastSavedChangeSeq = GetLatestChangeSeq();
	addRecentMap(gPawnSavePath);
	saveSaveSettings();
	MultiplayerMapSaved(gPawnSavePath);	// a session's server that keeps maps gets it too
	Toast(TOAST_SAVE, skipped > 0 ? _("Saved %s: %d object operation(s) (some unsupported objects skipped)") :
	      _("Saved %s: %d object operation(s)"), pathFilename(gPawnSavePath), saved);
	uiMapCheckBefore("saving");
	if(gContinueMapActionAfterSave)
		continuePendingMapAction();
	return true;
}

static void
uiSavePawnAsPopup(void)
{
	static char path[1024];
	static int format = PAWN_CODE_NATIVE;
	static int encoding = PAWN_TEXT_CP1252;

	if(gOpenSavePawnAs){
		std::string maps = workspaceMapsDir();
		if(gPawnSavePath[0] != '\0')
			strncpy(path, gPawnSavePath, sizeof(path)-1);
		else if(!maps.empty())
			snprintf(path, sizeof(path), "%s/map.pwn", maps.c_str());
		else if(!GetHaumeaDataPath(path, sizeof(path), "pawn-exports/map.pwn"))
			path[0] = '\0';
		path[sizeof(path)-1] = '\0';
		format = gPawnSaveFormat;
		encoding = GetPawnTextEncoding();
		gShowSavePawnAs = true;
		gOpenSavePawnAs = false;
		ImGui::SetNextWindowFocus();
	}

	if(BeginEditorDialog(L("Save Pawn Map As"), &gShowSavePawnAs)){
		ImGui::TextWrapped(_("Save the map as Pawn source. It holds every placed object and only the changed or removed GTA world objects, and Haumea opens it again as the map."));
		ImGui::InputText(_("Path"), path, sizeof(path));
		ImGui::SameLine();
		if(ImGui::Button(_("Browse..."))){
			char picked[1024];
			if(pickPawnFileDialog(picked, sizeof(picked), true, path)){
				strncpy(path, picked, sizeof(path)-1);
				path[sizeof(path)-1] = '\0';
			}
		}
		ImGui::RadioButton(_("Native CreateObject"), &format, PAWN_CODE_NATIVE);
		ImGui::SameLine();
		ImGui::RadioButton(_("Streamer CreateDynamicObject"), &format, PAWN_CODE_STREAMER);
		ImGui::RadioButton("Windows-1252##textenc", &encoding, PAWN_TEXT_CP1252);
		ImGui::SetItemTooltip(_("Text encoding of labels, material text, textdraws and notes.\n"
			"SA-MP and open.mp clients draw text as Windows-1252, as pawno writes it."));
		ImGui::SameLine();
		ImGui::RadioButton("UTF-8##textenc", &encoding, PAWN_TEXT_UTF8);
		ImGui::SetItemTooltip(_("Only for a server whose players' clients draw UTF-8 text:\n"
			"the standard SA-MP and open.mp clients show accents garbled."));
		ImGui::TextDisabled(_("Use an absolute path, or a path relative to the editor's working directory."));

		if(ImGui::Button(_("Save"), ImVec2(120, 0)) && path[0] != '\0'){
			char oldPath[sizeof(gPawnSavePath)];
			strncpy(oldPath, gPawnSavePath, sizeof(oldPath)-1);
			oldPath[sizeof(oldPath)-1] = '\0';
			int oldFormat = gPawnSaveFormat;
			PawnTextEncoding oldEncoding = GetPawnTextEncoding();
			strncpy(gPawnSavePath, path, sizeof(gPawnSavePath)-1);
			gPawnSavePath[sizeof(gPawnSavePath)-1] = '\0';
			gPawnSaveFormat = format;
			SetPawnTextEncoding((PawnTextEncoding)encoding);
			if(savePawnMap())
				gShowSavePawnAs = false;
			else{
				strncpy(gPawnSavePath, oldPath, sizeof(gPawnSavePath)-1);
				gPawnSavePath[sizeof(gPawnSavePath)-1] = '\0';
				gPawnSaveFormat = oldFormat;
				SetPawnTextEncoding(oldEncoding);
			}
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
			gShowSavePawnAs = false;
		EndEditorDialog();
	}
	// Closing the dialog without saving also cancels a pending New/Open/Quit.
	if(!gShowSavePawnAs && gContinueMapActionAfterSave && mapHasUnsavedChanges()){
		gContinueMapActionAfterSave = false;
		gPendingMapAction = MAP_ACTION_NONE;
	}
}

static void
uiExportPawnPopup(void)
{
	static char pawnName[128] = "map_objects";

	if(gOpenExportPawn){
		gShowExportPawn = true;
		gOpenExportPawn = false;
		pawnName[0] = '\0';
		gPawnExportFormat = PAWN_CODE_NATIVE;
		gPawnExportSelectionOnly = true;
		ImGui::SetNextWindowFocus();
	}

	if(BeginEditorDialog(L("Export Pawn Code"), &gShowExportPawn)){
		ImGui::Text(_("Export open.mp Pawn object code"));
		ImGui::InputText(_("Name"), pawnName, sizeof(pawnName));
		ImGui::RadioButton(_("Native CreateObject"), &gPawnExportFormat, PAWN_CODE_NATIVE);
		ImGui::SameLine();
		ImGui::RadioButton(_("Streamer CreateDynamicObject"), &gPawnExportFormat, PAWN_CODE_STREAMER);
		ImGui::Checkbox(_("Selection only"), &gPawnExportSelectionOnly);
		ImGui::Checkbox(_("Centered"), &gPawnExportCentered);
		ImGui::SetItemTooltip("%s", _("Write coordinates relative to the export's centre (its lowest point for height),\n"
			"so the code can be placed anywhere by adding a position. Removed GTA buildings keep their real coordinates."));

		int worldObjects = 0;
		int selectedObjects = 0;
		int unchangedWorldObjects = 0;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst && !inst->m_isDeleted && PawnObjectNeedsExport(inst)){
				selectedObjects++;
				if(IsPawnWorldObject(inst)) worldObjects++;
			}else if(inst && !inst->m_isDeleted && IsPawnWorldObject(inst)){
				unchangedWorldObjects++;
			}
		}
		int totalObjects = 0;
		int totalWorldObjects = 0;
		int totalUnchangedWorldObjects = 0;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst == nil)
				continue;
			if(PawnObjectNeedsExport(inst)){
				totalObjects++;
				if(IsPawnWorldObject(inst))
					totalWorldObjects++;
			}else if(!inst->m_isDeleted && IsPawnWorldObject(inst)){
				totalUnchangedWorldObjects++;
			}
		}
		int deletedWorldObjects = 0;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst && inst->m_isDeleted && IsPawnWorldObject(inst) && PawnObjectNeedsExport(inst))
				deletedWorldObjects++;
		}
		int exportableSelection = selectedObjects + deletedWorldObjects;
		int exportCount = gPawnExportSelectionOnly ? exportableSelection : totalObjects;
		ImGui::Text(gPawnExportSelectionOnly ? _("Export set: %d object(s) (selection + deleted world objects)") :
			_("Export set: %d object(s) (whole scene)"), exportCount);
		int exportWorldObjects = gPawnExportSelectionOnly ?
			worldObjects + deletedWorldObjects : totalWorldObjects;
		int omittedWorldObjects = gPawnExportSelectionOnly ?
			unchangedWorldObjects : totalUnchangedWorldObjects;
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		if(exportWorldObjects > 0)
			ImGui::TextWrapped(_("%d changed world object(s) will be removed per player with RemoveBuildingForPlayer and recreated by the exported Pawn code when they are not deleted."), exportWorldObjects);
		else if(omittedWorldObjects > 0)
			ImGui::TextWrapped(_("%d world object(s) in this export scope are unchanged and will be omitted. Move, rotate, edit, or delete one if it should become a Pawn change."), omittedWorldObjects);
		else
			ImGui::TextWrapped(_("Unchanged world objects are omitted. Changed or deleted world objects are exported as RemoveBuildingForPlayer plus scripted object calls."));
		ImGui::PopStyleColor();
		if(gPawnExportFormat == PAWN_CODE_NATIVE)
			ImGui::TextDisabled(_("Native CreateObject is global and has no interior parameter."));
		else
			ImGui::TextDisabled(_("Streamer objects use world -1 and preserve positive editor interiors."));

		char exportDir[1024];
		char exportPath[1024];
		char filename[256];
		bool validName = pawnExportNameValid(pawnName);
		bool havePath = validName && GetHaumeaDataPath(exportDir, sizeof(exportDir), "pawn-exports");
		if(validName && havePath){
			const char *suffix = pathHasExtensionCi(pawnName, ".pwn") ? "" : ".pwn";
			if(snprintf(filename, sizeof(filename), "%s%s", pawnName, suffix) >= (int)sizeof(filename) ||
			   !BuildPath(exportPath, sizeof(exportPath), exportDir, filename))
				havePath = false;
		}
		if(havePath)
			ImGui::TextDisabled(_("Output: %s"), exportPath);

		bool canExport = validName && havePath && (!gPawnExportSelectionOnly || exportableSelection > 0);
		if(ImGui::Button(_("Export"), ImVec2(120, 0)) && canExport){
			int skipped = 0;
			// Centered: move what is exported around the origin for the
			// write, then straight back. Nothing is marked changed.
			struct Moved { ObjectInst *inst; rw::V3d pos; bool dirty; };
			std::vector<Moved> moved;
			rw::V3d centre = { 0.0f, 0.0f, 0.0f };
			if(gPawnExportCentered){
				rw::V3d lo = { 1e30f, 1e30f, 1e30f }, hi = { -1e30f, -1e30f, -1e30f };
				for(CPtrNode *p = instances.first; p; p = p->next){
					ObjectInst *inst = (ObjectInst*)p->item;
					if(inst == nil || inst->m_isDeleted || IsPawnWorldObject(inst) || !PawnObjectNeedsExport(inst) ||
					   (gPawnExportSelectionOnly && !inst->m_selected))
						continue;
					moved.push_back({ inst, inst->m_translation, inst->m_isDirty });
					lo.x = min(lo.x, inst->m_translation.x); hi.x = max(hi.x, inst->m_translation.x);
					lo.y = min(lo.y, inst->m_translation.y); hi.y = max(hi.y, inst->m_translation.y);
					lo.z = min(lo.z, inst->m_translation.z);
				}
				if(!moved.empty())
					centre = rw::makeV3d((lo.x + hi.x)*0.5f, (lo.y + hi.y)*0.5f, lo.z);
				for(Moved &m : moved){
					m.inst->m_translation = sub(m.pos, centre);
					m.inst->UpdateMatrix();
				}
			}
			int exported = ExportPawnCode(exportPath,
				(PawnCodeFormat)gPawnExportFormat, gPawnExportSelectionOnly, &skipped);
			for(Moved &m : moved){
				m.inst->m_translation = m.pos;
				m.inst->UpdateMatrix();
				m.inst->m_isDirty = m.dirty;
			}
			if(exported > 0 && !moved.empty())
				Toast(TOAST_SAVE, _("Coordinates are relative to %.2f, %.2f, %.2f"), centre.x, centre.y, centre.z);
			if(exported > 0){
				Toast(TOAST_SAVE, skipped > 0 ? _("Exported %d Pawn object(s) (some skipped) to %s") :
				      _("Exported %d Pawn object(s) to %s"), exported, exportPath);
				gShowExportPawn = false;
			}else if(exported == 0){
				Toast(TOAST_SAVE, _("No exportable objects found"));
			}else
				Toast(TOAST_SAVE, _("Failed to export Pawn code"));
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
			gShowExportPawn = false;

		EndEditorDialog();
	}
}

// Explains an import result in plain words, including why anything was left out.
static void
uiPawnImportReport(const PawnImportReport &report)
{
	if(report.placed > 0 || report.removed > 0)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), _("Added %d object(s) and %d removed building(s)."),
			report.placed, report.removed);
	else
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("Nothing was imported."));
	if(report.unknownModelObjects > 0){
		std::string ids;
		for(size_t i = 0; i < report.unknownModels.size() && i < 12; i++){
			if(i) ids += ", ";
			ids += std::to_string(report.unknownModels[i]);
		}
		if(report.unknownModels.size() > 12)
			ids += ", ...";
		ImGui::TextWrapped(_("%d object(s) use models the editor does not have (%s). Models "
			"from other mods are not available."), report.unknownModelObjects, ids.c_str());
	}
	if(report.unreadableCalls > 0)
		ImGui::TextWrapped(_("%d call(s) were skipped because their position, model or handle is not a "
			"plain value (for example a formula or a variable that holds coordinates)."), report.unreadableCalls);
	if(report.unmatchedRemovals > 0)
		ImGui::TextWrapped(_("%d RemoveBuildingForPlayer line(s) did not match a GTA building at that spot."),
			report.unmatchedRemovals);
	if(report.unresolvedLinks > 0)
		ImGui::TextWrapped(_("%d material, attachment or interior line(s) referred to objects that were "
			"skipped."), report.unresolvedLinks);
	if(!report.notes.empty()){
		ImGui::TextWrapped(_("Left out because SA-MP has no equivalent:"));
		for(size_t i = 0; i < report.notes.size(); i++)
			ImGui::BulletText("%s", report.notes[i].c_str());
	}
}

static void
uiOpenReportWindow(void)
{
	if(!gShowOpenReport)
		return;
	ImGuiIO &io = ImGui::GetIO();
	// A note in the lower corner of the 3D view, not a box in the middle of it:
	// the map is open and usable, this only says what was left out.
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFontSize()),
		ImGuiCond_Appearing, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, 0.0f), ImGuiCond_Appearing);
	(void)io;
	char title[320];
	snprintf(title, sizeof(title), _("Opened %s"), gOpenReportName);
	strncat(title, "###open_report", sizeof(title) - strlen(title) - 1);
	if(ImGui::Begin(title, &gShowOpenReport, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)){
		uiPawnImportReport(GetLastPawnImportReport());
		// Models from the old modloader/Haumea import can become custom models.
		std::vector<int> legacy;
		if(FindLegacyImports(GetLastPawnImportReport().unknownModels, legacy) > 0){
			ImGui::Spacing();
			ImGui::TextWrapped(_("%d of those models were imported into the game's modloader/Haumea folder, "
				"which SA-MP players don't have. They can become custom models in the server's "
				"models folder (SA-MP 0.3.DL / open.mp)."), (int)legacy.size());
			if(ImGui::Button(_("Move them to the server's models folder"))){
				std::string err;
				const char *map = GetCustomModelsMapPath();
				std::string mapPath = map ? map : "";
				if(!MigrateLegacyImports(legacy, err))
					ToastFor(TOAST_SAVE, 6.0f, _("Couldn't move the models: %s"), err.c_str());
				else if(!mapPath.empty()){
					gShowOpenReport = false;
					openPawnMap(mapPath.c_str());	// read again with the new IDs
				}
			}
		}
		ImGui::Spacing();
		if(ImGui::Button(_("OK"), ImVec2(ImGui::GetFontSize() * 6.0f, 0)))
			gShowOpenReport = false;
	}
	ImGui::End();
}

static void
uiImportPawnPopup(void)
{
	static char pawnPath[1024] = "";
	static bool haveReport;

	if(gOpenImportPawn){
		gShowImportPawn = true;
		gOpenImportPawn = false;
		pawnPath[0] = '\0';
		haveReport = false;
		ImGui::SetNextWindowFocus();
	}

	if(BeginEditorDialog(L("Merge Pawn File"), &gShowImportPawn)){
		ImGui::TextWrapped(_("Adds the objects, textures and removed buildings from another .pwn or .inc "
			"file, a Texture Studio map (.db), an MTA:SA map (.map) or an IPL file to this map. Works with Haumea and Texture Studio "
			"exports and most hand-written maps."));
		ImGui::TextDisabled(_("To edit that file itself instead, use File > Open Map."));
		ImGui::Spacing();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
		ImGui::InputTextWithHint("##merge_path", _("Pick a .pwn, .db, .map or .ipl file"), pawnPath, sizeof(pawnPath));
		ImGui::SameLine();
		if(ImGui::Button(_("Browse..."))){
			char picked[1024];
			if(pickPawnFileDialog(picked, sizeof(picked), false, pawnPath)){
				strncpy(pawnPath, picked, sizeof(pawnPath)-1);
				pawnPath[sizeof(pawnPath)-1] = '\0';
				haveReport = false;
			}
		}
		bool canImport = pawnPath[0] != '\0';
		ImGui::BeginDisabled(!canImport);
		if(ImGui::Button(_("Merge into map"), ImVec2(ImGui::GetFontSize() * 9.0f, 0))){
			int skipped = 0;
			int imported = importMapFile(pawnPath, &skipped);
			haveReport = true;
			if(imported < 0)
				Toast(TOAST_SPAWN, _("Could not read %s"), pathFilename(pawnPath));
			else if(imported > 0)
				Toast(TOAST_SPAWN, _("Merged %s"), pathFilename(pawnPath));
			if(imported >= 0)
				OfferMoveMapTextDraws(gPawnSavePath[0] ? gPawnSavePath : pawnPath);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if(ImGui::Button(_("Close"), ImVec2(ImGui::GetFontSize() * 6.0f, 0)))
			gShowImportPawn = false;
		if(haveReport){
			ImGui::Separator();
			uiPawnImportReport(GetLastPawnImportReport());
		}

		EndEditorDialog();
	}
}

static bool
finalizeCustomImport(void)
{
	gCustomImport.error[0] = '\0';
	gCustomImport.warning[0] = '\0';

	if(!isSA()){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Custom import is wired for GTA San Andreas only in this v1."));
		return false;
	}
	if(gCustomImport.objectId < 0){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("No free stock-range ID was found."));
		return false;
	}
	if(GetObjectDef(gCustomImport.objectId)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("ID %d is already in use."), gCustomImport.objectId);
		return false;
	}
	if(hasInvalidModelTokenChars(gCustomImport.modelName) || hasInvalidModelTokenChars(gCustomImport.txdName)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Model/TXD names must use only letters, digits, and underscores."));
		return false;
	}
	if(GetObjectDef(gCustomImport.modelName, nil)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Model name %s already exists. Change the model name first."), gCustomImport.modelName);
		return false;
	}
	if(strlen(gCustomImport.modelName) >= 24){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Model name %s is too long for COL internal name/export (max 23 chars)."), gCustomImport.modelName);
		return false;
	}

	char dffLogical[256], txdLogical[256], colLogical[256];
	char dffTarget[1024], txdTarget[1024], colTarget[1024];
	char manifestPath[1024], iplPath[1024];
	if(!buildCustomImportModelLogicalPath(dffLogical, sizeof(dffLogical), gCustomImport.modelName, "dff") ||
	   !buildCustomImportModelLogicalPath(txdLogical, sizeof(txdLogical), gCustomImport.txdName, "txd") ||
	   !buildCustomImportColLogicalPath(colLogical, sizeof(colLogical), gCustomImport.modelName) ||
	   !BuildModloaderLogicalExportPath(CUSTOM_IMPORT_MANIFEST_LOGICAL_PATH, manifestPath, sizeof(manifestPath)) ||
	   !BuildModloaderLogicalExportPath(CUSTOM_IMPORT_IPL_LOGICAL_PATH, iplPath, sizeof(iplPath)) ||
	   !BuildModloaderLogicalExportPath(dffLogical, dffTarget, sizeof(dffTarget)) ||
	   !BuildModloaderLogicalExportPath(txdLogical, txdTarget, sizeof(txdTarget)) ||
	   !BuildModloaderLogicalExportPath(colLogical, colTarget, sizeof(colTarget))){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't build export paths."));
		return false;
	}

	int existingTxdSlot = FindTxdSlot(gCustomImport.txdName);
	bool reuseTxd = existingTxdSlot >= 0;
	if(reuseTxd){
		// Sharing is safe only when the selected TXD is the one currently
		// winning modloader resolution. Never replace an existing dictionary
		// just because another import uses the same basename.
		const char *activeTxd = ModloaderFindOverride(gCustomImport.txdName, "txd");
		if(activeTxd == nil || !filesHaveSameContents(gCustomImport.txdSource, activeTxd)){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error),
			         _("TXD name %s already exists, but the selected file differs from the active TXD (%s). "
			         "Select the same TXD file or use a different TXD name."),
			         gCustomImport.txdName, activeTxd ? activeTxd : _("not a loose modloader file"));
			return false;
		}
	}

	bool importCol = gCustomImport.hasCol;
	std::vector<FileRollbackEntry> rollbackEntries;
	if(importCol){
		int colEntryCount = 0;
		bool colAllMatch = false;
		bool colNeedsRename = false;
		if(!inspectColFileForImport(gCustomImport.colSource, &colEntryCount, &colAllMatch,
		                            &colNeedsRename, gCustomImport.modelName)){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't parse COL file %s."), gCustomImport.colSource);
			return false;
		}
		if(colEntryCount != 1 && !colAllMatch){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error),
			         _("COL import only supports automatic renaming for single-entry COL files."));
			return false;
		}
	}

	if(!captureRollbackEntry(rollbackEntries, dffTarget) ||
	   (!reuseTxd && !captureRollbackEntry(rollbackEntries, txdTarget)) ||
	   !captureRollbackEntry(rollbackEntries, manifestPath) ||
	   !captureRollbackEntry(rollbackEntries, iplPath) ||
	   !captureRollbackEntry(rollbackEntries, colTarget)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't snapshot files for rollback."));
		return false;
	}

	char idePath[1024];
	if(!BuildModloaderLogicalExportPath(CUSTOM_IMPORT_IDE_LOGICAL_PATH, idePath, sizeof(idePath))){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to resolve custom IDE path."));
		return false;
	}
	if(!captureRollbackEntry(rollbackEntries, idePath)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't snapshot files for rollback."));
		return false;
	}

	if(!copyFileExact(gCustomImport.dffSource, dffTarget) ||
	   (!reuseTxd && !copyFileExact(gCustomImport.txdSource, txdTarget))){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to copy one or more source files."));
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}

	ModloaderInit();
	const char *winningDff = ModloaderFindOverride(gCustomImport.modelName, "dff");
	const char *winningTxd = ModloaderFindOverride(gCustomImport.txdName, "txd");
	bool winningTxdMatches = winningTxd &&
		(reuseTxd ? filesHaveSameContents(gCustomImport.txdSource, winningTxd) :
		            pathsEqualCiNormalized(winningTxd, txdTarget));
	if(winningDff == nil || !pathsEqualCiNormalized(winningDff, dffTarget) ||
	   !winningTxdMatches){
		// ModloaderInit invalidates the returned pointers, so retain the paths
		// before restoring files and rebuilding its index.
		char shownDff[512];
		char shownTxd[512];
		snprintf(shownDff, sizeof(shownDff), "%s", winningDff ? winningDff : _("<none>"));
		snprintf(shownTxd, sizeof(shownTxd), "%s", winningTxd ? winningTxd : _("<none>"));
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		bool sourceInModloader = pathContainsModloaderDir(gCustomImport.dffSource) ||
		                        pathContainsModloaderDir(gCustomImport.txdSource);
		if(sourceInModloader)
			snprintf(gCustomImport.error, sizeof(gCustomImport.error),
					_("Import is shadowed before Haumea wins override resolution. DFF winner: %s | TXD winner: %s. "
			         "Tip: importing files from another modloader folder can cause this."),
			         shownDff, shownTxd);
		else
			snprintf(gCustomImport.error, sizeof(gCustomImport.error),
				         _("Import is shadowed before Haumea wins override resolution. DFF winner: %s | TXD winner: %s."),
			         shownDff, shownTxd);
		return false;
	}

	if(!ensureTextFileExists(iplPath, "inst\nend\n") ||
	   (importCol && !copyColWithInternalRename(gCustomImport.colSource, colTarget, gCustomImport.modelName))){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to prepare custom import files."));
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}

	if(!doesFileExist(idePath)){
		FILE *ide = fopen(idePath, "w");
		if(ide == nil){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't create %s"), idePath);
			rollbackTouchedFiles(rollbackEntries);
			ModloaderInit();
			return false;
		}
		fprintf(ide, "objs\nend\n");
		fclose(ide);
	}

	std::vector<std::string> lines;
	FILE *ide = fopen(idePath, "r");
	if(ide){
		char line[512];
		while(fgets(line, sizeof(line), ide)){
			trimLineEnding(line);
			lines.push_back(line);
		}
		fclose(ide);
	}
	bool inserted = false;
	char ideEntry[512];
	gCustomImport.previewObj.m_drawDist[0] = gCustomImport.drawDist;
	int ideFlags = computeFlagsFromObjectDef(&gCustomImport.previewObj);
	snprintf(ideEntry, sizeof(ideEntry), "%d, %s, %s, %.1f, %d",
	         gCustomImport.objectId, gCustomImport.modelName, gCustomImport.txdName,
	         gCustomImport.drawDist, ideFlags);
	for(size_t i = 0; i < lines.size(); i++){
		if(strcmp(lines[i].c_str(), ideEntry) == 0){
			inserted = true;
			break;
		}
		if(strcmp(lines[i].c_str(), "end") == 0){
			lines.insert(lines.begin() + (long)i, ideEntry);
			inserted = true;
			break;
		}
	}
	if(!inserted){
		lines.push_back("objs");
		lines.push_back(ideEntry);
		lines.push_back("end");
	}
	ide = fopen(idePath, "w");
	if(ide == nil){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Couldn't rewrite %s"), idePath);
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}
	for(size_t i = 0; i < lines.size(); i++)
		fprintf(ide, "%s\n", lines[i].c_str());
	fclose(ide);

	ObjectDef *obj = AddObjectDef(gCustomImport.objectId);
	if(obj == nil){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to allocate custom object."));
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}
	int createdTxdSlot = -1;
	auto rollbackRegisteredState = [&](){
		RemoveObjectDef(gCustomImport.objectId);
		if(createdTxdSlot >= 0)
			RemoveTxdSlot(createdTxdSlot);
	};
	obj->m_type = ObjectDef::ATOMIC;
	strncpy(obj->m_name, gCustomImport.modelName, MODELNAMELEN);
	obj->m_name[MODELNAMELEN-1] = '\0';
	obj->m_txdSlot = AddTxdSlot(gCustomImport.txdName);
	// A failed import may remove only the slot it created. A reused slot can
	// already belong to any number of loaded object definitions.
	if(existingTxdSlot < 0)
		createdTxdSlot = obj->m_txdSlot;
	obj->m_numAtomics = 1;
	obj->m_drawDist[0] = gCustomImport.drawDist;
	obj->SetFlags(ideFlags);
	obj->m_isTimed = false;
	obj->m_file = getOrCreateCustomImportGameFile(&gCustomImportIdeFile, CUSTOM_IMPORT_IDE_LOGICAL_PATH);
	obj->SetupBigBuilding(gCustomImport.objectId, gCustomImport.objectId + 1);

	RequestObject(gCustomImport.objectId);
	LoadAllRequestedObjects();
	if(obj->m_atomics[0] == nil){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error),
		         _("Failed to load imported DFF/TXD for %s."), gCustomImport.modelName);
		rollbackRegisteredState();
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}

	if(!importCol){
		AutoColStats stats = {};
		std::vector<char> generatedCol;
		char autoColError[256];
		autoColError[0] = '\0';
		if(!GenerateCol3FromAtomic(obj->m_atomics[0], gCustomImport.modelName, generatedCol, &stats,
		                           autoColError, sizeof(autoColError)) ||
		   !writeFileExact(colTarget, generatedCol.data(), generatedCol.size())){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error), "%s",
			         autoColError[0] ? autoColError : _("Failed to auto-generate COL from DFF geometry."));
			rollbackRegisteredState();
			rollbackTouchedFiles(rollbackEntries);
			ModloaderInit();
			return false;
		}
		int removedTriangles = stats.removedDuplicateIndexTriangles +
		                      stats.removedZeroAreaTriangles +
		                      stats.removedCollinearTriangles;
		if(stats.exceededSoftTriangleThreshold || removedTriangles > 0){
			snprintf(gCustomImport.warning, sizeof(gCustomImport.warning),
			         _("Auto-generated COL from DFF geometry (%d -> %d tris, %d -> %d verts)."),
			         stats.originalTriangles, stats.finalTriangles,
			         stats.originalVertices, stats.finalVertices);
		}
	}

	char manifestLine[512];
	snprintf(manifestLine, sizeof(manifestLine), "IDE %s", CUSTOM_IMPORT_IDE_LOGICAL_PATH);
	if(!appendUniqueLine(manifestPath, manifestLine)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to update %s"), manifestPath);
		rollbackRegisteredState();
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}
	snprintf(manifestLine, sizeof(manifestLine), "IPL %s", CUSTOM_IMPORT_IPL_LOGICAL_PATH);
	if(!appendUniqueLine(manifestPath, manifestLine)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to update %s"), manifestPath);
		rollbackRegisteredState();
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}
	snprintf(manifestLine, sizeof(manifestLine), "COLFILE 0 %s", colLogical);
	if(!appendUniqueLine(manifestPath, manifestLine)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("Failed to register COLFILE addition."));
		rollbackRegisteredState();
		rollbackTouchedFiles(rollbackEntries);
		ModloaderInit();
		return false;
	}

	ModloaderInit();

	{
		char mutableColPath[256];
		strncpy(mutableColPath, colLogical, sizeof(mutableColPath)-1);
		mutableColPath[sizeof(mutableColPath)-1] = '\0';
		GameFile *prevFile = FileLoader::currentFile;
		FileLoader::currentFile = NewGameFile(mutableColPath);
		FileLoader::LoadCollisionFile(colLogical);
		FileLoader::currentFile = prevFile;
		if(obj->m_colModel == nil && !importCol){
			snprintf(gCustomImport.error, sizeof(gCustomImport.error),
			         _("Auto-generated COL did not attach to model name %s."), gCustomImport.modelName);
			rollbackRegisteredState();
			rollbackTouchedFiles(rollbackEntries);
			ModloaderInit();
			return false;
		}
		if(obj->m_colModel == nil && gCustomImport.warning[0] == '\0'){
			snprintf(gCustomImport.warning, sizeof(gCustomImport.warning),
			         importCol ?
			         _("COL imported, but no collision matched model name %s.") :
			         _("COL auto-generated, but no collision matched model name %s."),
			         gCustomImport.modelName);
		}
	}

	SetCustomPlacementIpl(CUSTOM_IMPORT_IPL_LOGICAL_PATH, iplPath, false);
	SetSpawnObjectId(gCustomImport.objectId);
	if(!spawnCustomImportedObject(gCustomImport.objectId)){
		snprintf(gCustomImport.error, sizeof(gCustomImport.error), _("The object was imported but couldn't be spawned."));
		return false;
	}

	gBrowserIdeListDirty = true;
	if(gCustomImport.warning[0])
		Toast(TOAST_SPAWN, _("Imported %s (%d) with warning: %s"),
		      gCustomImport.modelName, gCustomImport.objectId, gCustomImport.warning);
	else
		Toast(TOAST_SPAWN, _("Imported %s (%d)"), gCustomImport.modelName, gCustomImport.objectId);
	gCustomImport.active = false;
	return true;
}

static void
uiCustomImportPopup(void)
{
	if(gOpenCustomImport){
		gOpenCustomImport = false;
		ImGui::SetNextWindowFocus();
	}

	if(!BeginEditorDialog(L("Import Custom Object"), &gCustomImport.active))
		return;

	ImGui::Text(_("Import custom object in front of camera"));
	ImGui::TextDisabled(_("Assets are written to modloader/Haumea; original game files stay untouched."));
	ImGui::Separator();
	ImGui::Text(_("Files"));
	if(ImGui::Button(gCustomImport.dffSource[0] ? pathFilename(gCustomImport.dffSource) : _("Choose DFF..."))){
		char picked[1024];
		if(chooseCustomImportFile(".dff", picked, sizeof(picked)))
			setCustomImportDffPath(picked);
	}
	ImGui::SameLine();
	ImGui::TextDisabled(_("Required"));
	if(gCustomImport.dffSource[0]){
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Clear##CustomImportDff"))){
			gCustomImport.dffSource[0] = '\0';
			gCustomImport.txdSource[0] = '\0';
			gCustomImport.txdName[0] = '\0';
			if(!gCustomImport.preferAutoCol){
				gCustomImport.colSource[0] = '\0';
				gCustomImport.hasCol = false;
			}
			clearCustomImportMessages();
		}
	}

	if(ImGui::Button(gCustomImport.txdSource[0] ? pathFilename(gCustomImport.txdSource) : _("Choose TXD..."))){
		char picked[1024];
		if(chooseCustomImportFile(".txd", picked, sizeof(picked)))
			setCustomImportTxdPath(picked);
	}
	ImGui::SameLine();
	ImGui::TextDisabled(_("Required"));
	if(gCustomImport.txdSource[0]){
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Clear##CustomImportTxd"))){
			gCustomImport.txdSource[0] = '\0';
			gCustomImport.txdName[0] = '\0';
			clearCustomImportMessages();
		}
	}

	if(ImGui::Button(gCustomImport.hasCol && gCustomImport.colSource[0] ? pathFilename(gCustomImport.colSource) : _("Choose COL..."))){
		char picked[1024];
		if(chooseCustomImportFile(".col", picked, sizeof(picked)))
			setCustomImportColPath(picked);
	}
	ImGui::SameLine();
	ImGui::TextDisabled(_("Optional"));
	if(gCustomImport.hasCol && gCustomImport.colSource[0]){
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Clear##CustomImportCol")))
			clearCustomImportColSelection();
	}

	if(!gCustomImport.dffSource[0] || !gCustomImport.txdSource[0]){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("DFF and TXD are required."));
	}else if(gCustomImport.hasCol && gCustomImport.colSource[0]){
		ImGui::TextDisabled(_("Using provided COL: %s"), pathFilename(gCustomImport.colSource));
	}else{
		ImGui::TextDisabled(_("No COL selected. Collision will be auto-generated."));
	}
		ImGui::TextDisabled(_("An existing TXD is shared when its contents match; name conflicts are rejected."));
		ImGui::TextDisabled(_("Tip: you can also drag & drop .dff/.txd/.col files anywhere in Haumea."));

	ImGui::Separator();
	ImGui::InputInt(_("Object ID"), &gCustomImport.objectId);
	int previousPreferredStartId = gCustomImportPreferredStartId;
	ImGui::InputInt(_("Preferred ID Start"), &gCustomImportPreferredStartId);
	sanitizeCustomImportSettings();
	if(gCustomImportPreferredStartId != previousPreferredStartId)
		saveSaveSettings();
	ImGui::SameLine();
	if(ImGui::Button(_("Suggest Free ID")))
		gCustomImport.objectId = findSuggestedCustomImportId();
	ImGui::InputText(_("Model"), gCustomImport.modelName, sizeof(gCustomImport.modelName));
	ImGui::InputText("TXD", gCustomImport.txdName, sizeof(gCustomImport.txdName));
	ImGui::DragFloat(_("Draw Distance"), &gCustomImport.drawDist, 1.0f, 1.0f, 10000.0f, "%.1f");
	gCustomImport.previewObj.m_drawDist[0] = gCustomImport.drawDist;

	ImGui::Separator();
	ImGui::Text(_("IDE Flags"));
	uiObjectFlagsEditor(&gCustomImport.previewObj);
	ImGui::TextDisabled(_("Raw flags: 0x%X"), computeFlagsFromObjectDef(&gCustomImport.previewObj));

	if(gCustomImport.hasCol){
		ImGui::Separator();
		ImGui::TextDisabled(_("COL will be exported as %s.col and renamed internally if needed"), gCustomImport.modelName);
	}else{
		ImGui::Separator();
		ImGui::TextDisabled(_("COL will be auto-generated from DFF geometry as %s.col"), gCustomImport.modelName);
	}

	if(gCustomImport.error[0]){
		ImGui::Separator();
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), "%s", gCustomImport.error);
	}
	if(gCustomImport.warning[0]){
		ImGui::Separator();
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", gCustomImport.warning);
	}

	ImGui::Separator();
	bool canImport = gCustomImport.dffSource[0] != '\0' && gCustomImport.txdSource[0] != '\0';
	ImGui::BeginDisabled(!canImport);
	if(ImGui::Button(_("Import"), ImVec2(120, 0))){
		if(finalizeCustomImport())
			gCustomImport.active = false;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if(ImGui::Button(_("Cancel"), ImVec2(120, 0))){
		gCustomImport.active = false;
		gCustomImport.error[0] = '\0';
		gCustomImport.warning[0] = '\0';
	}

	EndEditorDialog();
}

static void
uiKeyboardShortcutsWindow(void)
{
	struct ShortcutEntry {
		const char *section;
		const char *shortcut;
		const char *action;
		const char *context;
	};

	// Mouse and fixed keys; the rebindable ones come from the shortcut table.
	static const ShortcutEntry fixedShortcuts[] = {
		{ N_("File"), N_("Alt+F4"), N_("Exit Haumea Studio"), N_("Editor") },

		{ N_("Camera"), N_("RMB drag"), N_("Look around (the look button: Preferences > Camera)"), N_("3D view") },
		{ N_("Camera"), N_("Ctrl+Alt+RMB drag"), N_("Dolly camera along view direction"), N_("3D view") },
		{ N_("Camera"), N_("MMB drag"), N_("Pan camera"), N_("3D view") },
		{ N_("Camera"), N_("Alt+MMB click"), N_("Reset FOV"), N_("3D view") },
		{ N_("Camera"), N_("Alt+MMB drag"), N_("Arc rotate around camera target"), N_("3D view") },
		{ N_("Camera"), N_("Ctrl+Alt+MMB drag"), N_("Zoom camera toward target"), N_("3D view") },
		{ N_("Camera"), N_("Mouse wheel"), N_("Adjust FOV zoom"), N_("3D view") },
		{ N_("Camera"), N_("Ctrl+wheel"), N_("Interface size"), N_("Editor") },
		{ N_("Camera"), N_("RMB held + W / S"), N_("Fly forward / backward"), N_("3D view") },
		{ N_("Camera"), N_("RMB held + A / D"), N_("Fly left / right"), N_("3D view") },
		{ N_("Camera"), N_("RMB held + Shift+WASD"), N_("Fly fast"), N_("3D view") },
		{ N_("Camera"), N_("RMB held + Alt+WASD"), N_("Fly slowly"), N_("3D view") },

		{ N_("Selection"), N_("Click object"), N_("Select object"), N_("3D view") },
		{ N_("Selection"), N_("Shift+click object"), N_("Add object to selection"), N_("3D view") },
		{ N_("Selection"), N_("Alt+click object"), N_("Remove object from selection"), N_("3D view") },
		{ N_("Selection"), N_("Ctrl+click object"), N_("Toggle object selection"), N_("3D view") },
		{ N_("Selection"), N_("Shift+LMB drag"), N_("Rectangle select"), N_("3D view") },
		{ N_("Selection"), N_("Ctrl+Shift+LMB drag"), N_("Rectangle add to selection"), N_("3D view") },
		{ N_("Selection"), N_("Alt+Shift+LMB drag"), N_("Rectangle remove from selection"), N_("3D view") },
		{ N_("Selection"), N_("RMB on selection"), N_("Deselect selected object"), N_("Editor list") },
		{ N_("Selection"), N_("RMB on deleted object"), N_("Undelete object"), N_("Editor list") },
		{ N_("Selection"), N_("Shift while dragging"), N_("Use selected snap increment"), N_("Gizmo") },

		{ N_("Object Browser"), N_("Ctrl+F"), N_("Search"), N_("Object Browser") },
		{ N_("Object Browser"), N_("Up / Down"), N_("Move selected object through current filtered list"), N_("Object Browser list") },
		{ N_("Object Browser"), N_("Right-click object row"), N_("Add or remove favourite"), N_("Object Browser list") },
		{ N_("Object Browser"), N_("Click in 3D view"), N_("Place selected object"), N_("Place mode") },
		{ N_("Object Browser"), N_("Shift+click in 3D view"), N_("Place object and stay in place mode"), N_("Place mode") },
		{ N_("Object Browser"), N_("RMB"), N_("Exit place mode"), N_("Place mode") },

		{ N_("Brush"), N_("LMB"), N_("Paint selected object on surface"), N_("Brush mode") },
		{ N_("Brush"), N_("LMB drag"), N_("Paint continuously using spacing/delay settings"), N_("Brush mode") },
		{ N_("Brush"), N_("RMB drag"), N_("Look around while brushing"), N_("Brush mode") },
		{ N_("Brush"), N_("MMB drag"), N_("Pan or orbit camera while brushing"), N_("Brush mode") },
		{ N_("Brush"), N_("MMB click"), N_("Exit brush mode"), N_("Brush mode") },
	};
	static const char *contextNames[] = { N_("Editor"), N_("3D view"), N_("Panel under the mouse") };

	ImGui::SetNextWindowSize(ImVec2(780, 620), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(420, 260), ImVec2(10000, 10000));
	ImGui::Begin(LI(ICON_FA_LIST, "Keyboard Shortcuts"), &showShortcutsWindow);

	static ImGuiTextFilter filter;
	filter.Draw(_("Filter"), 280.0f);
	ImGui::SameLine();
	if(ImGui::Button(_("Clear")))
		filter.Clear();
	ImGui::SameLine();
	if(ImGui::Button(TI(ICON_FA_GEAR, "Change shortcuts...")))
		showPreferencesWindow = true;
	ImGui::SetItemTooltip(_("Preferences > Shortcuts"));

	ImGui::Separator();

	int shown = 0;
	const char *lastSection = nil;
	ImVec2 scrollSize = ImGui::GetContentRegionAvail();
	if(scrollSize.y < 140.0f)
		scrollSize.y = 140.0f;
	ImGui::BeginChild("##ShortcutsScroll", scrollSize, false, ImGuiWindowFlags_HorizontalScrollbar);
	ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_Resizable;
	if(ImGui::BeginTable("##Shortcuts", 4, flags)){
		ImGui::TableSetupColumn(_("Section"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn(_("Shortcut"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
		ImGui::TableSetupColumn(_("Action"));
		ImGui::TableSetupColumn(_("Context"), ImGuiTableColumnFlags_WidthFixed, 160.0f);
		ImGui::TableHeadersRow();

		int numFixed = (int)(sizeof(fixedShortcuts) / sizeof(fixedShortcuts[0]));
		for(int i = 0; i < SC_NUM_ACTIONS + numFixed; i++){
			ShortcutEntry s;
			if(i < SC_NUM_ACTIONS){
				const ShortcutInfo *info = GetShortcutInfo(i);
				const char *label = ShortcutLabel(i);
				s = { info->section, label[0] ? label : _("(none)"), info->name, contextNames[info->context] };
			}else{
				s = fixedShortcuts[i - SC_NUM_ACTIONS];
				s.shortcut = _(s.shortcut);
			}
			char searchable[512];
			snprintf(searchable, sizeof(searchable), "%s %s %s %s",
				_(s.section), s.shortcut, _(s.action), _(s.context));
			if(filter.IsActive() && !filter.PassFilter(searchable))
				continue;

			if(lastSection == nil || strcmp(lastSection, s.section) != 0){
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), "%s", _(s.section));
				lastSection = s.section;
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(s.shortcut);
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(_(s.action));
			ImGui::TableSetColumnIndex(3);
			ImGui::TextDisabled("%s", _(s.context));
			shown++;
		}

		ImGui::EndTable();
	}
	if(shown == 0)
		ImGui::TextDisabled(_("No shortcuts match the current filter."));
	ImGui::EndChild();

	ImGui::End();
}

// One "How do I..." answer: a closed section with a few short steps.
static bool
uiHelpTopic(const char *title)
{
	return ImGui::CollapsingHeader(title);
}

static void
uiHelpWindow(void)
{
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin(LI(ICON_FA_CIRCLE_QUESTION, "Help"), &showHelpWindow);
	// About.
	// Production says nothing; a beta release names its channel; a CI build
	// that isn't a release says what it is.
	if(strcmp(HAUMEA_CHANNEL, "beta") == 0)
		ImGui::Text(_("Haumea Studio %s, Beta channel"), HaumeaVersionDisplay());
	else if(strcmp(HAUMEA_CHANNEL, "production") == 0)
		ImGui::Text(_("Haumea Studio %s"), HaumeaVersionDisplay());
	else
		ImGui::Text(_("Haumea Studio %s (%s)"), HaumeaVersionDisplay(), HAUMEA_CHANNEL_DISPLAY);
	// A CI artifact (a main push or a pull request), not a release.
	if(strcmp(HAUMEA_CHANNEL, "dev") == 0 || strcmp(HAUMEA_CHANNEL, "pr") == 0)
		ImGui::TextDisabled(_("An unofficial build: it updates to the next official release."));
	ImGui::TextDisabled(_("The all-in-one editor for open.mp and SA-MP, by itsneufox."));
	if(ImGui::SmallButton(_("Licenses...")))
		showLicensesWindow = true;
	ImGui::SetItemTooltip(_("GNU GPL v2 or later, and the licenses of the components it includes."));

	ImGui::SeparatorText(_("How do I..."));
	if(uiHelpTopic(_("Start a map for my server"))){
		ImGui::BulletText(_("File > Workspace: pick your SA-MP or open.mp server folder."));
		ImGui::BulletText(_("File > New, place things, then %s saves a .pwn you include in a script."),
			uiKey(SC_SAVE_MAP));
		ImGui::BulletText(_("The saved file's first lines say which functions to call."));
	}
	if(uiHelpTopic(_("Place objects, pickups, actors..."))){
		ImGui::BulletText(_("Object Browser (%s): pick an object, Place, then click in the world."),
			uiKey(SC_WINDOW_BROWSER));
		ImGui::BulletText(_("Choose Characters, Markers, Text & audio or Zones in the browser."));
		ImGui::BulletText(_("Click something to edit it in Object Info (%s). %s stops placing."),
			uiKey(SC_WINDOW_OBJECT_INFO), uiKey(SC_CANCEL));
	}
	if(uiHelpTopic(_("Make an NPC walk a route"))){
		UiBadge(_("open.mp only"));
		ImGui::BulletText(_("Choose Paths in the browser, then place the points of a route."));
		ImGui::BulletText(_("Place an NPC, then in Object Info > NPC > Movement pick \"Follow a path\"."));
		ImGui::BulletText(_("Preview movement walks it in the viewport; the map saves NPC_MoveByPath."));
		ImGui::BulletText(_("For the game's roads, pick \"Walk the nodes\" (Windows > Nodes, %s)."),
			uiKey(SC_WINDOW_NODES));
	}
	if(uiHelpTopic(_("Use custom models"))){
		UiBadge("0.3.DL", _("SA-MP 0.3.DL and open.mp clients download custom models."));
		ImGui::BulletText(_("File > Import > Custom Model: pick a DFF and TXD and an ID."));
		ImGui::BulletText(_("It goes into the workspace's models folder and artconfig.txt."));
		ImGui::BulletText(_("Find it in Object Browser > Custom models and place it like any object."));
	}
	if(uiHelpTopic(_("Design a textdraw"))){
		ImGui::BulletText(_("Windows > TextDraws starts TextDraw mode: a canvas instead of the world."));
		ImGui::BulletText(_("Add a text, box, sprite or model preview and drag it on the screen."));
		ImGui::BulletText(_("Textdraws have their own .pwn file: Save in the TextDraws window or %s."), uiKey(SC_SAVE_MAP));
		ImGui::BulletText(_("Your gamemode includes it and calls _CreateTextDraws and _ShowTextDraws(playerid)."));
		ImGui::BulletText(_("Exit in the banner or %s goes back to the map."), uiKey(SC_CANCEL));
	}
	if(uiHelpTopic(_("Test the map in game"))){
		ImGui::BulletText(_("Test in game (%s) runs the map on a private open.mp server."), uiKey(SC_TEST_IN_GAME));
		ImGui::BulletText(_("Launch game joins it. After changes, Reload map in the Test server window."));
		ImGui::BulletText(_("Preferences > Test server picks the server package and port."));
		ImGui::BulletText(_("Map check (%s) lists what will break in game before you ship it."),
			uiKey(SC_WINDOW_MAP_CHECK));
	}

	ImGui::SeparatorText(_("More"));
	ImGui::TextLinkOpenURL(_("User guide"), "https://github.com/itsneufox/Haumea-Studio/blob/main/docs/USER_GUIDE.md");
	ImGui::SetItemTooltip(_("Setup, elements, NPCs, textdraws, testing and saving, with screenshots."));
	ImGui::SameLine();
	if(ImGui::Button(TI(ICON_FA_LIST, "Keyboard shortcuts")))
		showShortcutsWindow = true;
	ImGui::SetItemTooltip(_("Every command and its key (%s). Change them in Preferences > Shortcuts."),
		uiKey(SC_WINDOW_SHORTCUTS));
	ImGui::SameLine();
	ImGui::TextLinkOpenURL(_("Editor Discord"), "https://discord.gg/YSxtANrsB4");
	ImGui::SetItemTooltip(_("Help, bug reports and editor updates."));
	ImGui::SameLine();
	ImGui::TextLinkOpenURL(_("open.mp Discord"), "https://discord.gg/samp");
	ImGui::SetItemTooltip(_("Scripting and server help from the open.mp community."));

	ImGui::End();
}

static void
uiWeatherBox(const char *id, int *weather)
{
	int numWeathers = min(params.numWeathers, SAMP_NUM_WEATHERS);
	char preview[100];
	snprintf(preview, sizeof(preview), "%d - %s", *weather, params.weatherInfo[*weather].name);
	if(ImGui::BeginCombo(id, preview)){
		for(int n = 0; n < numWeathers; n++){
			bool is_selected = n == *weather;
			char str[100];
			snprintf(str, sizeof(str), "%d - %s", n, params.weatherInfo[n].name);
			if(ImGui::Selectable(str, is_selected))
				*weather = n;
			if(is_selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
}

static void
advanceHour(int diff)
{
	currentHour += diff;
	if(currentHour >= 24)
		currentHour = 0;
	else if(currentHour < 0)
		currentHour = 23;
}

static void
advanceMinute(int diff)
{
	currentMinute += diff;
	if(currentMinute >= 60){
		currentMinute = 0;
		advanceHour(1);
	}else if(currentMinute < 0){
		currentMinute = 59;
		advanceHour(-1);
	}
}

static void
uiTimeWeather(void)
{
	float rowWidth = ImGui::GetContentRegionAvail().x;
	float spacing = ImGui::GetStyle().ItemSpacing.x;

	ImGui::SeparatorText(_("Time"));
	ImGui::PushItemWidth((rowWidth - spacing) * 0.5f);
	ImGui::InputInt("##Hour", &currentHour, 1);
	advanceHour(0);
	ImGui::SetItemTooltip(_("Hour"));
	ImGui::SameLine();
	ImGui::InputInt("##Minute", &currentMinute, 1);
	advanceMinute(0);
	ImGui::SetItemTooltip(_("Minute"));
	ImGui::PopItemWidth();

	char clock[16];
	snprintf(clock, sizeof(clock), "%02d:%02d", currentHour, currentMinute);
	int totalMinute = currentHour*60 + currentMinute;
	ImGui::SetNextItemWidth(-FLT_MIN);
	if(ImGui::SliderInt("##TotalMinute", &totalMinute, 0, 24*60-1, clock)){
		currentHour = totalMinute/60;
		currentMinute = totalMinute%60;
	}
	static const struct { const char *name; int hour; } presets[] = {
		{ N_("Dawn"), 6 }, { N_("Noon"), 12 }, { N_("Dusk"), 20 }, { N_("Night"), 0 },
	};
	for(size_t i = 0; i < IM_ARRAYSIZE(presets); i++){
		if(i > 0)
			ImGui::SameLine();
		if(ImGui::Button(_(presets[i].name))){
			currentHour = presets[i].hour;
			currentMinute = 0;
		}
	}

	ImGui::SeparatorText(_("Weather"));
	ImGui::SetNextItemWidth(-FLT_MIN);
	uiWeatherBox("##Weather", &Weather::oldWeather);
	Weather::newWeather = Weather::oldWeather;
	Weather::interpolation = 0.0f;
	extraColours = -1;

	ImGui::Spacing();
	ImGui::TextDisabled(_("In game: SetPlayerTime(playerid, %d, %d); SetPlayerWeather(playerid, %d);"),
		currentHour, currentMinute, Weather::oldWeather);
	ImGui::TextDisabled(_("The editor only previews these; they are not saved in the map."));
}

// What the 3D view shows and how it looks. Everything here only changes the
// editor's preview; none of it is saved in the map.
static void
uiView(void)
{
	ImGui::SeparatorText(_("Show"));
	ImGui::Checkbox(_("Map elements"), &gRenderSampElements);
	ImGui::SetItemTooltip(_("3D text labels, gang zones and tags on pickups, actors,\nplayer classes and map icons."));
	ImGui::Checkbox(_("Collisions"), &gRenderCollision);
	ImGui::SetItemTooltip(_("What players and vehicles actually collide with."));
	ImGui::Checkbox(_("Particle effects"), &ParticlePreview::enabled);
	ImGui::SetItemTooltip(_("Animate fire, smoke, water and other particle objects in the viewport."));
	ImGui::Checkbox(_("Focus my map"), &gFocusMyMap);
	ImGui::SetItemTooltip(_("Dims the game's buildings so your map's objects stand out."));
	gStreamDistance = clamp(gStreamDistance, 50.0f, 5000.0f);
	gFrameLimit = gFrameLimit <= 0 ? 0 : clamp(gFrameLimit, 30, 360);

	if(isSA()){
		ImGui::SeparatorText(_("Player view"));
		char playerViewLabel[64];
		snprintf(playerViewLabel, sizeof(playerViewLabel), _("As a player sees it (%s)##player_view"), uiKey(SC_PLAYER_VIEW));
		ImGui::Checkbox(playerViewLabel, &gPlayerView);
		ImGui::SetItemTooltip(_("Previews virtual worlds, interiors, areas and client object limits.\nAll elements keep the stream distance set in Performance. Nothing is saved."));
		ImGui::BeginDisabled(!gPlayerView);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		ImGui::InputInt(_("Virtual world"), &gPlayerViewWorld);
		if(gPlayerViewWorld < 0)
			gPlayerViewWorld = 0;
		ImGui::EndDisabled();
	}

	ImGui::SeparatorText(_("Selection"));
	ImGui::Checkbox(_("Outline selected objects"), &gOutlineSelection);
	ImGui::SetItemTooltip(_("Draws the edges of selected and hovered objects, so invisible ones\n"
		"can be seen. Off leaves only the tint above."));
	char outlineLabel[64];
	snprintf(outlineLabel, sizeof(outlineLabel), _("Outline map objects (%s)##outline_map"), uiKey(SC_OUTLINE_OBJECTS));
	ImGui::Checkbox(outlineLabel, &gOutlineMapObjects);

	// What most maps never need: debug drawing, the game's own display
	// rules, the path nodes (also in Nodes) and the selection tint.
	ImGui::Spacing();
	if(ImGui::TreeNodeEx(L("Advanced"), ImGuiTreeNodeFlags_SpanAvailWidth)){
		ImGui::Checkbox(L("Object bounds"), &gRenderBounds);
		ImGui::SetItemTooltip(_("Shows collision bounding boxes without the collision mesh. Collisions takes precedence when both are enabled."));
		ImGui::Checkbox(_("Water"), &gRenderWater);
		if(gameversion == GAME_SA)
			ImGui::Checkbox(_("Animated objects"), &gPlayAnimations);
		ImGui::Checkbox(_("Light coronas"), &gRenderLightEffects);
		ImGui::SetItemTooltip(_("Street lamp and building light coronas, as the client draws them."));
		ImGui::Checkbox(_("Timed objects at any hour"), &gNoTimeCull);
		ImGui::SetItemTooltip(_("Objects the game only shows by day or by night are always drawn."));
		if(params.numAreas)
			ImGui::Checkbox(_("Every interior at once"), &gNoAreaCull);
		ImGui::SetItemTooltip(_("Off draws only the interior set on the menu bar, like the client."));

		if(isSA()){
			ImGui::SeparatorText(_("NPC nodes (NODES*.DAT)"));
			ImGui::Checkbox(_("Vehicle nodes"), &gRenderSaCarPaths);
			ImGui::SameLine();
			ImGui::Checkbox(_("Ped nodes"), &gRenderSaPedPaths);
			ImGui::SetItemTooltip(_("The game's path nodes. open.mp NPCs follow them with NPC_PlayNode."));
			ImGui::Checkbox(_("Node area grid"), &gRenderSaAreaGrid);
			ImGui::SetItemTooltip(_("The 8x8 grid of NODES0-63.DAT areas (750 units each).\nNodes cannot be moved across these boundaries."));
		}

		ImGui::SeparatorText(_("Selection"));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		ImGui::SliderInt(_("Highlight"), &gSelectionHighlightOpacity, 0, 100, "%d%%");
		ImGui::SetItemTooltip(_("Opacity of the red overlay drawn on selected objects."));
		ImGui::TreePop();
	}
}

enum TransformPanelEditKind
{
	TRANSFORM_PANEL_NONE,
	TRANSFORM_PANEL_POSITION,
	TRANSFORM_PANEL_ROTATION
};

struct TransformPanelState
{
	ObjectInst *leader;
	bool relative;
	bool invalidWarningShown;
	int editKind;
	float position[3];
	float rotation[3];
	float validPosition[3];
	float validRotation[3];
	rw::V3d startLeaderPos;
	rw::Quat startLeaderRot;
	std::vector<UndoTransform> transforms;
};

struct TransformClipboard
{
	bool valid;
	rw::V3d position;
	rw::V3d rotationDegrees;
};

static TransformPanelState gTransformPanel = {};
static TransformClipboard gTransformClipboard = {};
static bool gTransformPanelDrawnThisFrame;

static void
copyTransformValues(float dst[3], const float src[3])
{
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
}

static bool
finiteTransformValues(const float values[3])
{
	return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
}

static bool
finiteTransformVector(const rw::V3d &value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

static bool
finiteTransformRotation(const rw::Quat &value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) &&
	       std::isfinite(value.z) && std::isfinite(value.w);
}

static void
refreshTransformPanel(ObjectInst *inst)
{
	gTransformPanel.leader = inst;
	if(inst == nil)
		return;
	rw::V3d degrees = GetInstanceRotationDegrees(inst);
	if(gTransformPanel.relative){
		gTransformPanel.position[0] = 0.0f;
		gTransformPanel.position[1] = 0.0f;
		gTransformPanel.position[2] = 0.0f;
		gTransformPanel.rotation[0] = 0.0f;
		gTransformPanel.rotation[1] = 0.0f;
		gTransformPanel.rotation[2] = 0.0f;
	}else{
		// Float noise reads as -0.00; show it as 0.
		auto clean = [](float v){ return fabsf(v) < 0.0005f ? 0.0f : v; };
		gTransformPanel.position[0] = inst->m_translation.x;
		gTransformPanel.position[1] = inst->m_translation.y;
		gTransformPanel.position[2] = inst->m_translation.z;
		gTransformPanel.rotation[0] = clean(degrees.x);
		gTransformPanel.rotation[1] = clean(degrees.y);
		gTransformPanel.rotation[2] = clean(degrees.z);
	}
	copyTransformValues(gTransformPanel.validPosition, gTransformPanel.position);
	copyTransformValues(gTransformPanel.validRotation, gTransformPanel.rotation);
}

static void
finishTransformPanelEdit(void)
{
	if(gTransformPanel.editKind == TRANSFORM_PANEL_NONE)
		return;
	CommitObjectTransformTargets(gTransformPanel.transforms);
	gTransformPanel.editKind = TRANSFORM_PANEL_NONE;
	gTransformPanel.invalidWarningShown = false;
	refreshTransformPanel(gTransformPanel.leader);
}

static bool
beginTransformPanelEdit(ObjectInst *inst, int editKind)
{
	if(gTransformPanel.editKind != TRANSFORM_PANEL_NONE)
		finishTransformPanelEdit();
	bool capped = false;
	if(!CaptureObjectTransformTargets(inst, gBatchEditSelection,
	   gTransformPanel.transforms, &capped))
		return false;
	if(capped)
		Toast(TOAST_SELECTION, _("Transform limited to first %d linked object(s)"), MAX_BATCH_OBJECTS);
	gTransformPanel.leader = inst;
	gTransformPanel.editKind = editKind;
	gTransformPanel.invalidWarningShown = false;
	gTransformPanel.startLeaderPos = inst->m_translation;
	gTransformPanel.startLeaderRot = inst->m_rotation;
	return true;
}

static void
rejectInvalidTransformValues(float values[3], const float validValues[3])
{
	copyTransformValues(values, validValues);
	if(!gTransformPanel.invalidWarningShown){
		Toast(TOAST_SELECTION, _("Transform values must be finite numbers"));
		gTransformPanel.invalidWarningShown = true;
	}
}

static bool
uiTransformVectorField(const char *label, float values[3], float speed,
	const char *format, const char *tooltip, const char *copiedName,
	bool *deactivatedAfterEdit, bool *copyClicked)
{
	ImGui::PushID(copiedName);
	ImGui::TextUnformatted(label);
	bool changed = false;
	*deactivatedAfterEdit = false;
	const char *axes[] = { "X", "Y", "Z" };
	for(int axis = 0; axis < 3; axis++){
		ImGui::PushID(axis);
		// The axis letter in the gizmo's colour, and a stripe of it on the field.
		static const ImVec4 dark[3] = { ImVec4(0.90f, 0.33f, 0.29f, 1), ImVec4(0.36f, 0.73f, 0.45f, 1), ImVec4(0.44f, 0.65f, 0.94f, 1) };
		static const ImVec4 light[3] = { ImVec4(0.75f, 0.16f, 0.13f, 1), ImVec4(0.12f, 0.50f, 0.22f, 1), ImVec4(0.10f, 0.38f, 0.80f, 1) };
		ImVec4 axisColour = UiThemeIsLight() ? light[axis] : dark[axis];
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(axisColour, "%s", axes[axis]);
		ImGui::SameLine(ImGui::GetFontSize() * 2.2f);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::DragFloat("##value", &values[axis], speed, 0.0f, 0.0f, format)) changed = true;
		{
			ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(lo.x + 1.0f, lo.y + 3.0f), ImVec2(lo.x + 3.0f, hi.y - 3.0f),
				ImGui::GetColorU32(axisColour));
		}
		*deactivatedAfterEdit |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::SetItemTooltip("%s", tooltip);
		ImGui::PopID();
	}

	*copyClicked = ImGui::SmallButton(ICON_FA_COPY);
	if(*copyClicked){
		char text[128];
		snprintf(text, sizeof(text), "%.9g %.9g %.9g",
			values[0], values[1], values[2]);
		SetEditorClipboardText(text);
		gCopyableTextCopiedThisFrame = true;
		Toast(TOAST_COPY_PASTE, "%s", strcmp(copiedName, "Rotation") == 0 ? _("Rotation copied") : _("Position copied"));
	}
	ImGui::SetItemTooltip(_("Copy the visible X Y Z values"));
	ImGui::PopID();
	return changed;
}

static void
previewTransformPanelPosition(ObjectInst *inst)
{
	if(gTransformPanel.editKind != TRANSFORM_PANEL_POSITION &&
	   !beginTransformPanelEdit(inst, TRANSFORM_PANEL_POSITION))
		return;
	rw::V3d pos = {
		gTransformPanel.position[0],
		gTransformPanel.position[1],
		gTransformPanel.position[2]
	};
	if(gTransformPanel.relative)
		pos = add(gTransformPanel.startLeaderPos, pos);
	if(!finiteTransformVector(pos)){
		rejectInvalidTransformValues(gTransformPanel.position, gTransformPanel.validPosition);
		return;
	}
	// A GTA building's own coordinate typed as Pawn writes it (six decimals)
	// is its own float, not the nearest one to the text (13.648438 for the
	// IPL's 13.6484375): putting it back is no edit at all.
	if(!gTransformPanel.relative && IsPawnWorldObject(inst))
		for(int i = 0; i < 3; i++){
			float original = (&inst->m_origTranslation.x)[i];
			char typed[48], own[48];
			snprintf(typed, sizeof(typed), "%.6f", (&pos.x)[i]);
			snprintf(own, sizeof(own), "%.6f", original);
			if(strcmp(typed, own) == 0)
				(&pos.x)[i] = original;
		}
	PreviewObjectTransformTargets(inst, gTransformPanel.transforms, pos,
		gTransformPanel.startLeaderRot, UNDO_TRANSFORM_POS);
	copyTransformValues(gTransformPanel.validPosition, gTransformPanel.position);
}

static void
previewTransformPanelRotation(ObjectInst *inst)
{
	if(gTransformPanel.editKind != TRANSFORM_PANEL_ROTATION &&
	   !beginTransformPanelEdit(inst, TRANSFORM_PANEL_ROTATION))
		return;
	rw::V3d degrees = {
		gTransformPanel.rotation[0],
		gTransformPanel.rotation[1],
		gTransformPanel.rotation[2]
	};
	rw::Quat rotation = gTransformPanel.relative ?
		ApplyObjectRotationDelta(gTransformPanel.startLeaderRot, degrees,
			gGizmoSpace == GIZMO_WORLD) :
		MakeObjectRotationDegrees(degrees);
	if(!finiteTransformRotation(rotation)){
		rejectInvalidTransformValues(gTransformPanel.rotation, gTransformPanel.validRotation);
		return;
	}
	PreviewObjectTransformTargets(inst, gTransformPanel.transforms,
		gTransformPanel.startLeaderPos, rotation,
		UNDO_TRANSFORM_ROT);
	copyTransformValues(gTransformPanel.validRotation, gTransformPanel.rotation);
}

static void
applyTransformPanelValues(ObjectInst *inst, const rw::V3d &position,
	const rw::V3d &rotationDegrees, uint8 flags)
{
	finishTransformPanelEdit();
	if(!beginTransformPanelEdit(inst,
		(flags & UNDO_TRANSFORM_ROT) ? TRANSFORM_PANEL_ROTATION : TRANSFORM_PANEL_POSITION))
		return;
	PreviewObjectTransformTargets(inst, gTransformPanel.transforms, position,
		MakeObjectRotationDegrees(rotationDegrees), flags);
	finishTransformPanelEdit();
}

// How much rotation an instance has in SA-MP: objects turn on every axis;
// vehicles, actors, classes and NPCs only face a heading; the other
// elements have no rotation at all.
enum { UI_ROTATION_FULL, UI_ROTATION_HEADING, UI_ROTATION_NONE };

static int
uiRotationMode(const ObjectInst *inst)
{
	if(inst->m_isVehicle)
		return UI_ROTATION_HEADING;
	switch(GetSampElementKind(inst)){
	case SAMP_ELEMENT_OBJECT: return UI_ROTATION_FULL;
	case SAMP_ELEMENT_ACTOR:
	case SAMP_ELEMENT_CLASS:
	case SAMP_ELEMENT_NPC: return UI_ROTATION_HEADING;
	}
	return UI_ROTATION_NONE;
}

static void
uiTransformPanel(ObjectInst *inst)
{
	gTransformPanelDrawnThisFrame = true;
	int rotationMode = uiRotationMode(inst);
	// Delta editing is for object layouts; a heading is typed as it is.
	if(rotationMode != UI_ROTATION_FULL && gTransformPanel.relative){
		finishTransformPanelEdit();
		gTransformPanel.relative = false;
		refreshTransformPanel(inst);
	}
	if(gTransformPanel.leader != inst){
		finishTransformPanelEdit();
		refreshTransformPanel(inst);
	}else if(gTransformPanel.editKind == TRANSFORM_PANEL_NONE)
		refreshTransformPanel(inst);

	bool absolute = !gTransformPanel.relative;
	if(rotationMode == UI_ROTATION_FULL){
		if(ImGui::RadioButton(_("Absolute"), absolute)){
			finishTransformPanelEdit();
			gTransformPanel.relative = false;
			refreshTransformPanel(inst);
		}
		ImGui::SameLine();
		if(ImGui::RadioButton(_("Delta"), !absolute)){
			finishTransformPanelEdit();
			gTransformPanel.relative = true;
			refreshTransformPanel(inst);
		}
		ImGui::SetItemTooltip(_("Delta values are offsets from the current transform and reset to zero after each edit."));
	}


	bool positionDeactivated = false;
	bool positionCopyClicked = false;
	bool positionChanged = uiTransformVectorField(
		gTransformPanel.relative ? _("Move by") : _("Position"),
		gTransformPanel.position, 0.1f, "%.3f",
		_("Drag to adjust. Double-click to enter an exact value."),
		"Position", &positionDeactivated, &positionCopyClicked);
	if(positionChanged){
		if(!finiteTransformValues(gTransformPanel.position))
			rejectInvalidTransformValues(gTransformPanel.position, gTransformPanel.validPosition);
		else
			previewTransformPanelPosition(inst);
	}
	if(positionDeactivated && gTransformPanel.editKind == TRANSFORM_PANEL_POSITION)
		finishTransformPanelEdit();
	if(positionCopyClicked)
		finishTransformPanelEdit();

	if(rotationMode == UI_ROTATION_HEADING){
		ImGui::TextUnformatted(_("Heading"));
		ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
		if(ImGui::DragFloat("##heading", &gTransformPanel.rotation[2], 0.5f, 0.0f, 0.0f, "%.1f\xC2\xB0")){
			if(!finiteTransformValues(gTransformPanel.rotation))
				rejectInvalidTransformValues(gTransformPanel.rotation, gTransformPanel.validRotation);
			else
				previewTransformPanelRotation(inst);
		}
		ImGui::SetItemTooltip(_("The way it faces, in degrees (the Z rotation). Drag, or double-click to type."));
		if(ImGui::IsItemDeactivatedAfterEdit() && gTransformPanel.editKind == TRANSFORM_PANEL_ROTATION)
			finishTransformPanelEdit();
	}
	if(rotationMode != UI_ROTATION_FULL)
		return;

	bool rotationDeactivated = false;
	bool rotationCopyClicked = false;
	bool rotationChanged = uiTransformVectorField(
		gTransformPanel.relative ? _("Turn by") : _("Rotation"),
		gTransformPanel.rotation, 0.5f, "%.2f",
		gTransformPanel.relative ?
			_("Degrees. Drag to adjust, double-click to type. Delta rotation uses the World/Local axes.") :
			_("Degrees. Drag to adjust, double-click to type."),
		"Rotation", &rotationDeactivated, &rotationCopyClicked);
	if(rotationChanged){
		if(!finiteTransformValues(gTransformPanel.rotation))
			rejectInvalidTransformValues(gTransformPanel.rotation, gTransformPanel.validRotation);
		else
			previewTransformPanelRotation(inst);
	}
	if(rotationDeactivated && gTransformPanel.editKind == TRANSFORM_PANEL_ROTATION)
		finishTransformPanelEdit();
	if(rotationCopyClicked)
		finishTransformPanelEdit();

	if(ImGui::SmallButton(TI(ICON_FA_COPY, "Copy transform"))){
		finishTransformPanelEdit();
		gTransformClipboard.valid = true;
		gTransformClipboard.position = inst->m_translation;
		gTransformClipboard.rotationDegrees = GetInstanceRotationDegrees(inst);
		char text[256];
		snprintf(text, sizeof(text),
			"Position %.9g %.9g %.9g\nRotation %.9g %.9g %.9g",
			gTransformClipboard.position.x, gTransformClipboard.position.y, gTransformClipboard.position.z,
			gTransformClipboard.rotationDegrees.x, gTransformClipboard.rotationDegrees.y,
			gTransformClipboard.rotationDegrees.z);
		SetEditorClipboardText(text);
	}
	ImGui::SameLine();
	TransformClipboard systemClipboard = {};
	const char *clipboardText = ImGui::GetClipboardText();
	if(clipboardText){
		float px, py, pz, rx, ry, rz;
		if(sscanf(clipboardText, "Position %f %f %f\nRotation %f %f %f",
		   &px, &py, &pz, &rx, &ry, &rz) == 6){
			systemClipboard.position = { px, py, pz };
			systemClipboard.rotationDegrees = { rx, ry, rz };
			systemClipboard.valid = finiteTransformVector(systemClipboard.position) &&
				finiteTransformVector(systemClipboard.rotationDegrees);
		}
	}
	ImGui::BeginDisabled(!systemClipboard.valid && !gTransformClipboard.valid);
	if(ImGui::SmallButton(TI(ICON_FA_PASTE, "Paste transform"))){
		const TransformClipboard &source = systemClipboard.valid ? systemClipboard : gTransformClipboard;
		applyTransformPanelValues(inst, source.position,
			source.rotationDegrees, UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
	}
	ImGui::EndDisabled();

	if(ImGui::SmallButton(_("Reset rotation")))
		applyTransformPanelValues(inst, inst->m_translation, { 0.0f, 0.0f, 0.0f },
			UNDO_TRANSFORM_ROT);
	ImGui::SetItemTooltip(_("Set X, Y and Z rotation to zero."));
	ImGui::SameLine();
	if(ImGui::SmallButton(_("Make upright"))){
		rw::V3d degrees = GetObjectRotationDegrees(inst->m_rotation);
		applyTransformPanelValues(inst, inst->m_translation, { 0.0f, 0.0f, degrees.z },
			UNDO_TRANSFORM_ROT);
	}
	ImGui::SetItemTooltip(_("Remove tilt while preserving the object's Z heading."));
}

static rw::Geometry*
uiGetMaterialGeometry(ObjectInst *inst)
{
	if(inst == nil)
		return nil;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return nil;
	if(obj->m_type == ObjectDef::ATOMIC){
		if(inst->m_rwObject)
			return ((rw::Atomic*)inst->m_rwObject)->geometry;
		return obj->m_atomics[0] ? obj->m_atomics[0]->geometry : nil;
	}
	if(inst->m_rwObject){
		rw::Clump *clump = (rw::Clump*)inst->m_rwObject;
		FORLIST(lnk, clump->atomics)
			return rw::Atomic::fromClump(lnk)->geometry;
	}else if(obj->m_clump){
		FORLIST(lnk, obj->m_clump->atomics)
			return rw::Atomic::fromClump(lnk)->geometry;
	}
	return nil;
}

// The objects a material edit of inst's slot goes to: inst, and with "Edit
// all selected" every other selected object that has that slot.
static void
uiMaterialTargets(ObjectInst *inst, int slot, std::vector<ObjectInst*> &out)
{
	out.clear();
	out.push_back(inst);
	if(!gBatchEditSelection)
		return;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other == inst || other->m_isDeleted || other->m_isVehicle || !IsPawnObjectEditable(other) ||
		   GetSampElementKind(other) != SAMP_ELEMENT_OBJECT)
			continue;
		rw::Geometry *geometry = uiGetMaterialGeometry(other);
		if(geometry && slot < geometry->matList.numMaterials)
			out.push_back(other);
	}
}

bool
GetBatchEditSelection(void)
{
	return gBatchEditSelection;
}

void
SetBatchEditSelection(bool on)
{
	gBatchEditSelection = on;
}

int
GetInstanceMaterialCount(ObjectInst *inst)
{
	rw::Geometry *geometry = uiGetMaterialGeometry(inst);
	return geometry ? geometry->matList.numMaterials : 0;
}

static bool
uiTxdChainContains(int txdSlot, int wantedSlot)
{
	for(int depth = 0; txdSlot >= 0 && depth < GetNumTxdSlots(); depth++){
		if(txdSlot == wantedSlot)
			return true;
		TxdDef *txd = GetTxdDef(txdSlot);
		if(txd == nil)
			break;
		txdSlot = txd->parentId;
	}
	return false;
}

static void
uiBuildMaterialTextureChoices(int txdSlot)
{
	gMaterialTextureChoices.clear();
	TxdDef *selectedTxd = GetTxdDef(txdSlot);
	if(selectedTxd == nil)
		return;
	if(!IsTxdLoaded(txdSlot))
		LoadTxd(txdSlot);
	if(!IsTxdLoaded(txdSlot))
		return;

	int sourceModel = -1;
	// Prefer a model that directly uses this TXD. If the user picked a parent
	// dictionary, use a model whose TXD inherits it so Pawn export has a model
	// ID that can resolve the texture.
	for(int i = 0; i < NUMOBJECTDEFS; i++){
		ObjectDef *obj = GetObjectDef(i);
		if(obj && obj->m_txdSlot == txdSlot){
			sourceModel = i;
			break;
		}
	}
	if(sourceModel < 0){
		for(int i = 0; i < NUMOBJECTDEFS; i++){
			ObjectDef *obj = GetObjectDef(i);
			if(obj && uiTxdChainContains(obj->m_txdSlot, txdSlot)){
				sourceModel = i;
				break;
			}
		}
	}

	// Child TXDs can inherit textures from their parents. List the full chain,
	// but retain the selected TXD as the lookup source so the preview and Pawn
	// export use the same dictionary name.
	for(int dictionarySlot = txdSlot, depth = 0;
	    dictionarySlot >= 0 && depth < GetNumTxdSlots(); depth++){
		TxdDef *txd = GetTxdDef(dictionarySlot);
		if(txd == nil || txd->txd == nil)
			break;
		for(int j = 0; j < GetTxdTextureCount(dictionarySlot); j++){
			rw::Texture *texture = GetTxdTexture(dictionarySlot, j);
			if(texture == nil || texture->name[0] == '\0')
				continue;
			bool duplicate = false;
			for(size_t k = 0; k < gMaterialTextureChoices.size(); k++)
				if(rw::strcmp_ci(gMaterialTextureChoices[k].textureName, texture->name) == 0){
					duplicate = true;
					break;
				}
			if(duplicate)
				continue;
			MaterialTextureChoice choice;
			memset(&choice, 0, sizeof(choice));
			choice.txdSlot = txdSlot;
			choice.sourceModel = sourceModel;
			choice.textureDictionarySlot = dictionarySlot;
			choice.textureIndex = j;
			strncpy(choice.txdName, selectedTxd->name, sizeof(choice.txdName)-1);
			strncpy(choice.textureName, texture->name, sizeof(choice.textureName)-1);
			gMaterialTextureChoices.push_back(choice);
		}
		dictionarySlot = txd->parentId;
	}
}

static void
uiInitMaterialOverrideFromBase(ObjectInst *inst, int slot, rw::Geometry *geometry,
	ObjectMaterialOverride *edited)
{
	if(inst == nil || edited == nil)
		return;
	InitObjectMaterialOverride(edited, inst->m_objectId);
	edited->active = true;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	TxdDef *txd = obj ? GetTxdDef(obj->m_txdSlot) : nil;
	if(txd){
		strncpy(edited->txdName, txd->name, sizeof(edited->txdName)-1);
		edited->txdSlot = obj->m_txdSlot;
	}
	if(geometry && slot >= 0 && slot < geometry->matList.numMaterials){
		rw::Material *base = geometry->matList.materials[slot];
		if(base){
			// Leave the colour at 0 ("keep the model's colour"): any other value
			// strips the vertex lighting in game, like SA-MP does.
			if(base->texture){
				edited->hasTexture = true;
				strncpy(edited->textureName, base->texture->name,
					sizeof(edited->textureName)-1);
			}
		}
	}
}

static void
uiOpenMaterialTexturePopup(ObjectInst *inst, int slot)
{
	if(inst == nil)
		return;
	gMaterialTextureBrowserInst = inst;
	gMaterialTextureBrowserSlot = slot;
	uiInitMaterialOverrideFromBase(inst, slot, uiGetMaterialGeometry(inst),
		&gMaterialTextureBrowserInitial);
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	gMaterialTextureBrowserTxd = obj ? obj->m_txdSlot : -1;
	if(gMaterialTextureBrowserTxd >= 0 && !IsTxdLoaded(gMaterialTextureBrowserTxd))
		LoadTxd(gMaterialTextureBrowserTxd);
	uiBuildMaterialTextureChoices(gMaterialTextureBrowserTxd);
	gMaterialTextureFilter.Clear();
	gMaterialTxdFilter.Clear();
	// Open from uiMaterialTexturePopup after uiMaterialInfo has popped its
	// per-slot ID scope, so OpenPopup and BeginPopupModal share the same ID.
	gOpenMaterialTexturePopup = true;
}

static void
uiApplyMaterialTextureChoice(const MaterialTextureChoice &choice)
{
	if(gMaterialTextureBrowserInst == nil || gMaterialTextureBrowserSlot < 0)
		return;
	// The texture goes to the same slot of every selected object that has it,
	// each keeping its own colour and other settings; one undo step.
	std::vector<ObjectInst*> targets;
	uiMaterialTargets(gMaterialTextureBrowserInst, gMaterialTextureBrowserSlot, targets);
	UndoBeginGroup();
	for(ObjectInst *target : targets){
		ObjectMaterialOverride edited;
		const ObjectMaterialOverride *current = GetObjectMaterialOverride(target, gMaterialTextureBrowserSlot);
		if(current && (current->active || current->textEnabled))
			edited = *current;
		else if(target == gMaterialTextureBrowserInst)
			edited = gMaterialTextureBrowserInitial;
		else
			uiInitMaterialOverrideFromBase(target, gMaterialTextureBrowserSlot,
				uiGetMaterialGeometry(target), &edited);
		edited.active = true;
		edited.textEnabled = false;
		edited.hasTexture = true;
		edited.sourceModel = choice.sourceModel;
		edited.txdSlot = choice.txdSlot;
		strncpy(edited.txdName, choice.txdName, sizeof(edited.txdName)-1);
		edited.txdName[sizeof(edited.txdName)-1] = '\0';
		strncpy(edited.textureName, choice.textureName, sizeof(edited.textureName)-1);
		edited.textureName[sizeof(edited.textureName)-1] = '\0';
		SetObjectMaterialOverride(target, gMaterialTextureBrowserSlot, edited);
	}
	UndoEndGroup();
}

static void
uiMaterialTexturePopup(void)
{
	if(gOpenMaterialTexturePopup){
		ImGui::OpenPopup(L("Choose Material Texture"));
		gOpenMaterialTexturePopup = false;
	}
	if(gMaterialTextureBrowserInst == nil || gMaterialTextureBrowserSlot < 0)
		return;
	if(!ImGui::BeginPopupModal(L("Choose Material Texture"), nil, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	TxdDef *selectedTxd = GetTxdDef(gMaterialTextureBrowserTxd);
	const char *txdPreview = selectedTxd ? selectedTxd->name : _("Select TXD...");
	if(ImGui::BeginCombo("TXD", txdPreview)){
		gMaterialTxdFilter.Draw(_("Filter TXDs"), 220.0f);
		if(ImGui::BeginChild("##material_txd_list", ImVec2(500.0f, 180.0f), true)){
			for(int i = 0; i < GetNumTxdSlots(); i++){
				TxdDef *txd = GetTxdDef(i);
				if(txd == nil || txd->name[0] == '\0' ||
				   !gMaterialTxdFilter.PassFilter(txd->name))
					continue;
				ImGui::PushID(i);
				if(ImGui::Selectable(txd->name, i == gMaterialTextureBrowserTxd)){
					gMaterialTextureBrowserTxd = i;
					LoadTxd(i);
					uiBuildMaterialTextureChoices(i);
					gMaterialTextureFilter.Clear();
				}
				if(i == gMaterialTextureBrowserTxd)
					ImGui::SetItemDefaultFocus();
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		ImGui::EndCombo();
	}
	gMaterialTextureFilter.Draw(_("Filter"), 220.0f);
	ImGui::TextDisabled(_("Choose a TXD, preview its textures, then click one to assign it to this material slot."));
	ImGui::Text(_("%d texture(s)"), (int)gMaterialTextureChoices.size());
	bool textureChildVisible = ImGui::BeginChild("##material_texture_list", ImVec2(520.0f, 330.0f), true);
	if(textureChildVisible){
		float availableWidth = ImGui::GetContentRegionAvail().x;
		int columns = clamp((int)(availableWidth / 112.0f), 1, 5);
		if(ImGui::BeginTable("##material_texture_grid", columns,
			ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)){
		for(size_t i = 0; i < gMaterialTextureChoices.size(); i++){
			MaterialTextureChoice &choice = gMaterialTextureChoices[i];
			char label[MODELNAMELEN + PAWN_TEXTURE_NAME_LEN + 8];
			snprintf(label, sizeof(label), "%s:%s", choice.txdName, choice.textureName);
			if(!gMaterialTextureFilter.PassFilter(label))
				continue;
			ImGui::TableNextColumn();
			ImGui::PushID((int)i);
			rw::Texture *texture = GetTxdTexture(choice.textureDictionarySlot,
				choice.textureIndex);
			bool clicked;
			if(texture && texture->raster)
				clicked = ImGui::ImageButton("##texture",
					(void*)(intptr_t)texture, ImVec2(80.0f, 64.0f));
			else
				clicked = ImGui::Button(_("No preview"), ImVec2(80.0f, 64.0f));
			ImGui::TextWrapped("%s", choice.textureName);
			if(clicked){
				uiApplyMaterialTextureChoice(choice);
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
		}
		if(gMaterialTextureChoices.empty())
			ImGui::TextDisabled(selectedTxd ? _("No textures found in this TXD or its parents.") : _("Select a TXD to see its textures."));
		else if(gMaterialTextureFilter.IsActive())
			ImGui::TextDisabled(_("Filtered texture names include the source TXD."));
	}
	ImGui::EndChild();
	if(ImGui::Button(_("Cancel")))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void
uiTextureBrowserWindow(void)
{
	DrawTextureBrowserWindow(&showTextureBrowserWindow);
}

static const int uiMaterialSizeValues[] = {
	10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140
};
static const char *uiMaterialSizeNames[] = {
	"32 x 32", "64 x 32", "64 x 64", "128 x 32", "128 x 64", "128 x 128",
	"256 x 32", "256 x 64", "256 x 128", "256 x 256", "512 x 64", "512 x 128",
	"512 x 256", "512 x 512"
};

static int
uiMaterialSizeIndex(int materialSize)
{
	for(int i = 0; i < IM_ARRAYSIZE(uiMaterialSizeValues); i++)
		if(uiMaterialSizeValues[i] == materialSize)
			return i;
	return 8; // OBJECT_MATERIAL_SIZE_256x128
}

static ImGuiTextFilter gMaterialFontFilter;

enum { UI_MATERIAL_ORIGINAL, UI_MATERIAL_TEXTURE, UI_MATERIAL_TEXT };

static int
uiMaterialMode(const ObjectMaterialOverride &material)
{
	if(material.textEnabled)
		return UI_MATERIAL_TEXT;
	return material.active ? UI_MATERIAL_TEXTURE : UI_MATERIAL_ORIGINAL;
}

// Explains how SA-MP resolves the slot's SetObjectMaterial call. Returns
// false when the game would not show what the user picked.
static bool
uiMaterialTextureStatus(const ObjectMaterialOverride &material, char *buf, size_t size)
{
	return CheckMaterialTexture(material, buf, size, true) == MATERIAL_TEXTURE_OK;
}

// A texture as a framed square, large when hovered, so a slot shows what it
// looks like and not only its name. Returns true when it was drawn.
static void
uiTexturePreviewBox(const char *caption, rw::Texture *tex, const char *name, float side)
{
	ImGui::BeginGroup();
	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(caption, ImVec2(side, side));
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(pos, ImVec2(pos.x + side, pos.y + side), IM_COL32(60, 60, 66, 255), 4.0f);
	if(tex)
		dl->AddImageRounded((ImTextureID)tex, pos, ImVec2(pos.x + side, pos.y + side),
			ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f);
	else{
		ImVec2 ts = ImGui::CalcTextSize("?");
		dl->AddText(ImVec2(pos.x + (side - ts.x)*0.5f, pos.y + (side - ts.y)*0.5f), IM_COL32(150, 150, 158, 255), "?");
	}
	dl->AddRect(pos, ImVec2(pos.x + side, pos.y + side), hovered ? IM_COL32(255, 220, 60, 255) : IM_COL32(20, 20, 24, 255),
		4.0f, 0, hovered ? 2.0f : 1.0f);
	ImGui::EndGroup();
	if(hovered){
		ImGui::BeginTooltip();
		if(tex)
			ImGui::Image((ImTextureID)tex, ImVec2(256, 256));
		ImGui::TextUnformatted(name && name[0] ? name : _("untextured"));
		if(tex)
			ImGui::TextDisabled("%d x %d", tex->raster ? tex->raster->width : 0, tex->raster ? tex->raster->height : 0);
		ImGui::EndTooltip();
	}
}

static void
uiStatusLine(bool ok, const char *text)
{
	if(ok)
		ImGui::TextDisabled("%s", text);
	else{
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		ImGui::TextWrapped("%s", text);
		ImGui::PopStyleColor();
	}
}

static bool
uiMaterialFontPicker(ObjectMaterialOverride *material)
{
	bool changed = false;
	float buttonWidth = ImGui::GetFrameHeight();
	ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::InputText("##font", material->font, sizeof(material->font)))
		changed = true;
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::BeginCombo("##fontlist", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLarge)){
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		gMaterialFontFilter.Draw("##fontfilter", 220.0f);
		const std::vector<std::string> &families = GetMaterialFontFamilies();
		for(size_t i = 0; i < families.size(); i++){
			const char *family = families[i].c_str();
			if(!gMaterialFontFilter.PassFilter(family))
				continue;
			bool selected = rw::strcmp_ci(family, material->font) == 0;
			if(ImGui::Selectable(family, selected)){
				strncpy(material->font, family, sizeof(material->font)-1);
				material->font[sizeof(material->font)-1] = '\0';
				changed = true;
			}
			if(selected)
				ImGui::SetItemDefaultFocus();
		}
		if(families.empty())
			ImGui::TextDisabled(_("No font files found."));
		ImGui::EndCombo();
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextUnformatted(_("Font"));

	std::string description;
	int fontStatus = DescribeMaterialFont(material->font, material->bold, &description);
	if(fontStatus == MATERIAL_FONT_FOUND)
		ImGui::TextDisabled(_("Preview font: %s"), description.c_str());
	else if(fontStatus == MATERIAL_FONT_SUBSTITUTE){
		ImGui::TextDisabled(_("Preview font: %s (substitute)"), description.c_str());
		if(ImGui::IsItemHovered())
			ImGui::SetTooltip(_("\"%s\" is not installed here. This free font has the same letter widths,\n"
				"so the text fits as in game; the letter shapes differ a little."), material->font);
	}else{
		char text[512];
		snprintf(text, sizeof(text), _("\"%s\" is not installed here; the preview uses %s. "
			"Players without it get Windows' substitute font."), material->font, description.c_str());
		uiStatusLine(false, text);
	}
	if(strlen(material->font) >= 32)
		uiStatusLine(false, _("SA-MP rejects font names of 32 characters or more."));
	return changed;
}

// The user's fonts folder, made if it isn't there yet.
static void
uiOpenFontsFolder(void)
{
	std::string dir = GetMaterialUserFontsDir();
	if(dir.empty())
		return;
	EnsureParentDirectoriesForPath((dir + "/x").c_str());
	OpenInFileManager(dir);
}

static void
uiMaterialTextPreview(const ObjectMaterialOverride &material)
{
	rw::Texture *texture = GetObjectMaterialTextPreview(material);
	if(texture == nil || texture->raster == nil)
		return;
	int width = texture->raster->width, height = texture->raster->height;
	float maxWidth = min(ImGui::GetContentRegionAvail().x, 384.0f);
	float scale = min(maxWidth / width, 256.0f / height);
	ImVec2 size(width * scale, height * scale);
	// Checkerboard behind the texture so transparent backgrounds read clearly.
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const float cell = 8.0f;
	for(float y = 0.0f; y < size.y; y += cell)
		for(float x = 0.0f; x < size.x; x += cell){
			bool dark = ((int)(x/cell) + (int)(y/cell)) & 1;
			draw->AddRectFilled(ImVec2(origin.x + x, origin.y + y),
				ImVec2(origin.x + min(x + cell, size.x), origin.y + min(y + cell, size.y)),
				dark ? IM_COL32(90, 90, 90, 255) : IM_COL32(130, 130, 130, 255));
		}
	ImGui::Image((void*)(intptr_t)texture, size);
	ImGui::TextDisabled(_("%d x %d texture, as SA-MP renders it"), width, height);
}

static void
uiMaterialInfo(ObjectInst *inst)
{
	if(inst == nil)
		return;
	if(inst->m_isVehicle){
		ImGui::TextDisabled(_("Vehicle colors and respawn settings are in the Instance panel."));
		return;
	}
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return;
	bool canEdit = IsPawnObjectEditable(inst);
	if(IsPawnWorldObject(inst)){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		ImGui::TextWrapped(_("World object: it stays unchanged and is omitted from Pawn export. If you move, rotate, delete, or edit its materials, export removes the original per player and creates the scripted replacement."));
		ImGui::PopStyleColor();
	}
	if(!canEdit)
		ImGui::TextDisabled(_("Material controls are locked for this object."));
	if(ImGui::Checkbox(L("Show material indexes"), &gShowMaterialIndexes))
		BumpChangeSeq();
	ImGui::SetItemTooltip("%s", _("Paints each material slot of the selected objects with its number, white on green,\n"
		"as Texture Studio does, to see which part is which index. The game materials stay unchanged."));
	{
		std::vector<ObjectInst*> targets;
		uiMaterialTargets(inst, 0, targets);
		if(targets.size() > 1)
			ImGui::TextDisabled(_("Applies to %d selected (each slot to those that have it)."), (int)targets.size());
	}

	rw::Geometry *geometry = uiGetMaterialGeometry(inst);
	int materialCount = geometry ? geometry->matList.numMaterials : 0;
	if(materialCount <= 0){
		ImGui::TextDisabled(_("No material slots are loaded for this object yet."));
		return;
	}
	if(materialCount > MAX_PAWN_MATERIALS){
		ImGui::TextDisabled(_("This model has %d materials; SA-MP can only change the first %d."),
			materialCount, MAX_PAWN_MATERIALS);
		materialCount = MAX_PAWN_MATERIALS;
	}

	for(int slot = 0; slot < materialCount; slot++){
		// A color edit can replace the instance's private geometry immediately.
		// Fetch it again after each slot instead of reading a freed geometry on
		// the next iteration of this same ImGui frame.
		geometry = uiGetMaterialGeometry(inst);
		if(geometry == nil || slot >= geometry->matList.numMaterials)
			break;
		ObjectMaterialOverride *material = GetObjectMaterialOverride(inst, slot);
		if(material == nil)
			continue;
		ImGui::PushID(slot);
		rw::Material *base = slot < geometry->matList.numMaterials
			? geometry->matList.materials[slot] : nil;
		ObjectDef *def = GetObjectDef(inst->m_objectId);
		rw::Geometry *baseGeometry = def && def->m_type == ObjectDef::ATOMIC && def->m_atomics[0]
			? def->m_atomics[0]->geometry : nil;
		if(baseGeometry && slot < baseGeometry->matList.numMaterials)
			base = baseGeometry->matList.materials[slot];
		const char *baseName = base && base->texture && base->texture->name[0]
			? base->texture->name : _("untextured");

		ObjectMaterialOverride edited = *material;
		bool changed = false;
		int mode = uiMaterialMode(edited);
		ImGui::SeparatorText("");
		ImGui::Text(_("Slot %d"), slot);
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", baseName);
		ImGui::BeginDisabled(!canEdit);
		ImGui::SameLine();
		const char *modes[] = { _("Original"), _("Texture / color"), _("Text") };
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::Combo("##mode", &mode, modes, IM_ARRAYSIZE(modes))){
			if(mode == UI_MATERIAL_ORIGINAL){
				edited.active = false;
				edited.textEnabled = false;
			}else if(mode == UI_MATERIAL_TEXTURE){
				if(!edited.active){
					ObjectMaterialOverride fresh;
					uiInitMaterialOverrideFromBase(inst, slot, geometry, &fresh);
					// Keep any text settings so switching back restores them.
					memcpy(fresh.text, edited.text, sizeof(fresh.text));
					memcpy(fresh.font, edited.font, sizeof(fresh.font));
					fresh.fontSize = edited.fontSize;
					fresh.bold = edited.bold;
					fresh.textColor = edited.textColor;
					fresh.backgroundColor = edited.backgroundColor;
					fresh.alignment = edited.alignment;
					fresh.materialSize = edited.materialSize;
					edited = fresh;
				}
				edited.active = true;
				edited.textEnabled = false;
			}else{
				edited.textEnabled = true;
				if(edited.text[0] == '\0')
					strncpy(edited.text, "Text", sizeof(edited.text)-1);
			}
			changed = true;
		}

		// What the slot looks like now: the model's own texture and, when it is
		// replaced, the chosen one.
		{
			float side = ImGui::GetFontSize()*3.6f;
			uiTexturePreviewBox("##orig_tex", base ? base->texture : nil, baseName, side);
			if(mode == UI_MATERIAL_TEXTURE && edited.hasTexture){
				rw::Texture *chosen = nil;
				if(edited.sourceModel > 0 && edited.txdName[0] && edited.textureName[0]){
					int txdSlot = FindTxdSlot(edited.txdName);
					if(txdSlot >= 0)
						chosen = FindTextureInTxd(txdSlot, edited.textureName);
				}
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("->");
				ImGui::SameLine();
				uiTexturePreviewBox("##new_tex", chosen, edited.sourceModel == 0 ? _("blank white") : edited.textureName, side);
			}
		}
		if(mode == UI_MATERIAL_TEXTURE){
			if(ImGui::Button(_("Browse textures...")))
				uiOpenMaterialTexturePopup(inst, slot);
			int model = edited.hasTexture ? edited.sourceModel : -1;
			if(ImGui::InputInt(_("Model ID"), &model)){
				model = clamp(model, -1, 65534);
				edited.hasTexture = model >= 0;
				edited.sourceModel = model;
				changed = true;
			}
			ImGui::SetItemTooltip(_("-1 keeps the original texture, 0 is SA-MP's blank white texture.\n"
				"Otherwise SA-MP loads this model so its TXD is available."));
			if(edited.hasTexture && edited.sourceModel > 0){
				if(ImGui::InputText("TXD", edited.txdName, sizeof(edited.txdName))) changed = true;
				if(ImGui::InputText(_("Texture"), edited.textureName, sizeof(edited.textureName))) changed = true;
			}
			char status[512];
			bool ok = uiMaterialTextureStatus(edited, status, sizeof(status));
			uiStatusLine(ok, status);

			bool recolor = edited.color.red || edited.color.green ||
				edited.color.blue || edited.color.alpha;
			if(ImGui::Checkbox(_("Change color"), &recolor)){
				edited.color = recolor ? rw::RGBA{ 255, 255, 255, 255 } : rw::RGBA{ 0, 0, 0, 0 };
				changed = true;
			}
			if(recolor){
				ImGui::SameLine();
				rw::RGBAf tint;
				convColor(&tint, &edited.color);
				if(ImGui::ColorEdit4("##tint", &tint.red, ImGuiColorEditFlags_AlphaBar)){
					convColor(&edited.color, &tint);
					if(!(edited.color.red || edited.color.green || edited.color.blue || edited.color.alpha))
						edited.color.alpha = 1;	// 0x00000000 would mean "no change"
					changed = true;
				}
				ImGui::TextDisabled(_("As in game, a color removes the object's vertex lighting and reflections."));
			}else
				ImGui::TextDisabled(_("Color 0: the model's own color is kept."));
		}else if(mode == UI_MATERIAL_TEXT){
			if(ImGui::InputTextMultiline(_("Text"), edited.text, sizeof(edited.text), ImVec2(0.0f, 70.0f)))
				changed = true;
			ImGui::TextDisabled(_("{RRGGBB} switches color (opaque) for the following text. Enter starts a new line."));
			if(edited.text[0] == '\0')
				uiStatusLine(false, _("SA-MP ignores material text calls with empty text."));
			if(uiMaterialFontPicker(&edited)) changed = true;
			if(ImGui::SliderInt(_("Font size"), &edited.fontSize, 1, 255)) changed = true;
			int materialSizeIndex = uiMaterialSizeIndex(edited.materialSize);
			if(ImGui::Combo(_("Material size"), &materialSizeIndex, uiMaterialSizeNames,
				IM_ARRAYSIZE(uiMaterialSizeNames))){
				edited.materialSize = uiMaterialSizeValues[materialSizeIndex];
				changed = true;
			}
			const char *alignments[] = { _("Left (top)"), _("Center (middle)"), _("Right (top)") };
			if(ImGui::Combo(_("Alignment"), &edited.alignment, alignments, IM_ARRAYSIZE(alignments))) changed = true;
			if(ImGui::Checkbox(_("Bold"), &edited.bold)) changed = true;
			rw::RGBAf textColor;
			convColor(&textColor, &edited.textColor);
			if(ImGui::ColorEdit4(_("Text color"), &textColor.red, ImGuiColorEditFlags_AlphaBar)){
				convColor(&edited.textColor, &textColor);
				changed = true;
			}
			rw::RGBAf backgroundColor;
			convColor(&backgroundColor, &edited.backgroundColor);
			if(ImGui::ColorEdit4(_("Background"), &backgroundColor.red, ImGuiColorEditFlags_AlphaBar)){
				convColor(&edited.backgroundColor, &backgroundColor);
				changed = true;
			}
			uiMaterialTextPreview(edited);
		}
		if(changed){
			// The same slot of every selected object that has it, one undo step.
			std::vector<ObjectInst*> targets;
			uiMaterialTargets(inst, slot, targets);
			UndoBeginGroup();
			for(ObjectInst *target : targets)
				SetObjectMaterialOverride(target, slot, edited);
			UndoEndGroup();
		}
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	ImGui::Spacing();
	if(ImGui::SmallButton(_("Rescan fonts"))){
		RescanMaterialFonts();
		RefreshMaterialTextTextures();
	}
	ImGui::SetItemTooltip(_("Searches your fonts folder, the GTA folder (SA-MP's fonts), the Wine prefix's\n"
		"Windows\\Fonts and the system font folders again."));
	ImGui::SameLine();
	if(ImGui::SmallButton(_("Open fonts folder")))
		uiOpenFontsFolder();
	ImGui::SetItemTooltip(_("Your fonts folder: SA-MP's fonts and the free stand-ins are put there;\n"
		"add other .ttf files to it, then Rescan fonts."));
}


static ImVec4
vehicleColourVec(int index)
{
	uint32 c = GetSampVehicleColour(index);
	return ImVec4((c >> 24 & 0xFF)/255.0f, (c >> 16 & 0xFF)/255.0f, (c >> 8 & 0xFF)/255.0f, 1.0f);
}

// SA-MP's 256 vehicle colours as swatches; -1 is random.
static bool
uiVehicleColourPicker(const char *label, int *colour)
{
	bool changed = false;
	ImGui::PushID(label);
	ImVec2 swatch(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
	if(*colour < 0){
		if(ImGui::Button("?", swatch))
			ImGui::OpenPopup("colours");
	}else if(ImGui::ColorButton("##current", vehicleColourVec(*colour), ImGuiColorEditFlags_NoTooltip, swatch))
		ImGui::OpenPopup("colours");
	ImGui::SetItemTooltip(_("Pick a colour"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
	int value = *colour;
	if(ImGui::InputInt("##value", &value)){
		*colour = clamp(value, -1, 255);
		changed = true;
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(label);
	if(ImGui::BeginPopup("colours")){
		if(ImGui::Button(_("Random (-1)"))){
			*colour = -1;
			changed = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::TextDisabled(_("0-127 are the game's, 128-255 SA-MP's"));
		ImVec2 cell(ImGui::GetFontSize() * 1.2f, ImGui::GetFontSize() * 1.2f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
		for(int i = 0; i < 256; i++){
			if(i % 16)
				ImGui::SameLine();
			ImGui::PushID(i);
			bool current = i == *colour;
			if(current)
				ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
			if(ImGui::ColorButton("##c", vehicleColourVec(i), ImGuiColorEditFlags_NoTooltip |
			   (current ? 0 : ImGuiColorEditFlags_NoBorder), cell)){
				*colour = i;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			if(current)
				ImGui::PopStyleVar();
			ImGui::SetItemTooltip("%d", i);
			ImGui::PopID();
		}
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

static void
uiVehicleSettings(ObjectInst *inst, std::vector<ObjectInst*> &selected, bool batch, bool canEdit)
{
	ImGui::BeginDisabled(!canEdit);
	bool isStatic = inst->m_vehicleStatic;
	int color1 = inst->m_vehicleColor1, color2 = inst->m_vehicleColor2;
	int respawn = inst->m_vehicleRespawnDelay;
	bool staticChanged = ImGui::Checkbox(_("Static vehicle (AddStaticVehicleEx)"), &isStatic);
	if(IsTrainEngineModel(inst->m_objectId)){
		int ids = GetTrainVehicleIds(inst->m_objectId);
		if(ids > 1)
			ImGui::TextWrapped(_("A train: the engine and 3 carriages behind it, %d vehicle ids. Only the engine is "
				"written; the game adds the carriages."), ids);
		else
			ImGui::TextWrapped(_("A tram: no carriages."));
		ImGui::TextDisabled(_("It runs one way with an angle over 180 and the other way up to 180. Dragging "
			"slides it along the track (hold Alt to move it freely)."));
		if(!isStatic)
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Trains need Static vehicle: open.mp's CreateVehicle refuses them."));
	}
	bool color1Changed = uiVehicleColourPicker("Colour 1", &color1);
	ImGui::SetItemTooltip(_("A vehicle colour, 0-255. -1 picks one at random when it spawns."));
	bool color2Changed = uiVehicleColourPicker("Colour 2", &color2);
	ImGui::SetItemTooltip(_("A vehicle colour, 0-255. -1 picks one at random when it spawns."));
	bool respawnChanged = ImGui::InputInt(_("Respawn delay"), &respawn);
	ImGui::SetItemTooltip(_("Seconds. -1 with a static vehicle writes AddStaticVehicle."));

	// Paintjob: only models with <model>1.txd have one.
	int numPaintjobs = GetVehiclePaintjobCount(inst->m_objectId);
	int paintjob = inst->m_vehiclePaintjob;
	bool paintjobChanged = false;
	if(numPaintjobs > 0){
		const char *labels[] = { _("None"), _("Paintjob 0"), _("Paintjob 1"), _("Paintjob 2") };
		if(ImGui::BeginCombo(_("Paintjob"), labels[clamp(paintjob, 0, 3)])){
			for(int i = 0; i <= numPaintjobs; i++)
				if(ImGui::Selectable(labels[i], i == paintjob)){
					paintjob = i;
					paintjobChanged = true;
				}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip(_("ChangeVehiclePaintjob. Colour 1 tints parts of some paintjobs."));
	}

	char plate[VEHICLE_PLATE_LEN];
	memcpy(plate, inst->m_vehiclePlate, sizeof(plate));
	bool plateChanged = ImGui::InputTextWithHint(_("Number plate"), _("default"), plate, sizeof(plate));
	ImGui::SetItemTooltip(_("SetVehicleNumberPlate, up to 32 characters. Empty keeps the game's plate. GTA's preview displays the first 8 characters."));

	// Parts: one per slot, only those that fit this model.
	int changedSlot = -1, changedComponent = 0;
	if(ImGui::TreeNode(L("Tuning parts"))){
		bool any = false;
		for(int slot = 0; slot < VEHICLE_MOD_SLOTS; slot++){
			int current = inst->m_vehicleMods[slot];
			bool fits = false;
			for(int c = 1000; c <= 1193 && !fits; c++)
				fits = GetVehicleComponentSlot(c) == slot && IsVehicleComponentValid(inst->m_objectId, c);
			if(!fits)
				continue;
			any = true;
			char preview[64];
			ObjectDef *def = current ? GetObjectDef(current) : nil;
			if(current)
				snprintf(preview, sizeof(preview), "%d %s", current, def ? def->m_name : "");
			else
				snprintf(preview, sizeof(preview), "%s", _("None"));
			ImGui::PushID(slot);
			if(ImGui::BeginCombo(GetVehicleComponentSlotName(slot), preview)){
				if(ImGui::Selectable(_("None"), current == 0)){
					changedSlot = slot;
					changedComponent = 0;
				}
				for(int c = 1000; c <= 1193; c++){
					if(GetVehicleComponentSlot(c) != slot || !IsVehicleComponentValid(inst->m_objectId, c))
						continue;
					ObjectDef *part = GetObjectDef(c);
					char item[64];
					snprintf(item, sizeof(item), "%d %s", c, part ? part->m_name : "");
					if(ImGui::Selectable(item, c == current)){
						changedSlot = slot;
						changedComponent = c;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}
		if(!any)
			ImGui::TextDisabled(_("No parts fit this model."));
		ImGui::TreePop();
	}
	ImGui::SetItemTooltip(_("AddVehicleComponent. Only parts SA-MP accepts on this model are listed."));
	ImGui::EndDisabled();

	if(staticChanged || color1Changed || color2Changed || respawnChanged ||
	   paintjobChanged || plateChanged || changedSlot >= 0){
		// Changed fields go to every selected vehicle when batch editing.
		UndoBeginGroup();
		for(size_t i = 0; i < selected.size(); i++){
			ObjectInst *target = selected[i];
			if(!target->m_isVehicle || (target != inst && !batch))
				continue;
			VehicleState before = GetVehicleState(target);
			bool rebuild = false;
			if(staticChanged) target->m_vehicleStatic = isStatic;
			if(color1Changed) target->m_vehicleColor1 = clamp(color1, -1, 255);
			if(color2Changed) target->m_vehicleColor2 = clamp(color2, -1, 255);
			if(respawnChanged) target->m_vehicleRespawnDelay = clamp(respawn, -1, 86400);
			if(paintjobChanged && paintjob <= GetVehiclePaintjobCount(target->m_objectId)){
				target->m_vehiclePaintjob = paintjob;
				rebuild = true;
			}
			if(plateChanged)
				memcpy(target->m_vehiclePlate, plate, sizeof(plate));
			if(changedSlot >= 0 && (changedComponent == 0 ||
			   IsVehicleComponentValid(target->m_objectId, changedComponent))){
				target->m_vehicleMods[changedSlot] = (uint16)changedComponent;
				rebuild = true;
			}
			StampChangeSeq(target);
			target->m_isDirty = true;
			if(rebuild)
				target->RefreshVehicleUpgrades();
			else{
				target->RefreshVehicleColors();
				if(plateChanged)
					target->RefreshVehiclePlate();
			}
			UndoRecordVehicle(target, before);
		}
		UndoEndGroup();
	}
}

static void
uiInstInfo(ObjectInst *inst)
{
	bool canEdit = IsPawnObjectEditable(inst);
	uiTransformPanel(inst);

	ImGui::BeginDisabled(!canEdit);
	std::vector<ObjectInst*> selected;
	uiSelectedInstances(selected);
	bool batch = gBatchEditSelection && selected.size() > 1;
	int interior = inst->m_area;
	if(ImGui::InputInt(_("Interior"), &interior)){
		interior = clamp(interior, 0, MAX_INTERIOR_AREA_CODE);
		for(size_t i = 0; i < selected.size(); i++){
			ObjectInst *target = selected[i];
			if((target != inst && !batch) || target->m_area == interior)
				continue;
			target->m_area = interior;
			StampChangeSeq(target);
			target->m_isDirty = true;
		}
	}
	if(batch)
		ImGui::SetItemTooltip(_("Sets the interior of all %d selected."), (int)selected.size());
	ImGui::SetItemTooltip(_("Streamer objects and vehicles use it; native CreateObject objects\nshow in every interior."));
	// The streamer tells interior 0 (only there) from -1 (every interior).
	if(interior == 0 && !inst->m_isVehicle && gPawnSaveFormat == PAWN_CODE_STREAMER){
		bool zeroOnly = GetObjectPawnProperties(inst)->interiorZeroOnly;
		if(ImGui::Checkbox(_("Only interior 0"), &zeroOnly)){
			UndoBeginGroup();
			for(ObjectInst *target : selected){
				if((target != inst && !batch) || target->m_isVehicle || target->m_area != 0)
					continue;
				ObjectPawnProperties properties = *GetObjectPawnProperties(target);
				properties.interiorZeroOnly = zeroOnly;
				SetObjectPawnProperties(target, properties);
			}
			UndoEndGroup();
		}
		ImGui::SetItemTooltip(_("Written as interiorid 0: streamed only to players in interior 0 (outside).\n"
			"Off: -1, every interior."));
	}
	ImGui::EndDisabled();
}

static void
uiObjInfo(ObjectDef *obj)
{
	int i;
	TxdDef *txd;

	txd = GetTxdDef(obj->m_txdSlot);

	ImGui::Text(_("ID: %d\n"), PawnModelId(obj->m_id));
	InputTextReadonly<MODELNAMELEN>(_("Model"), obj->m_name);
	InputTextReadonly<MODELNAMELEN>("TXD", txd ? txd->name : "");

	InputTextReadonly<1024>("IDE", obj->m_file ? obj->m_file->name : "");
	if(obj->m_colModel && !obj->m_gotChildCol)
		InputTextReadonly<1024>("COL", obj->m_colModel->file ? obj->m_colModel->file->name : "");

	ImGui::Text(_("Draw dist:"));
	for(i = 0; i < obj->m_numAtomics; i++){
		ImGui::SameLine();
		ImGui::Text("%.0f", obj->m_drawDist[i]);
	}
	ImGui::Text(_("Min Draw dist: %.0f"), obj->m_minDrawDist);

	if(obj->m_isTimed){
		ImGui::Text(_("Time: %d %d (visible now: %s)"),
			obj->m_timeOn, obj->m_timeOff,
			IsHourInRange(obj->m_timeOn, obj->m_timeOff) ? _("yes") : _("no"));
	}

	if(obj->m_relatedModel)
		ImGui::Text(_("Related: %s\n"), obj->m_relatedModel->m_name);
	if(obj->m_relatedTimeModel)
		ImGui::Text(_("Related timed: %s\n"), obj->m_relatedTimeModel->m_name);

}

struct CamSetting {
	char name[256];
	rw::V3d pos;
	rw::V3d target;
	float fov;

	int hour, minute;
	int weather1, weather2;
	int extracolors;

	int area;
};

std::vector<CamSetting> camSettings;

static void
loadCamSettings(void)
{
	CamSetting cam;
	char line[256], *p, *pp;
	FILE *f;

	f = fopenHaumeaDataRead("camsettings.txt", "camsettings.txt");
	if(f == nil)
		return;
	camSettings.clear();
	while(fgets(line, sizeof(line), f)){
		p = line;
		while(*p && isspace(*p)) p++;
		if(*p != '"')
			continue;
		pp = ++p;
		while(*p && *p != '"') p++;
		if(*p != '"')
			continue;
		*p++ = '\0';
		strncpy(cam.name, pp, sizeof(cam.name));
		sscanf(p, "%f %f %f  %f %f %f  %f  %d %d %d %d  %d",
			&cam.pos.x, &cam.pos.y, &cam.pos.z,
			&cam.target.x, &cam.target.y, &cam.target.z,
			&cam.fov,
			&cam.hour, &cam.minute, &cam.weather1, &cam.weather2,
			&cam.area);
		cam.fov = ClampCameraFov(cam.fov);
		if(cam.area < 0)
			cam.area = 0;
		cam.hour %= 24;
		cam.minute %= 60;
		cam.weather1 %= params.numWeathers;
		cam.weather2 %= params.numWeathers;
		camSettings.push_back(cam);
	}

	fclose(f);
}

static void
loadSaveSettings(void)
{
	FILE *f;
	char line[1024];
	char key[128];
	const char *value;
	std::vector<std::string> recentTextDraws;
	int intValue;
	bool boolValue;
	rw::V3d vecValue;
	int savedSpawnObjectId = -1;
	int loadedSettingsVersion = 0;

	sanitizeAutosaveSettings();
	sanitizeCustomImportSettings();
	gAutosaveLastSeenSeq = gAutosaveLastSavedSeq = GetLatestChangeSeq();
	gRenderMode = gRenderOnlyHD ? 1 : gRenderOnlyLod ? 2 : 0;
	gBrowserTabRestorePending = true;
	loadWindowStateFromSettingsFile();
	gPersistentSettingsLoaded = true;
	gPawnLastSavedChangeSeq = GetLatestChangeSeq();

	f = fopenHaumeaDataRead("savesettings.txt", "savesettings.txt");
	if(f == nil)
		return;

	while(fgets(line, sizeof(line), f)){
		if(!splitSettingLine(line, key, sizeof(key), &value))
			continue;
		if(strcmp(key, "pawn_save_path") == 0){
			parseQuotedStringValue(value, gPawnSavePath, sizeof(gPawnSavePath));
		}else if(strcmp(key, "recent_map") == 0){
			if(gNumRecentMaps < MAX_RECENT_MAPS &&
			   parseQuotedStringValue(value, gRecentMaps[gNumRecentMaps], sizeof(gRecentMaps[0])))
				gNumRecentMaps++;
		}else if(strcmp(key, "recent_textdraws") == 0){
			char path[1024];
			if(parseQuotedStringValue(value, path, sizeof(path)))
				recentTextDraws.push_back(path);
		}else if(LoadTextDrawSetting(key, value) || KeycastLoadSetting(key, value) ||
		         RadarLoadSetting(key, value) || DiscordPresenceLoadSetting(key, value)){
		}else if(strcmp(key, "shortcut") == 0){
			ShortcutParseSetting(value);
		}else if(strcmp(key, "language") == 0){
			// A code like pt-BR: trimmed, no quotes.
			char language[64];
			snprintf(language, sizeof(language), "%s", value + strspn(value, " \t"));
			language[strcspn(language, " \t\r\n")] = '\0';
			I18nSetLanguage(language);
		}else if(strcmp(key, "ui_scale") == 0){
			if(parseFloatSetting(value, &gUiScale)){
				gUiScale = clamp(gUiScale, 0.6f, 2.5f);
				ImGui::GetIO().FontGlobalScale = gUiScale;
			}
		}else if(strcmp(key, "ui_theme") == 0){
			if(parseIntSetting(value, &intValue))
				SetUiTheme(intValue);
		}else if(strcmp(key, "outline_selection") == 0){
			if(parseBoolSetting(value, &boolValue)) gOutlineSelection = boolValue;
		}else if(strcmp(key, "outline_map_objects") == 0){
			if(parseBoolSetting(value, &boolValue)) gOutlineMapObjects = boolValue;
		}else if(strcmp(key, "panel_snap") == 0){
			if(parseBoolSetting(value, &boolValue)) gPanelSnap = boolValue;
		}else if(strcmp(key, "show_code_hint") == 0){
			if(parseBoolSetting(value, &boolValue)) gShowCodeHint = boolValue;
		}else if(strcmp(key, "show_textdraws_hint") == 0){
			if(parseBoolSetting(value, &boolValue)) gShowTextDrawsHint = boolValue;
		}else if(strcmp(key, "show_start_guide") == 0){
			if(parseBoolSetting(value, &boolValue)) gShowStartGuide = boolValue;
		}else if(strcmp(key, "reopen_last_map") == 0){
			if(parseBoolSetting(value, &boolValue)) gReopenLastMap = boolValue;
		}else if(strcmp(key, "pawn_save_format") == 0){
			if(parseIntSetting(value, &intValue) &&
			   (intValue == PAWN_CODE_NATIVE || intValue == PAWN_CODE_STREAMER))
				gPawnSaveFormat = intValue;
		}else if(strcmp(key, "settings_version") == 0){
			parseIntSetting(value, &loadedSettingsVersion);

		}else if(strcmp(key, "save_destination") == 0){
			if(parseIntSetting(value, &intValue) && intValue == SAVE_DESTINATION_MODLOADER)
				gSaveDestination = SAVE_DESTINATION_MODLOADER;
			else
				gSaveDestination = SAVE_DESTINATION_ORIGINAL_FILES;
		}else if(strcmp(key, "confirm_reload_game") == 0){
			if(parseBoolSetting(value, &boolValue))
				gConfirmReloadGame = boolValue;
		}else if(strcmp(key, "autosave") == 0){
			if(parseBoolSetting(value, &boolValue)) gAutosaveEnabled = boolValue;
		}else if(strcmp(key, "autosave_interval") == 0){
			parseIntSetting(value, &gAutosaveIntervalSeconds);
		}else if(strcmp(key, "autosave_keep") == 0){
			parseIntSetting(value, &gAutosaveKeepCount);
		}else if(strcmp(key, "custom_import_start_id") == 0){
			parseIntSetting(value, &gCustomImportPreferredStartId);
		}else if(strcmp(key, "show_editor_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showEditorWindow = boolValue;
		}else if(strcmp(key, "show_instance_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showInstanceWindow = boolValue;
		}else if(strcmp(key, "show_log_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showLogWindow = boolValue;
			}else if(strcmp(key, "show_help_window") == 0){
				if(parseBoolSetting(value, &boolValue)) showHelpWindow = boolValue;
			}else if(strcmp(key, "show_shortcuts_window") == 0){
				if(parseBoolSetting(value, &boolValue)) showShortcutsWindow = boolValue;
			}else if(strcmp(key, "show_time_weather_window") == 0){
				if(parseBoolSetting(value, &boolValue)) showTimeWeatherWindow = boolValue;
		}else if(strcmp(key, "show_view_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showViewWindow = boolValue;
		}else if(strcmp(key, "show_rendering_window") == 0){
			{ if(parseBoolSetting(value, &boolValue) && boolValue) showViewWindow = true; }
		}else if(strcmp(key, "show_browser_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showBrowserWindow = boolValue;
		}else if(strcmp(key, "show_diff_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showDiffWindow = boolValue;
		}else if(strcmp(key, "show_map_objects_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showMapObjectsWindow = boolValue;
		}else if(strcmp(key, "show_history_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showHistoryWindow = boolValue;
		}else if(strcmp(key, "show_tools_window") == 0){
			if(parseBoolSetting(value, &boolValue)) showToolsWindow = boolValue;
		}else if(strcmp(key, "toast_enabled") == 0){
			if(parseBoolSetting(value, &boolValue)) toastEnabled = boolValue;
		}else if(strncmp(key, "toast_category_", 15) == 0){
			int idx = atoi(key + 15);
			if(idx >= 0 && idx < TOAST_NUM_CATEGORIES && parseBoolSetting(value, &boolValue))
				toastCategoryEnabled[idx] = boolValue;
		}else if(strcmp(key, "time_hour") == 0){
			parseIntSetting(value, &currentHour);
		}else if(strcmp(key, "time_minute") == 0){
			parseIntSetting(value, &currentMinute);
		}else if(strcmp(key, "current_area") == 0){
			parseIntSetting(value, &currentArea);
		}else if(strcmp(key, "weather_old") == 0){
			parseIntSetting(value, &Weather::oldWeather);
		}else if(strcmp(key, "weather_new") == 0){
			parseIntSetting(value, &Weather::newWeather);
		}else if(strcmp(key, "weather_interpolation") == 0){
			parseFloatSetting(value, &Weather::interpolation);
		}else if(strcmp(key, "extra_colours") == 0){
			parseIntSetting(value, &extraColours);
		}else if(strcmp(key, "day_night_balance") == 0){
			parseFloatSetting(value, &gDayNightBalance);
		}else if(strcmp(key, "wet_road_effect") == 0){
			parseFloatSetting(value, &gWetRoadEffect);
		}else if(strcmp(key, "neo_light_map_strength") == 0){
			parseFloatSetting(value, &gNeoLightMapStrength);
		}else if(strcmp(key, "camera_position") == 0){
			if(parseVec3Setting(value, &vecValue))
				TheCamera.m_position = vecValue;
		}else if(strcmp(key, "camera_target") == 0){
			if(parseVec3Setting(value, &vecValue))
				TheCamera.m_target = vecValue;
		}else if(strcmp(key, "camera_fov") == 0){
			parseFloatSetting(value, &TheCamera.m_fov);
		}else if(strcmp(key, "draw_target") == 0){
			if(parseBoolSetting(value, &boolValue)) gDrawTarget = boolValue;
		}else if(strcmp(key, "selection_highlight_opacity") == 0){
			parseIntSetting(value, &gSelectionHighlightOpacity);
		}

		// Keep the settings dispatch in separate chains. A single very long
		// else-if chain exceeds MSVC's nested-block limit as settings are added.
		if(strcmp(key, "render_collision") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderCollision = boolValue;
		}else if(strcmp(key, "render_bounds") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderBounds = boolValue;
		}else if(strcmp(key, "render_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderZones = boolValue;
		}else if(strcmp(key, "render_samp_elements") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSampElements = boolValue;
		}else if(strcmp(key, "render_map_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderMapZones = boolValue;
		}else if(strcmp(key, "render_navig_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderNavigZones = boolValue;
		}else if(strcmp(key, "render_info_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderInfoZones = boolValue;
		}else if(strcmp(key, "render_cull_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderCullZones = boolValue;
		}else if(strcmp(key, "render_attrib_zones") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderAttribZones = boolValue;
		}else if(strcmp(key, "render_particle_effects") == 0){
			if(parseBoolSetting(value, &boolValue)) ParticlePreview::enabled = boolValue;
		}else if(strcmp(key, "render_light_effects") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderLightEffects = boolValue;
		}else if(strcmp(key, "render_effect_markers") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderEffects = boolValue;
		}else if(strcmp(key, "render_legacy_ped_paths") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderLegacyPedPaths = boolValue;
		}else if(strcmp(key, "render_legacy_car_paths") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderLegacyCarPaths = boolValue;
		}else if(strcmp(key, "render_sa_ped_paths") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaPedPaths = boolValue;
		}else if(strcmp(key, "render_sa_ped_path_walkers") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaPedPathWalkers = boolValue;
		}else if(strcmp(key, "render_sa_car_paths") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaCarPaths = boolValue;
		}else if(strcmp(key, "render_sa_car_path_traffic") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaCarPathTraffic = boolValue;
		}else if(strcmp(key, "render_sa_area_grid") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaAreaGrid = boolValue;
		}else if(strcmp(key, "sa_ped_path_walker_count") == 0){
			parseIntSetting(value, &gSaPedPathWalkerCount);
		}else if(strcmp(key, "sa_car_path_traffic_count") == 0){
			parseIntSetting(value, &gSaCarPathTrafficCount);
		}else if(strcmp(key, "sa_car_path_traffic_speed_scale") == 0){
			parseFloatSetting(value, &gSaCarPathTrafficSpeedScale);
		}else if(strcmp(key, "sa_car_path_traffic_freeze_routes") == 0){
			if(parseBoolSetting(value, &boolValue)) gSaCarPathTrafficFreezeRoutes = boolValue;
		}else if(strcmp(key, "render_sa_car_path_parked_cars") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderSaCarPathParkedCars = boolValue;
		}else if(strcmp(key, "sa_car_path_parked_car_count") == 0){
			parseIntSetting(value, &gSaCarPathParkedCarCount);
		}else if(strcmp(key, "render_water") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderWater = boolValue;
		}else if(strcmp(key, "play_animations") == 0){
			if(parseBoolSetting(value, &boolValue)) gPlayAnimations = boolValue;
		}else if(strcmp(key, "render_mode") == 0){
			parseIntSetting(value, &gRenderMode);
		}else if(strcmp(key, "draw_distance") == 0){
			parseFloatSetting(value, &TheCamera.m_LODmult);
		}else if(strcmp(key, "fly_acceleration") == 0){
			if(parseBoolSetting(value, &boolValue)) gFlyAcceleration = boolValue;
		}else if(strcmp(key, "fly_speed") == 0){
			parseFloatSetting(value, &gFlySpeed);
		}else if(strcmp(key, "fly_fast_mul") == 0){
			parseFloatSetting(value, &gFlyFastMul);
		}else if(UpdaterParseSetting(key, value)){
		}else if(strcmp(key, "fly_slow_mul") == 0){
			parseFloatSetting(value, &gFlySlowMul);
		}else if(strcmp(key, "fov_wheel_step") == 0){
			parseFloatSetting(value, &gFovWheelStep);
		}else if(strcmp(key, "camera_look_button") == 0){
			parseIntSetting(value, &gCameraLookButton);
		}else if(strcmp(key, "render_all_timed_objects") == 0){
			if(parseBoolSetting(value, &boolValue)) gNoTimeCull = boolValue;
		}else if(strcmp(key, "render_all_areas") == 0){
			if(parseBoolSetting(value, &boolValue)) gNoAreaCull = boolValue;
		}else if(strcmp(key, "render_postfx") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderPostFX = boolValue;
		}else if(strcmp(key, "aa_samples") == 0){
			if(parseIntSetting(value, &intValue))
				gRequestedAASamples = sanitizeAASamples((uint32)max(intValue, 1));
		}else if(strcmp(key, "renderer") == 0 || strcmp(key, "renderer_gl_failed") == 0){
			parseRendererSetting(key, value);
		}else if(strcmp(key, "use_blur_ambient") == 0){
			if(parseBoolSetting(value, &boolValue)) gUseBlurAmb = boolValue;
		}else if(strcmp(key, "override_blur_ambient") == 0){
			if(parseBoolSetting(value, &boolValue)) gOverrideBlurAmb = boolValue;
		}else if(strcmp(key, "colour_filter") == 0){
			parseIntSetting(value, &gColourFilter);
		}else if(strcmp(key, "radiosity") == 0){
			if(parseBoolSetting(value, &boolValue)) gRadiosity = boolValue;
		}else if(strcmp(key, "building_pipe") == 0){
			parseIntSetting(value, &gBuildingPipeSwitch);
		}else if(strcmp(key, "backface_culling") == 0){
			if(parseBoolSetting(value, &boolValue)) gDoBackfaceCulling = boolValue;
		}else if(strcmp(key, "ps2_alpha_test") == 0){
			if(parseBoolSetting(value, &boolValue)) params.ps2AlphaTest = boolValue;
		}else if(strcmp(key, "alpha_ref") == 0){
			parseIntSetting(value, &params.alphaRef);
		}else if(strcmp(key, "render_background") == 0){
			if(parseBoolSetting(value, &boolValue)) gRenderBackground = boolValue;
		}else if(strcmp(key, "enable_fog") == 0){
			// Before the fog modes: off stays off.
			if(parseBoolSetting(value, &boolValue) && !boolValue) gFogMode = FOG_OFF;
		}else if(strcmp(key, "fog_mode") == 0){
			parseIntSetting(value, &gFogMode);
		}else if(strcmp(key, "stream_distance") == 0){
			parseFloatSetting(value, &gStreamDistance);
		}else if(strcmp(key, "vsync") == 0){
			if(parseBoolSetting(value, &boolValue)) gVSync = boolValue;
		}else if(strcmp(key, "frame_limit") == 0){
			parseIntSetting(value, &gFrameLimit);
		}else if(strcmp(key, "focus_my_map") == 0){
			if(parseBoolSetting(value, &boolValue)) gFocusMyMap = boolValue;
		}else if(strcmp(key, "enable_timecycle_boxes") == 0){
			if(parseBoolSetting(value, &boolValue)) gEnableTimecycleBoxes = boolValue;
		}else if(strcmp(key, "gizmo_enabled") == 0){
			if(parseBoolSetting(value, &boolValue)) gGizmoEnabled = boolValue;
		}else if(strcmp(key, "gizmo_mode") == 0){
			parseIntSetting(value, &gGizmoMode);
		}else if(strcmp(key, "gizmo_space") == 0){
			parseIntSetting(value, &gGizmoSpace);
		}else if(strcmp(key, "gizmo_pivot") == 0){
			parseIntSetting(value, &gGizmoPivot);
			gGizmoPivot=clamp(gGizmoPivot,0,3);
		}else if(strcmp(key, "gizmo_custom_pivot") == 0){
			if(parseVec3Setting(value,&vecValue))gGizmoCustomPivot=vecValue;
		}else if(strcmp(key, "gizmo_snap") == 0){
			if(parseBoolSetting(value, &boolValue)) gGizmoSnap = boolValue;
		}else if(strcmp(key, "gizmo_snap_angle") == 0){
			parseFloatSetting(value, &gGizmoSnapAngle);
		}else if(strcmp(key, "gizmo_snap_translate") == 0){
			parseFloatSetting(value, &gGizmoSnapTranslate);
		}else if(strcmp(key, "object_snap_mode") == 0){
			if(parseBoolSetting(value, &boolValue)) gObjectSnapMode = boolValue;
		}else if(strcmp(key, "object_snap_distance") == 0){
			parseFloatSetting(value, &gObjectSnapDistance);
		}else if(strcmp(key, "place_snap_to_objects") == 0){
			if(parseBoolSetting(value, &boolValue)) gPlaceSnapToObjects = boolValue;
		}else if(strcmp(key, "place_snap_to_ground") == 0){
			if(parseBoolSetting(value, &boolValue)) gPlaceSnapToGround = boolValue;
		}else if(strcmp(key, "drag_follow_ground") == 0){
			if(parseBoolSetting(value, &boolValue)) gDragFollowGround = boolValue;
		}else if(strcmp(key, "drag_align_to_surface") == 0){
			if(parseBoolSetting(value, &boolValue)) gDragAlignToSurface = boolValue;
		}else if(strcmp(key, "brush_z_offset") == 0){
			parseFloatSetting(value, &gBrushZOffset);
		}else if(strcmp(key, "brush_align_to_surface") == 0){
			if(parseBoolSetting(value, &boolValue)) gBrushAlignToSurface = boolValue;
		}else if(strcmp(key, "brush_random_yaw") == 0){
			// Legacy setting (pre-range): map to 0/360 range when enabled.
			if(parseBoolSetting(value, &boolValue) && boolValue){
				gBrushYawMin = 0.0f;
				gBrushYawMax = 360.0f;
			}
		}else if(strcmp(key, "brush_yaw_min") == 0){
			parseFloatSetting(value, &gBrushYawMin);
		}else if(strcmp(key, "brush_yaw_max") == 0){
			parseFloatSetting(value, &gBrushYawMax);
		}else if(strcmp(key, "brush_spacing") == 0){
			parseFloatSetting(value, &gBrushSpacing);
		}else if(strcmp(key, "brush_radius") == 0){
			parseFloatSetting(value, &gBrushRadius);
		}else if(strcmp(key, "brush_count") == 0){
			parseIntSetting(value, &gBrushCount);
		}else if(strcmp(key, "brush_delay_ms") == 0){
			parseFloatSetting(value, &gBrushDelayMs);
		}else if(strcmp(key, "editor_camera_name") == 0){
			parseQuotedStringValue(value, gEditorCameraName, sizeof(gEditorCameraName));
		}else if(strcmp(key, "editor_model_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gEditorModelFilter, buf);
		}else if(strcmp(key, "editor_txd_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gEditorTxdFilter, buf);
		}else if(strcmp(key, "editor_highlight_matches") == 0){
			if(parseBoolSetting(value, &boolValue)) gEditorHighlightMatches = boolValue;
		}else if(strcmp(key, "browser_selected_category") == 0){
			parseIntSetting(value, &gBrowserSelectedCategory);
		}else if(strcmp(key, "browser_selected_ide") == 0){
			parseQuotedStringValue(value, gBrowserSelectedIde, sizeof(gBrowserSelectedIde));
		}else if(strcmp(key, "browser_thumbnail_view") == 0){
			if(parseBoolSetting(value, &boolValue)) gBrowserThumbnailView = boolValue;
		}else if(strcmp(key, "browser_tile_size") == 0){
			parseFloatSetting(value, &gBrowserTileSize);
			gBrowserTileSize = clamp(gBrowserTileSize, 4.0f, 14.0f);
		}else if(strcmp(key, "browser_element_feature") == 0){
			parseIntSetting(value, &gBrowserElementFeature);
		}else if(strcmp(key, "browser_element_kind") == 0){
			parseIntSetting(value, &gBrowserElementKind);
		}else if(strcmp(key, "browser_kind") == 0){
			parseIntSetting(value, &gBrowserKind);
		}else if(strcmp(key, "browser_active_tab") == 0){
			parseIntSetting(value, &gBrowserActiveTab);
		}else if(strcmp(key, "browser_category_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gBrowserCategoryFilter, buf);
		}else if(strcmp(key, "browser_ide_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gBrowserIdeFilter, buf);
		}else if(strcmp(key, "browser_filter_preset") == 0){
			char buf[1024];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				AddObjectFilterPresetLine(buf);
		}else if(strcmp(key, "browser_search_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gBrowserSearchFilter, buf);
		}else if(strcmp(key, "browser_favourites_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gBrowserFavFilter, buf);
		}else if(strcmp(key, "browser_prefab_filter") == 0){
			char buf[256];
			if(parseQuotedStringValue(value, buf, sizeof(buf)))
				setTextFilterValue(gBrowserPrefabFilter, buf);
		}else if(strcmp(key, "browser_selected_prefab") == 0){
			parseIntSetting(value, &gBrowserSelectedPrefab);
		}else if(strcmp(key, "browser_selected_object") == 0){
			parseIntSetting(value, &savedSpawnObjectId);
		}else if(strcmp(key, "diff_filter") == 0){
			parseIntSetting(value, &gDiffFilter);
		}
	}
	// Saved newest first; each one added goes on top.
	for(size_t i = recentTextDraws.size(); i-- > 0;)
		AddRecentTextDrawFile(recentTextDraws[i].c_str());

	// Version 1 changed the camera target marker to opt-in. Existing settings
	// files stored the old enabled default, so migrate every user once.
	if(loadedSettingsVersion < 1)
		gDrawTarget = false;
	// Version 2 gives every existing user the softer selection highlight once.
	// Later saves retain whatever opacity the user chooses.
	if(loadedSettingsVersion < 2)
		gSelectionHighlightOpacity = 25;
	// Version 3 opens the Object Browser on recently placed objects instead
	// of the whole catalogue, which starts with vehicles.
	if(loadedSettingsVersion < 3 && gBrowserSelectedCategory == BROWSER_CATEGORY_ALL)
		gBrowserSelectedCategory = BROWSER_CATEGORY_RECENT;

	// Adopt the redesigned workspace once, then retain user docking changes.
	if(loadedSettingsVersion < 5){
		gResetPanelLayout = true;
		showBrowserWindow = showInstanceWindow = true;
		showToolsWindow = showMapObjectsWindow = false;
		gBrowserActiveTab = BROWSER_TAB_CATEGORIES;
		gBrowserSelectedCategory = BROWSER_CATEGORY_ALL;
		savedSpawnObjectId = -1;
		gBrowserThumbnailView = true;
		gBrowserTileSize = 7.0f;
	}
	if(loadedSettingsVersion < 8) gFogMode = FOG_LIGHT;
	// The editor's new arrangement (map objects left, details right, browser
	// below the 3D view): adopt it once.
	if(loadedSettingsVersion < 9){
		gResetPanelLayout = true;
		showBrowserWindow = showInstanceWindow = showMapObjectsWindow = true;
	}
	if(loadedSettingsVersion < 7){
		gStreamDistance = 1000.0f;
		gFogMode = FOG_LIGHT;
	}


	// Haumea writes maps as Pawn source. Mod Loader is no longer used; the
	// setting only keeps old code paths away from the game's own files.
	gSaveDestination = SAVE_DESTINATION_MODLOADER;
	sanitizeAutosaveSettings();
	sanitizeCustomImportSettings();
	normalizePersistentSettings();
	gRequestedAASamples = sanitizeAASamples(gRequestedAASamples,
		rw::Engine::getMaxMultiSamplingLevels());
	sk::requestedMultiSamplingLevels = gRequestedAASamples;

	if(savedSpawnObjectId >= 0 && savedSpawnObjectId < NUMOBJECTDEFS && GetObjectDef(savedSpawnObjectId))
		SetSpawnObjectId(savedSpawnObjectId);
	fclose(f);
	// Older versions kept a single save path and no recent list.
	if(gNumRecentMaps == 0 && gPawnSavePath[0] != '\0'){
		FILE *existing = fopen(gPawnSavePath, "rb");
		if(existing){
			fclose(existing);
			addRecentMap(gPawnSavePath);
		}
	}
}

static void
saveCamSettings(void)
{
	FILE *f;

	f = fopenHaumeaDataWrite("camsettings.txt");
	if(f == nil)
		return;

	for(int i = 0; i < camSettings.size(); i++){
		CamSetting *cam = &camSettings[i];
		fprintf(f, "\"%s\" %f %f %f  %f %f %f  %f  %d %d %d %d  %d\n",
			cam->name,
			cam->pos.x, cam->pos.y, cam->pos.z,
			cam->target.x, cam->target.y, cam->target.z,
			cam->fov,
			cam->hour, cam->minute, cam->weather1, cam->weather2,
			cam->area);
	}

	fclose(f);
}

static void
saveSaveSettings(void)
{
	FILE *f;

	sanitizeAutosaveSettings();
	sanitizeCustomImportSettings();
	sampleEditorWindowState();
	normalizePersistentSettings();
	f = fopenHaumeaDataWrite("savesettings.txt");
	if(f == nil)
		return;

	writeQuotedSetting(f, "pawn_save_path", gPawnSavePath);
	for(int i = 0; i < gNumRecentMaps; i++)
		writeQuotedSetting(f, "recent_map", gRecentMaps[i]);
	for(size_t i = 0; i < GetRecentTextDrawFiles().size(); i++)
		writeQuotedSetting(f, "recent_textdraws", GetRecentTextDrawFiles()[i].c_str());
	SaveTextDrawSettings(f);
	KeycastSaveSettings(f);
	RadarSaveSettings(f);
	fprintf(f, "reopen_last_map %d\n", gReopenLastMap ? 1 : 0);
	fprintf(f, "show_start_guide %d\n", gShowStartGuide ? 1 : 0);
	fprintf(f, "show_textdraws_hint %d\n", gShowTextDrawsHint ? 1 : 0);
	fprintf(f, "show_code_hint %d\n", gShowCodeHint ? 1 : 0);
	fprintf(f, "panel_snap %d\n", gPanelSnap ? 1 : 0);
	fprintf(f, "outline_map_objects %d\n", gOutlineMapObjects ? 1 : 0);
	fprintf(f, "outline_selection %d\n", gOutlineSelection ? 1 : 0);
	fprintf(f, "ui_scale %.3g\n", gUiScale);
	fprintf(f, "ui_theme %d\n", GetUiTheme());
	fprintf(f, "language %s\n", I18nGetLanguage());
	ShortcutWriteSettings(f);
	fprintf(f, "pawn_save_format %d\n", gPawnSaveFormat);
	fprintf(f, "settings_version %d\n", SAVE_SETTINGS_VERSION);

	fprintf(f, "window_width %d\n", gSavedWindowWidth);
	fprintf(f, "window_height %d\n", gSavedWindowHeight);
	if(gSavedWindowPlacementValid){
		fprintf(f, "window_x %d\n", gSavedWindowX);
		fprintf(f, "window_y %d\n", gSavedWindowY);
	}
	fprintf(f, "window_maximized %d\n", gSavedWindowMaximized ? 1 : 0);
	fprintf(f, "save_destination %d\n", (int)gSaveDestination);
	fprintf(f, "confirm_reload_game %d\n", gConfirmReloadGame ? 1 : 0);
	DiscordPresenceSaveSettings(f);
	fprintf(f, "autosave %d\n", gAutosaveEnabled ? 1 : 0);
	fprintf(f, "autosave_interval %d\n", gAutosaveIntervalSeconds);
	fprintf(f, "autosave_keep %d\n", gAutosaveKeepCount);
	fprintf(f, "custom_import_start_id %d\n", gCustomImportPreferredStartId);
	fprintf(f, "show_editor_window %d\n", showEditorWindow ? 1 : 0);
	fprintf(f, "show_instance_window %d\n", showInstanceWindow ? 1 : 0);
	fprintf(f, "show_log_window %d\n", showLogWindow ? 1 : 0);
	fprintf(f, "show_help_window %d\n", showHelpWindow ? 1 : 0);
	fprintf(f, "show_shortcuts_window %d\n", showShortcutsWindow ? 1 : 0);
	fprintf(f, "show_time_weather_window %d\n", showTimeWeatherWindow ? 1 : 0);
	fprintf(f, "show_view_window %d\n", showViewWindow ? 1 : 0);
	fprintf(f, "show_browser_window %d\n", showBrowserWindow ? 1 : 0);
	fprintf(f, "show_diff_window %d\n", showDiffWindow ? 1 : 0);
	fprintf(f, "show_map_objects_window %d\n", showMapObjectsWindow ? 1 : 0);
	fprintf(f, "show_history_window %d\n", showHistoryWindow ? 1 : 0);
	fprintf(f, "show_tools_window %d\n", showToolsWindow ? 1 : 0);
	fprintf(f, "toast_enabled %d\n", toastEnabled ? 1 : 0);
	for(int i = 0; i < TOAST_NUM_CATEGORIES; i++)
		fprintf(f, "toast_category_%d %d\n", i, toastCategoryEnabled[i] ? 1 : 0);
	fprintf(f, "time_hour %d\n", currentHour);
	fprintf(f, "time_minute %d\n", currentMinute);
	fprintf(f, "current_area %d\n", currentArea);
	fprintf(f, "weather_old %d\n", Weather::oldWeather);
	fprintf(f, "weather_new %d\n", Weather::newWeather);
	fprintf(f, "weather_interpolation %.9g\n", Weather::interpolation);
	fprintf(f, "extra_colours %d\n", extraColours);
	fprintf(f, "day_night_balance %.9g\n", gDayNightBalance);
	fprintf(f, "wet_road_effect %.9g\n", gWetRoadEffect);
	fprintf(f, "neo_light_map_strength %.9g\n", gNeoLightMapStrength);
	fprintf(f, "camera_position %.9g %.9g %.9g\n", TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z);
	fprintf(f, "camera_target %.9g %.9g %.9g\n", TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z);
	fprintf(f, "camera_fov %.9g\n", TheCamera.m_fov);
	fprintf(f, "draw_target %d\n", gDrawTarget ? 1 : 0);
	fprintf(f, "selection_highlight_opacity %d\n", gSelectionHighlightOpacity);
	fprintf(f, "render_collision %d\n", gRenderCollision ? 1 : 0);
	fprintf(f, "render_bounds %d\n", gRenderBounds ? 1 : 0);
	fprintf(f, "render_zones %d\n", gRenderZones ? 1 : 0);
	fprintf(f, "render_samp_elements %d\n", gRenderSampElements ? 1 : 0);
	fprintf(f, "render_map_zones %d\n", gRenderMapZones ? 1 : 0);
	fprintf(f, "render_navig_zones %d\n", gRenderNavigZones ? 1 : 0);
	fprintf(f, "render_info_zones %d\n", gRenderInfoZones ? 1 : 0);
	fprintf(f, "render_cull_zones %d\n", gRenderCullZones ? 1 : 0);
	fprintf(f, "render_attrib_zones %d\n", gRenderAttribZones ? 1 : 0);
	fprintf(f, "render_particle_effects %d\n", ParticlePreview::enabled ? 1 : 0);
	fprintf(f, "render_light_effects %d\n", gRenderLightEffects ? 1 : 0);
	fprintf(f, "render_effect_markers %d\n", gRenderEffects ? 1 : 0);
	fprintf(f, "render_legacy_ped_paths %d\n", gRenderLegacyPedPaths ? 1 : 0);
	fprintf(f, "render_legacy_car_paths %d\n", gRenderLegacyCarPaths ? 1 : 0);
	fprintf(f, "render_sa_ped_paths %d\n", gRenderSaPedPaths ? 1 : 0);
	fprintf(f, "render_sa_ped_path_walkers %d\n", gRenderSaPedPathWalkers ? 1 : 0);
	fprintf(f, "render_sa_car_paths %d\n", gRenderSaCarPaths ? 1 : 0);
	fprintf(f, "render_sa_car_path_traffic %d\n", gRenderSaCarPathTraffic ? 1 : 0);
	fprintf(f, "render_sa_area_grid %d\n", gRenderSaAreaGrid ? 1 : 0);
	fprintf(f, "sa_ped_path_walker_count %d\n", gSaPedPathWalkerCount);
	fprintf(f, "sa_car_path_traffic_count %d\n", gSaCarPathTrafficCount);
	fprintf(f, "sa_car_path_traffic_speed_scale %.9g\n", gSaCarPathTrafficSpeedScale);
	fprintf(f, "sa_car_path_traffic_freeze_routes %d\n", gSaCarPathTrafficFreezeRoutes ? 1 : 0);
	fprintf(f, "render_sa_car_path_parked_cars %d\n", gRenderSaCarPathParkedCars ? 1 : 0);
	fprintf(f, "sa_car_path_parked_car_count %d\n", gSaCarPathParkedCarCount);
	fprintf(f, "render_water %d\n", gRenderWater ? 1 : 0);
	fprintf(f, "play_animations %d\n", gPlayAnimations ? 1 : 0);
	fprintf(f, "render_mode %d\n", gRenderMode);
	fprintf(f, "draw_distance %.9g\n", TheCamera.m_LODmult);
	fprintf(f, "fly_acceleration %d\n", gFlyAcceleration ? 1 : 0);
	fprintf(f, "fly_speed %.9g\n", gFlySpeed);
	fprintf(f, "fly_fast_mul %.9g\n", gFlyFastMul);
	fprintf(f, "fly_slow_mul %.9g\n", gFlySlowMul);
	UpdaterWriteSettings(f);
	fprintf(f, "fov_wheel_step %.9g\n", gFovWheelStep);
	fprintf(f, "camera_look_button %d\n", gCameraLookButton);
	fprintf(f, "render_all_timed_objects %d\n", gNoTimeCull ? 1 : 0);
	fprintf(f, "render_all_areas %d\n", gNoAreaCull ? 1 : 0);
	fprintf(f, "render_postfx %d\n", gRenderPostFX ? 1 : 0);
	fprintf(f, "aa_samples %u\n", (unsigned)gRequestedAASamples);
#ifdef _WIN32
	fprintf(f, "renderer %s\n", rendererNames[gRendererChoice]);
	fprintf(f, "renderer_gl_failed %d\n", gRendererGlFailed ? 1 : 0);
#endif
	fprintf(f, "use_blur_ambient %d\n", gUseBlurAmb ? 1 : 0);
	fprintf(f, "override_blur_ambient %d\n", gOverrideBlurAmb ? 1 : 0);
	fprintf(f, "colour_filter %d\n", gColourFilter);
	fprintf(f, "radiosity %d\n", gRadiosity ? 1 : 0);
	fprintf(f, "building_pipe %d\n", gBuildingPipeSwitch);
	fprintf(f, "backface_culling %d\n", gDoBackfaceCulling ? 1 : 0);
	fprintf(f, "ps2_alpha_test %d\n", params.ps2AlphaTest ? 1 : 0);
	fprintf(f, "alpha_ref %d\n", params.alphaRef);
	fprintf(f, "render_background %d\n", gRenderBackground ? 1 : 0);
	fprintf(f, "fog_mode %d\n", gFogMode);
	fprintf(f, "stream_distance %.9g\n", gStreamDistance);
	fprintf(f, "vsync %d\n", gVSync ? 1 : 0);
	fprintf(f, "frame_limit %d\n", gFrameLimit);
	fprintf(f, "focus_my_map %d\n", gFocusMyMap ? 1 : 0);
	fprintf(f, "enable_timecycle_boxes %d\n", gEnableTimecycleBoxes ? 1 : 0);
	fprintf(f, "gizmo_enabled %d\n", gGizmoEnabled ? 1 : 0);
	fprintf(f, "gizmo_mode %d\n", gGizmoMode);
	fprintf(f, "gizmo_space %d\n", gGizmoSpace);
	fprintf(f, "gizmo_pivot %d\n", gGizmoPivot);
	fprintf(f, "gizmo_custom_pivot %.9g %.9g %.9g\n",gGizmoCustomPivot.x,gGizmoCustomPivot.y,gGizmoCustomPivot.z);
	fprintf(f, "gizmo_snap %d\n", gGizmoSnap ? 1 : 0);
	fprintf(f, "gizmo_snap_angle %.9g\n", gGizmoSnapAngle);
	fprintf(f, "gizmo_snap_translate %.9g\n", gGizmoSnapTranslate);
	fprintf(f, "object_snap_mode %d\n", gObjectSnapMode ? 1 : 0);
	fprintf(f, "object_snap_distance %.9g\n", gObjectSnapDistance);
	fprintf(f, "place_snap_to_objects %d\n", gPlaceSnapToObjects ? 1 : 0);
	fprintf(f, "place_snap_to_ground %d\n", gPlaceSnapToGround ? 1 : 0);
	fprintf(f, "drag_follow_ground %d\n", gDragFollowGround ? 1 : 0);
	fprintf(f, "drag_align_to_surface %d\n", gDragAlignToSurface ? 1 : 0);
	fprintf(f, "brush_z_offset %.9g\n", gBrushZOffset);
	fprintf(f, "brush_align_to_surface %d\n", gBrushAlignToSurface ? 1 : 0);
	fprintf(f, "brush_yaw_min %.9g\n", gBrushYawMin);
	fprintf(f, "brush_yaw_max %.9g\n", gBrushYawMax);
	fprintf(f, "brush_spacing %.9g\n", gBrushSpacing);
	fprintf(f, "brush_radius %.9g\n", gBrushRadius);
	fprintf(f, "brush_count %d\n", gBrushCount);
	fprintf(f, "brush_delay_ms %.9g\n", gBrushDelayMs);
	writeQuotedSetting(f, "editor_camera_name", gEditorCameraName);
	writeQuotedSetting(f, "editor_model_filter", gEditorModelFilter.InputBuf);
	writeQuotedSetting(f, "editor_txd_filter", gEditorTxdFilter.InputBuf);
	fprintf(f, "editor_highlight_matches %d\n", gEditorHighlightMatches ? 1 : 0);
	fprintf(f, "browser_selected_category %d\n", gBrowserSelectedCategory);
	writeQuotedSetting(f, "browser_selected_ide", gBrowserSelectedIde);
	fprintf(f, "browser_thumbnail_view %d\n", gBrowserThumbnailView ? 1 : 0);
	fprintf(f, "browser_tile_size %.3g\n", gBrowserTileSize);
	fprintf(f, "browser_active_tab %d\n", gBrowserActiveTab);
	fprintf(f, "browser_kind %d\n", gBrowserKind);
	fprintf(f, "browser_element_feature %d\n", gBrowserElementFeature);
	fprintf(f, "browser_element_kind %d\n", gBrowserElementKind);
	writeQuotedSetting(f, "browser_category_filter", gBrowserCategoryFilter.InputBuf);
	writeQuotedSetting(f, "browser_ide_filter", gBrowserIdeFilter.InputBuf);
	writeQuotedSetting(f, "browser_search_filter", gBrowserSearchFilter.InputBuf);
	for(int i = 0; i < NumObjectFilterPresets(); i++)
		writeQuotedSetting(f, "browser_filter_preset", GetObjectFilterPresetLine(i).c_str());
	writeQuotedSetting(f, "browser_favourites_filter", gBrowserFavFilter.InputBuf);
	writeQuotedSetting(f, "browser_prefab_filter", gBrowserPrefabFilter.InputBuf);
	fprintf(f, "browser_selected_prefab %d\n", gBrowserSelectedPrefab);
	fprintf(f, "browser_selected_object %d\n", GetSpawnObjectId());
	fprintf(f, "diff_filter %d\n", gDiffFilter);
	fclose(f);
}

void
SaveEditorSettingsNow(void)
{
	if(!gPersistentSettingsLoaded)
		return;
	saveSaveSettings();
}

static void
getCurrentCamSetting(CamSetting *cam)
{
	for(char *p = cam->name; *p; p++)
		if(*p == '"') *p = ' ';
	cam->pos = TheCamera.m_position;
	cam->target = TheCamera.m_target;
	cam->fov = TheCamera.m_fov;
	cam->hour = currentHour;
	cam->minute = currentMinute;
	cam->weather1 = Weather::oldWeather;
	cam->weather2 = Weather::newWeather;
	cam->area = currentArea;
}

bool
UiCombo(const char *label, int *current, const char *const *items, int count)
{
	return ImGui::Combo(label, current,
		[](void *data, int i){ return _(((const char *const *)data)[i]); }, (void*)items, count);
}

static void
uiEditorWindow(void)
{
	static char buf[256];

	CPtrNode *p;
	ObjectInst *inst;
	ObjectDef *obj;
	TxdDef *txd;

	uiNextPanelSizeConstraints();
	ImGui::Begin(TI(ICON_FA_VIDEO, "Camera###Camera & Debug"), &showEditorWindow);
	uiFitItemsToPanel();
	ImGui::TextDisabled(_("Camera speed and controls are in Windows > Preferences."));

	if(ImGui::TreeNodeEx(L("Camera"), ImGuiTreeNodeFlags_DefaultOpen)){
		ImGui::InputFloat3(_("Position"), (float*)&TheCamera.m_position);
		ImGui::InputFloat3(_("Target"), (float*)&TheCamera.m_target);
		ImGui::SameLine();
		ImGui::Checkbox(_("show"), &gDrawTarget);
		ImGui::SliderFloat(_("FOV"), &TheCamera.m_fov, CAMERA_FOV_MIN, CAMERA_FOV_MAX, "%.0f");
		TheCamera.m_fov = ClampCameraFov(TheCamera.m_fov);
		ImGui::SameLine();
		if(ImGui::Button(_("Reset##fov")))
			TheCamera.m_fov = CAMERA_FOV_DEFAULT;
		ImGui::SetItemTooltip(_("Restore the default camera FOV (70°)."));
		char setCam[256];
		snprintf(setCam, sizeof(setCam), "SetPlayerCameraPos(playerid, %.4f, %.4f, %.4f);\n"
			"SetPlayerCameraLookAt(playerid, %.4f, %.4f, %.4f);",
			TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z,
			TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z);
		if(ImGui::Button(TI(ICON_FA_COPY, "Copy as Pawn")))
			SetEditorClipboardText(setCam);
		ImGui::SetItemTooltip("%s", setCam);

		ImGui::SeparatorText(_("Saved views"));
		ImGui::InputText(_("name"), gEditorCameraName, sizeof(gEditorCameraName));
		ImGui::SameLine();
		if(ImGui::Button(_("Save"))){
			CamSetting cam;
			strncpy(cam.name, gEditorCameraName, sizeof(cam.name));
			getCurrentCamSetting(&cam);
			camSettings.push_back(cam);
			saveCamSettings();
		}

		for(int i = 0; i < camSettings.size(); i++){
			CamSetting *cam = &camSettings[i];
			ImGui::PushID(i);
			sprintf(buf, "%-20s", cam->name);
			// Saved views have no undo, so Delete and Replace ask first.
			bool del = false;
			if(ImGui::Button(_("Delete")))
				ImGui::OpenPopup("##delete_view");
			if(ImGui::BeginPopup("##delete_view")){
				ImGui::Text(_("Delete the view \"%s\"?"), cam->name);
				if(ImGui::Button(_("Delete##confirm"))){
					del = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if(ImGui::Button(_("Cancel")))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			if(ImGui::Button(_("Replace")))
				ImGui::OpenPopup("##replace_view");
			ImGui::SetItemTooltip(_("Saves the current view over this one, named as in the field above."));
			if(ImGui::BeginPopup("##replace_view")){
				ImGui::Text(_("Replace \"%s\" with the current view?"), cam->name);
				if(ImGui::Button(_("Replace##confirm"))){
					strncpy(cam->name, gEditorCameraName, sizeof(cam->name));
					getCurrentCamSetting(cam);
					saveCamSettings();
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if(ImGui::Button(_("Cancel")))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			if(ImGui::Selectable(buf)){
				strncpy(gEditorCameraName, cam->name, sizeof(gEditorCameraName));
				TheCamera.m_position = cam->pos;
				TheCamera.m_target = cam->target;
				TheCamera.m_fov = cam->fov;
				currentHour = cam->hour;
				currentMinute = cam->minute;
				Weather::oldWeather = cam->weather1;
				Weather::newWeather = cam->weather2;
				if(params.numAreas)
					currentArea = cam->area;
			}
			ImGui::PopID();
			if(del){
				memmove(&camSettings[i], &camSettings[i+1], (camSettings.size()-i-1)*sizeof(CamSetting));
				camSettings.pop_back();
				saveCamSettings();
				i--;
			}
		}
		ImGui::TreePop();
	}

	if(ImGui::TreeNode(L("Find objects by model or TXD"))){
		gEditorModelFilter.Draw(_("Model (inc,-exc)")); ImGui::SameLine();
		if(ImGui::Button(_("Clear##Model")))
			gEditorModelFilter.Clear();
		gEditorTxdFilter.Draw(_("Txd (inc,-exc)")); ImGui::SameLine();
		if(ImGui::Button(_("Clear##Txd")))
			gEditorTxdFilter.Clear();
		ImGui::Checkbox(_("Highlight matches"), &gEditorHighlightMatches);

		uint32 instanceChangeSeq = GetLatestChangeSeq();
		if(gEditorCachedInstancesFirst != instances.first ||
		   gEditorCachedInstanceChangeSeq != instanceChangeSeq ||
		   gEditorCachedModelFilter != gEditorModelFilter.InputBuf ||
		   gEditorCachedTxdFilter != gEditorTxdFilter.InputBuf){
			gEditorFilteredInstances.clear();
			for(p = instances.first; p; p = p->next){
				inst = (ObjectInst*)p->item;
				if(!IsPawnObjectSelectable(inst))
					continue;
				obj = GetObjectDef(inst->m_objectId);
				txd = GetTxdDef(obj->m_txdSlot);
				if(gEditorModelFilter.PassFilter(obj->m_name) &&
				   gEditorTxdFilter.PassFilter(txd->name))
					gEditorFilteredInstances.push_back(inst);
			}
			gEditorCachedInstancesFirst = instances.first;
			gEditorCachedInstanceChangeSeq = instanceChangeSeq;
			gEditorCachedModelFilter = gEditorModelFilter.InputBuf;
			gEditorCachedTxdFilter = gEditorTxdFilter.InputBuf;
		}

		if(gEditorHighlightMatches)
			for(size_t i = 0; i < gEditorFilteredInstances.size(); i++)
				if(!gEditorFilteredInstances[i]->m_isDeleted)
					gEditorFilteredInstances[i]->m_highlight = HIGHLIGHT_FILTER;

		ImGuiListClipper clipper;
		clipper.Begin((int)gEditorFilteredInstances.size());
		while(clipper.Step()){
			for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
				inst = gEditorFilteredInstances[i];
				obj = GetObjectDef(inst->m_objectId);
				txd = GetTxdDef(obj->m_txdSlot);

				int numPops = 0;
				if(inst->m_isDeleted){
					ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(128, 128, 128));
					numPops++;
				}else if(inst->m_selected){
					ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(255, 0, 0));
					numPops++;
				}
				ImGui::PushID(inst);
				sprintf(buf, "%s%-20s %-20s %8.2f %8.2f %8.2f",
					inst->m_isDeleted ? "[X] " : "",
					obj->m_name, txd->name,
					inst->m_translation.x, inst->m_translation.y, inst->m_translation.z);
				ImGui::Selectable(buf);
				ImGui::PopID();
				if(ImGui::IsItemHovered()){
					if(ImGui::IsMouseClicked(1)){
						if(inst->m_isDeleted && IsPawnObjectEditable(inst))
							inst->Undelete();
						else
							inst->Select();
					}
					if(ImGui::IsMouseDoubleClicked(0))
						inst->JumpTo();
				}
				if(numPops)
					ImGui::PopStyleColor(numPops);
				if(!inst->m_isDeleted){
					if(ImGui::IsItemHovered())
						inst->m_highlight = HIGHLIGHT_HOVER;
				}
			}
		}
		ImGui::TreePop();
	}

	ImGui::End();
}

// First-run placement for the main panels, so they open side by side instead
// of stacked in a corner. Panels the user has moved keep their own layout.
enum PanelSlot { PANEL_LEFT_TOP, PANEL_LEFT_BOTTOM, PANEL_RIGHT_TOP, PANEL_RIGHT_BOTTOM };

// Space between panels, and between panels and the screen edges.
static float
uiPanelGap(void)
{
	return floorf(ImGui::GetFontSize() * 0.3f);
}

static float
uiStatusBarHeight(void)
{
	return floorf(ImGui::GetFontSize() + 11.0f);
}

// The part of the main window between the menu bar and the status bar;
// both are viewport side bars, so the work area already leaves them out.
static ImRect
uiPanelWorkArea(void)
{
	ImGuiViewport *viewport = ImGui::GetMainViewport();
	return ImRect(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x,
		viewport->WorkPos.y + viewport->WorkSize.y));
}

static void
uiDefaultPanelLayout(PanelSlot slot)
{
	ImRect area = uiPanelWorkArea();
	float gap = uiPanelGap();
	float top = area.Min.y + gap;
	float bottom = area.Max.y - gap;
	float width = clamp(area.GetWidth() * 0.24f, ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 30.0f);
	float split = top + (bottom - top) * 0.62f;
	bool left = slot == PANEL_LEFT_TOP || slot == PANEL_LEFT_BOTTOM;
	bool upper = slot == PANEL_LEFT_TOP || slot == PANEL_RIGHT_TOP;
	float x = left ? area.Min.x + gap : area.Max.x - width - gap;
	float y = upper ? top : split + gap;
	float height = upper ? split - top : bottom - split - gap;
#ifdef IMGUI_HAS_DOCK
	// Reset docks the panels instead (uiDockSpace); this is only where a
	// panel floats the first time.
	ImGuiCond cond = ImGuiCond_FirstUseEver;
#else
	ImGuiCond cond = gResetPanelLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
#endif
	ImGui::SetNextWindowPos(ImVec2(x, y), cond);
	ImGui::SetNextWindowSize(ImVec2(width, height), cond);
	if(gResetPanelLayout)
		ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
}

static void
uiResetPanelLayout(void)
{
	showBrowserWindow = true;
	showInstanceWindow = true;
	showMapObjectsWindow = true;
	showToolsWindow = false;
	gResetPanelLayout = true;
}

#ifdef IMGUI_HAS_DOCK
// Default arrangement, after Unreal Engine 5's editor: the map's objects in
// a column on the left, the selection's details and the tools on the right,
// and the object browser in a strip under the 3D view, which stays dominant.
static void
uiBuildDefaultDockLayout(ImGuiID dockspace)
{
	ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::DockBuilderRemoveNode(dockspace);
	ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
	float w = viewport->WorkSize.x;
	// Room for Object Info's fields; a wide screen gets a little more.
	float side = clamp(std::max(380.0f * GetUiScale(), w * 0.17f) / w, 0.16f, 0.34f);
	float outliner = clamp(std::max(260.0f * GetUiScale(), w * 0.13f) / w, 0.12f, 0.26f);
	ImGuiID center = dockspace;
	ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, side, nil, &center);
	ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, outliner / (1.0f - side), nil, &center);
	ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.32f, nil, &center);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_LAYER_GROUP, "Map Objects"), left);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_CIRCLE_INFO, "Object Info"), right);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_WRENCH, "Tools"), right);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_CLOCK_ROTATE_LEFT, "History"), right);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_MAGNIFYING_GLASS, "Object Browser"), bottom);
	ImGui::DockBuilderDockWindow(LI(ICON_FA_PAINTBRUSH, "Texture Browser"), bottom);
	ImGui::DockBuilderFinish(dockspace);
}

// Panels dock around the edges; the middle stays empty and lets the mouse
// through to the 3D view. The layout is kept in imgui.ini.
static void
uiDockSpace(void)
{
	ImGuiID map = ImHashStr("HaumeaDockSpace");
	ImGuiID textdraws = ImHashStr("HaumeaTextDrawDockSpace");
	bool editingTextDraws = InTextDrawMode();
	// A panel that is closed never asks for its label, which is what moves an
	// old layout's settings to its ### ID; History is new and often closed.
	LI(ICON_FA_CLOCK_ROTATE_LEFT, "History");
	if(!editingTextDraws && (gResetPanelLayout || ImGui::DockBuilderGetNode(map) == nil))
		uiBuildDefaultDockLayout(map);
	if(editingTextDraws && (gResetPanelLayout || ImGui::DockBuilderGetNode(textdraws) == nil)){
		ImGuiViewportP *viewport = (ImGuiViewportP*)ImGui::GetMainViewport();
		ImRect work = viewport->GetBuildWorkRect();
		ImGui::DockBuilderRemoveNode(textdraws);
		ImGui::DockBuilderAddNode(textdraws, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodePos(textdraws, work.Min);
		ImGui::DockBuilderSetNodeSize(textdraws, work.GetSize());
		ImGuiID center = textdraws;
		float side = clamp(350.0f * GetUiScale() / work.GetWidth(), 0.22f, 0.42f);
		ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, side, nil, &center);
		ImGui::DockBuilderDockWindow("###textdraws", right);
		ImGui::DockBuilderFinish(textdraws);
	}
	// The Terminal's first time open: a strip along the bottom of the 3D view,
	// split off only then, so the layout has no empty area without it.
	if(!editingTextDraws && showTerminalWindow && ImGui::FindWindowSettingsByID(ImHashStr("###terminal")) == nil &&
	   ImGui::FindWindowByName(">_ Terminal###terminal") == nil){
		if(ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(map)){
			ImGuiID centre = central->ID, bottom;
			ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.30f, &bottom, &centre);
			ImGui::DockBuilderDockWindow(">_ Terminal###terminal", bottom);
			ImGui::DockBuilderFinish(map);
		}
	}
	// Each tab has its own close button: no second one at the end of the strip.
	ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode |
		(ImGuiDockNodeFlags)ImGuiDockNodeFlags_NoCloseButton;
	ImGuiID active = editingTextDraws ? textdraws : map;
	ImGuiID hidden = editingTextDraws ? map : textdraws;
	if(ImGui::DockBuilderGetNode(hidden))
		ImGui::DockSpace(hidden, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
	ImGui::DockSpaceOverViewport(active, ImGui::GetMainViewport(), flags);
	uiViewportOverlay();
}

static void
uiDrawDockTabUnderlines(void)
{
	ImGuiContext &context = *ImGui::GetCurrentContext();
	float thickness = 2.0f * GetUiScale();
	for(ImGuiWindow *window : context.Windows){
		ImGuiDockNode *node = window->DockNode;
		if(!window->Active || !window->DockIsActive || !window->DockTabIsVisible || !node ||
		   !node->HostWindow || !node->TabBar || node->IsHiddenTabBar() || node->IsNoTabBar())
			continue;
		ImRect tab = window->DC.DockTabItemRect;
		if(tab.GetWidth() <= thickness || tab.GetHeight() <= thickness)
			continue;
		ImDrawList *draw = node->HostWindow->DrawList;
		draw->PushClipRect(tab.Min, tab.Max, false);
		// The open tab of the focused group gets an accent line along its top.
		bool focused = context.NavWindow && context.NavWindow->RootWindowDockTree == window->RootWindowDockTree;
		if(focused)
			draw->AddRectFilled(tab.Min, ImVec2(tab.Max.x, tab.Min.y + thickness), ImGui::GetColorU32(ImGuiCol_CheckMark));
		draw->PopClipRect();
	}
}

#endif

// Panels that can be snapped to: regular editor windows, not popups,
// tooltips, toasts or the fixed status bar.
static bool
uiIsSnappablePanel(ImGuiWindow *window)
{
	if(window == nil || !window->WasActive || window->Hidden)
		return false;
	if(window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup |
	   ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove))
		return false;
#ifdef IMGUI_HAS_VIEWPORT
	if(window->Viewport != (ImGuiViewportP*)ImGui::GetMainViewport())
		return false;
#endif
	return true;
}

// Closest snap target found so far on one axis.
struct PanelSnapAxis
{
	float best;
	float offset;
	float line;
};

static void
uiTrySnap(PanelSnapAxis *axis, float edge, float target)
{
	float d = target - edge;
	if(fabsf(d) < axis->best){
		axis->best = fabsf(d);
		axis->offset = d;
		axis->line = target;
	}
}

static float
uiPanelSnapThreshold(void)
{
	return ImGui::GetFontSize() * 0.9f;
}

enum { PANEL_EDGE_LEFT = 1, PANEL_EDGE_RIGHT = 2, PANEL_EDGE_TOP = 4, PANEL_EDGE_BOTTOM = 8 };

// Snap targets for one edge of panel rect r: the work area, and the edges of
// the other open panels beside it (just past them, or in line with them).
static void
uiSnapPanelEdge(PanelSnapAxis *axis, ImGuiWindow *window, const ImRect &r, int edge)
{
	float gap = uiPanelGap();
	float threshold = uiPanelSnapThreshold();
	ImRect area = uiPanelWorkArea();
	bool xAxis = (edge & (PANEL_EDGE_LEFT | PANEL_EDGE_RIGHT)) != 0;
	bool minSide = (edge & (PANEL_EDGE_LEFT | PANEL_EDGE_TOP)) != 0;
	float pos = xAxis ? (minSide ? r.Min.x : r.Max.x) : (minSide ? r.Min.y : r.Max.y);
	float areaMin = xAxis ? area.Min.x : area.Min.y;
	float areaMax = xAxis ? area.Max.x : area.Max.y;
	uiTrySnap(axis, pos, minSide ? areaMin + gap : areaMax - gap);

	for(ImGuiWindow *other : GImGui->Windows){
		if(other == window || other->RootWindow != other || !uiIsSnappablePanel(other))
			continue;
		ImRect o(other->Pos, ImVec2(other->Pos.x + other->Size.x, other->Pos.y + other->Size.y));
		bool beside = xAxis ?
			r.Min.y < o.Max.y + threshold && r.Max.y > o.Min.y - threshold :
			r.Min.x < o.Max.x + threshold && r.Max.x > o.Min.x - threshold;
		if(!beside)
			continue;
		float oMin = xAxis ? o.Min.x : o.Min.y;
		float oMax = xAxis ? o.Max.x : o.Max.y;
		uiTrySnap(axis, pos, minSide ? oMax + gap : oMin - gap);
		uiTrySnap(axis, pos, minSide ? oMin : oMax);
	}
}

static float gPanelSnapLineX = -1.0f, gPanelSnapLineY = -1.0f;
static ImGuiWindow *gResizeSnapWindow;
static int gResizeSnapEdges;

// Which edges of which panel the user is dragging to resize, if any.
static void
uiFindResizingPanel(void)
{
	static const int cornerEdges[4] = {
		PANEL_EDGE_RIGHT | PANEL_EDGE_BOTTOM, PANEL_EDGE_LEFT | PANEL_EDGE_BOTTOM,
		PANEL_EDGE_LEFT | PANEL_EDGE_TOP, PANEL_EDGE_RIGHT | PANEL_EDGE_TOP
	};
	static const int borderEdges[4] = {
		PANEL_EDGE_LEFT, PANEL_EDGE_RIGHT, PANEL_EDGE_TOP, PANEL_EDGE_BOTTOM
	};
	gResizeSnapWindow = nil;
	gResizeSnapEdges = 0;
	ImGuiContext &g = *GImGui;
	if(!gPanelSnap || g.ActiveId == 0 || ImGui::GetIO().KeyAlt)
		return;
	for(ImGuiWindow *window : g.Windows){
		if(!uiIsSnappablePanel(window) || (window->Flags & ImGuiWindowFlags_NoResize))
			continue;
#ifdef IMGUI_HAS_DOCK
		if(window->DockIsActive)
			continue;
#endif
		for(int n = 0; n < 4; n++){
			if(g.ActiveId == ImGui::GetWindowResizeCornerID(window, n))
				gResizeSnapEdges = cornerEdges[n];
			else if(g.ActiveId == ImGui::GetWindowResizeBorderID(window, (ImGuiDir)n))
				gResizeSnapEdges = borderEdges[n];
		}
		if(gResizeSnapEdges){
			gResizeSnapWindow = window;
			return;
		}
	}
}

// ImGui sizes a panel being resized from the mouse inside Begin, and runs
// this constraint callback on the result: move the dragged edges onto a
// snap target there, keeping the opposite edges where they are.
static void
uiPanelResizeSnapCallback(ImGuiSizeCallbackData *data)
{
	if(GImGui->CurrentWindow != gResizeSnapWindow || gResizeSnapEdges == 0)
		return;
	int edges = gResizeSnapEdges;
	ImVec2 pos = data->Pos, current = data->CurrentSize, size = data->DesiredSize;
	ImRect r;
	r.Min.x = (edges & PANEL_EDGE_LEFT) ? pos.x + current.x - size.x : pos.x;
	r.Min.y = (edges & PANEL_EDGE_TOP) ? pos.y + current.y - size.y : pos.y;
	r.Max = ImVec2(r.Min.x + size.x, r.Min.y + size.y);

	PanelSnapAxis x = { uiPanelSnapThreshold(), 0.0f, -1.0f };
	PanelSnapAxis y = x;
	if(edges & (PANEL_EDGE_LEFT | PANEL_EDGE_RIGHT))
		uiSnapPanelEdge(&x, gResizeSnapWindow, r, edges & (PANEL_EDGE_LEFT | PANEL_EDGE_RIGHT));
	if(edges & (PANEL_EDGE_TOP | PANEL_EDGE_BOTTOM))
		uiSnapPanelEdge(&y, gResizeSnapWindow, r, edges & (PANEL_EDGE_TOP | PANEL_EDGE_BOTTOM));
	float minSize = ImGui::GetFontSize() * 6.0f;
	if(x.line >= 0.0f){
		float w = size.x + ((edges & PANEL_EDGE_LEFT) ? -x.offset : x.offset);
		if(w >= minSize){
			size.x = w;
			gPanelSnapLineX = x.line;
		}
	}
	if(y.line >= 0.0f){
		float h = size.y + ((edges & PANEL_EDGE_TOP) ? -y.offset : y.offset);
		if(h >= minSize){
			size.y = h;
			gPanelSnapLineY = y.line;
		}
	}
	data->DesiredSize = size;
}

// Before a panel's Begin, so resizing it snaps too.
static void
uiNextPanelSizeConstraints(ImVec2 minSize)
{
	ImGui::SetNextWindowSizeConstraints(minSize, ImVec2(FLT_MAX, FLT_MAX), uiPanelResizeSnapCallback);
}

// Magnetic panels: while a panel is dragged by its title bar, its edges stick
// to the work area and to the edges of the other open panels; resizing snaps
// the same way. Hold Alt to move freely. Runs before any window is
// submitted, so the snapped position is the one drawn this frame.
static void
uiSnapPanels(void)
{
	gPanelSnapLineX = gPanelSnapLineY = -1.0f;
	uiFindResizingPanel();
	ImGuiContext &g = *GImGui;
	if(!gPanelSnap || g.MovingWindow == nil || !ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
	   ImGui::GetIO().KeyAlt)
		return;
	ImGuiWindow *window = g.MovingWindow->RootWindow;
	if(!uiIsSnappablePanel(window))
		return;
#ifdef IMGUI_HAS_DOCK
	if(window->DockIsActive)
		return;
#endif

	ImRect r(window->Pos, ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y));
	PanelSnapAxis x = { uiPanelSnapThreshold(), 0.0f, -1.0f };
	PanelSnapAxis y = x;
	uiSnapPanelEdge(&x, window, r, PANEL_EDGE_LEFT);
	uiSnapPanelEdge(&x, window, r, PANEL_EDGE_RIGHT);
	uiSnapPanelEdge(&y, window, r, PANEL_EDGE_TOP);
	uiSnapPanelEdge(&y, window, r, PANEL_EDGE_BOTTOM);
	if(x.line < 0.0f && y.line < 0.0f)
		return;
	ImGui::SetWindowPos(window, ImVec2(window->Pos.x + x.offset, window->Pos.y + y.offset), ImGuiCond_Always);
	gPanelSnapLineX = x.line;
	gPanelSnapLineY = y.line;
}

// A thin accent line where the dragged panel snapped.
static void
uiDrawPanelSnapGuides(void)
{
	if(gPanelSnapLineX < 0.0f && gPanelSnapLineY < 0.0f)
		return;
	ImRect area = uiPanelWorkArea();
	ImDrawList *dl = EditorForegroundDrawList();
	ImU32 col = ImGui::GetColorU32(ImGuiCol_SeparatorActive, 0.8f);
	if(gPanelSnapLineX >= 0.0f)
		dl->AddLine(ImVec2(gPanelSnapLineX, area.Min.y), ImVec2(gPanelSnapLineX, area.Max.y), col, 1.5f);
	if(gPanelSnapLineY >= 0.0f)
		dl->AddLine(ImVec2(area.Min.x, gPanelSnapLineY), ImVec2(area.Max.x, gPanelSnapLineY), col, 1.5f);
}

static void
uiToolsWindow(void)
{
	uiDefaultPanelLayout(PANEL_RIGHT_BOTTOM);
	uiNextPanelSizeConstraints();
	if(!ImGui::Begin(LI(ICON_FA_WRENCH, "Tools"), &showToolsWindow)){
		ImGui::End();
		return;
	}
	uiFitItemsToPanel();

	// The toolbar keeps common actions close; less-used placement settings
	// stay available here without filling the compact panel.
	if(ImGui::CollapsingHeader(L("Placement"), ImGuiTreeNodeFlags_DefaultOpen)){
		ImGui::Checkbox(_("Land on objects"), &gPlaceSnapToObjects);
		ImGui::SetItemTooltip(_("New objects sit on the object under the cursor."));
		ImGui::Checkbox(_("Land on ground"), &gPlaceSnapToGround);
		ImGui::SetItemTooltip(_("New objects sit on the ground under the cursor."));
	}
	if(ImGui::CollapsingHeader(L("Dragging"))){
		ImGui::BeginDisabled(!gDragFollowGround);
		ImGui::Checkbox(_("Tilt to slope"), &gDragAlignToSurface);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Match the object's angle to the ground slope. Enable Follow ground in More on the toolbar."));
	}
	if(ImGui::CollapsingHeader(L("Snapping"))){
		if(isSA()){
			ImGui::InputFloat(_("Snap distance"), &gObjectSnapDistance, 0.05f, 0.5f, "%.2f m");
			ImGui::SetItemTooltip(_("Object snap (%s) uses this distance."), uiKey(SC_OBJECT_SNAP));
			gObjectSnapDistance = clamp(gObjectSnapDistance, 0.01f, 10.0f);
		}
		ImGui::InputFloat(_("Move step"), &gGizmoSnapTranslate, 0.25f, 1.0f, "%g m");
		ImGui::SetItemTooltip(_("Hold Shift while moving with the magnet on."));
		gGizmoSnapTranslate = clamp(gGizmoSnapTranslate, 0.01f, 1000.0f);
		ImGui::InputFloat(_("Rotate step"), &gGizmoSnapAngle, 5.0f, 15.0f, "%g\xC2\xB0");
		ImGui::SetItemTooltip(_("Hold Shift while rotating with the magnet on."));
		gGizmoSnapAngle = clamp(gGizmoSnapAngle, 0.1f, 180.0f);
	}

	ImGui::Spacing();
	MapToolsPanel();
	if(ImGui::CollapsingHeader(LI(ICON_FA_PAINTBRUSH, "Brush"), gBrushMode ? ImGuiTreeNodeFlags_DefaultOpen : 0)){
		int brushObjId = GetSpawnObjectId();
		ObjectDef *brushObj = brushObjId >= 0 ? GetObjectDef(brushObjId) : nil;

		if(gBrushMode){
			ImVec4 active(0.95f, 0.75f, 0.20f, 1.0f);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.25f, 0.05f, 1.0f));
			char exitLabel[64];
			snprintf(exitLabel, sizeof(exitLabel), _("Exit Brush (%s)##exit_brush"), uiKey(SC_CANCEL));
			if(ImGui::Button(exitLabel))
				ExitBrushMode();
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if(brushObj)
				ImGui::TextColored(active, _("Painting: %s"), brushObj->m_name);
			else
				ImGui::TextColored(active, _("No object selected"));
		}else{
			ImGui::BeginDisabled(brushObjId < 0);
			if(ImGui::Button(_("Start Brush")))
				EnterBrushMode(brushObjId);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if(brushObj)
				ImGui::TextDisabled(_("Ready: %s"), brushObj->m_name);
			else
				ImGui::TextDisabled(_("Select an object in the Browser first"));
		}
		if(gCameraLookButton == 2)
			ImGui::TextDisabled(_("LMB = paint   \xC2\xB7   MMB-drag = look around   \xC2\xB7   Esc = cancel"));
		else
			ImGui::TextDisabled(_("LMB = paint   \xC2\xB7   %s-drag = look around   \xC2\xB7   Esc = cancel"),
				cameraMouseButtonName(cameraBrushLookButton()));

		ImGui::InputFloat(_("Z offset (m)"), &gBrushZOffset, 0.1f, 1.0f, "%.2f");
		ImGui::SetItemTooltip(_("Shift along Z after ground snap.\n"
			"Negative sinks the object into the ground (useful for trees on slopes).\n"
			"Positive lifts it off the surface."));

		ImGui::Checkbox(_("Align to surface##brush"), &gBrushAlignToSurface);
		ImGui::SetItemTooltip(_("Rotate placed objects so their up-axis matches the surface normal.\n"
			"Use with care — some tree models look better upright on slopes."));

		// Brush size + scatter
		ImGui::InputFloat(_("Radius (m)"), &gBrushRadius, 0.5f, 5.0f, "%.2f");
		if(gBrushRadius < 0.0f) gBrushRadius = 0.0f;
		if(gBrushRadius > 500.0f) gBrushRadius = 500.0f;
		ImGui::SetItemTooltip(_("Brush disc size. 0 = single object under cursor.\n"
			"When > 0, each click scatters Count objects inside this radius."));

		ImGui::BeginDisabled(gBrushRadius <= 0.01f);
		ImGui::InputInt(_("Count / click"), &gBrushCount, 1, 5);
		if(gBrushCount < 1) gBrushCount = 1;
		if(gBrushCount > 128) gBrushCount = 128;
		ImGui::SetItemTooltip(_("Number of objects scattered per click when Radius > 0.\n"
			"Capped at 128 to keep a single click undoable as one step."));
		ImGui::EndDisabled();

		// Yaw range
		float yaw[2] = { gBrushYawMin, gBrushYawMax };
		if(ImGui::InputFloat2(_("Yaw min/max (\xC2\xB0)"), yaw, "%.0f")){
			gBrushYawMin = yaw[0];
			gBrushYawMax = yaw[1];
		}
		if(gBrushYawMin < -360.0f) gBrushYawMin = -360.0f;
		if(gBrushYawMax > 360.0f) gBrushYawMax = 360.0f;
		if(gBrushYawMax < gBrushYawMin) gBrushYawMax = gBrushYawMin;
		ImGui::SetItemTooltip(_("Yaw rotation range in degrees.\n"
			"Both equal = fixed angle (0/0 = no rotation).\n"
			"Different = uniform random between them (e.g. 0/360 for full random)."));

		// Drag pacing
		ImGui::InputFloat(_("Spacing (m)"), &gBrushSpacing, 0.25f, 1.0f, "%.2f");
		if(gBrushSpacing < 0.0f) gBrushSpacing = 0.0f;
		ImGui::SetItemTooltip(_("Minimum world distance between drag-paint bursts.\n"
			"0 disables the distance gate (delay alone controls the rate).\n"
			"Single clicks always fire regardless."));

		ImGui::InputFloat(_("Delay (ms)"), &gBrushDelayMs, 10.0f, 100.0f, "%.0f");
		if(gBrushDelayMs < 0.0f) gBrushDelayMs = 0.0f;
		if(gBrushDelayMs > 10000.0f) gBrushDelayMs = 10000.0f;
		ImGui::SetItemTooltip(_("Minimum time between drag-paint bursts, in milliseconds.\n"
			"0 = no delay. Combines with Spacing — both constraints must pass."));
	}
	MapToolsMorePanel();
	RadarArtPanel();
	ImGui::End();
}

// Settings that are set once and rarely touched again.
static MapCheckResult gMapCheck;
static bool gMapCheckDone;
static bool gMapCheckShowNotes = true;

// Run before saving and testing; it never stops either, only reports.
static void
uiMapCheckBefore(const char *action)
{
	RunMapCheck(gPawnSaveFormat, &gMapCheck, true);
	gMapCheckDone = true;
	std::string summary = MapCheckSummary(gMapCheck);
	if(summary.empty())
		return;
	const char *fmt = strcmp(action, "saving") == 0 ? _("Map check before saving: %s. %s opens the list.") :
		_("Map check before testing: %s. %s opens the list.");
	ToastFor(TOAST_SAVE, 8.0f, fmt, summary.c_str(), uiKey(SC_WINDOW_MAP_CHECK));
}

static void
uiMapCheckWindow(void)
{
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 28.0f), ImGuiCond_FirstUseEver);
	uiNextPanelSizeConstraints();
	if(!ImGui::Begin(LI(ICON_FA_LIST, "Map Check"), &showMapCheckWindow)){
		ImGui::End();
		return;
	}
	uiFitItemsToPanel();
	if(!gMapCheckDone || ImGui::Button(TI(ICON_FA_ROTATE, "Check again"))){
		RunMapCheck(gPawnSaveFormat, &gMapCheck, true);
		gMapCheckDone = true;
	}
	ImGui::SameLine();
	ImGui::Checkbox(_("Notes"), &gMapCheckShowNotes);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", gMapCheck.pawnFormat == PAWN_CODE_STREAMER ? _("for Streamer code") : _("for native code"));
	std::string summary = MapCheckSummary(gMapCheck);
	if(summary.empty())
		ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), TI(ICON_FA_CHECK, "Nothing that will break in game."));
	else
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", summary.c_str());
	ImGui::Separator();

	ImGui::BeginChild("##map_check_items");
	const char *lastCategory = nil;
	for(size_t i = 0; i < gMapCheck.items.size(); i++){
		const MapCheckItem &item = gMapCheck.items[i];
		if(item.severity == MAPCHECK_INFO && !gMapCheckShowNotes)
			continue;
		if(lastCategory == nil || strcmp(lastCategory, item.category) != 0){
			ImGui::SeparatorText(item.category);
			lastCategory = item.category;
		}
		ImVec4 colour = item.severity == MAPCHECK_ERROR ? ImVec4(1.0f, 0.40f, 0.35f, 1.0f) :
			item.severity == MAPCHECK_WARNING ? UiSemanticColor(UI_COLOR_WARNING) :
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		const char *icon = item.severity == MAPCHECK_ERROR ? ICON_FA_XMARK :
			item.severity == MAPCHECK_WARNING ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO;
		ImGui::PushID((int)i);
		ImGui::TextColored(colour, "%s", icon);
		ImGui::SetItemTooltip("%s", MapCheckSeverityName(item.severity));
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0.0f);
		if(item.instances.empty() && item.textDraw < 0)
			ImGui::TextUnformatted(item.text.c_str());
		else if(item.textDraw >= 0){
			if(ImGui::Selectable(item.text.c_str()))
				SelectTextDraw(item.textDraw);
		}
		else if(ImGui::Selectable(item.text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)){
			ClearSelection();
			for(int id : item.instances){
				ObjectInst *inst = GetInstanceByID(id);
				if(inst && !inst->m_isDeleted){
					SetInstanceHidden(inst, false);
					inst->Select();
				}
			}
			if(ImGui::IsMouseDoubleClicked(0))
				FocusSelection();
		}
		if(!item.instances.empty())
			ImGui::SetItemTooltip(_("Click to select it, double-click to go there."));
		ImGui::PopTextWrapPos();
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::End();
}

float
GetUiScale(void)
{
	return gUiScale;
}

void
SetUiScale(float scale)
{
	gUiScale = clamp(scale, 0.6f, 2.5f);
	ImGui::GetIO().FontGlobalScale = gUiScale;
	saveSaveSettings();
}

void
StepUiScale(float steps)
{
	// Tenths, so a few steps land back on 1.0.
	SetUiScale(floorf(gUiScale * 10.0f + 0.5f) / 10.0f + steps * 0.1f);
	Toast(TOAST_SELECTION, _("Interface size %.0f%%"), gUiScale * 100.0f);
}

// The chord being rebound in Preferences > Shortcuts, or -1.
static int gShortcutCaptureAction = -1;
static int gShortcutCaptureSlot;

static void
uiShortcutPreferences(void)
{
	bool changed = false;
	if(gShortcutCaptureAction >= 0){
		// Keys go to the rebind, not to the camera or other shortcuts.
		ImGui::SetNextFrameWantCaptureKeyboard(true);
		ShortcutChord chord;
		if(ShortcutReadPressedChord(&chord)){
			SetShortcutChord(gShortcutCaptureAction, gShortcutCaptureSlot, chord);
			gShortcutCaptureAction = -1;
			changed = true;
		}
	}
	ShortcutsSetCapturing(gShortcutCaptureAction >= 0);

	ImGui::TextWrapped(_("Click a key to change it, then press the new key with any Ctrl, Shift or Alt. "
		"Right-click a key to clear it. Shortcuts never fire while you type in a field."));
	if(ImGui::Button(TI(ICON_FA_ROTATE_LEFT, "Reset all to defaults"))){
		ResetShortcuts();
		gShortcutCaptureAction = -1;
		changed = true;
	}
	static ImGuiTextFilter filter;
	ImGui::SameLine();
	filter.Draw("##shortcut_filter", -FLT_MIN);
	if(!filter.IsActive())
		ImGui::SetItemTooltip(_("Filter by action or key"));

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
		ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if(ImGui::BeginTable("##shortcut_prefs", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))){
		float keyWidth = ImGui::GetFontSize() * 7.5f;
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(_("Action"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(_("Key"), ImGuiTableColumnFlags_WidthFixed, keyWidth);
		ImGui::TableSetupColumn(_("Also"), ImGuiTableColumnFlags_WidthFixed, keyWidth);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableHeadersRow();
		const char *lastSection = nil;
		for(int action = 0; action < SC_NUM_ACTIONS; action++){
			const ShortcutInfo *info = GetShortcutInfo(action);
			char searchable[256];
			snprintf(searchable, sizeof(searchable), "%s %s %s", _(info->section), _(info->name), ShortcutLabel(action));
			if(filter.IsActive() && !filter.PassFilter(searchable))
				continue;
			if(lastSection == nil || strcmp(lastSection, info->section) != 0){
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::SeparatorText(_(info->section));
				lastSection = info->section;
			}
			ImGui::PushID(action);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(_(info->name));
			for(int slot = 0; slot < SHORTCUT_CHORDS; slot++){
				ImGui::TableSetColumnIndex(1 + slot);
				ImGui::PushID(slot);
				bool capturing = gShortcutCaptureAction == action && gShortcutCaptureSlot == slot;
				char name[64];
				ShortcutChordName(GetShortcutChord(action, slot), name, sizeof(name));
				int conflict = capturing ? -1 : ShortcutConflict(action, slot);
				if(capturing)
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				else if(conflict >= 0)
					ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
				char label[96];
				snprintf(label, sizeof(label), "%s##key", capturing ? _("Press a key...") : name[0] ? name : "-");
				if(ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f))){
					if(capturing)
						gShortcutCaptureAction = -1;
					else{
						gShortcutCaptureAction = action;
						gShortcutCaptureSlot = slot;
					}
				}
				if(capturing || conflict >= 0)
					ImGui::PopStyleColor();
				if(ImGui::IsItemClicked(ImGuiMouseButton_Right)){
					SetShortcutChord(action, slot, ShortcutChord{ 0, 0 });
					if(capturing)
						gShortcutCaptureAction = -1;
					changed = true;
				}
				if(conflict >= 0)
					ImGui::SetItemTooltip(_("Also used by \"%s\" (%s). Both run when it is pressed."),
						_(GetShortcutInfo(conflict)->name), _(GetShortcutInfo(conflict)->section));
				else if(capturing)
					ImGui::SetItemTooltip(_("Press the new key. Click again to keep the old one."));
				ImGui::PopID();
			}
			ImGui::TableSetColumnIndex(3);
			if(!ShortcutIsDefault(action)){
				if(ImGui::SmallButton(ICON_FA_ROTATE_LEFT)){
					for(int slot = 0; slot < SHORTCUT_CHORDS; slot++)
						SetShortcutChord(action, slot, GetDefaultShortcutChord(action, slot));
					changed = true;
				}
				char defaults[2][48];
				ShortcutChordName(GetDefaultShortcutChord(action, 0), defaults[0], sizeof(defaults[0]));
				ShortcutChordName(GetDefaultShortcutChord(action, 1), defaults[1], sizeof(defaults[1]));
				ImGui::SetItemTooltip(_("Back to the default: %s%s%s"), defaults[0][0] ? defaults[0] : _("none"),
					defaults[1][0] ? " / " : "", defaults[1]);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if(changed)
		saveSaveSettings();
}

static void
uiGeneralPreferences(void)
{
	bool changed = false;

	ImGui::SeparatorText(_("Interface"));
	if(uiLanguagePreferences())
		changed = true;
	float scale = ImGui::GetIO().FontGlobalScale;
	if(ImGui::SliderFloat(_("UI scale"), &scale, 0.6f, 2.5f, "%.2fx")){
		ImGui::GetIO().FontGlobalScale = scale;
		gUiScale = scale;
	}
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	int theme = GetUiTheme();
	if(ImGui::BeginCombo(_("Theme"), GetUiThemeName(theme))){
		for(int t = 0; t < NumUiThemes(); t++){
			if(t == UI_THEME_FIRST_PALETTE)
				ImGui::Separator();
			if(ImGui::Selectable(GetUiThemeName(t), t == theme)){
				SetUiTheme(t);
				changed = true;
			}
			if(t == theme)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	changed |= ImGui::Checkbox(_("Snap panels to edges and each other"), &gPanelSnap);
	ImGui::SetItemTooltip(_("Hold Alt while dragging to place a panel freely."));
	changed |= ImGui::Checkbox(_("Show getting started card on empty maps"), &gShowStartGuide);
	changed |= KeycastPreferences();
	if(ImGui::Button(TI(ICON_FA_TABLE_COLUMNS, "Reset panel layout")))
		uiResetPanelLayout();

	ImGui::SeparatorText(_("Maps"));
	changed |= ImGui::Checkbox(_("Reopen last map on startup"), &gReopenLastMap);
	bool autosaveChanged = ImGui::Checkbox(_("Autosave unsaved maps"), &gAutosaveEnabled);
	ImGui::SetItemTooltip(_("Writes a copy of the open map to the autosave folder in Haumea's config\n"
		"folder while it has unsaved changes. File > Recover Autosave opens them.\n"
		"The map file itself is only written by Save."));
	ImGui::BeginDisabled(!gAutosaveEnabled);
	ImGui::Indent();
	int autosaveMinutes = gAutosaveIntervalSeconds / 60;
	if(ImGui::SliderInt(_("Every (minutes)"), &autosaveMinutes, 1, 30)){
		gAutosaveIntervalSeconds = autosaveMinutes * 60;
		autosaveChanged = true;
	}
	autosaveChanged |= ImGui::SliderInt(_("Copies to keep"), &gAutosaveKeepCount, 1, 50);
	ImGui::Unindent();
	ImGui::EndDisabled();
	if(autosaveChanged){
		sanitizeAutosaveSettings();
		changed = true;
	}
	changed |= ImGui::Checkbox(_("Confirm before reloading game files"), &gConfirmReloadGame);
	ImGui::SetItemTooltip(_("When off, Reload Game Files restarts at once if there are no unsaved changes.\nUnsaved changes always ask first."));

	ImGui::SeparatorText(_("Notifications"));
	uiNotificationSettings();

	if(changed)
		saveSaveSettings();
}

static void
uiCameraPreferences(void)
{
	bool changed = false;
	ImGui::SeparatorText(_("Mouse"));
	ImGui::Text(_("Look around with"));
	for(int button = 1; button <= 3; button++){
		ImGui::SameLine();
		char label[32];
		snprintf(label, sizeof(label), "%s##camera_look_button", cameraMouseButtonName(button));
		if(ImGui::RadioButton(label, gCameraLookButton == button)){
			gCameraLookButton = button;
			changed = true;
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	ImGui::SetItemTooltip("%s", gCameraLookButton == 1 ?
		_("Hold it and drag to look, and fly with W/A/S/D (Q/E down and up) while it's held;\n"
		"a click without dragging still selects.\n"
		"While brushing, LMB paints, so hold RMB to look.") :
		_("Hold it and drag to look, and fly with W/A/S/D (Q/E down and up) while it's held;\n"
		"a click without dragging still selects."));
	ImGui::SeparatorText(_("Flying"));
	changed |= ImGui::Checkbox(_("Accelerate fly movement"), &gFlyAcceleration);
	ImGui::SetItemTooltip(_("When off, flying (W/A/S/D and Q/E with the look button held) goes at the constant fly speed below."));
	ImGui::BeginDisabled(gFlyAcceleration);
	ImGui::SliderFloat(_("Fly speed"), &gFlySpeed, 0.1f, 70.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::SliderFloat(_("Fast (Shift)"), &gFlyFastMul, 1.0f, 10.0f, "x%.2f");
	ImGui::SliderFloat(_("Slow (Alt)"), &gFlySlowMul, 0.05f, 1.0f, "x%.2f");
	ImGui::SliderFloat(_("FOV wheel step"), &gFovWheelStep, 0.1f, 15.0f, "%.2f deg");
	ImGui::SetItemTooltip(_("How much the field of view changes per mouse wheel step."));

	if(changed)
		saveSaveSettings();
}

static void
uiPerformancePreferences(void)
{
	bool changed = false;
	ImGui::TextDisabled(_("Stream distance"));
	ImGui::SetNextItemWidth(-FLT_MIN);
	changed |= ImGui::SliderFloat("##stream_distance", &gStreamDistance, 50.0f, 5000.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SetItemTooltip(_("One distance for all objects, independent of model, weather and time of day."));
	ImGui::TextWrapped(_("Higher distances show more objects and use more GPU and memory."));
	// The SA-MP client's own far clip (CGame::UpdateFarClippingPlane):
	// players never see further than this, so past it is editor-only.
	if(gStreamDistance > 1250.0f)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), "%s",
			_("Players see at most 1250 m (400 m inside interiors): objects further away only show in the editor."));
	bool fog = gFogMode != FOG_OFF;
	if(ImGui::Checkbox(_("Distance fog"), &fog)){
		gFogMode = fog ? FOG_LIGHT : FOG_OFF;
		changed = true;
	}
	ImGui::SetItemTooltip(_("Blends distant objects into the horizon to soften the stream cutoff."));
	ImGui::Spacing();
	ImGui::TextDisabled(_("Frame rate"));
	ImGui::Text(_("%.0f FPS  /  %.1f ms"), ImGui::GetIO().Framerate, 1000.0f/max(ImGui::GetIO().Framerate, 1.0f));
	changed |= ImGui::Checkbox(_("VSync"), &gVSync);
	ImGui::SetItemTooltip(_("Matches the display refresh rate to reduce tearing. Turn off for uncapped FPS."));
	bool limited = gFrameLimit > 0;
	if(ImGui::Checkbox(_("Limit frame rate"), &limited)){
		gFrameLimit = limited ? 144 : 0;
		changed = true;
	}
	if(limited){
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed |= ImGui::SliderInt("##frame_limit", &gFrameLimit, 30, 360, "%d FPS", ImGuiSliderFlags_AlwaysClamp);
	}else if(!gVSync)
		ImGui::TextDisabled(_("Uncapped"));
	ImGui::TextDisabled(_("Background windows run at up to 30 FPS."));
	ImGui::Spacing();
	// Renderer details most people never change: folded.
	if(!ImGui::TreeNodeEx(L("Graphics"), ImGuiTreeNodeFlags_SpanAvailWidth)){
		if(changed) saveSaveSettings();
		return;
	}
	uint32 activeAASamples = sanitizeAASamples(rw::Engine::getMultiSamplingLevels());
	uint32 maxAASamples = sanitizeAASamples(rw::Engine::getMaxMultiSamplingLevels());
	static const uint32 aaOptions[] = { 1, 2, 4, 8, 16 };
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	if(ImGui::BeginCombo(_("Anti-aliasing"), getAASamplesLabel(gRequestedAASamples))){
		for(uint32 samples : aaOptions){
			if(samples > 1 && (maxAASamples <= 1 || samples > maxAASamples))
				continue;

			bool selected = gRequestedAASamples == samples;
			if(ImGui::Selectable(getAASamplesLabel(samples), selected)){
				if(gRequestedAASamples != samples){
					gRequestedAASamples = samples;
					sk::requestedMultiSamplingLevels = samples;
					SaveEditorSettingsNow();
					Toast(TOAST_SAVE, _("Anti-aliasing set to %s. Restart Haumea to apply it."),
						getAASamplesLabel(samples));
				}
			}
			if(selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	if(maxAASamples <= 1)
		ImGui::TextDisabled(_("MSAA is not reported by the current renderer/device."));
	else if(activeAASamples != gRequestedAASamples)
		ImGui::TextDisabled(_("Restart Haumea to apply the new anti-aliasing level."));
	if(RendererChoiceAvailable()){
		// The renderers' own names aren't translated.
		auto label = [](int i){ return i == RENDERER_AUTO ? _("Automatic") : i == RENDERER_OPENGL ? "OpenGL 3.3" : "Direct3D 9"; };
#ifdef RW_D3D9
		const int running = RENDERER_D3D9;
#else
		const int running = RENDERER_OPENGL;
#endif
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if(ImGui::BeginCombo(_("Renderer"), label(gRendererChoice))){
			for(int i = RENDERER_AUTO; i <= RENDERER_D3D9; i++){
				bool selected = gRendererChoice == i;
				if(ImGui::Selectable(label(i), selected) && !selected){
					gRendererChoice = i;
					gRendererGlFailed = false;	// a choice tries OpenGL again
					SaveEditorSettingsNow();
				}
				if(selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip(_("Automatic uses OpenGL, and Direct3D 9 when OpenGL doesn't start on this PC."));
		int wanted = gRendererChoice != RENDERER_AUTO ? gRendererChoice :
			gRendererGlFailed ? RENDERER_D3D9 : RENDERER_OPENGL;
		ImGui::TextDisabled(_("Running: %s"), label(running));
		if(gRendererGlFailed && gRendererChoice == RENDERER_AUTO)
			ImGui::TextDisabled(_("OpenGL didn't start on this PC, so Automatic uses Direct3D 9."));
		if(wanted != running){
			ImGui::TextDisabled(_("Restart Haumea to switch renderers."));
			ImGui::SameLine();
			if(ImGui::SmallButton(_("Restart now")))
				restartHaumeaFromUi();
		}
	}
	ImGui::Checkbox(_("Post effects"), &gRenderPostFX);
	if(params.timecycle == GAME_SA){
		ImGui::SameLine();
		bool colourFilter = gColourFilter != 0;
		if(ImGui::Checkbox(_("Colour filter"), &colourFilter))
			gColourFilter = colourFilter ? PLATFORM_PC : 0;
		ImGui::SetItemTooltip(_("The PC game's time-of-day colour grading."));
	}
	ImGui::Checkbox(_("Sky"), &gRenderBackground);
	ImGui::SameLine();
	ImGui::Checkbox(_("Backface culling"), &gDoBackfaceCulling);
	if(params.timecycle == GAME_SA)
		ImGui::Checkbox(_("Interior and tunnel lighting"), &gEnableTimecycleBoxes);
	ImGui::TreePop();
	if(changed) saveSaveSettings();
}

static void
uiPreferencesWindow(void)
{
	// Leave room for translated tab names, including Privacy and Namaka.
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 62.0f, ImGui::GetFontSize() * 32.0f), ImGuiCond_FirstUseEver);
	uiNextPanelSizeConstraints();
	if(!ImGui::Begin(LI(ICON_FA_GEAR, "Preferences"), &showPreferencesWindow)){
		ImGui::End();
		gShortcutCaptureAction = -1;
		ShortcutsSetCapturing(false);
		return;
	}
	// A list of sections on the left and the chosen one on the right, like
	// a system settings window: every section's name is always in view.
	enum { PREF_GENERAL, PREF_PERFORMANCE, PREF_CAMERA, PREF_WORKSPACE, PREF_TESTSERVER,
		PREF_SHORTCUTS, PREF_UPDATES, PREF_PRIVACY, PREF_NAMAKA, PREF_COUNT };
	static int section = PREF_GENERAL;
	const char *names[PREF_COUNT] = {
		TI(ICON_FA_GEAR, "General"), TI(ICON_FA_BOLT, "Performance"), TI(ICON_FA_VIDEO, "Camera"),
		TI(ICON_FA_FOLDER_OPEN, "Workspace"), TI(ICON_FA_GAMEPAD, "Test server"), TI(ICON_FA_LIST, "Shortcuts"),
		TI(ICON_FA_ROTATE, "Updates"), TI(ICON_FA_LOCK, "Privacy"), ICON_FA_USERS "  Namaka",
	};
	float fs = ImGui::GetFontSize();
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
	ImGui::BeginChild("##pref_sections", ImVec2(fs*11.5f, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
	for(int i = 0; i < PREF_COUNT; i++){
		bool on = section == i;
		if(on){
			ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(a.x, a.y, a.z, 0.28f));
		}
		if(ImGui::Selectable(names[i], on, 0, ImVec2(0, fs*1.9f)))
			section = i;
		if(on)
			ImGui::PopStyleColor();
	}
	ImGui::PopStyleVar(2);
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	ImGui::SameLine(0.0f, fs*0.8f);

	ImGui::BeginChild("##pref_content", ImVec2(0, 0));
	uiFitItemsToPanel();
	// One width for every field, with its label beside it: the page reads as a column.
	ImGui::PushItemWidth(std::min(fs*18.0f, ImGui::GetContentRegionAvail().x*0.6f));
	if(section != PREF_SHORTCUTS){
		gShortcutCaptureAction = -1;
		ShortcutsSetCapturing(false);
	}
	switch(section){
	case PREF_GENERAL:
		uiGeneralPreferences();
		ImGui::Spacing();
		if(ImGui::Button(_("Open fonts folder")))
			uiOpenFontsFolder();
		ImGui::SetItemTooltip(_("Fonts for material text: SA-MP's own and the free stand-ins are put\n"
			"there; add other .ttf files to it."));
		break;
	case PREF_PERFORMANCE: uiPerformancePreferences(); break;
	case PREF_CAMERA: uiCameraPreferences(); break;
	case PREF_WORKSPACE: uiWorkspaceSettings(); break;
	case PREF_TESTSERVER: TestServerPreferences(); break;
	case PREF_SHORTCUTS: uiShortcutPreferences(); break;
	case PREF_UPDATES: UpdaterPreferences(); break;
	case PREF_PRIVACY:
		if(DiscordPresencePreferences())saveSaveSettings();
		ImGui::Spacing();
		TelemetryPreferences();
		ImGui::Spacing();
		CrashReportsPreferences();
		break;
	case PREF_NAMAKA:
		NamakaListenSwitch();
		if(ImGui::Button(_("Open Namaka...")))
			showNamakaWindow = true;
		ImGui::SetItemTooltip(_("Install, repair or update Namaka, and connect an AI assistant (Claude, Codex…)."));
		break;
	}
	ImGui::PopItemWidth();
	ImGui::EndChild();
	ImGui::End();
}

static const char*
uiObjectLabel(const ObjectInst *inst, char *buf, size_t size)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	ObjectDef *obj = inst ? GetObjectDef(inst->m_objectId) : nil;
	int kind = GetSampElementKind(inst);
	char what[64];
	if(kind == SAMP_ELEMENT_LABEL){
		// The label's first line, without colour embeds.
		size_t n = 0;
		for(const char *p = properties->labelText; *p && *p != '\n' && n + 1 < 28; p++){
			if(p[0] == '{' && strlen(p) >= 8 && p[7] == '}'){ p += 7; continue; }
			what[n++] = *p;
		}
		what[n] = '\0';
		if(strlen(properties->labelText) > n)
			snprintf(what + n, sizeof(what) - n, "...");
	}else if(kind != SAMP_ELEMENT_OBJECT)
		snprintf(what, sizeof(what), "%s", _(GetSampElementKindName(kind)));
	else
		snprintf(what, sizeof(what), "%s", obj ? obj->m_name : "?");
	if(inst == nil)
		snprintf(buf, size, "%s", _("(none)"));
	else if(properties->name[0])
		snprintf(buf, size, "%s (%s)", properties->name, what);
	else if(kind == SAMP_ELEMENT_LABEL)
		snprintf(buf, size, "\"%s\" #%d", what, inst->m_id);
	else
		snprintf(buf, size, "%s #%d", what, inst->m_id);
	return buf;
}

// Distinct groups used by live objects, sorted.
static void
uiCollectObjectGroups(std::vector<std::string> &groups)
{
	groups.clear();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_pawnProperties == nil ||
		   inst->m_pawnProperties->group[0] == '\0')
			continue;
		std::string group = inst->m_pawnProperties->group;
		if(std::find(groups.begin(), groups.end(), group) == groups.end())
			groups.push_back(group);
	}
	std::sort(groups.begin(), groups.end());
}

static bool
uiGroupPicker(const char *label, char *group, size_t size)
{
	bool changed = false;
	float buttonWidth = ImGui::GetFrameHeight();
	ImGui::PushID(label);
	ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::InputTextWithHint("##group", _("no group"), group, size))
		changed = true;
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::BeginCombo("##groups", "", ImGuiComboFlags_NoPreview)){
		std::vector<std::string> groups;
		uiCollectObjectGroups(groups);
		if(ImGui::Selectable(_("(no group)"), group[0] == '\0')){
			group[0] = '\0';
			changed = true;
		}
		for(size_t i = 0; i < groups.size(); i++)
			if(ImGui::Selectable(groups[i].c_str(), groups[i] == group)){
				strncpy(group, groups[i].c_str(), size-1);
				group[size-1] = '\0';
				changed = true;
			}
		ImGui::EndCombo();
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextUnformatted(label);
	ImGui::PopID();
	return changed;
}

// ---- SA-MP elements ----

struct UiNamedValue { int value; const char *name; };

// The search text of the open ID combo; one popup is open at a time.
static char uiIdComboFilter[48];

static const UiNamedValue uiPickupModels[] = {
	{ 1239, N_("Info") }, { 1240, N_("Health (heart)") }, { 1242, N_("Armour") },
	{ 1241, N_("Adrenaline") }, { 1247, N_("Bribe (star)") }, { 1212, N_("Money") },
	{ 1274, N_("Dollar sign") }, { 1550, N_("Money bag") }, { 1210, N_("Briefcase") },
	{ 1272, N_("House (blue)") }, { 1273, N_("House (green)") }, { 1277, N_("Save disk") },
	{ 1275, N_("Clothes") }, { 1254, N_("Skull") }, { 1313, N_("Two skulls") },
	{ 1252, N_("Bomb") }, { 1253, N_("Camera") }, { 1276, N_("Tiki") },
	{ 1318, N_("Arrow") }, { 1559, N_("Diamond (entrance)") }, { 1248, N_("GTA III sign") },
	{ 1314, N_("Two players") }, { 1310, N_("Parachute") },
};

static const UiNamedValue uiPickupTypes[] = {
	{ 0, N_("0  Not pickupable") }, { 1, N_("1  Always there, cannot be picked up by script") },
	{ 2, N_("2  Respawns 30 s after pickup") }, { 3, N_("3  Respawns after death") },
	{ 4, N_("4  Disappears 15-20 s after creation") }, { 8, N_("8  Disappears without effect") },
	{ 11, N_("11  Blows up vehicles") }, { 12, N_("12  Blows up vehicles") },
	{ 13, N_("13  Slows vehicles down") }, { 14, N_("14  Picked up from vehicles only") },
	{ 15, N_("15  Respawns 30 s after pickup") }, { 19, N_("19  Disappears, plays pickup sound") },
	{ 22, N_("22  Respawns after death") }, { 23, N_("23  Disappears, can be walked through") },
};

static const char *uiWeaponNames[47] = {
	N_("Fist"), N_("Brass knuckles"), N_("Golf club"), N_("Nightstick"), N_("Knife"), N_("Baseball bat"),
	N_("Shovel"), N_("Pool cue"), N_("Katana"), N_("Chainsaw"), N_("Purple dildo"), N_("Dildo"),
	N_("Vibrator"), N_("Silver vibrator"), N_("Flowers"), N_("Cane"), N_("Grenade"), N_("Tear gas"),
	N_("Molotov"), N_(""), N_(""), N_(""), N_("Colt 45"), N_("Silenced pistol"), N_("Desert Eagle"),
	N_("Shotgun"), N_("Sawn-off"), N_("Combat shotgun"), N_("Micro SMG / Uzi"), N_("MP5"), N_("AK-47"),
	N_("M4"), N_("Tec-9"), N_("Country rifle"), N_("Sniper rifle"), N_("RPG"), N_("Heat-seeking RPG"),
	N_("Flamethrower"), N_("Minigun"), N_("Satchel charge"), N_("Detonator"), N_("Spraycan"),
	N_("Fire extinguisher"), N_("Camera"), N_("Night vision"), N_("Thermal goggles"), N_("Parachute"),
};

static const char *uiMapIconNames[64] = {
	N_("Marker (colour)"), N_("White square"), N_("Player"), N_("Player menu map"), N_("North"),
	N_("Airyard"), N_("Ammu-Nation"), N_("Barber"), N_("Big Smoke"), N_("Boatyard"), N_("Burger Shot"),
	N_("Quarry"), N_("Catalina"), N_("Cesar"), N_("Cluckin' Bell"), N_("Carl Johnson"), N_("C.R.A.S.H."),
	N_("Diner"), N_("Emmet"), N_("Enemy attack"), N_("Fire"), N_("Girlfriend"), N_("Hospital"), N_("Loco"),
	N_("Madd Dogg"), N_("Caligula's"), N_("MC Strap"), N_("Mod garage"), N_("OG Loc"), N_("Well Stacked Pizza"),
	N_("Police"), N_("Property (free)"), N_("Property (taken)"), N_("Race"), N_("Ryder"), N_("Save house"),
	N_("School"), N_("Mystery"), N_("Sweet"), N_("Tattoo"), N_("The Truth"), N_("Waypoint"), N_("Toreno's ranch"),
	N_("Triads"), N_("Triads casino"), N_("Clothes"), N_("Woozie"), N_("Zero"), N_("Date disco"),
	N_("Date drink"), N_("Date food"), N_("Truck"), N_("Cash"), N_("Flag"), N_("Gym"), N_("Impound"),
	N_("Light (red dot)"), N_("Runway"), N_("Gang (Ballas)"), N_("Gang (purple)"), N_("Gang (yellow)"),
	N_("Gang (neutral)"), N_("Gang (Grove)"), N_("Pay 'n' Spray"),
};

static const char *uiMapIconStyles[] = {
	N_("Local (MAPICON_LOCAL)"), N_("Global (MAPICON_GLOBAL)"),
	N_("Local checkpoint (MAPICON_LOCAL_CHECKPOINT)"), N_("Global checkpoint (MAPICON_GLOBAL_CHECKPOINT)"),
};

// Pawn colours are 0xRRGGBBAA.
// A numbered thing (skin, pickup model, sound) as one combo showing
// "id  name". Its popup takes any ID typed in and searches the named ones;
// draw the items with uiIdComboItem between this and ImGui::EndCombo().
// "id  name", unless the name already starts with its number.
static void
uiIdComboText(char *out, size_t size, int id, const char *name)
{
	char number[16];
	int length = snprintf(number, sizeof(number), "%d", id);
	if(name && strncmp(name, number, length) == 0 && name[length] == ' ')
		snprintf(out, size, "%s", name);
	else
		snprintf(out, size, "%d  %s", id, name ? name : "");
}

static bool
uiIdComboBegin(const char *label, int *value, const char *name, bool *changed)
{
	char preview[96];
	uiIdComboText(preview, sizeof(preview), *value, name);
	if(!ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge))
		return false;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
	if(ImGui::InputInt(_("ID"), value, 0, 0)){
		*value = max(*value, 0);
		*changed = true;
	}
	ImGui::SameLine();
	if(ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##filter", _("search"), uiIdComboFilter, sizeof(uiIdComboFilter));
	ImGui::Separator();
	return true;
}

static void
uiIdComboItem(int id, const char *name, int *value, bool *changed)
{
	char item[128];
	uiIdComboText(item, sizeof(item), id, name);
	if(uiIdComboFilter[0] && !ImStristr(item, nil, uiIdComboFilter, nil))
		return;
	if(ImGui::Selectable(item, id == *value)){
		*value = id;
		*changed = true;
	}
}

static bool
uiNamedValueCombo(const char *label, int *value, const UiNamedValue *values, int numValues)
{
	const char *name = nil;
	for(int i = 0; i < numValues; i++)
		if(values[i].value == *value){
			name = _(values[i].name);
			break;
		}
	bool changed = false;
	if(uiIdComboBegin(label, value, name, &changed)){
		for(int i = 0; i < numValues; i++)
			uiIdComboItem(values[i].value, _(values[i].name), value, &changed);
		ImGui::EndCombo();
	}
	return changed;
}

static bool
uiSkinPicker(const char *label, int *skin)
{
	ObjectDef *obj = GetObjectDef(*skin);
	bool changed = false;
	// Skin 0 (CJ) has no model file and is drawn as skin 7.
	const char *name = *skin == 0 ? "CJ" : obj && obj->m_isPed ? obj->m_name : _("(no model)");
	if(uiIdComboBegin(label, skin, name, &changed)){
		// Game skins, then the server's AddCharModel skins (20001-30000).
		for(int i = 0; i < 30001; i = i == 399 ? 20001 : i + 1){
			ObjectDef *ped = GetObjectDef(i);
			if(ped && ped->m_isPed)
				uiIdComboItem(i, ped->m_name, skin, &changed);
		}
		ImGui::EndCombo();
	}
	return changed;
}

// Settings of a pickup, label, actor, class, gang zone or map icon.
static void uiStartSampPlacement(int kind, int model);

// An NPC path by name: the map's paths, or a new name typed in.
// Pawn colours are 0xRRGGBBAA.
static bool
uiPawnColorEdit(const char *label, uint32 *color)
{
	float c[4] = {
		((*color >> 24) & 0xFF) / 255.0f, ((*color >> 16) & 0xFF) / 255.0f,
		((*color >> 8) & 0xFF) / 255.0f, (*color & 0xFF) / 255.0f,
	};
	if(!ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar))
		return false;
	uint32 out = 0;
	for(int i = 0; i < 4; i++)
		out = (out << 8) | (uint32)clamp((int)(c[i] * 255.0f + 0.5f), 0, 255);
	*color = out;
	return true;
}

static bool
uiNameListCombo(const char *label, int *value, const char *const *names, int numNames)
{
	char preview[96];
	if(*value >= 0 && *value < numNames && names[*value][0])
		snprintf(preview, sizeof(preview), "%d  %s", *value, _(names[*value]));
	else
		snprintf(preview, sizeof(preview), "%d", *value);
	bool changed = false;
	if(ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge)){
		for(int i = 0; i < numNames; i++){
			if(names[i][0] == '\0')
				continue;
			char item[96];
			snprintf(item, sizeof(item), "%d  %s", i, _(names[i]));
			if(ImGui::Selectable(item, i == *value)){
				*value = i;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

// An NPC path by name: the map's paths, or a new name typed in.
static bool
uiNpcPathPicker(const char *label, char *pathName, size_t size, bool allowNone)
{
	std::vector<std::string> names;
	GetNpcPathNames(names);
	bool changed = false;
	if(ImGui::BeginCombo(label, pathName[0] ? pathName : _("None"))){
		if(allowNone && ImGui::Selectable(_("None"), pathName[0] == '\0')){
			pathName[0] = '\0';
			changed = true;
		}
		for(size_t i = 0; i < names.size(); i++){
			std::vector<ObjectInst*> points;
			GetNpcPathPoints(names[i].c_str(), points);
			char item[96];
			snprintf(item, sizeof(item), _("%s  (%d points)"), names[i].c_str(), (int)points.size());
			if(ImGui::Selectable(item, names[i] == pathName)){
				snprintf(pathName, size, "%s", names[i].c_str());
				changed = true;
			}
		}
		ImGui::Separator();
		static char newName[PAWN_OBJECT_NAME_LEN];
		ImGui::TextDisabled(_("New path"));
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool enter = ImGui::InputTextWithHint("##newpath", _("new path name"), newName, sizeof(newName),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::BeginDisabled(!IsValidPawnObjectName(newName));
		if((ImGui::Button(_("Use"), ImVec2(-FLT_MIN, 0)) || enter) && IsValidPawnObjectName(newName)){
			snprintf(pathName, size, "%s", newName);
			newName[0] = '\0';
			changed = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::EndCombo();
	}
	return changed;
}

static void
uiSelectNpcPath(const char *pathName)
{
	std::vector<ObjectInst*> points;
	GetNpcPathPoints(pathName, points);
	if(points.empty())
		return;
	ClearSelection();
	for(size_t i = 0; i < points.size(); i++)
		points[i]->Select();
}

// CreateDynamicRaceCP / SetPlayerRaceCheckpoint types.
static bool uiIsDirectory(const std::string &path);

static const char *uiRaceCheckpointTypes[] = {
	N_("0  Normal"), N_("1  Finish"), N_("2  Nothing"), N_("3  Air normal"), N_("4  Air finish"),
	N_("5  Air (rotates and stops)"), N_("6  Air (grows, shrinks, disappears)"),
	N_("7  Air (swings down and up)"), N_("8  Air (swings up and down)"),
	N_("9  Automatic (normal, finish at the end)"), N_("10  Automatic air"),
};

static void
uiSelectRace(const char *raceName)
{
	std::vector<ObjectInst*> race;
	GetRaceCheckpoints(raceName, race);
	if(race.empty())
		return;
	ClearSelection();
	for(size_t i = 0; i < race.size(); i++)
		race[i]->Select();
}

// Common PlayerPlaySound IDs, from the open.mp wiki's Sound IDs page. Any
// AudioEvents.txt ID works; these are the ones worth finding by name.
static const UiNamedValue uiSoundIds[] = {
	{ 1002, N_("Weak hit") }, { 1009, N_("Crash") }, { 1020, N_("Constant machinery noise") },
	{ 1021, N_("Machinery") }, { 1022, N_("Motor, stopping") }, { 1027, N_("Weaker hit") },
	{ 1035, N_("Garage door opening") }, { 1039, N_("Parachute opening") }, { 1052, N_("Blip A") },
	{ 1053, N_("Blip B") }, { 1056, N_("Race: green light") }, { 1057, N_("Race: red light (start)") },
	{ 1058, N_("Selection") }, { 1062, N_("Videogame music: Go Go Space Monkey") },
	{ 1068, N_("Videogame music: Duality") }, { 1076, N_("Videogame music: Let's Get Ready to Bumble") },
	{ 1083, N_("Map: place a marker") }, { 1084, N_("Map: remove a marker") }, { 1085, N_("Blip C") },
	{ 1095, N_("Loud hit") }, { 1097, N_("Background music") }, { 1100, N_("Metal fence rattle A") },
	{ 1101, N_("Metal fence rattle B") }, { 1130, N_("Punch A") }, { 1131, N_("Hit wooden object") },
	{ 1132, N_("Camera click") }, { 1133, N_("Vehicle component added (Transfender)") },
	{ 1134, N_("Spray can") }, { 1135, N_("Hit") }, { 1136, N_("Punch B") }, { 1137, N_("High blip") },
	{ 1140, N_("Crash A") }, { 1141, N_("Crash B") }, { 1142, N_("Stadium crowd") }, { 1144, N_("Splash") },
	{ 1145, N_("Throw a satchel") }, { 1147, N_("Car horn") }, { 1148, N_("Crash C") },
	{ 1150, N_("Selection B") }, { 1159, N_("Explosion") }, { 1163, N_("Two-feet stomp") },
	{ 1165, N_("Hangar doors") }, { 1183, N_("Driving school results music") },
	{ 1185, N_("Bike and boat school results music") }, { 1187, N_("Flight school results music") },
	{ 1190, N_("Slap") }, { 3200, N_("Air horn") }, { 3201, N_("Air horn (long)") },
	{ 3400, N_("Air conditioning") }, { 3401, N_("Ringing bell (continuous)") }, { 3600, N_("Calling tone") },
	{ 3800, N_("Videotape (continuous)") }, { 4200, N_("Bandit wheel start") }, { 4201, N_("Falling coins") },
	{ 4202, N_("Casino blip A") }, { 4203, N_("Casino blip B") }, { 4400, N_("Barber's electric razor") },
	{ 5200, N_("Continuous buzz") }, { 5201, N_("Bling") }, { 6000, N_("Blast door sliding") },
	{ 6001, N_("Repeating siren") }, { 6002, N_("Heavy door") }, { 6003, N_("Electricity") },
	{ 6200, N_("Seagulls") }, { 6400, N_("Door buzzer") }, { 6401, N_("Lift bell") },
	{ 135, N_("Ventilation (0.3.7-R2)") }, { 137, N_("Ammu-Nation interior (0.3.7-R2)") },
	{ 138, N_("Area 51 interior (0.3.7-R2)") }, { 140, N_("Disco, rock (0.3.7-R2)") },
	{ 143, N_("Marco's Bistro music (0.3.7-R2)") }, { 144, N_("Diner music (0.3.7-R2)") },
	{ 146, N_("Casino music (0.3.7-R2)") }, { 147, N_("Disco, electro (0.3.7-R2)") },
	{ 148, N_("Plane interior hum (0.3.7-R2)") }, { 151, N_("Fan (0.3.7-R2)") },
	{ 152, N_("Bar (0.3.7-R2)") }, { 157, N_("Disco, hip-hop (0.3.7-R2)") },
	{ 159, N_("Horse race (0.3.7-R2)") }, { 166, N_("Riot (0.3.7-R2)") }, { 167, N_("Rain (0.3.7-R2)") },
	{ 170, N_("Strip club (0.3.7-R2)") }, { 174, N_("Water (0.3.7-R2)") },
	{ 176, N_("SA intro music (0.3.7-R2)") }, { 182, N_("Mission passed theme (0.3.7-R2)") },
};

static const char*
uiSoundIdName(int id)
{
	for(size_t i = 0; i < IM_ARRAYSIZE(uiSoundIds); i++)
		if(uiSoundIds[i].value == id)
			return _(uiSoundIds[i].name);
	return nil;
}

static bool
uiSoundIdPicker(const char *label, int *value)
{
	bool changed = false;
	const char *name = uiSoundIdName(*value);
	if(uiIdComboBegin(label, value, name ? name : _("other sound"), &changed)){
		for(size_t i = 0; i < IM_ARRAYSIZE(uiSoundIds); i++)
			uiIdComboItem(uiSoundIds[i].value, _(uiSoundIds[i].name), value, &changed);
		ImGui::EndCombo();
	}
	return changed;
}

// The vehicle list for an NPC: "infernus #50774", and who already sits in a seat.
static void
uiVehicleLabel(const ObjectInst *vehicle, char *out, size_t size)
{
	ObjectDef *obj = GetObjectDef(vehicle->m_objectId);
	const ObjectPawnProperties *properties = GetObjectPawnProperties(vehicle);
	if(properties->name[0])
		snprintf(out, size, "%s (%s)", properties->name, obj ? obj->m_name : _("vehicle"));
	else
		snprintf(out, size, "%s #%d", obj ? obj->m_name : _("vehicle"), vehicle->m_id);
}

static const char *uiNpcSeats[] = { N_("Driver"), N_("Front passenger"), N_("Back left"), N_("Back right") };

// A menu of the map's vehicles; true and *picked when one is chosen.
static bool
uiVehicleMenuItems(const ObjectInst *current, ObjectInst **picked)
{
	bool any = false, chosen = false;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *vehicle = (ObjectInst*)p->item;
		if(!vehicle->m_isVehicle || vehicle->m_isDeleted)
			continue;
		any = true;
		char label[128];
		uiVehicleLabel(vehicle, label, sizeof(label));
		ImGui::PushID(vehicle->m_id);
		if(ImGui::Selectable(label, vehicle == current)){
			*picked = vehicle;
			chosen = true;
		}
		ImGui::PopID();
	}
	if(!any)
		ImGui::TextDisabled(_("No vehicles in the map yet."));
	return chosen;
}

// NPC_PutInVehicle: the vehicle and seat, picked from a list or from the
// selection (select the NPC and a vehicle).
static bool
uiNpcVehicle(ObjectInst *inst, ObjectPawnProperties &edited)
{
	bool changed = false;
	ObjectInst *vehicle = edited.npcVehicle >= 0 ? GetInstanceByID(edited.npcVehicle) : nil;
	if(vehicle && (vehicle->m_isDeleted || !vehicle->m_isVehicle))
		vehicle = nil;
	char preview[128];
	if(vehicle)
		uiVehicleLabel(vehicle, preview, sizeof(preview));
	else
		snprintf(preview, sizeof(preview), "%s", edited.npcVehicle >= 0 ? _("(deleted vehicle)") : _("On foot"));
	if(ImGui::BeginCombo(_("In vehicle"), preview, ImGuiComboFlags_HeightLarge)){
		if(ImGui::Selectable(_("On foot"), edited.npcVehicle < 0)){
			edited.npcVehicle = -1;
			changed = true;
		}
		ObjectInst *picked = nil;
		if(uiVehicleMenuItems(vehicle, &picked)){
			edited.npcVehicle = picked->m_id;
			changed = true;
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip(_("NPC_PutInVehicle, written once the NPC and the vehicle exist.\n"
		"Select the NPC and a vehicle together to pick it from the world."));
	// The other half of a two-instance selection, when it is a vehicle.
	ObjectInst *other = nil;
	int numSelected = 0;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *selected = (ObjectInst*)p->item;
		if(selected->m_isDeleted)
			continue;
		numSelected++;
		if(selected != inst)
			other = selected;
	}
	if(numSelected == 2 && other && other->m_isVehicle && other != vehicle){
		char label[160], name[128];
		uiVehicleLabel(other, name, sizeof(name));
		snprintf(label, sizeof(label), _("Put in %s"), name);
		if(ImGui::SmallButton(label)){
			edited.npcVehicle = other->m_id;
			changed = true;
		}
	}
	if(edited.npcVehicle >= 0){
		if(UiCombo(_("Seat"), &edited.npcSeat, uiNpcSeats, IM_ARRAYSIZE(uiNpcSeats))) changed = true;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *npc = (ObjectInst*)p->item;
			if(npc != inst && !npc->m_isDeleted && GetNpcVehicle(npc) == vehicle && vehicle &&
			   npc->m_pawnProperties->npcSeat == edited.npcSeat){
				ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("%s already sits there."),
					npc->m_pawnProperties->npcName);
				break;
			}
		}
		if(vehicle && ImGui::SmallButton(_("Select vehicle"))){
			ClearSelection();
			vehicle->Select();
		}
	}
	return changed;
}

static const char *uiNpcMoveTypes[] = { N_("None"), N_("Walk"), N_("Jog"), N_("Sprint"), N_("Drive"), N_("Auto") };

// ApplyActorAnimation / NPC_ApplyAnimation: a library, then one of its
// animations from a searchable list. Picking one plays it in the viewport.
static bool
uiAnimationPicker(ObjectInst *inst, ObjectPawnProperties &edited)
{
	bool changed = false;
	ImGui::PushID("animation");
	static int libInstance = -1, shownLib = -1;
	static char filter[PAWN_ANIM_NAME_LEN];
	if(libInstance != inst->m_id){
		libInstance = inst->m_id;
		shownLib = edited.animLib[0] ? AnimFindLibrary(edited.animLib) : -1;
		filter[0] = '\0';
	}
	int numLibs = AnimNumLibraries();
	if(numLibs == 0){
		ImGui::TextDisabled(_("No animation libraries found (anim/ped.ifp, anim/anim.img)."));
		ImGui::PopID();
		return false;
	}
	if(ImGui::BeginCombo(_("Library"), shownLib >= 0 ? AnimLibraryName(shownLib) : _("Pick a library"),
		ImGuiComboFlags_HeightLarge)){
		for(int lib = 0; lib < numLibs; lib++)
			if(ImGui::Selectable(AnimLibraryName(lib), lib == shownLib))
				shownLib = lib;
		ImGui::EndCombo();
	}
	ImGui::InputTextWithHint("##filter", _("search animations"), filter, sizeof(filter));
	if(shownLib >= 0 &&
	   ImGui::BeginListBox("##names", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 8.0f))){
		int count = AnimNumInLibrary(shownLib);
		for(int i = 0; i < count; i++){
			const char *name = AnimName(shownLib, i);
			if(filter[0] && !ImStristr(name, nil, filter, nil))
				continue;
			bool current = rw::strcmp_ci(edited.animName, name) == 0 &&
				rw::strcmp_ci(edited.animLib, AnimLibraryName(shownLib)) == 0;
			char item[64];
			snprintf(item, sizeof(item), "%s  (%.1f s)", name, AnimDuration(shownLib, i));
			// Shown dim: a skin can't play it, or it twists the skin.
			int problem = AnimPedProblem(AnimLibraryName(shownLib), name);
			if(problem != ANIM_PED_OK)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			bool picked = ImGui::Selectable(item, current);
			if(problem != ANIM_PED_OK){
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip("%s", problem == ANIM_PED_OBJECT_ONLY ?
					_("Moves only an object's parts: a skin stands still.") : _("Twists the skin out of shape."));
			}
			if(picked && !current){
				if(!edited.animName[0]){
					// First pick: ApplyActorAnimation's usual values.
					edited.animDelta = 4.1f;
					edited.animLoop = true;
					edited.animLockX = edited.animLockY = edited.animFreeze = false;
					edited.animTime = 0;
				}
				snprintf(edited.animLib, sizeof(edited.animLib), "%s", AnimLibraryName(shownLib));
				snprintf(edited.animName, sizeof(edited.animName), "%s", name);
				changed = true;
			}
		}
		ImGui::EndListBox();
	}
	if(edited.animName[0]){
		ImGui::Text(_("Plays %s / %s"), edited.animLib, edited.animName);
		ImGui::SameLine();
		if(ImGui::SmallButton(_("None"))){
			edited.animLib[0] = edited.animName[0] = '\0';
			changed = true;
		}
		int problem = AnimPedProblem(edited.animLib, edited.animName);
		if(problem != ANIM_PED_OK)
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", problem == ANIM_PED_OBJECT_ONLY ?
				_("Moves only an object's parts: a skin stands still.") : _("Twists the skin out of shape."));
		if(ImGui::InputFloat(_("Delta"), &edited.animDelta, 0.1f, 1.0f, "%.2f")) changed = true;
		ImGui::SetItemTooltip(_("Playback speed in game (4.1 is normal). The editor plays at normal speed."));
		if(ImGui::Checkbox(_("Loop"), &edited.animLoop)) changed = true;
		ImGui::SameLine();
		if(ImGui::Checkbox(_("Lock X"), &edited.animLockX)) changed = true;
		ImGui::SameLine();
		if(ImGui::Checkbox(_("Lock Y"), &edited.animLockY)) changed = true;
		ImGui::SameLine();
		if(ImGui::Checkbox(_("Freeze"), &edited.animFreeze)) changed = true;
		ImGui::SetItemTooltip(_("Hold the last frame when it ends."));
		if(ImGui::InputInt(_("Time (ms)"), &edited.animTime, 100, 1000)){
			edited.animTime = max(edited.animTime, 0);
			changed = true;
		}
		ImGui::SetItemTooltip(_("How long it plays; 0 plays until stopped."));
	}else
		ImGui::TextDisabled(_("None: the skin stands still."));
	ImGui::PopID();
	return changed;
}

static bool
uiSampElementSettings(ObjectInst *inst, ObjectPawnProperties &edited)
{
	bool changed = false;
	switch(edited.elementKind){
	case SAMP_ELEMENT_PICKUP:
		if(uiNamedValueCombo(_("Model"), &edited.elementModel, uiPickupModels, IM_ARRAYSIZE(uiPickupModels)))
			changed = true;
		if(inst->m_objectId != edited.elementModel)
			ImGui::TextDisabled(_("Model %d is not loaded; the pickup is drawn as model %d."),
				edited.elementModel, inst->m_objectId);
		if(uiNamedValueCombo(_("Type"), &edited.pickupType, uiPickupTypes, IM_ARRAYSIZE(uiPickupTypes)))
			changed = true;
		break;
	case SAMP_ELEMENT_LABEL:
		if(ImGui::InputTextMultiline(_("Text"), edited.labelText, sizeof(edited.labelText),
			ImVec2(0.0f, ImGui::GetTextLineHeight() * 4.0f)))
			changed = true;
		ImGui::SetItemTooltip(_("{RRGGBB} changes the colour from there on, as in game."));
		if(uiPawnColorEdit(_("Colour"), &edited.labelColor)) changed = true;
		if(ImGui::InputFloat(_("Draw distance"), &edited.drawDistance, 1.0f, 10.0f, "%.1f")) changed = true;
		if(ImGui::Checkbox(_("Hide behind walls (test LOS)"), &edited.labelTestLOS)) changed = true;
		break;
	case SAMP_ELEMENT_ACTOR:
		if(uiSkinPicker(_("Skin"), &edited.elementModel)) changed = true;
		if(ImGui::CollapsingHeader(L("Advanced###actor_advanced"))){
			if(ImGui::Checkbox(_("Invulnerable"), &edited.actorInvulnerable)) changed = true;
			if(ImGui::InputFloat(_("Health"), &edited.actorHealth, 1.0f, 10.0f, "%.1f")) changed = true;
		}
		if(ImGui::CollapsingHeader(L("Animation###actor-animation"),
		   edited.animName[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			if(uiAnimationPicker(inst, edited)) changed = true;
		break;
	case SAMP_ELEMENT_CLASS: {
		if(uiSkinPicker(_("Skin"), &edited.elementModel)) changed = true;
		bool hasTeam = edited.classTeam >= 0;
		if(ImGui::Checkbox(_("Team (AddPlayerClassEx)"), &hasTeam)){
			edited.classTeam = hasTeam ? 0 : -1;
			changed = true;
		}
		if(hasTeam){
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
			if(ImGui::InputInt("##team", &edited.classTeam)){
				edited.classTeam = clamp(edited.classTeam, 0, 255);
				changed = true;
			}
		}
		for(int w = 0; w < PAWN_CLASS_WEAPONS; w++){
			ImGui::PushID(w);
			char label[32];
			snprintf(label, sizeof(label), _("Weapon %d"), w + 1);
			ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.65f);
			if(uiNameListCombo("##weapon", &edited.classWeapons[w], uiWeaponNames, IM_ARRAYSIZE(uiWeaponNames)))
				changed = true;
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.35f - ImGui::GetStyle().ItemSpacing.x);
			ImGui::BeginDisabled(edited.classWeapons[w] == 0);
			if(ImGui::InputInt("##ammo", &edited.classAmmo[w], 0, 0)) changed = true;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextUnformatted(label);
			ImGui::PopID();
		}
		break;
	}
	case SAMP_ELEMENT_NPC: {
		UiBadge(_("open.mp only"));
		ImGui::SetItemTooltip(_("Saved as NPC_Create and NPC_Spawn. SA-MP 0.3.7 servers have no\n"
			"NPC natives; there NPCs are separate npcmodes scripts."));
		// The name buffer is per NPC so an invalid name can be typed out.
		static int npcNameInstance = -1;
		static bool npcNameEditing;
		static char npcNameBuffer[PAWN_NPC_NAME_LEN];
		if(npcNameInstance != inst->m_id || (!npcNameEditing && strcmp(npcNameBuffer, edited.npcName) != 0)){
			strncpy(npcNameBuffer, edited.npcName, sizeof(npcNameBuffer));
			npcNameInstance = inst->m_id;
		}
		ImGui::InputText(_("Name"), npcNameBuffer, sizeof(npcNameBuffer));
		npcNameEditing = ImGui::IsItemActive();
		ImGui::SetItemTooltip(_("Its player name, shown over its head and in the player list."));
		ObjectInst *nameOwner = IsValidNpcName(npcNameBuffer) ? FindNpcByName(npcNameBuffer, inst) : nil;
		if(!IsValidNpcName(npcNameBuffer))
			ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR),
				_("3-24 characters: letters, digits and [ ] ( ) $ @ . _ ="));
		else if(nameOwner)
			ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("Another NPC is already called that."));
		else if(strcmp(npcNameBuffer, edited.npcName) != 0){
			strncpy(edited.npcName, npcNameBuffer, sizeof(edited.npcName));
			changed = true;
		}
		if(uiSkinPicker(_("Skin"), &edited.elementModel)) changed = true;
		ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.65f);
		if(uiNameListCombo("##npcweapon", &edited.npcWeapon, uiWeaponNames, IM_ARRAYSIZE(uiWeaponNames)))
			changed = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.35f - ImGui::GetStyle().ItemSpacing.x);
		ImGui::BeginDisabled(edited.npcWeapon == 0);
		if(ImGui::InputInt("##npcammo", &edited.npcAmmo, 0, 0)) changed = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextUnformatted(_("Weapon"));
		if(ImGui::InputFloat(_("Health"), &edited.actorHealth, 1.0f, 10.0f, "%.1f")) changed = true;
		if(ImGui::InputFloat(_("Armour"), &edited.npcArmour, 1.0f, 10.0f, "%.1f")) changed = true;
		if(ImGui::Checkbox(_("Invulnerable"), &edited.actorInvulnerable)) changed = true;
		if(ImGui::CollapsingHeader(L("Animation###npc-animation"),
		   edited.animName[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			if(uiAnimationPicker(inst, edited)) changed = true;
		if(ImGui::CollapsingHeader(L("Vehicle###npc-vehicle"),
		   edited.npcVehicle >= 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			if(uiNpcVehicle(inst, edited)) changed = true;

		// One thing at a time: open.mp stops a path when a node or a
		// recording starts, so the choice is explicit.
		enum { NPC_STANDS, NPC_FOLLOWS_PATH, NPC_PLAYS_NODE, NPC_PLAYS_RECORDING };
		static const char *npcModes[] = { N_("Stand still"), N_("Follow a path"), N_("Walk the nodes"),
			N_("Play a recording") };
		int mode = edited.npcRecording[0] ? NPC_PLAYS_RECORDING : edited.npcPlaysNode ? NPC_PLAYS_NODE :
			edited.pathName[0] ? NPC_FOLLOWS_PATH : NPC_STANDS;
		bool showMovement = ImGui::CollapsingHeader(L("Movement###npc-movement"),
			mode != NPC_STANDS ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		if(showMovement){
			static int pendingModeInstance = -1, pendingMode;
			if(pendingModeInstance == inst->m_id && mode == NPC_STANDS)
				mode = pendingMode;	// chosen, but nothing picked for it yet
			else
				pendingModeInstance = -1;
			if(UiCombo(_("Does"), &mode, npcModes, IM_ARRAYSIZE(npcModes))){
				if(mode != NPC_FOLLOWS_PATH)
					edited.pathName[0] = '\0';
				if(mode != NPC_PLAYS_RECORDING){
					edited.npcRecording[0] = '\0';
					edited.npcRecordingAtNpc = false;
				}
				edited.npcPlaysNode = mode == NPC_PLAYS_NODE;
				pendingModeInstance = inst->m_id;
				pendingMode = mode;
				changed = true;
			}
			if(mode == NPC_FOLLOWS_PATH || mode == NPC_PLAYS_NODE){
				if(UiCombo(_("Move"), &edited.npcMoveType, uiNpcMoveTypes, IM_ARRAYSIZE(uiNpcMoveTypes)))
					changed = true;
				ImGui::SetItemTooltip(_("Drive needs the NPC in a vehicle."));
				bool autoSpeed = edited.npcMoveSpeed < 0.0f;
				if(ImGui::Checkbox(_("Speed for the move type"), &autoSpeed)){
					edited.npcMoveSpeed = autoSpeed ? -1.0f : 0.56444f;
					changed = true;
				}
				if(!autoSpeed){
					ImGui::SameLine();
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
					if(ImGui::InputFloat("##speed", &edited.npcMoveSpeed, 0.05f, 0.1f, "%.3f")){
						edited.npcMoveSpeed = max(edited.npcMoveSpeed, 0.0f);
						changed = true;
					}
				}
			}
			if(mode == NPC_FOLLOWS_PATH || mode == NPC_PLAYS_NODE){
				bool previewing = NpcPreviewIsActive(inst);
				if(ImGui::SmallButton(previewing ? TI(ICON_FA_STOP, "Stop preview") : TI(ICON_FA_PLAY, "Preview movement"))){
					if(previewing)
						NpcPreviewStop(inst);
					else if(!NpcPreviewStart(inst))
						ToastFor(TOAST_SELECTION, 3.0f, _("Nothing to walk yet: pick a path with points, or a node."));
				}
				ImGui::SetItemTooltip(_("Walks the NPC in the viewport the way open.mp moves it.\n"
					"It goes back to its place when the preview stops."));
			}
			if(mode == NPC_FOLLOWS_PATH){
				if(uiNpcPathPicker(_("Path"), edited.pathName, sizeof(edited.pathName), false)) changed = true;
				ImGui::SetItemTooltip(_("NPC_MoveByPath: once spawned, the NPC moves along this path once."));
				std::vector<ObjectInst*> points;
				GetNpcPathPoints(edited.pathName, points);
				if(!edited.pathName[0])
					ImGui::TextDisabled(_("Pick a path, or type a new name and place its points."));
				else if(points.empty())
					ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
						_("No points yet. Choose Paths in the browser to add them."));
				if(ImGui::Checkbox(_("Last point first (reversed)"), &edited.npcPathReversed)) changed = true;
				static const char *repeatModes[] = { N_("Once"), N_("Loop"), N_("Back and forth") };
				if(UiCombo(_("Repeat"), &edited.npcPathRepeat, repeatModes, IM_ARRAYSIZE(repeatModes))) changed = true;
				ImGui::SetItemTooltip(_("open.mp's NPC_MoveByPath walks the path once. Loop and Back and forth save\n"
					"a <map>_OnNPCFinishMovePath(npcid, pathid) helper: call it from your\n"
					"OnNPCFinishMovePath and the NPC starts the path again (reversed each time\n"
					"for back and forth)."));
				if(!points.empty() && ImGui::SmallButton(_("Select the path's points")))
					uiSelectNpcPath(edited.pathName);
			}else if(mode == NPC_PLAYS_NODE){
				if(ImGui::InputInt(_("Node file"), &edited.npcNode)){
					edited.npcNode = clamp(edited.npcNode, 0, 63);
					changed = true;
				}
				ImGui::SetItemTooltip(_("NODES%d.DAT in the server's scriptfiles/NPCs/nodes."), edited.npcNode);
				if(ImGui::InputInt(_("Start point"), &edited.npcNodePoint)){
					edited.npcNodePoint = clamp(edited.npcNodePoint, 0, 65535);
					changed = true;
				}
				ImGui::SetItemTooltip(_("NPC_SetNodePoint. The NPC is moved onto this point and walks on from it."));
				int area, point;
				bool vehicle;
				rw::V3d nodePos;
				bool haveNode = SAPaths::selectedNode &&
					SAPaths::GetNodeInfo(SAPaths::selectedNode, &area, &point, &vehicle, &nodePos);
				ImGui::BeginDisabled(!haveNode);
				if(ImGui::SmallButton(_("Use the selected node"))){
					edited.npcNode = area;
					edited.npcNodePoint = point;
					changed = true;
				}
				ImGui::EndDisabled();
				if(haveNode)
					ImGui::SetItemTooltip(_("NODES%d.DAT point %d"), area, point);
				else
					ImGui::SetItemTooltip(_("Select a node in the Nodes window (Windows > Nodes) first."));
				if(ImGui::InputFloat(_("Radius"), &edited.npcNodeRadius, 0.1f, 1.0f, "%.2f m")) changed = true;
				ImGui::SetItemTooltip(_("Random sideways offset from the node line, so NPCs do not walk in single file."));
				if(ImGui::Checkbox(_("Face the way it walks"), &edited.npcNodeSetAngle)) changed = true;
			}else if(mode == NPC_PLAYS_RECORDING){
				// A driver recording moves the NPC's vehicle; the NPC rides along.
				ObjectInst *vehicle = edited.npcVehicle >= 0 ? GetInstanceByID(edited.npcVehicle) : nil;
				if(vehicle && (vehicle->m_isDeleted || !vehicle->m_isVehicle))
					vehicle = nil;
				bool driving = vehicle && edited.npcSeat == 0;
				if(uiRecPlayer(driving ? vehicle : inst, edited.npcRecording, sizeof(edited.npcRecording))) changed = true;
				int recType = RecTypeOfName(edited.npcRecording);
				if(recType == REC_DRIVER && !driving){
					if(ImGui::SmallButton(_("Use a vehicle...")))
						ImGui::OpenPopup("##recvehicle");
					ImGui::SetItemTooltip(_("Puts the NPC in a vehicle as its driver."));
					if(ImGui::BeginPopup("##recvehicle")){
						ObjectInst *picked = nil;
						if(uiVehicleMenuItems(vehicle, &picked)){
							edited.npcVehicle = picked->m_id;
							edited.npcSeat = 0;
							changed = true;
						}
						ImGui::EndPopup();
					}
				}else if(recType == REC_ONFOOT && vehicle){
					ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("An on-foot recording: the NPC has to be on foot."));
					if(ImGui::SmallButton(_("Get out"))){
						edited.npcVehicle = -1;
						changed = true;
					}
				}
				if(edited.npcRecording[0]){
					if(ImGui::Checkbox(_("Start the route at the NPC"), &edited.npcRecordingAtNpc)) changed = true;
					ImGui::SetItemTooltip(_("Off plays it where it was recorded. On moves the whole route so it\n"
						"starts where the NPC stands (NPC_StartPlayback's start position)."));
				}
			}
		}
		break;
	}
	case SAMP_ELEMENT_PATHPOINT: {
		UiBadge(_("open.mp only"));
		ImGui::SetItemTooltip(_("Saved as NPC_CreatePath and NPC_AddPointToPath."));
		if(uiNpcPathPicker(_("Path"), edited.pathName, sizeof(edited.pathName), false)) changed = true;
		ImGui::SetItemTooltip(_("The path's Pawn variable. NPCs follow it by this name."));
		std::vector<ObjectInst*> points;
		GetNpcPathPoints(edited.pathName, points);
		int position = 0;
		for(size_t i = 0; i < points.size(); i++)
			if(points[i] == inst)
				position = (int)i + 1;
		if(ImGui::InputInt(_("Order"), &edited.pathOrder)){
			edited.pathOrder = max(edited.pathOrder, 0);
			changed = true;
		}
		ImGui::SetItemTooltip(_("Point %d of %d. Points are walked from the lowest order up."),
			position, (int)points.size());
		if(ImGui::InputFloat(_("Stop range"), &edited.pathStopRange, 0.1f, 1.0f, "%.2f m")) changed = true;
		ImGui::SetItemTooltip(_("How close the NPC has to get before it heads for the next point."));
		if(ImGui::SmallButton(_("Select whole path")))
			uiSelectNpcPath(edited.pathName);
		ImGui::SameLine();
		if(ImGui::SmallButton(TI(ICON_FA_PLUS, "Add points"))){
			SetSpawnPathName(edited.pathName);
			uiStartSampPlacement(SAMP_ELEMENT_PATHPOINT, GetSampElementProxyModel(SAMP_ELEMENT_PATHPOINT));
		}
		ImGui::SetItemTooltip(_("Click in the world to add points to the end of this path.\n"
			"Shift+click keeps placing."));
		break;
	}
	case SAMP_ELEMENT_CHECKPOINT: {
		UiBadge(_("streamer only"));
		ImGui::SetItemTooltip(_("Saved as CreateDynamicCP / CreateDynamicRaceCP."));
		if(gPawnSaveFormat != PAWN_CODE_STREAMER){
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Not saved: this map is native code."));
			ImGui::SetItemTooltip(_("Native code has no map checkpoints: a player has one checkpoint at a\n"
				"time there (SetPlayerCheckpoint / SetPlayerRaceCheckpoint). Save the map\n"
				"as streamer code to keep them."));
		}
		static const char *cpKinds[] = { N_("Checkpoint"), N_("Race checkpoint") };
		if(UiCombo(_("Kind"), &edited.cpKind, cpKinds, IM_ARRAYSIZE(cpKinds))){
			if(edited.cpKind == SAMP_CHECKPOINT_RACE && edited.cpRace[0] == '\0'){
				snprintf(edited.cpRace, sizeof(edited.cpRace), "%s",
					GetSpawnRaceName()[0] ? GetSpawnRaceName() : "race");
				std::vector<ObjectInst*> race;
				GetRaceCheckpoints(edited.cpRace, race);
				edited.cpOrder = race.empty() ? 0 : GetObjectPawnProperties(race.back())->cpOrder + 1;
			}
			changed = true;
		}
		if(ImGui::InputFloat(_("Size"), &edited.cpSize, 0.5f, 2.0f, "%.1f")){
			edited.cpSize = max(edited.cpSize, 0.0f);
			changed = true;
		}
		ImGui::SetItemTooltip(_("The checkpoint's size; players enter it within about this distance."));
		if(edited.cpKind == SAMP_CHECKPOINT_RACE){
			if(UiCombo(_("Type"), &edited.cpRaceType, uiRaceCheckpointTypes, IM_ARRAYSIZE(uiRaceCheckpointTypes)))
				changed = true;
			ImGui::SetItemTooltip(_("Normal shows an arrow to the next checkpoint; Finish a flag.\n"
				"The Air types are rings for planes. Automatic is Normal until the race's end, then Finish."));
			char race[PAWN_OBJECT_NAME_LEN];
			snprintf(race, sizeof(race), "%s", edited.cpRace);
			if(ImGui::InputText(_("Race"), race, sizeof(race)) && IsValidPawnObjectName(race)){
				// Joining another race puts it on that race's end.
				std::vector<ObjectInst*> joined;
				GetRaceCheckpoints(race, joined);
				edited.cpOrder = 0;
				for(size_t i = 0; i < joined.size(); i++)
					if(joined[i] != inst)
						edited.cpOrder = max(edited.cpOrder, GetObjectPawnProperties(joined[i])->cpOrder + 1);
				snprintf(edited.cpRace, sizeof(edited.cpRace), "%s", race);
				changed = true;
			}
			ImGui::SetItemTooltip(_("Checkpoints of one race point at each other in order."));
			std::vector<ObjectInst*> points;
			GetRaceCheckpoints(edited.cpRace, points);
			int position = 0;
			for(size_t i = 0; i < points.size(); i++)
				if(points[i] == inst)
					position = (int)i + 1;
			if(ImGui::InputInt(_("Order"), &edited.cpOrder)){
				edited.cpOrder = max(edited.cpOrder, 0);
				changed = true;
			}
			ImGui::SetItemTooltip(_("Checkpoint %d of %d. Each points at the next one up; the last at none."),
				position, (int)points.size());
			if(position == (int)points.size() && edited.cpRaceType != 1 && edited.cpRaceType != 4 &&
			   edited.cpRaceType < SAMP_RACE_CP_AUTO_GROUND && edited.cpRaceMode != SAMP_RACE_CIRCUIT)
				ImGui::TextDisabled(_("The last checkpoint: Finish (or Air finish) shows a flag."));

			// The whole race's settings, on every one of its checkpoints.
			bool raceChanged = false;
			static const char *raceModes[] = { N_("Point to point"), N_("Circuit") };
			if(UiCombo(_("Route"), &edited.cpRaceMode, raceModes, IM_ARRAYSIZE(raceModes))){
				if(edited.cpRaceMode == SAMP_RACE_CIRCUIT && edited.cpRaceLaps < 1)
					edited.cpRaceLaps = 3;
				raceChanged = true;
			}
			ImGui::SetItemTooltip(_("Circuit: the last checkpoint leads back to the first, lap after lap."));
			if(edited.cpRaceMode == SAMP_RACE_CIRCUIT && ImGui::InputInt(_("Laps"), &edited.cpRaceLaps)){
				edited.cpRaceLaps = clamp(edited.cpRaceLaps, 1, (int)SAMP_RACE_MAX_LAPS);
				raceChanged = true;
			}
			if(ImGui::Checkbox(_("Playable race"), &edited.cpRacePlayable))
				raceChanged = true;
			ImGui::SetItemTooltip(_("Saved for haumea_races.inc: each player races on their own with\n"
				"StartMapRace, seeing one checkpoint at a time, with laps and times.\n"
				"Works without the streamer. Off: every checkpoint is a fixed marker."));
			if(raceChanged){
				SetRaceSettings(edited.cpRace, edited.cpRaceMode, edited.cpRaceLaps, edited.cpRacePlayable);
				changed = true;
			}
			if(edited.cpRacePlayable){
				std::string include = WorkspacePath("qawno/include");
				if(include.empty() || !uiIsDirectory(include))
					include = WorkspacePath("pawno/include");
				ImGui::BeginDisabled(include.empty() || !uiIsDirectory(include));
				if(ImGui::SmallButton(_("Save haumea_races.inc to the workspace"))){
					std::string path = include + "/haumea_races.inc";
					if(WriteHaumeaRacesInclude(path.c_str()))
						Toast(TOAST_SAVE, _("Saved %s"), path.c_str());
					else
						Toast(TOAST_SAVE, _("Could not write %s"), path.c_str());
				}
				ImGui::EndDisabled();
				ImGui::SetItemTooltip(_("Into the workspace's qawno/include (or pawno/include), where pawncc finds it."));
			}

			// Driving it in the editor.
			bool previewing = RacePreviewActive() && strcmp(RacePreviewRace(), edited.cpRace) == 0;
			auto frameTarget = [&](){
				std::vector<ObjectInst*> race;
				GetRaceCheckpoints(RacePreviewRace(), race);
				int target = RacePreviewTarget();
				if(target < 0 || target >= (int)race.size())
					return;
				rw::V3d d = sub(race[target]->m_translation, TheCamera.m_target);
				TheCamera.m_target = add(TheCamera.m_target, d);
				TheCamera.m_position = add(TheCamera.m_position, d);
			};
			if(!previewing){
				if(ImGui::SmallButton(TI(ICON_FA_PLAY, "Preview race")) && StartRacePreview(edited.cpRace))
					frameTarget();
				ImGui::SetItemTooltip(_("Shows the race as a player drives it: one checkpoint at a time."));
			}else{
				if(RacePreviewLaps() > 1)
					ImGui::Text(_("Lap %d of %d, checkpoint %d of %d"), RacePreviewLap(), RacePreviewLaps(),
						RacePreviewTarget() + 1, RacePreviewCount());
				else
					ImGui::Text(_("Checkpoint %d of %d"), RacePreviewTarget() + 1, RacePreviewCount());
				if(ImGui::SmallButton(_("Next checkpoint"))){
					AdvanceRacePreview();
					if(RacePreviewActive())
						frameTarget();
					else
						Toast(TOAST_SELECTION, _("Race finished"));
				}
				ImGui::SameLine();
				if(ImGui::SmallButton(TI(ICON_FA_STOP, "Stop preview")))
					StopRacePreview();
			}
			if(ImGui::SmallButton(_("Select whole race")))
				uiSelectRace(edited.cpRace);
			ImGui::SameLine();
			if(ImGui::SmallButton(TI(ICON_FA_PLUS, "Add checkpoints"))){
				SetSpawnRaceName(edited.cpRace);
				uiStartSampPlacement(SAMP_ELEMENT_CHECKPOINT, GetSampElementProxyModel(SAMP_ELEMENT_CHECKPOINT));
			}
			ImGui::SetItemTooltip(_("Click in the world to add checkpoints to the end of this race.\n"
				"Shift+click keeps placing."));
		}
		break;
	}
	case SAMP_ELEMENT_AREA: {
		UiBadge(_("streamer only"));
		ImGui::SetItemTooltip(_("Saved as CreateDynamicCircle .. CreateDynamicPolygon. Streamer items\n"
			"limited to it are shown only to players inside it."));
		if(gPawnSaveFormat != PAWN_CODE_STREAMER){
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Not saved: this map is native code."));
			ImGui::SetItemTooltip(_("Native code has no dynamic areas. Save the map as streamer code to keep\n"
				"them, and the items limited to them."));
		}
		static const char *shapes[] = { N_("Circle"), N_("Cylinder"), N_("Sphere"), N_("Rectangle"), N_("Cuboid"), N_("Cube"), N_("Polygon") };
		if(UiCombo(_("Shape"), &edited.areaShape, shapes, IM_ARRAYSIZE(shapes))){
			if(edited.areaShape == SAMP_AREA_POLYGON && edited.areaPointCount == 0){
				// A square to start from, the size of the box or circle it was.
				float h = edited.areaSize.x > 0.0f ? edited.areaSize.x * 0.5f : max(edited.areaRadius, 5.0f);
				float square[4][2] = { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
				memcpy(edited.areaPoints, square, sizeof(square));
				edited.areaPointCount = 4;
			}
			changed = true;
		}
		ImGui::SetItemTooltip(_("Circle and rectangle cover every height. The others go up from where the\n"
			"area stands (a sphere is around it)."));
		switch(edited.areaShape){
		case SAMP_AREA_CIRCLE: case SAMP_AREA_CYLINDER: case SAMP_AREA_SPHERE:
			if(ImGui::DragFloat(_("Radius"), &edited.areaRadius, 0.25f, 0.5f, 20000.0f, "%.1f m")) changed = true;
			break;
		case SAMP_AREA_RECTANGLE:
			if(ImGui::DragFloat2(_("Size (X, Y)"), &edited.areaSize.x, 0.25f, 0.5f, 40000.0f, "%.1f m")) changed = true;
			break;
		case SAMP_AREA_CUBOID: case SAMP_AREA_CUBE:
			if(ImGui::DragFloat3(_("Size (X, Y, Z)"), &edited.areaSize.x, 0.25f, 0.5f, 40000.0f, "%.1f m")) changed = true;
			break;
		}
		if(edited.areaShape == SAMP_AREA_CYLINDER || edited.areaShape == SAMP_AREA_POLYGON){
			if(ImGui::DragFloat(_("Height"), &edited.areaHeight, 0.25f, 0.0f, 20000.0f, "%.1f m")) changed = true;
			if(edited.areaShape == SAMP_AREA_POLYGON)
				ImGui::SetItemTooltip(_("Up from where the area stands. 0 covers every height."));
		}
		if(edited.areaShape == SAMP_AREA_POLYGON){
			ImGui::Text(_("%d corner(s)"), edited.areaPointCount);
			ImGui::SameLine();
			bool picking = IsAreaPointPicking(inst);
			if(ImGui::SmallButton(picking ? _("Stop adding") : TI(ICON_FA_PLUS, "Add corners"))){
				if(picking)
					StopAreaPointPicking();
				else
					StartAreaPointPicking(inst);
			}
			ImGui::SetItemTooltip(_("Click in the world to add a corner; Shift+click keeps adding.\n"
				"Esc or a right click stops. Drag a corner's handle to move it."));
			ImGui::SameLine();
			if(ImGui::SmallButton(_("Remove last")) && edited.areaPointCount > 0){
				edited.areaPointCount--;
				changed = true;
			}
			if(edited.areaPointCount < 3)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("A polygon needs at least 3 corners."));
		}
		int items = CountItemsInArea(edited.name);
		ImGui::TextDisabled(_("%d item(s) shown only inside it"), items);
		ImGui::SetItemTooltip(_("Pick this area in an object's World & streaming, under \"Only inside area\".\n"
			"Items name the area by its variable (Name above)."));
		if(items > 0){
			ImGui::SameLine();
			if(ImGui::SmallButton(_("Select them"))){
				std::string name = edited.name;
				ClearSelection();
				for(CPtrNode *p = instances.first; p; p = p->next){
					ObjectInst *item = (ObjectInst*)p->item;
					if(!item->m_isDeleted && name == GetObjectPawnProperties(item)->areaName)
						item->Select();
				}
			}
		}
		break;
	}
	case SAMP_ELEMENT_SOUND: {
		static const char *soundKinds[] = { N_("Audio stream"), N_("Game sound") };
		if(UiCombo(_("Kind"), &edited.soundKind, soundKinds, IM_ARRAYSIZE(soundKinds))) changed = true;
		ImGui::SetItemTooltip(_("Audio stream: a URL every player hears near here, in every world.\n"
			"Game sound: a GTA sound played when a player walks in. It needs _SoundTick on a\n"
			"timer, or _SoundArea with the streamer; the saved file's header says how."));
		if(edited.soundKind == SAMP_SOUND_STREAM){
			if(ImGui::InputTextWithHint(_("URL"), _("http://... .mp3, .ogg or .pls"), edited.soundUrl,
				sizeof(edited.soundUrl))) changed = true;
			ImGui::SetItemTooltip(_("PlayAudioStreamForPlayer. Started for each player in _ShowForPlayer."));
			if(!edited.soundUrl[0])
				ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("No URL yet."));
			if(ImGui::Checkbox(_("Use position"), &edited.soundUsePos)) changed = true;
			ImGui::SetItemTooltip(_("On: it gets louder as the player walks up to it and fades out at\n"
				"the distance. Off: the player hears it everywhere at full volume."));
			ImGui::BeginDisabled(!edited.soundUsePos);
			if(ImGui::InputFloat(_("Distance"), &edited.soundRadius, 1.0f, 10.0f, "%.1f m")) changed = true;
			ImGui::EndDisabled();
			int streams = 0;
			for(CPtrNode *p = instances.first; p; p = p->next){
				ObjectInst *other = (ObjectInst*)p->item;
				if(!other->m_isDeleted && GetSampElementKind(other) == SAMP_ELEMENT_SOUND &&
				   GetObjectPawnProperties(other)->soundKind == SAMP_SOUND_STREAM)
					streams++;
			}
			if(streams > 1)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
					_("%d audio streams: a player hears one at a time, the last one started."), streams);
		}else{
			if(uiSoundIdPicker(_("Sound"), &edited.soundId)) changed = true;
			ImGui::SetItemTooltip(_("PlayerPlaySound. Any ID from the game's data/AudioEvents.txt works."));
			if(ImGui::InputFloat(_("Radius"), &edited.soundRadius, 1.0f, 10.0f, "%.1f m")) changed = true;
			ImGui::SetItemTooltip(_("It plays when a player walks within this distance."));
			if(ImGui::Checkbox(_("Stop when the player leaves"), &edited.soundStopOnLeave)) changed = true;
			ImGui::SetItemTooltip(_("Plays sound 0, which stops the sound playing. Use it for music and loops."));
		}
		break;
	}
	case SAMP_ELEMENT_GANGZONE:
		if(ImGui::DragFloat2(_("Size (X, Y)"), &edited.zoneSizeX, 0.5f, 1.0f, 12000.0f, "%.1f")) changed = true;
		if(uiPawnColorEdit(_("Colour"), &edited.zoneColor)) changed = true;
		{
			static const char *zoneModes[] = { N_("Global"), N_("Per player"), N_("Streamed per player") };
			if(UiCombo(_("Shown as"), &edited.zoneMode, zoneModes, IM_ARRAYSIZE(zoneModes))) changed = true;
			ImGui::SetItemTooltip(_("Global: GangZoneCreate, 1024 on the whole server.\n"
				"Per player: CreatePlayerGangZone for every player, 1024 each.\n"
				"Streamed: per-player zones made around each player as they move (streamer areas)."));
			if(edited.zoneMode != SAMP_ZONE_GLOBAL){
				UiBadge(_("open.mp only"), _("CreatePlayerGangZone is an open.mp native; SA-MP 0.3.7 has no per-player zones."));
				if(edited.zoneMode == SAMP_ZONE_STREAMED){
					ImGui::SameLine();
					UiBadge(_("streamer"), _("The map includes the streamer for its areas."));
				}
			}
		}
		break;
	case SAMP_ELEMENT_MAPICON:
		if(uiNameListCombo(_("Icon"), &edited.mapIconType, uiMapIconNames, IM_ARRAYSIZE(uiMapIconNames)))
			changed = true;
		if(uiPawnColorEdit(_("Colour"), &edited.mapIconColor)) changed = true;
		ImGui::SetItemTooltip(_("Only used by icons 0 and 1 (the coloured markers)."));
		if(UiCombo(_("Style"), &edited.mapIconStyle, uiMapIconStyles, IM_ARRAYSIZE(uiMapIconStyles)))
			changed = true;
		break;
	}
	return changed;
}

// Batch editing: a field changed on the primary selection is copied to the
// other selected instances that have it, leaving their other fields alone.

struct UiBatchField { size_t offset, size; bool elementOnly; };
#define UI_BATCH_FIELD(field, elementOnly) \
	{ offsetof(ObjectPawnProperties, field), sizeof(((ObjectPawnProperties*)0)->field), elementOnly }
static const UiBatchField uiBatchFields[] = {
	UI_BATCH_FIELD(group, false), UI_BATCH_FIELD(comment, false),
	UI_BATCH_FIELD(drawDistance, false), UI_BATCH_FIELD(streamDistance, false),
	UI_BATCH_FIELD(virtualWorld, false),
	UI_BATCH_FIELD(moveEnabled, false), UI_BATCH_FIELD(moveOffset, false), UI_BATCH_FIELD(moveRotation, false),
	UI_BATCH_FIELD(moveRotates, false), UI_BATCH_FIELD(moveSpeed, false),
	UI_BATCH_FIELD(pickupType, true), UI_BATCH_FIELD(labelText, true),
	UI_BATCH_FIELD(labelColor, true), UI_BATCH_FIELD(labelTestLOS, true),
	UI_BATCH_FIELD(elementModel, true), UI_BATCH_FIELD(actorInvulnerable, true),
	UI_BATCH_FIELD(actorHealth, true), UI_BATCH_FIELD(classTeam, true),
	UI_BATCH_FIELD(classWeapons[0], true), UI_BATCH_FIELD(classWeapons[1], true),
	UI_BATCH_FIELD(classWeapons[2], true), UI_BATCH_FIELD(classAmmo[0], true),
	UI_BATCH_FIELD(classAmmo[1], true), UI_BATCH_FIELD(classAmmo[2], true),
	UI_BATCH_FIELD(zoneSizeX, true), UI_BATCH_FIELD(zoneSizeY, true),
	UI_BATCH_FIELD(zoneColor, true), UI_BATCH_FIELD(zoneMode, true), UI_BATCH_FIELD(mapIconType, true),
	UI_BATCH_FIELD(mapIconColor, true), UI_BATCH_FIELD(mapIconStyle, true),
	UI_BATCH_FIELD(npcArmour, true), UI_BATCH_FIELD(npcWeapon, true),
	UI_BATCH_FIELD(npcAmmo, true), UI_BATCH_FIELD(pathName, true),
	UI_BATCH_FIELD(pathStopRange, true), UI_BATCH_FIELD(npcMoveType, true),
	UI_BATCH_FIELD(npcMoveSpeed, true), UI_BATCH_FIELD(npcPathReversed, true), UI_BATCH_FIELD(npcPathRepeat, true),
	UI_BATCH_FIELD(npcRecording, true), UI_BATCH_FIELD(npcRecordingAtNpc, true),
	UI_BATCH_FIELD(npcPlaysNode, true), UI_BATCH_FIELD(npcNode, true),
	UI_BATCH_FIELD(npcNodePoint, true), UI_BATCH_FIELD(npcNodeRadius, true),
	UI_BATCH_FIELD(npcNodeSetAngle, true), UI_BATCH_FIELD(animLib, true),
	UI_BATCH_FIELD(animName, true), UI_BATCH_FIELD(animDelta, true),
	UI_BATCH_FIELD(animLoop, true), UI_BATCH_FIELD(animLockX, true),
	UI_BATCH_FIELD(animLockY, true), UI_BATCH_FIELD(animFreeze, true),
	UI_BATCH_FIELD(animTime, true), UI_BATCH_FIELD(soundKind, true),
	UI_BATCH_FIELD(soundUrl, true), UI_BATCH_FIELD(soundId, true),
	UI_BATCH_FIELD(soundRadius, true), UI_BATCH_FIELD(soundUsePos, true),
	UI_BATCH_FIELD(soundStopOnLeave, true), UI_BATCH_FIELD(cpKind, true),
	UI_BATCH_FIELD(cpSize, true), UI_BATCH_FIELD(cpRaceType, true),
	UI_BATCH_FIELD(cpRace, true),
	UI_BATCH_FIELD(areaShape, true), UI_BATCH_FIELD(areaRadius, true), UI_BATCH_FIELD(areaSize, true),
	UI_BATCH_FIELD(areaHeight, true),
	// Any item kind that takes an area: select the canal's objects, pick the area.
	UI_BATCH_FIELD(areaName, false),
};
#undef UI_BATCH_FIELD

static void
uiSelectedInstances(std::vector<ObjectInst*> &out)
{
	out.clear();
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *selected = (ObjectInst*)p->item;
		if(selected && !selected->m_isDeleted)
			out.push_back(selected);
	}
}

void
OpenObjectInspector(void)
{
	showInstanceWindow = true;
}

static void
uiReplayParticlesAction(ObjectInst *inst, bool menu)
{
	if(!ParticlePreview::HasParticleEffect(inst)) return;
	bool clicked=menu ? ImGui::MenuItem(_("Replay particles")) : ImGui::Button(_("Replay particles"));
	ImGui::SetItemTooltip(_("Restart the selected particle emitters, including one-shot effects such as explosions."));
	if(clicked){
		if(inst->m_selected){
			std::vector<ObjectInst*> targets;
			uiSelectedInstances(targets);
			for(ObjectInst *target:targets) ParticlePreview::Restart(target);
		}else ParticlePreview::Restart(inst);
	}
}

// Explicitly include an original GTA object, or the current selection, in Pawn.
static void
uiConvertWorldObjectAction(ObjectInst *inst, bool menu)
{
	if(IsIplMapDocumentOpen()) return;
	std::vector<ObjectInst*> targets;
	if(inst->m_selected) uiSelectedInstances(targets);
	else targets.push_back(inst);
	bool available = false;
	for(ObjectInst *target : targets)
		if(IsPawnWorldObject(target) && IsInstInIplMapDocument(target) &&
		   !target->m_isDeleted && !GetObjectPawnProperties(target)->forceWorldReplacement)
			available = true;
	if(!available) return;
	bool clicked = menu ? ImGui::MenuItem(_("Convert to map object")) :
		ImGui::Button(_("Convert to map object"));
	ImGui::SetItemTooltip(_("Keep the selected GTA objects in place. Saving removes the originals and recreates them as CreateObject or CreateDynamicObject, using the map's format."));
	if(clicked){
		finishTransformPanelEdit();
		ConvertWorldObjectsToMapObjects(targets);
	}
}

// Same kind of thing: objects with objects, labels with labels, and so on.
static bool
uiSameElementKind(const ObjectInst *a, const ObjectInst *b)
{
	return a->m_isVehicle == b->m_isVehicle && GetSampElementKind(a) == GetSampElementKind(b);
}

// Applies edited (changed from before) to source and, when batch editing,
// the changed fields to the rest of the selection. One undo step.
static void
uiCommitProperties(ObjectInst *source, const ObjectPawnProperties &before,
	const ObjectPawnProperties &edited)
{
	UndoBeginGroup();
	SetObjectPawnProperties(source, edited);
	std::vector<ObjectInst*> selected;
	uiSelectedInstances(selected);
	if(gBatchEditSelection && selected.size() > 1){
		const char *b = (const char*)&before;
		const char *e = (const char*)&edited;
		for(size_t i = 0; i < selected.size(); i++){
			ObjectInst *other = selected[i];
			if(other == source || other->m_isVehicle)
				continue;
			ObjectPawnProperties properties = *GetObjectPawnProperties(other);
			bool sameKind = uiSameElementKind(source, other);
			bool any = false;
			for(size_t f = 0; f < IM_ARRAYSIZE(uiBatchFields); f++){
				const UiBatchField &field = uiBatchFields[f];
				if(memcmp(b + field.offset, e + field.offset, field.size) == 0 ||
				   (field.elementOnly && !sameKind))
					continue;
				memcpy((char*)&properties + field.offset, e + field.offset, field.size);
				any = true;
			}
			if(any)
				SetObjectPawnProperties(other, properties);
		}
	}
	UndoEndGroup();
}

// How many selected instances a batch edit reaches, for the panel heading.
// "Select all" for elements and vehicles, next to Delete.
static void
uiSelectAllOfKindButton(ObjectInst *inst)
{
	int kind = GetSampElementKind(inst);
	if(kind == SAMP_ELEMENT_OBJECT && !inst->m_isVehicle)
		return;
	ImGui::SameLine();
	if(ImGui::SmallButton(_("Select all"))){
		ClearSelection();
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *other = (ObjectInst*)p->item;
			if(!other->m_isDeleted && uiSameElementKind(inst, other))
				other->Select();
		}
	}
	ImGui::SetItemTooltip(_("Select every %s in the map, to edit them together."),
		inst->m_isVehicle ? _("vehicle") : _(GetSampElementKindName(kind)));
}

static void
uiBatchEditHeader(ObjectInst *inst)
{
	std::vector<ObjectInst*> selected;
	uiSelectedInstances(selected);
	if(selected.size() < 2)
		return;
	int same = 0;
	for(size_t i = 0; i < selected.size(); i++)
		if(uiSameElementKind(inst, selected[i]))
			same++;
	ImGui::Checkbox(_("Edit all selected"), &gBatchEditSelection);
	ImGui::SetItemTooltip(_("A field you change here is set on every selected instance that has it,\n"
		"and Position & Rotation move and turn the whole selection, keeping its layout\n"
		"(the first selected is the pivot). Other fields keep their own values.\n"
		"%s undoes the whole change."), uiKey(SC_UNDO));
	ImGui::SameLine();
	if(gBatchEditSelection)
		ImGui::TextDisabled(_("%d selected, %d of this kind"), (int)selected.size(), same);
	else
		ImGui::TextDisabled(_("only this one"));
}

// Object Info shows an instance's Pawn settings in parts, in this order:
// the variable, the element's own settings, world and streaming, the group
// and note, and (objects only) the attachment.
enum { UI_PROPS_VARIABLE, UI_PROPS_ELEMENT, UI_PROPS_WORLD, UI_PROPS_NOTES, UI_PROPS_ATTACH, UI_PROPS_MOVE };

// Whether an element kind has anything under World & streaming.
static bool
uiHasWorldSettings(int kind)
{
	return kind != SAMP_ELEMENT_CLASS && kind != SAMP_ELEMENT_GANGZONE && kind != SAMP_ELEMENT_PATHPOINT;
}

// "Only inside area": the dynamic area a streamer item is limited to (its
// areaid), from the map's areas. Streamer only.
static bool
uiAreaPicker(ObjectPawnProperties &edited)
{
	std::vector<std::string> names;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(!inst->m_isDeleted && properties->elementKind == SAMP_ELEMENT_AREA && properties->name[0])
			names.push_back(properties->name);
	}
	std::sort(names.begin(), names.end());
	bool changed = false;
	bool missing = edited.areaName[0] && FindAreaByName(edited.areaName) == nil;
	char preview[64];
	if(missing)
		snprintf(preview, sizeof(preview), _("%s (missing)"), edited.areaName);
	else
		snprintf(preview, sizeof(preview), "%s", edited.areaName[0] ? edited.areaName : _("Everywhere"));
	if(ImGui::BeginCombo(_("Only inside area"), preview)){
		if(ImGui::Selectable(_("Everywhere"), edited.areaName[0] == '\0')){
			edited.areaName[0] = '\0';
			changed = true;
		}
		for(const std::string &name : names)
			if(ImGui::Selectable(name.c_str(), name == edited.areaName)){
				snprintf(edited.areaName, sizeof(edited.areaName), "%s", name.c_str());
				changed = true;
			}
		if(names.empty())
			ImGui::TextDisabled(_("No dynamic areas. Choose Zones in the browser to add one."));
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip(_("Shown only to players inside this dynamic area (the streamer's areaid).\n"
		"Select several items to set it on all of them."));
	ImGui::SameLine();
	UiBadge(_("streamer only"));
	if(edited.areaName[0] && gPawnSaveFormat != PAWN_CODE_STREAMER)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Ignored: this map is native code."));
	return changed;
}

static bool
uiAttachmentFields(ObjectPawnProperties &edited, bool playerTarget, bool showSlot, bool syncRotation,
	bool hasObject = true)
{
	bool changed = false;
	if(hasObject){
		if(ImGui::DragFloat3(_("Offset"), &edited.attachOffset.x, 0.01f, 0.0f, 0.0f, "%.3f")) changed = true;
		if(ImGui::DragFloat3(_("Rotation"), &edited.attachRotation.x, 0.5f, -360.0f, 360.0f, "%.2f")) changed = true;
	}
	if(playerTarget){
		static const char *bones[] = {
			N_("Spine"), N_("Head"), N_("Left upper arm"), N_("Right upper arm"),
			N_("Left hand"), N_("Right hand"), N_("Left thigh"), N_("Right thigh"),
			N_("Left foot"), N_("Right foot"), N_("Right calf"), N_("Left calf"),
			N_("Left forearm"), N_("Right forearm"), N_("Left clavicle"),
			N_("Right clavicle"), N_("Neck"), N_("Jaw")
		};
		int bone = clamp(edited.attachBone - 1, 0, 17);
		if(UiCombo(_("Bone"), &bone, bones, 18)){
			edited.attachBone = bone + 1;
			changed = true;
		}
		ImGui::SetItemTooltip(_("Choose the ped bone. Its coloured axes are marked in the viewport."));
		if(showSlot && ImGui::SliderInt(_("Player slot"), &edited.attachSlot, 0, 9)) changed = true;
		if(ImGui::DragFloat3(_("Scale"), &edited.attachScale.x, 0.01f, 0.01f, 50.0f, "%.3f")) changed = true;
		if(hasObject){
			if(ImGui::InputScalar(_("Material colour 1"), ImGuiDataType_U32, &edited.attachColor1,
				nil, nil, "0x%08X", ImGuiInputTextFlags_CharsHexadecimal)) changed = true;
			if(ImGui::InputScalar(_("Material colour 2"), ImGuiDataType_U32, &edited.attachColor2,
				nil, nil, "0x%08X", ImGuiInputTextFlags_CharsHexadecimal)) changed = true;
			ImGui::SetItemTooltip(_("Optional material colours for the attached model."));
		}
	}else if(hasObject && syncRotation){
		if(ImGui::Checkbox(_("Sync rotation"), &edited.attachSyncRotation)) changed = true;
		ImGui::SetItemTooltip(_("On: the object turns with its parent. Off: it keeps the rotation above\n"
			"in world space (AttachObjectToObject's SyncRotation)."));
	}
	return changed;
}

static void
uiObjectProperties(ObjectInst *inst, int part)
{
	// Vehicles have their own section; their variable and group are shared.
	if(inst == nil || (inst->m_isVehicle && part != UI_PROPS_VARIABLE && part != UI_PROPS_NOTES))
		return;
	bool canEdit = IsPawnObjectEditable(inst);
	const ObjectPawnProperties before = *GetObjectPawnProperties(inst);
	ObjectPawnProperties edited = before;
	int kind = edited.elementKind;
	bool changed = false;
	char label[128];
	ImGui::BeginDisabled(!canEdit);
	if(part == UI_PROPS_VARIABLE){
	// The name buffer is per object so an invalid name can be typed out.
	static int nameInstance = -1;
	static bool nameEditing;
	static char nameBuffer[PAWN_OBJECT_NAME_LEN];
	if(nameInstance != inst->m_id || (!nameEditing && strcmp(nameBuffer, edited.name) != 0)){
		strncpy(nameBuffer, edited.name, sizeof(nameBuffer));
		nameInstance = inst->m_id;
	}
	ImGui::TextUnformatted(_("Name"));
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##pawn_variable", _("automatic"), nameBuffer, sizeof(nameBuffer));
	nameEditing = ImGui::IsItemActive();
	ImGui::SetItemTooltip(_("Pawn variable for this object, e.g. bank_door. Leave empty and Haumea\n"
		"names it only when materials or attachments need a handle."));
	bool nameValid = nameBuffer[0] == '\0' || IsValidPawnObjectName(nameBuffer);
	ObjectInst *nameOwner = nameValid ? FindObjectByPawnName(nameBuffer, inst) : nil;
	if(!nameValid)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR),
			_("Use letters, digits and _, not starting with a digit, and no Pawn keyword."));
	else if(nameOwner)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("%s already uses this name."),
			uiObjectLabel(nameOwner, label, sizeof(label)));
	else if(strcmp(nameBuffer, edited.name) != 0){
		strncpy(edited.name, nameBuffer, sizeof(edited.name));
		changed = true;
	}
	}

	if(part == UI_PROPS_NOTES){
		if(uiGroupPicker(_("Group"), edited.group, sizeof(edited.group))) changed = true;
		ImGui::SetItemTooltip(_("Groups sort Map Objects and get a heading comment in the .pwn."));
		if(ImGui::InputTextWithHint(_("Note"), _("comment in the .pwn"),
			edited.comment, sizeof(edited.comment))) changed = true;
		ImGui::SetItemTooltip(_("Written as a comment after this line in the saved map."));
	}

	if(part == UI_PROPS_ELEMENT && kind != SAMP_ELEMENT_OBJECT)
		if(uiSampElementSettings(inst, edited)) changed = true;

	if(part == UI_PROPS_WORLD){
	if(kind == SAMP_ELEMENT_OBJECT){
		if(ImGui::InputFloat(_("Draw distance"), &edited.drawDistance, 10.0f, 100.0f, "%.1f")) changed = true;
		ImGui::SetItemTooltip(_("CreateObject DrawDistance / streamer drawdistance. 0 uses the default\n"
			"(the model's own distance natively, STREAMER_OBJECT_DD for the streamer)."));
	}
	if(kind != SAMP_ELEMENT_CLASS && kind != SAMP_ELEMENT_GANGZONE && kind != SAMP_ELEMENT_NPC &&
	   kind != SAMP_ELEMENT_PATHPOINT && kind != SAMP_ELEMENT_SOUND && kind != SAMP_ELEMENT_AREA){
		if(ImGui::InputFloat(_("Stream distance"), &edited.streamDistance, 10.0f, 100.0f, "%.1f")) changed = true;
		ImGui::SetItemTooltip(_("Streamer only. 0 uses the streamer's default for this kind."));
	}
	if(kind == SAMP_ELEMENT_NPC){
		if(ImGui::InputInt(_("Virtual world"), &edited.virtualWorld)) changed = true;
		ImGui::SetItemTooltip(_("NPC_SetVirtualWorld. NPCs are in one world; the interior is set\n"
			"under Position."));
	}else if(kind == SAMP_ELEMENT_SOUND){
		ImGui::BeginDisabled(edited.soundKind == SAMP_SOUND_STREAM);
		if(ImGui::InputInt(_("Virtual world"), &edited.virtualWorld)) changed = true;
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Game sounds only: -1 plays it in every world. The interior is set\n"
			"under Position."));
	}else if(kind != SAMP_ELEMENT_CLASS && kind != SAMP_ELEMENT_GANGZONE && kind != SAMP_ELEMENT_PATHPOINT){
		if(ImGui::InputInt(_("Virtual world"), &edited.virtualWorld)) changed = true;
		ImGui::SetItemTooltip("%s", kind == SAMP_ELEMENT_OBJECT ?
			_("Streamer only; -1 shows the object in every world.\n"
			"Native CreateObject objects exist in all worlds.") :
			_("-1 shows it in every world (with the streamer; native labels and\n"
			"actors use world 0 then)."));
	}
	if(SampElementTakesArea(kind) && !inst->m_isVehicle)
		if(uiAreaPicker(edited)) changed = true;
	}

	if(part == UI_PROPS_MOVE && kind == SAMP_ELEMENT_OBJECT){
		if(ImGui::Checkbox(_("Moves when the map loads"), &edited.moveEnabled)){
			if(edited.moveEnabled && edited.moveSpeed <= 0.0f){
				edited.moveSpeed = 2.0f;
				edited.moveOffset = { 0.0f, 0.0f, 3.0f };
			}
			changed = true;
		}
		ImGui::SetItemTooltip(_("MoveObject (MoveDynamicObject with the streamer) right after the object is made."));
		if(edited.moveEnabled){
			if(ImGui::DragFloat3(_("Move by"), &edited.moveOffset.x, 0.05f, -20000.0f, 20000.0f, "%.3f")) changed = true;
			ImGui::SetItemTooltip(_("How far it goes, in metres along the world axes. It follows the object when\n"
				"you move it."));
			if(ImGui::InputFloat(_("Speed"), &edited.moveSpeed, 0.5f, 5.0f, "%.2f m/s")) changed = true;
			if(ImGui::Checkbox(_("Turns on the way"), &edited.moveRotates)){
				if(edited.moveRotates)
					edited.moveRotation = GetInstanceRotationDegrees(inst);
				changed = true;
			}
			if(edited.moveRotates &&
			   ImGui::DragFloat3(_("Turns to"), &edited.moveRotation.x, 0.5f, -360.0f, 360.0f, "%.1f")) changed = true;
			float length = sqrtf(edited.moveOffset.x*edited.moveOffset.x + edited.moveOffset.y*edited.moveOffset.y +
				edited.moveOffset.z*edited.moveOffset.z);
			if(edited.moveSpeed > 0.0f)
				ImGui::TextDisabled(_("%.2f m in %.2f s"), length, length / edited.moveSpeed);
			ImGui::Checkbox(_("Preview the movement"), &gPreviewObjectMoves);
		}
	}

	if(part != UI_PROPS_ATTACH || kind != SAMP_ELEMENT_OBJECT){
		if(changed)
			uiCommitProperties(inst, before, edited);
		ImGui::EndDisabled();
		return;
	}

	ObjectInst *parent = GetAttachParent(inst);
	ObjectInst *other = nil;
	int numSelected = 0;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *selected = (ObjectInst*)p->item;
		if(selected->m_isDeleted)
			continue;
		numSelected++;
		if(selected != inst)
			other = selected;
	}
	if(parent || (IsPlayerAttachedObject(inst) && edited.attachParent == PAWN_ATTACH_PLAYER)){
		if(edited.attachParent == PAWN_ATTACH_PLAYER)
			ImGui::Text(_("Attached to player preview (skin %d)"), edited.attachSkin);
		else
			ImGui::Text(_("Attached to %s"), uiObjectLabel(parent, label, sizeof(label)));
		if(edited.attachParent != PAWN_ATTACH_PLAYER){
			if(ImGui::SmallButton(_("Select parent"))){
				ClearSelection();
				parent->Select();
			}
			ImGui::SameLine();
		}
		if(ImGui::SmallButton(_("Detach"))){
			DetachObjectInstance(inst);
			edited = *GetObjectPawnProperties(inst);
		}
		if(uiAttachmentFields(edited, IsPlayerAttachedObject(inst), true, parent && !parent->m_isVehicle))
			changed = true;
		if(ImGui::SmallButton(_("Edit in attachment workspace"))){
			if(edited.attachParent == PAWN_ATTACH_PLAYER)
				OpenPlayerAttachmentWorkspace(edited.attachSkin, false);
			else
				OpenAttachmentWorkspace(parent, false);
		}
		ImGui::TextDisabled(_("Moving this object updates the offset; moving the parent carries it along."));
	}else if(edited.attachParent >= 0){
		uiStatusLine(false, _("The parent was deleted, so this attachment is not exported."));
		if(ImGui::SmallButton(_("Detach"))){
			DetachObjectInstance(inst);
			edited = *GetObjectPawnProperties(inst);
		}
	}else if(numSelected == 2 && other){
		char otherLabel[128];
		uiObjectLabel(other, otherLabel, sizeof(otherLabel));
		char buttonLabel[192];
		bool canAttachToOther = other->m_isVehicle || IsPlayerAttachmentTarget(other) ||
			GetSampElementKind(other) == SAMP_ELEMENT_OBJECT;
		bool otherCanBeChild = !other->m_isVehicle && GetSampElementKind(other) == SAMP_ELEMENT_OBJECT;
		if(canAttachToOther){
			snprintf(buttonLabel, sizeof(buttonLabel), _("Attach this to %s"), otherLabel);
			if(ImGui::Button(buttonLabel) && !AttachObjectInstance(inst, other))
				Toast(TOAST_SPAWN, _("Could not attach this object to that target."));
		}
		if(otherCanBeChild){
			snprintf(buttonLabel, sizeof(buttonLabel), _("Attach %s to this"), otherLabel);
			if(ImGui::Button(buttonLabel) && !AttachObjectInstance(other, inst))
				Toast(TOAST_SPAWN, _("Could not attach this object to that target."));
		}
		if(!canAttachToOther && !otherCanBeChild)
			ImGui::TextDisabled(_("Select an object, player, NPC or vehicle as the target."));
		edited = *GetObjectPawnProperties(inst);
	}else if(numSelected > 1)
		ImGui::TextDisabled(_("Use Attach selected to target or select two objects to attach them."));

	if(changed)
		uiCommitProperties(inst, before, edited);

	if(numSelected > 1){
		ImGui::SeparatorText(_("Selection"));
		static char batchGroup[PAWN_OBJECT_GROUP_LEN];
		uiGroupPicker("##batchgroup", batchGroup, sizeof(batchGroup));
		ImGui::SameLine();
		char buttonLabel[64];
		snprintf(buttonLabel, sizeof(buttonLabel), _("Group %d selected"), numSelected);
		if(ImGui::Button(buttonLabel)){
			UndoBeginGroup();
			for(CPtrNode *p = selection.first; p; p = p->next){
				ObjectInst *selected = (ObjectInst*)p->item;
				if(selected->m_isDeleted || selected->m_isVehicle)
					continue;
				ObjectPawnProperties properties = *GetObjectPawnProperties(selected);
				strncpy(properties.group, batchGroup, sizeof(properties.group)-1);
				SetObjectPawnProperties(selected, properties);
			}
			UndoEndGroup();
		}
		snprintf(buttonLabel, sizeof(buttonLabel), _("Copy streaming to %d selected"), numSelected);
		if(ImGui::Button(buttonLabel)){
			const ObjectPawnProperties *source = GetObjectPawnProperties(inst);
			UndoBeginGroup();
			for(CPtrNode *p = selection.first; p; p = p->next){
				ObjectInst *selected = (ObjectInst*)p->item;
				if(selected == inst || selected->m_isDeleted || selected->m_isVehicle)
					continue;
				ObjectPawnProperties properties = *GetObjectPawnProperties(selected);
				properties.drawDistance = source->drawDistance;
				properties.streamDistance = source->streamDistance;
				properties.virtualWorld = source->virtualWorld;
				SetObjectPawnProperties(selected, properties);
			}
			UndoEndGroup();
		}
	}
	ImGui::EndDisabled();
}

// Spacing and quiet surfaces group inspector controls without extra outlines.
static bool
uiInspectorSection(const char *label, ImGuiTreeNodeFlags flags = 0)
{
	ImGui::Spacing();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
	bool open = ImGui::CollapsingHeader(label, flags);
	ImGui::PopStyleVar();
	if(open) ImGui::Spacing();
	return open;
}

static void
uiAttachmentWorkspaceWindow(void)
{
	if(!IsAttachmentWorkspaceActive())
		return;
	bool open = true;
	ImRect workArea = uiPanelWorkArea();
#ifdef IMGUI_HAS_DOCK
	// Over the 3D view, at its left edge: the panels dock on the right, so
	// the model browser stays reachable even when it opens after this.
	if(ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaDockSpace")))
		workArea = ImRect(central->Pos, ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y));
#endif
	ImGui::SetNextWindowPos(ImVec2(workArea.Min.x + uiPanelGap(), workArea.Min.y + uiPanelGap()),
		ImGuiCond_FirstUseEver, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f),
		ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y - 48.0f));
	if(ImGui::Begin(LI(ICON_FA_LINK, "Attachment Workspace"), &open, ImGuiWindowFlags_AlwaysAutoResize)){
		ObjectInst *target = GetAttachmentWorkspaceTarget();
		ObjectInst *player = GetAttachmentWorkspacePlayerPreview();
		if(target && target->m_isDeleted)
			target = nil;
		if(player){
			int skin = GetAttachmentWorkspacePlayerSkin();
			if(ImGui::InputInt(_("Player skin"), &skin)){
				SetAttachmentWorkspacePlayerSkin(skin);
				target = GetAttachmentWorkspaceTarget();
			}
			ImGui::SetItemTooltip(_("Preview skin used to position bone attachments."));
		}else if(target){
			char targetLabel[128];
			ImGui::Text(_("Target: %s"), uiObjectLabel(target, targetLabel, sizeof(targetLabel)));
		}else
			ImGui::TextWrapped(_("Select a ped, NPC, or vehicle in the map."));

		ImGui::BeginDisabled(target == nil);
		bool voidView = AttachmentWorkspaceHidesWorld();
		if(ImGui::Button(voidView ? _("Return to map") : _("Isolate view")))
			SetAttachmentWorkspaceVoid(!voidView);
		ImGui::SetItemTooltip(voidView ?
			_("Leave the isolated attachment view and restore the map camera.") :
			_("Show only the target and its attachments against an empty background."));
		ImGui::SameLine();
		if(ImGui::Button(_("Focus"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_FOCUS);
		ImGui::SetItemTooltip(_("Center the target and fit it in view."));
		if(ImGui::Button(_("Front"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_FRONT);
		ImGui::SameLine();
		if(ImGui::Button(_("Back"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_BACK);
		ImGui::SameLine();
		if(ImGui::Button(_("Left"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_LEFT);
		ImGui::SameLine();
		if(ImGui::Button(_("Right"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_RIGHT);
		ImGui::SameLine();
		if(ImGui::Button(_("Top"))) SetAttachmentWorkspaceCameraView(ATTACHMENT_VIEW_TOP);
		ImGui::EndDisabled();

		bool mapEditMode = !InTextDrawMode() && !InPawnIdeMode();
		ImGui::BeginDisabled(!mapEditMode || target == nil);
		static ObjectInst *placementTarget;
		static bool placementInitialized;
		static ObjectPawnProperties placement;
		static int editedAttachmentId = -1;
		if(!placementInitialized || placementTarget != target){
			InitObjectPawnProperties(&placement);
			placement.attachBone = 2;
			placementTarget = target;
			placementInitialized = true;
			editedAttachmentId = -1;
		}
		std::vector<ObjectInst*> attachments;
		for(CPtrNode *p = instances.first; target && p; p = p->next){
			ObjectInst *attached = (ObjectInst*)p->item;
			if(!attached->m_isDeleted && !attached->m_isVehicle &&
			   GetSampElementKind(attached) == SAMP_ELEMENT_OBJECT && GetAttachParent(attached) == target)
				attachments.push_back(attached);
		}
		std::vector<ObjectInst*> candidates;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *selected = (ObjectInst*)p->item;
			if(selected && selected != target && !selected->m_isDeleted && !selected->m_isVehicle &&
			   GetSampElementKind(selected) == SAMP_ELEMENT_OBJECT && IsPawnObjectEditable(selected) &&
			   GetAttachParent(selected) != target)
				candidates.push_back(selected);
		}
		ObjectInst *child = nil;
		for(CPtrNode *p = selection.first; target && p; p = p->next){
			ObjectInst *selected = (ObjectInst*)p->item;
			if(selected && !selected->m_isDeleted && !selected->m_isVehicle &&
			   GetSampElementKind(selected) == SAMP_ELEMENT_OBJECT && GetAttachParent(selected) == target){
				child = selected;
				break;
			}
		}
		// Clicking the host keeps editing a real attachment. A selected loose
		// object instead exposes only the choices for attaching that object.
		if(child == nil && candidates.empty() && !attachments.empty()){
			child = attachments.front();
			for(ObjectInst *attached : attachments)
				if(attached->m_id == editedAttachmentId){
					child = attached;
					break;
				}
		}
		char childLabel[128];
		if(!attachments.empty()){
			if(ImGui::BeginCombo(_("Attachment"), child ? uiObjectLabel(child, childLabel, sizeof(childLabel)) : _("Choose an attachment"))){
				for(ObjectInst *attached : attachments){
					ImGui::PushID(attached->m_id);
					if(ImGui::Selectable(uiObjectLabel(attached, childLabel, sizeof(childLabel)), child == attached)){
						ClearSelection();
						attached->Select();
						child = attached;
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}else
			ImGui::TextDisabled(_("No attached objects"));
		if(child)
			editedAttachmentId = child->m_id;
		bool playerTarget = player || IsPlayerAttachmentTarget(target);
		if(!child && playerTarget)
			ImGui::SeparatorText(_("Placement settings"));
		ObjectPawnProperties settings = child ? *GetObjectPawnProperties(child) : placement;
		if(uiAttachmentFields(settings, playerTarget, child != nil, target && !target->m_isVehicle, child != nil)){
			if(child) SetObjectPawnProperties(child, settings);
			else placement = settings;
		}
		SetAttachmentTargetPickOptions(settings.attachBone, settings.attachScale);
		if(child && ImGui::Button(_("Detach"))){
			DetachObjectInstance(child);
			ClearSelection();
			if(target && !player) target->Select();
		}

		ImGui::Separator();
		if(ImGui::Button(_("Browse models"))){
			showBrowserWindow = true;
			gBrowserDiscoveryFocusPending = true;
			// Docked browser panels stay below floating windows. Move this
			// window out of the browser's way before handing input to it.
			ImGuiWindow *browser = ImGui::FindWindowByName(LI(ICON_FA_MAGNIFYING_GLASS, "Object Browser"));
			ImRect area = uiPanelWorkArea();
			float browserLeft = browser ? browser->Pos.x : area.Min.x;
			float browserRight = browser ? browser->Pos.x + browser->Size.x : area.Min.x + area.GetWidth() * 0.25f;
			ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
			if(pos.x < browserRight && pos.x + size.x > browserLeft){
				pos.x = browserRight < area.GetCenter().x ? area.Max.x - size.x - uiPanelGap() : area.Min.x + uiPanelGap();
				ImGui::SetWindowPos(pos);
			}
		}
		ImGui::SameLine();
		int model = GetSpawnObjectId();
		ObjectDef *modelDef = GetObjectDef(model);
		bool canAddModel = target && modelDef && !modelDef->m_isVehicle && !modelDef->m_isPed &&
			GetSpawnElementKind() == SAMP_ELEMENT_OBJECT;
		ImGui::BeginDisabled(!canAddModel);
		if(ImGui::Button(_("Add selected model"))){
			UndoBeginGroup();
			rw::Quat rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
			ObjectInst *added = SpawnDocumentInstance(model, target->m_translation, rotation);
			if(added){
				bool attached = player ? AttachObjectToPlayer(added, GetAttachmentWorkspacePlayerSkin(), false) :
					AttachObjectInstance(added, target, settings.attachSyncRotation, settings.attachBone, &settings.attachScale, false);
				if(attached){
					ObjectPawnProperties properties = *GetObjectPawnProperties(added);
					properties.attachOffset = settings.attachOffset;
					properties.attachRotation = settings.attachRotation;
					if(playerTarget){
						properties.attachBone = settings.attachBone;
						properties.attachScale = settings.attachScale;
						properties.attachColor1 = settings.attachColor1;
						properties.attachColor2 = settings.attachColor2;
					}
					SetObjectPawnProperties(added, properties, false);
					UpdateObjectAttachments();
					ClearSelection();
					added->Select();
					UndoRecordPaste(&added, 1);
				}else{
					RestoreDocumentTransform(added, added->m_translation, added->m_rotation, true);
					Toast(TOAST_SPAWN, _("Could not attach to the selected target."));
				}
			}
			UndoEndGroup();
		}
		ImGui::EndDisabled();
		if(!canAddModel) ImGui::SetItemTooltip(_("Choose an object model in the browser first."));

		ImGui::BeginDisabled(candidates.empty());
		if(ImGui::Button(_("Attach selected objects"))){
			UndoBeginGroup();
			for(ObjectInst *selected : candidates){
				bool attached = player ? AttachObjectToPlayer(selected, GetAttachmentWorkspacePlayerSkin()) :
					AttachObjectInstance(selected, target, settings.attachSyncRotation, settings.attachBone, &settings.attachScale);
				if(attached && playerTarget){
					ObjectPawnProperties properties = *GetObjectPawnProperties(selected);
					properties.attachBone = settings.attachBone;
					properties.attachScale = settings.attachScale;
					properties.attachColor1 = settings.attachColor1;
					properties.attachColor2 = settings.attachColor2;
					SetObjectPawnProperties(selected, properties);
					if(player) RebaseObjectAttachment(selected);
				}else if(!attached)
					Toast(TOAST_SPAWN, _("Could not attach to the selected target."));
			}
			UndoEndGroup();
			UpdateObjectAttachments();
		}
		ImGui::SetItemTooltip(_("Select objects in the map, then attach them to this target without moving them."));
		ImGui::EndDisabled();
		ImGui::EndDisabled();
	}
	ImGui::End();
	if(!open) CloseAttachmentWorkspace();
}

static void
uiAttachmentTargetPickWindow(void)
{
	if(!AttachmentTargetPickActive())
		return;

	static const char *bones[] = {
		N_("Spine"), N_("Head"), N_("Left upper arm"), N_("Right upper arm"),
		N_("Left hand"), N_("Right hand"), N_("Left thigh"), N_("Right thigh"),
		N_("Left foot"), N_("Right foot"), N_("Right calf"), N_("Left calf"),
		N_("Left forearm"), N_("Right forearm"), N_("Left clavicle"),
		N_("Right clavicle"), N_("Neck"), N_("Jaw")
	};
	ImGui::SetNextWindowPos(ImVec2(24.0f, 96.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
	if(ImGui::Begin(LI(ICON_FA_LINK, "Attach to target###attachment_target_pick"), nil,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)){
		ImGui::TextWrapped(_("Click a ped, NPC, vehicle, or object in the viewport. Esc or right-click cancels."));
		int bone = 2;
		rw::V3d scale = { 1.0f, 1.0f, 1.0f };
		GetAttachmentTargetPickOptions(&bone, &scale);
		int boneIndex = bone - 1;
		bool changed = UiCombo(_("Bone"), &boneIndex, bones, 18);
		if(changed)
			bone = boneIndex + 1;
		ImGui::SetItemTooltip(_("Choose the ped bone. Its coloured axes are marked in the viewport."));
		if(ImGui::DragFloat3(_("Scale"), &scale.x, 0.01f, 0.01f, 50.0f, "%.2f"))
			changed = true;
		ImGui::SetItemTooltip(_("Bone and scale apply to ped/NPC targets."));
		if(changed)
			SetAttachmentTargetPickOptions(bone, scale);
		if(ImGui::Button(_("Cancel target selection")))
			CancelAttachmentTargetPick();
	}
	ImGui::End();
}

static void
uiInstWindow(void)
{
	uiDefaultPanelLayout(PANEL_RIGHT_TOP);
	uiNextPanelSizeConstraints();
	ImGui::Begin(LI(ICON_FA_CIRCLE_INFO, "Object Info"), &showInstanceWindow);
	uiFitItemsToPanel();

	if(selection.first){
		int numSelected = 0;
		int numWorldChanged = 0;
		for(CPtrNode *sel = selection.first; sel; sel = sel->next){
			ObjectInst *selected = (ObjectInst*)sel->item;
			if(selected == nil || selected->m_isDeleted)
				continue;
			numSelected++;
			if(IsPawnWorldObject(selected) && PawnObjectNeedsExport(selected))
				numWorldChanged++;
		}

		ObjectInst *inst = (ObjectInst*)selection.first->item;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		bool canEdit = IsPawnObjectEditable(inst);
		int kind = GetSampElementKind(inst);
		bool isObject = !inst->m_isVehicle && kind == SAMP_ELEMENT_OBJECT;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(!isObject && !inst->m_isVehicle)
			ImGui::TextUnformatted(_(GetSampElementKindName(kind)));
		else if(properties->name[0])
			ImGui::Text("%s", properties->name);
		else if(GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
			ImGui::Text("%s", _(GetSampElementKindName(GetSampElementKind(inst))));
		else
			ImGui::Text("%s", obj ? obj->m_name : "?");
		if(isObject && numSelected == 1){
			static int modelIdInstance = -1;
			static int modelIdInput;
			static int modelIdCurrent;
			static bool modelIdWasActive;
			static bool invalidModelId;
			if(modelIdInstance != inst->m_id){
				modelIdInstance = inst->m_id;
				modelIdInput = PawnModelId(inst->m_objectId);
				modelIdCurrent = inst->m_objectId;
				modelIdWasActive = false;
				invalidModelId = false;
			}else if(modelIdCurrent != inst->m_objectId && !modelIdWasActive){
				modelIdInput = PawnModelId(inst->m_objectId);
				modelIdCurrent = inst->m_objectId;
				invalidModelId = false;
			}
			bool canChangeModel = canEdit && CanChangeInstanceModelId(inst);
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
			ImGui::BeginDisabled(!canChangeModel);
			bool submitted = ImGui::InputInt(_("Model ID"), &modelIdInput, 1, 100,
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%s\n%s", _("Model ID"),
				_("Search several IDs or ranges. Custom models use their negative Pawn IDs."));
			if(submitted){
				if(canChangeModel && ChangeInstanceModelId(inst, EditorModelId(modelIdInput))){
					invalidModelId = false;
					modelIdInput = PawnModelId(inst->m_objectId);
					modelIdCurrent = inst->m_objectId;
				}else{
					invalidModelId = true;
					modelIdInput = PawnModelId(inst->m_objectId);
				}
			}
			modelIdWasActive = ImGui::IsItemActive() || ImGui::IsItemFocused();
			if(!canChangeModel){
				// A lock beside the field says why it is read-only, without a paragraph.
				ImGui::SameLine();
				ImGui::TextDisabled(ICON_FA_LOCK);
				ImGui::SetItemTooltip("%s", _("Model IDs can't be changed for objects linked to an LOD or outside the active map."));
			}
			else if(invalidModelId)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("Unknown model"));
		}else if(isObject || inst->m_isVehicle){
			ImGui::SameLine();
			ImGui::TextDisabled(_("%s  model %d"), properties->name[0] && obj ? obj->m_name : "",
				inst->m_objectId);
		}
		if(numSelected > 1){
			ImGui::SameLine();
			ImGui::TextDisabled(_("(+%d more selected)"), numSelected - 1);
		}
		if(!isObject && numSelected == 1)
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
		if(ImGui::SmallButton(ICON_FA_GEAR "##inspector_actions"))
			ImGui::OpenPopup("##inspector_actions");
		ImGui::SetItemTooltip(_("More actions"));
		if(ImGui::BeginPopup("##inspector_actions")){
			ImGui::BeginDisabled(!canEdit);
			if(inst->m_isDeleted){
				if(ImGui::MenuItem(_("Undelete"))) inst->Undelete();
			}else if(ImGui::MenuItem(TI(ICON_FA_TRASH, "Delete"))){
				finishTransformPanelEdit();
				inst->Delete();
			}
			uiSelectAllOfKindButton(inst);
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		if(IsPawnWorldObject(inst)){
			ImGui::SameLine();
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), properties->forceWorldReplacement ?
				_("Map object") : _("GTA world object"));
			ImGui::SetItemTooltip("%s", properties->forceWorldReplacement ?
				_("Keep the selected GTA objects in place. Saving removes the originals and recreates them as CreateObject or CreateDynamicObject, using the map's format.") :
				_("Unchanged world objects are not saved in the map. Once you move,\n"
				"rotate, retexture or delete one, the map removes the original with\n"
				"RemoveBuildingForPlayer and creates your version instead."));
		}
		uiConvertWorldObjectAction(inst, false);
		uiReplayParticlesAction(inst, false);
		if(numWorldChanged > 0 && numSelected > 1)
			ImGui::TextDisabled(_("%d selected world object(s) will be replaced in the map."), numWorldChanged);
		ImGui::Spacing();

		// The same order for every kind: what it is called, what it is,
		// where it is, then the rarely used parts, closed unless in use.
		ImGui::BeginDisabled(!canEdit);
		uiBatchEditHeader(inst);
		ImGui::EndDisabled();
		uiObjectProperties(inst, UI_PROPS_VARIABLE);
		if(inst->m_isVehicle){
			if(uiInspectorSection(L("Vehicle"), ImGuiTreeNodeFlags_DefaultOpen)){
				std::vector<ObjectInst*> selected;
				uiSelectedInstances(selected);
				uiVehicleSettings(inst, selected, gBatchEditSelection && selected.size() > 1, canEdit);
			}
		}else if(!isObject){
			// Its own ID: a field may have the kind's name (a sound's Sound).
			char header[64];
			snprintf(header, sizeof(header), "%s###element", _(GetSampElementKindName(kind)));
			if(uiInspectorSection(header, ImGuiTreeNodeFlags_DefaultOpen))
				uiObjectProperties(inst, UI_PROPS_ELEMENT);
		}
		if(uiInspectorSection(uiRotationMode(inst) == UI_ROTATION_FULL ? _("Position & Rotation###where") :
		   _("Position###where"), ImGuiTreeNodeFlags_DefaultOpen))
			uiInstInfo(inst);
		if(!inst->m_isVehicle && uiHasWorldSettings(kind) &&
		   uiInspectorSection(kind == SAMP_ELEMENT_NPC || kind == SAMP_ELEMENT_SOUND ? _("World###world") :
		   _("World & streaming###world")))
			uiObjectProperties(inst, UI_PROPS_WORLD);
		// Open only for an object that is attached (or carried by a player).
		if(isObject && uiInspectorSection(L("Attachment"), properties->attachParent != -1 || IsPlayerAttachedObject(inst) ?
		   ImGuiTreeNodeFlags_DefaultOpen : 0))
			uiObjectProperties(inst, UI_PROPS_ATTACH);
		if(isObject && !IsPlayerAttachedObject(inst) && properties->attachParent == -1 &&
		   uiInspectorSection(L("Movement"), properties->moveEnabled ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			uiObjectProperties(inst, UI_PROPS_MOVE);
		if(isObject && uiInspectorSection(L("Materials"),
		   HasObjectMaterialOverrides(inst) ? ImGuiTreeNodeFlags_DefaultOpen : 0))
			uiMaterialInfo(inst);
		{
			char notesHeader[96];
			if(properties->group[0])
				snprintf(notesHeader, sizeof(notesHeader), _("Group: %s###notes"), properties->group);
			else
				snprintf(notesHeader, sizeof(notesHeader), _("Group & note###notes"));
			if(uiInspectorSection(notesHeader))
				uiObjectProperties(inst, UI_PROPS_NOTES);
		}
		if((isObject || inst->m_isVehicle) && uiInspectorSection(L("Model details"))){
			uiObjInfo(obj);
			char exportDir[1024];
			bool haveExportDir = GetHaumeaDataPath(exportDir, sizeof(exportDir), "dff-txd-exports");
			if(ImGui::Button(_("Export DFF"))){
				int failed = 0;
				int exported = haveExportDir ? ExportSelectedDffs(exportDir, &failed) : 0;
				if(haveExportDir && (exported > 0 || failed == 0))
					Toast(TOAST_SAVE, failed > 0 ? _("Exported %d DFF(s) to %s (some skipped)") :
					      _("Exported %d DFF(s) to %s"), exported, exportDir);
				else
					Toast(TOAST_SAVE, _("Failed to export DFF(s)"));
			}
			ImGui::SameLine();
			if(ImGui::Button(_("Export TXD"))){
				int failed = 0;
				int exported = haveExportDir ? ExportSelectedTxds(exportDir, &failed) : 0;
				if(haveExportDir && (exported > 0 || failed == 0))
					Toast(TOAST_SAVE, failed > 0 ? _("Exported %d TXD(s) to %s (some skipped)") :
					      _("Exported %d TXD(s) to %s"), exported, exportDir);
				else
					Toast(TOAST_SAVE, _("Failed to export TXD(s)"));
			}
			ImGui::SetItemTooltip(_("Copies the selected models' files to %s"), exportDir);
		}
	}else{
		// Nothing selected: say how to select, offer the next steps, and
		// use the space for the objects placed most recently.
		ImGui::TextUnformatted(_("Nothing selected"));
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped(_("Click an object in the world to edit it. Shift+drag selects several."));
		ImGui::PopStyleColor();
		ImGui::Spacing();
		if(ImGui::Button(TI(ICON_FA_MAGNIFYING_GLASS, "Browse")))
			showBrowserWindow = true;
		ImGui::SetItemTooltip(_("Object Browser (%s): pick something to place."), uiKey(SC_WINDOW_BROWSER));
		ImGui::Spacing();

		if(SAPaths::selectedNode && SAPaths::HasInfoToShow()){
			if(gSaNodeJustSelected)
				ImGui::SetNextItemOpen(true);
			if(uiInspectorSection(_("NPC node (NODES*.DAT)###sanode")))
				SAPaths::DrawInfoPanel();
		}

	}
		ImGui::End();
}

// Helper: check if category index is or is a child of parent
static bool isCategoryOrChild(int cat, int parent);

// Which shelf a model belongs on: skins and vehicles each have their own.
static int
browserKindOf(const ObjectDef *obj)
{
	return obj->m_isPed ? BROWSER_KIND_SKINS : obj->m_isVehicle ? BROWSER_KIND_VEHICLES : BROWSER_KIND_OBJECTS;
}

// The categories a shelf offers in its drop-down.
static bool
browserKindHasCategory(int kind, int cat)
{
	bool vehicle = isCategoryOrChild(cat, CAT_VEHICLES_ROOT);
	if(kind == BROWSER_KIND_VEHICLES) return vehicle && cat != CAT_VEHICLES_ROOT;
	if(kind == BROWSER_KIND_SKINS) return false;
	return !vehicle && cat != CAT_PEOPLE;
}

static bool
isCategoryOrChild(int cat, int parent)
{
	if(cat == parent) return true;
	// Check if cat's parent chain leads to parent
	if(cat >= 0 && cat < NUM_OBJ_CATEGORIES){
		int p = objCategories[cat].parent;
		if(p == parent) return true;
		// Only 2 levels deep max
		if(p >= 0 && objCategories[p].parent == parent) return true;
	}
	return false;
}

// Helper: build indented category name for dropdown
static void
buildCategoryLabel(int idx, char *buf, int bufsize)
{
	int depth = 0;
	if(objCategories[idx].parent >= 0){
		depth = 1;
		if(objCategories[objCategories[idx].parent].parent >= 0)
			depth = 2;
	}
	char prefix[16] = "";
	for(int d = 0; d < depth; d++) strcat(prefix, "  ");
	snprintf(buf, bufsize, "%s%s", prefix, objCategories[idx].name);
}

static void
selectBrowserObject(int i)
{
	SetSpawnObjectId(i);
	RequestObject(i);
	int lodId = GetLodForObject(i);
	if(lodId >= 0) RequestObject(lodId);
}

static int
findObjectInFilteredList(int *filtered, int numFiltered, int objectId)
{
	for(int row = 0; row < numFiltered; row++)
		if(filtered[row] == objectId)
			return row;
	return -1;
}

static void
uiObjectFavouritePopup(int objectId)
{
	if(ImGui::BeginPopupContextItem()){
		if(IsFavourite(objectId)){
			if(ImGui::MenuItem(_("Remove from Favourites")))
				ToggleFavourite(objectId);
		}else{
			if(ImGui::MenuItem(_("Add to Favourites")))
				ToggleFavourite(objectId);
		}
		ImGui::Separator();
		if(ImGui::MenuItem(_("Find instances"))) uiOpenObjectDiscovery(objectId, false);
		if(ImGui::MenuItem(_("Find similar models"))) uiOpenObjectDiscovery(objectId, true);
		ImGui::EndPopup();
	}
}

// Shared object list renderer with clipper
static void
uiObjectList(int *filtered, int numFiltered, int selId)
{
	ImGui::BeginChild("##ObjList", ImVec2(0, 0), true);

	int scrollToRow = -1;
	bool listFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	bool textInputActive = ImGui::GetIO().WantTextInput;
	if(listFocused && !textInputActive && numFiltered > 0){
		int selectedRow = findObjectInFilteredList(filtered, numFiltered, selId);
		if(ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)){
			int nextRow = selectedRow < 0 ? 0 : min(selectedRow + 1, numFiltered - 1);
			selectBrowserObject(filtered[nextRow]);
			selId = filtered[nextRow];
			scrollToRow = nextRow;
		}else if(ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)){
			int nextRow = selectedRow < 0 ? numFiltered - 1 : max(selectedRow - 1, 0);
			selectBrowserObject(filtered[nextRow]);
			selId = filtered[nextRow];
			scrollToRow = nextRow;
		}
	}

	if(scrollToRow >= 0){
		float itemHeight = ImGui::GetTextLineHeightWithSpacing();
		float targetMin = scrollToRow * itemHeight;
		float targetMax = targetMin + itemHeight;
		float scrollY = ImGui::GetScrollY();
		float visibleMax = scrollY + ImGui::GetWindowHeight();
		if(targetMin < scrollY)
			ImGui::SetScrollY(targetMin);
		else if(targetMax > visibleMax)
			ImGui::SetScrollY(targetMax - ImGui::GetWindowHeight());
	}

	ImGuiListClipper clipper;
	clipper.Begin(numFiltered);
	static char buf[256];
	while(clipper.Step()){
		for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++){
			int i = filtered[row];
			ObjectDef *obj = GetObjectDef(i);
			bool isSelected = (i == selId);
			sprintf(buf, "%5d  %s", PawnModelId(obj->m_id), obj->m_name);

			ImGui::PushID(i);
			if(ImGui::Selectable(buf, isSelected))
				selectBrowserObject(i);
			uiObjectFavouritePopup(i);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

// Tile grids size from the UI font so they follow the UI scale, and stretch
// to fill the panel width.
struct TileGridLayout
{
	int columns;
	float tileW, tileH, rowH;
	float pad, imageSide;
};

static TileGridLayout
uiTileGridLayout(void)
{
	TileGridLayout layout;
	float font = ImGui::GetFontSize();
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	// Room for the scrollbar whether it shows or not: sizing the tiles to the
	// width without it made a full grid overflow, the scrollbar come, the tiles
	// shrink, the scrollbar go, and so on every frame (the browser flickered).
	float availW = ImGui::GetContentRegionAvail().x;
	if(!ImGui::GetCurrentWindow()->ScrollbarY)
		availW -= ImGui::GetStyle().ScrollbarSize;
	float minTile = font * gBrowserTileSize;
	layout.columns = max(availW >= font * 12.0f ? 2 : 1, (int)((availW + spacing) / (minTile + spacing)));
	layout.tileW = floorf((availW - spacing * (layout.columns - 1)) / layout.columns);
	layout.pad = floorf(font * 0.3f);
	layout.imageSide = layout.tileW - layout.pad * 2.0f;
	layout.tileH = layout.pad * 3.0f + layout.imageSide + ImGui::GetTextLineHeight();
	layout.rowH = layout.tileH + ImGui::GetStyle().ItemSpacing.y;
	return layout;
}

// One clickable tile: a thumbnail with an optional corner badge and a
// single-line title that ends in "..." when it does not fit.
static bool
uiAssetTile(const TileGridLayout &layout, rw::Texture *thumb, const char *badge,
	const char *title, bool selected)
{
	bool clicked = ImGui::InvisibleButton("##tile", ImVec2(layout.tileW, layout.tileH));
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	ImVec2 min = ImGui::GetItemRectMin();
	ImVec2 max = ImGui::GetItemRectMax();
	float rounding = ImGui::GetFontSize() * 0.3f;

	if(selected)
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_Header), rounding);
	else
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);

	ImVec2 imageMin(min.x + layout.pad, min.y + layout.pad);
	ImVec2 imageMax(imageMin.x + layout.imageSide, imageMin.y + layout.imageSide);
	if(thumb && thumb->raster)
		drawList->AddImageRounded((void*)(intptr_t)thumb, imageMin, imageMax,
			ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, rounding);
	else{
		drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
		const char *loading = "...";
		ImVec2 size = ImGui::CalcTextSize(loading);
		drawList->AddText(ImVec2((imageMin.x + imageMax.x - size.x) * 0.5f,
			(imageMin.y + imageMax.y - size.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), loading);
	}

	if(badge && badge[0] && (hovered || selected)){
		ImVec2 size = ImGui::CalcTextSize(badge);
		float inset = layout.pad * 0.6f;
		ImVec2 badgeMin(imageMin.x + inset, imageMin.y + inset);
		ImVec2 badgeMax(badgeMin.x + size.x + layout.pad * 1.4f, badgeMin.y + size.y + layout.pad * 0.4f);
		drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(0, 0, 0, 150), rounding * 0.6f);
		drawList->AddText(ImVec2(badgeMin.x + layout.pad * 0.7f, badgeMin.y + layout.pad * 0.2f),
			IM_COL32(235, 235, 235, 255), badge);
	}

	ImVec2 titleMin(min.x + layout.pad, imageMax.y + layout.pad);
	ImVec2 titleMax(max.x - layout.pad, titleMin.y + ImGui::GetTextLineHeight());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(
		selected || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
	ImGui::RenderTextEllipsis(drawList, titleMin, titleMax, titleMax.x, title, nil, nil);
	ImGui::PopStyleColor();

	if(selected)
		drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_CheckMark), rounding, 0, 2.0f);
	return clicked;
}

static void
uiObjectGrid(int *filtered, int numFiltered, int selId)
{
	ImGui::BeginChild("##ObjGrid", ImVec2(0, 0), false);
	TileGridLayout layout = uiTileGridLayout();
	int rows = (numFiltered + layout.columns - 1) / layout.columns;

	ImGuiListClipper clipper;
	clipper.Begin(rows, layout.rowH);
	while(clipper.Step()){
		for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++){
			for(int col = 0; col < layout.columns; col++){
				int filteredRow = row * layout.columns + col;
				if(filteredRow >= numFiltered)
					break;
				int i = filtered[filteredRow];
				ObjectDef *obj = GetObjectDef(i);
				if(obj == nil)
					continue;
				if(col > 0)
					ImGui::SameLine();
				ImGui::PushID(i);
				char badge[16];
				snprintf(badge, sizeof(badge), "%d", obj->m_id);
				if(uiAssetTile(layout, GetObjectThumbnailTexture(i), badge, obj->m_name, i == selId))
					selectBrowserObject(i);
				if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
					// Double-click starts placing it.
					if(gBrushMode) ExitBrushMode();
					if(gPrefabPlaceMode) ExitPrefabPlaceMode();
					gPlaceMode = true;
				}
				uiObjectFavouritePopup(i);
				if(ImGui::IsItemHovered())
					ImGui::SetItemTooltip(_("%d  %s\nDouble-click to place"), PawnModelId(obj->m_id), obj->m_name);
				ImGui::PopID();
			}
		}
	}
	ImGui::EndChild();
}

static void
uiObjectResults(int *filtered, int numFiltered, int selId)
{
	if(gBrowserThumbnailView)
		uiObjectGrid(filtered, numFiltered, selId);
	else
		uiObjectList(filtered, numFiltered, selId);
}

static void
getPrefabDisplayName(const char *file, char *buf, size_t bufSize)
{
	snprintf(buf, bufSize, "%s", file ? file : "");
	size_t len = strlen(buf);
	if(len > 7 && strcmp(buf + len - 7, ".haumea") == 0)
		buf[len - 7] = '\0';
}

static void
uiCenteredSquarePreview(rw::Texture *texture, float maxFontHeights)
{
	if(texture == nil || texture->raster == nil)
		return;

	float availableWidth = ImGui::GetContentRegionAvail().x;
	float previewSize = min(availableWidth, ImGui::GetFontSize() * maxFontHeights);
	if(previewSize <= 0.0f)
		return;

	float cursorX = ImGui::GetCursorPosX();
	if(availableWidth > previewSize)
		ImGui::SetCursorPosX(cursorX + (availableWidth - previewSize) * 0.5f);
	ImGui::Image((void*)(intptr_t)texture, ImVec2(previewSize, previewSize),
		ImVec2(0, 1), ImVec2(1, 0));
}

// ---- The prefab library ----
// Prefabs from Haumea's config folder (Mine), the workspace server's
// prefabs/ (Workspace), the starter set shipped with the editor (Starter) and
// the old game-folder location, with their subfolders.


static bool
uiIsDirectory(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void
uiScanPrefabFolder(const std::string &dir, const std::string &folder, int depth)
{
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if(h != INVALID_HANDLE_VALUE){
		do names.push_back(fd.cFileName); while(FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR *d = opendir(dir.c_str());
	if(d){
		while(struct dirent *e = readdir(d))
			names.push_back(e->d_name);
		closedir(d);
	}
#endif
	std::sort(names.begin(), names.end());
	for(const std::string &name : names){
		if(name.empty() || name[0] == '.')
			continue;
		std::string path = dir + "/" + name;
		if(uiIsDirectory(path)){
			if(depth < 4)
				uiScanPrefabFolder(path, folder + "/" + name, depth + 1);
		}else if(name.size() > 7 && name.compare(name.size() - 7, 7, ".haumea") == 0){
			PrefabListing prefab;
			prefab.path = path;
			prefab.name = name.substr(0, name.size() - 7);
			prefab.folder = folder;
			if(ReadPrefabInfo(path.c_str(), &prefab.info))
				gBrowserPrefabs.push_back(prefab);
		}
	}
}

// The starter prefabs: next to the editor in a release, in the source tree
// (tools/haumea/prefabs) when it runs from bin/<platform>/Release.
static std::string
uiStarterPrefabDir(void)
{
	char root[1024];
	if(!GetEditorRootDirectory(root, sizeof(root)))
		return "";
	std::string bundled = std::string(root) + "/prefabs";
	if(uiIsDirectory(bundled))
		return bundled;
	std::string source = std::string(root) + "/../../../tools/haumea/prefabs";
	return uiIsDirectory(source) ? source : "";
}

static void
refreshBrowserPrefabList(void)
{
	std::string selected = gBrowserSelectedPrefab >= 0 && gBrowserSelectedPrefab < (int)gBrowserPrefabs.size() ?
		gBrowserPrefabs[gBrowserSelectedPrefab].path : "";
	gBrowserPrefabs.clear();
	char prefabDir[512];
	if(GetHaumeaDataPath(prefabDir, sizeof(prefabDir), "prefabs"))
		uiScanPrefabFolder(prefabDir, "Mine", 0);
	if(GetWorkspaceDir()[0])
		uiScanPrefabFolder(WorkspacePath("prefabs"), "Workspace", 0);
	std::string starter = uiStarterPrefabDir();
	if(!starter.empty())
		uiScanPrefabFolder(starter, "Starter", 0);
	// Where prefabs used to live, before the config folder.
	if(GetGameDir()[0] && uiIsDirectory(GamePath("prefabs")))
		uiScanPrefabFolder(GamePath("prefabs"), "Game folder", 0);
	gBrowserSelectedPrefab = -1;
	for(size_t i = 0; i < gBrowserPrefabs.size(); i++)
		if(gBrowserPrefabs[i].path == selected)
			gBrowserSelectedPrefab = (int)i;
	gBrowserPrefabListDirty = false;
}

void
ListPrefabs(std::vector<PrefabListing> &out)
{
	refreshBrowserPrefabList();
	out = gBrowserPrefabs;
}

std::string
PrefabSaveDir(bool workspace, const char *folder)
{
	std::string dir;
	char base[512];
	if(workspace)
		dir = GetWorkspaceDir()[0] ? WorkspacePath("prefabs") : "";
	else if(GetHaumeaDataPath(base, sizeof(base), "prefabs"))
		dir = base;
	if(!dir.empty() && folder && folder[0])
		dir += std::string("/") + folder;
	return dir;
}

static void
importBrowserPrefab(int prefabIndex)
{
	if(prefabIndex < 0 || prefabIndex >= (int)gBrowserPrefabs.size())
		return;
	if(gPlaceMode) SpawnExitPlaceMode();
	if(gBrushMode) ExitBrushMode();
	EnterPrefabPlaceMode(gBrowserPrefabs[prefabIndex].path.c_str());
	Toast(TOAST_SPAWN, _("Click to place. Q/E or the wheel turn it, Esc stops."));
}

static void
uiPrefabTooltip(const PrefabListing &prefab)
{
	if(prefab.info.tags.empty())
		ImGui::SetItemTooltip(_("%s\n%d instance(s), %d SA-MP element(s), %d vehicle(s)\n%s\nDouble-click to place"),
			prefab.name.c_str(), prefab.info.instances, prefab.info.elements, prefab.info.vehicles,
			prefab.path.c_str());
	else
		ImGui::SetItemTooltip(_("%s\n%d instance(s), %d SA-MP element(s), %d vehicle(s)\nTags: %s\n%s\nDouble-click to place"),
			prefab.name.c_str(), prefab.info.instances, prefab.info.elements, prefab.info.vehicles,
			prefab.info.tags.c_str(), prefab.path.c_str());
}

static void
uiPrefabList(int *filtered, int numFiltered)
{
	ImGui::BeginChild("##PrefabList", ImVec2(0, 0), true);
	ImGuiListClipper clipper;
	clipper.Begin(numFiltered);
	while(clipper.Step()){
		for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++){
			int i = filtered[row];
			const PrefabListing &prefab = gBrowserPrefabs[i];
			ImGui::PushID(i);
			char label[320];
			snprintf(label, sizeof(label), "%s  (%d)", prefab.name.c_str(), prefab.info.instances);
			if(ImGui::Selectable(label, gBrowserSelectedPrefab == i))
				gBrowserSelectedPrefab = i;
			if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				importBrowserPrefab(i);
			uiPrefabTooltip(prefab);
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

static void
uiPrefabGrid(int *filtered, int numFiltered)
{
	ImGui::BeginChild("##PrefabGrid", ImVec2(0, 0), false);
	TileGridLayout layout = uiTileGridLayout();
	int rows = (numFiltered + layout.columns - 1) / layout.columns;

	ImGuiListClipper clipper;
	clipper.Begin(rows, layout.rowH);
	while(clipper.Step()){
		for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++){
			for(int col = 0; col < layout.columns; col++){
				int filteredRow = row * layout.columns + col;
				if(filteredRow >= numFiltered)
					break;
				int i = filtered[filteredRow];
				const PrefabListing &prefab = gBrowserPrefabs[i];
				if(col > 0)
					ImGui::SameLine();
				ImGui::PushID(i);
				char badge[16];
				snprintf(badge, sizeof(badge), "%d", prefab.info.instances);
				if(uiAssetTile(layout, GetPrefabThumbnailTexture(prefab.path.c_str()), badge, prefab.name.c_str(),
					i == gBrowserSelectedPrefab))
					gBrowserSelectedPrefab = i;
				if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					importBrowserPrefab(i);
				uiPrefabTooltip(prefab);
				ImGui::PopID();
			}
		}
	}
	ImGui::EndChild();
}

static void
uiPrefabResults(int *filtered, int numFiltered)
{
	if(gBrowserThumbnailView)
		uiPrefabGrid(filtered, numFiltered);
	else
		uiPrefabList(filtered, numFiltered);
}

// The folders as a small tree, each with how many prefabs it holds.
static void
uiPrefabFolders(void)
{
	std::map<std::string, int> counts;	// a folder counts its subfolders' prefabs too
	for(const PrefabListing &prefab : gBrowserPrefabs){
		std::string folder = prefab.folder;
		while(true){
			counts[folder]++;
			size_t slash = folder.rfind('/');
			if(slash == std::string::npos)
				break;
			folder.resize(slash);
		}
	}
	char label[320];
	snprintf(label, sizeof(label), _("All (%d)"), (int)gBrowserPrefabs.size());
	if(ImGui::Selectable(label, gBrowserPrefabFolder.empty()))
		gBrowserPrefabFolder.clear();
	for(auto &c : counts){
		int depth = (int)std::count(c.first.begin(), c.first.end(), '/');
		size_t slash = c.first.rfind('/');
		std::string name = slash == std::string::npos ? c.first : c.first.substr(slash + 1);
		ImGui::Indent(ImGui::GetFontSize() * (depth + 1));
		snprintf(label, sizeof(label), "%s %s (%d)##%s", ICON_FA_FOLDER_OPEN, name.c_str(), c.second, c.first.c_str());
		if(ImGui::Selectable(label, gBrowserPrefabFolder == c.first))
			gBrowserPrefabFolder = c.first;
		ImGui::Unindent(ImGui::GetFontSize() * (depth + 1));
	}
}

static void
uiBrowserPrefabsTab(void)
{
	if(gBrowserPrefabListDirty)
		refreshBrowserPrefabList();

	float refreshWidth = ImGui::CalcTextSize(ICON_FA_ROTATE).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(-(refreshWidth + ImGui::GetStyle().ItemSpacing.x));
	if(ImGui::InputTextWithHint("##search_prefabs", TI(ICON_FA_MAGNIFYING_GLASS, "Search names, tags and folders"),
	   gBrowserPrefabFilter.InputBuf, IM_ARRAYSIZE(gBrowserPrefabFilter.InputBuf)))
		gBrowserPrefabFilter.Build();
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_ROTATE "##refresh_prefabs"))
		refreshBrowserPrefabList();
	ImGui::SetItemTooltip(_("Read the prefab folders again."));

	char foldersHeader[160];
	snprintf(foldersHeader, sizeof(foldersHeader), _("Folder: %s###prefabfolders"),
		gBrowserPrefabFolder.empty() ? _("all") : gBrowserPrefabFolder.c_str());
	if(ImGui::CollapsingHeader(foldersHeader))
		uiPrefabFolders();

	if(gBrowserSelectedPrefab >= 0 && gBrowserSelectedPrefab < (int)gBrowserPrefabs.size()){
		const PrefabListing &prefab = gBrowserPrefabs[gBrowserSelectedPrefab];
		// A small picture with the details and buttons beside it.
		rw::Texture *thumb = GetPrefabThumbnailTexture(prefab.path.c_str());
		float side = ImGui::GetFontSize() * 6.0f;
		if(thumb && thumb->raster)
			ImGui::Image((void*)(intptr_t)thumb, ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
		else
			ImGui::Dummy(ImVec2(side, side));
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextUnformatted(prefab.name.c_str());
		ImGui::TextDisabled(_("%s, %d instance(s)"), prefab.folder.c_str(), prefab.info.instances);
		ImGui::SetItemTooltip(_("%d SA-MP element(s), %d vehicle(s)\n%s"), prefab.info.elements,
			prefab.info.vehicles, prefab.path.c_str());
		if(!prefab.info.tags.empty())
			ImGui::TextDisabled(_("Tags: %s"), prefab.info.tags.c_str());
		bool placing = gPrefabPlaceMode && prefab.path == GetPrefabPlacePath();
		if(ImGui::Button(placing ? _("Stop placing") : TI(ICON_FA_PLUS, "Place"))){
			if(gPrefabPlaceMode)
				ExitPrefabPlaceMode();
			if(!placing)
				importBrowserPrefab(gBrowserSelectedPrefab);
		}
		ImGui::SetItemTooltip(_("Click in the world to place it. Q/E (or , and .) and the wheel turn it, Shift\n"
			"in smaller steps; Shift+click keeps placing; Esc stops."));
		ImGui::SameLine();
		ImGui::TextDisabled("%.0f\xC2\xB0", fmodf(fmodf(gPrefabPlaceHeading, 360.0f) + 360.0f, 360.0f));
		ImGui::SetItemTooltip(_("The heading it is placed at."));
		ImGui::Checkbox(_("Keep placing"), &gPrefabPlaceRepeat);
		ImGui::SetItemTooltip(_("Stay in placement after each click, as Shift+click does."));
		ImGui::SameLine();
		ImGui::Checkbox(_("As a group"), &gPrefabPlaceAsGroup);
		ImGui::SetItemTooltip(_("Put what is placed in a group named after the prefab."));
		ImGui::EndGroup();
		ImGui::Separator();
	}

	std::vector<int> filtered;
	for(int i = 0; i < (int)gBrowserPrefabs.size(); i++){
		const PrefabListing &prefab = gBrowserPrefabs[i];
		if(!gBrowserPrefabFolder.empty() && prefab.folder != gBrowserPrefabFolder &&
		   prefab.folder.compare(0, gBrowserPrefabFolder.size() + 1, gBrowserPrefabFolder + "/") != 0)
			continue;
		if(!gBrowserPrefabFilter.PassFilter(prefab.name.c_str()) && !gBrowserPrefabFilter.PassFilter(prefab.info.tags.c_str()) &&
		   !gBrowserPrefabFilter.PassFilter(prefab.folder.c_str()))
			continue;
		filtered.push_back(i);
	}
	ImGui::TextDisabled(_("Prefabs: %d"), (int)filtered.size());
	if(gBrowserPrefabs.empty())
		ImGui::TextDisabled(_("No prefabs yet: select objects and use File > Export > Selection as Prefab."));
	else if(!filtered.empty())
		uiPrefabResults(filtered.data(), (int)filtered.size());
}

static void
uiOpenObjectDiscovery(int model, bool similar, const char *ids)
{
	showBrowserWindow = true;
	gBrowserDiscoveryFocusPending = true;
	selectBrowserObject(model);
	if(similar) BrowserFindSimilar(model);
	else if(ids) BrowserFindInstanceQuery(ids);
	else BrowserFindInstances(model);
}

static void
uiBrowserSelectedObject(int selId)
{
	if(selId < 0)
		return;

	ObjectDef *sel = GetObjectDef(selId);
	if(sel == nil)
		return;


	ImGui::BeginGroup();
	ImGui::TextUnformatted(sel->m_name);
	ImGui::TextDisabled(_("ID %d"), PawnModelId(sel->m_id));

	// Place and Brush are mutually exclusive
	if(gPlaceMode){
		if(ImGui::Button(_("Stop placing")))
			SpawnExitPlaceMode();
	}else if(ImGui::Button(TI(ICON_FA_PLUS, "Place"))){
		if(gBrushMode) ExitBrushMode();
		if(gPrefabPlaceMode) ExitPrefabPlaceMode();
		gPlaceMode = true;
	}
	ImGui::SetItemTooltip(_("Choose Place, then click the world. Double-click a tile also works."));
	ImGui::SameLine();
	ImGui::Checkbox(_("Keep placing"), &gPlaceRepeat);
	ImGui::SetItemTooltip(_("Stay in placement after each click, as Shift+click does."));
	float iconButtonsWidth = ImGui::GetFrameHeight()*2 + ImGui::GetStyle().ItemSpacing.x*2;
	if(ImGui::GetItemRectMax().x + iconButtonsWidth < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
		ImGui::SameLine();
	bool favourite = IsFavourite(selId);
	if(favourite)
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
	if(ImGui::Button(ICON_FA_STAR "##favourite"))
		ToggleFavourite(selId);
	if(favourite)
		ImGui::PopStyleColor();
	ImGui::SetItemTooltip("%s", favourite ? _("Remove from Favourites") : _("Add to Favourites"));
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_GEAR "##object_actions"))
		ImGui::OpenPopup("##object_actions");
	ImGui::SetItemTooltip(_("More actions"));
	if(ImGui::BeginPopup("##object_actions")){
		if(ImGui::MenuItem(gBrushMode ? _("Stop brush") : _("Brush with this object"))){
			if(gBrushMode) ExitBrushMode();
			else EnterBrushMode(selId);
		}
		if(ImGui::MenuItem(_("Find instances"))){
			const char *ids = !BrowserDiscoveryActive() && gBrowserActiveTab == BROWSER_TAB_CATEGORIES &&
				ParseModelIdQuery(gBrowserCategoryFilter.InputBuf, nil) ? gBrowserCategoryFilter.InputBuf : nil;
			uiOpenObjectDiscovery(selId, false, ids);
		}
		if(ImGui::MenuItem(_("Find similar models")))
			uiOpenObjectDiscovery(selId, true);
		ImGui::Spacing();
		if(ImGui::MenuItem(_("Copy model name")))
			SetEditorClipboardText(sel->m_name);
		if(ImGui::MenuItem(_("Copy model ID"))){
			char id[16];
			snprintf(id, sizeof(id), "%d", sel->m_id);
			SetEditorClipboardText(id);
		}
		ImGui::EndPopup();
	}
	if(ImGui::CollapsingHeader(L("Model details###browser_model_details"))){
		TxdDef *txd = GetTxdDef(sel->m_txdSlot);
		ImGui::TextDisabled(_("TXD %s"), txd ? txd->name : "-");
		int lodId = GetLodForObject(selId);
		ObjectDef *lod = lodId >= 0 ? GetObjectDef(lodId) : nil;
		if(lod)
			ImGui::TextDisabled(_("Draw distance %.0f  ·  LOD %s"), sel->GetLargestDrawDist(), lod->m_name);
		else
			ImGui::TextDisabled(_("Draw distance %.0f"), sel->GetLargestDrawDist());
	}
	ImGui::EndGroup();
	ImGui::Spacing();
}

// Shown in place of the recent list until something has been placed:
// shortcuts into the categories mappers use most.
static void
uiBrowserStartSuggestions(void)
{
	static const char *suggestions[] = {
		N_("Trees"), N_("Bushes and plants"), N_("Rocks and cliffs"), N_("Fences, walls and barriers"),
		N_("Roads"), N_("Benches, bins and street furniture"), N_("Building blocks"),
		N_("Signs and statues"), N_("Houses"), N_("Furniture"), N_("Lights"), N_("Boxes and crates")
	};
	ImGui::Spacing();
	ImGui::TextDisabled(_("Start with a category."));
	ImGui::Spacing();
	// Cards like a content browser's folders, each showing one of its models.
	// The first model of each category, found once.
	static std::vector<int> examples;
	if((int)examples.size() != NUM_OBJ_CATEGORIES){
		examples.assign(NUM_OBJ_CATEGORIES, -1);
		for(int i = 0; i < NUMOBJECTDEFS; i++){
			if(GetObjectDef(i) == nil)
				continue;
			int cat = GetObjectCategory(i);
			for(int c = 0; c < NUM_OBJ_CATEGORIES && cat >= 0; c++)
				if(examples[c] < 0 && isCategoryOrChild(cat, c))
					examples[c] = i;
		}
	}
	ImGui::BeginChild("##category_cards", ImVec2(0, 0), ImGuiChildFlags_None);
	TileGridLayout layout = uiTileGridLayout();
	int shown = 0;
	for(int i = 0; i < IM_ARRAYSIZE(suggestions); i++){
		int cat = -1;
		for(int c = 0; c < NUM_OBJ_CATEGORIES; c++)
			if(strcmp(objCategories[c].name, suggestions[i]) == 0){
				cat = c;
				break;
			}
		if(cat < 0)
			continue;
		if(shown++ % layout.columns)
			ImGui::SameLine();
		ImGui::PushID(i);
		rw::Texture *thumb = examples[cat] >= 0 ? GetObjectThumbnailTexture(examples[cat]) : nil;
		if(uiAssetTile(layout, thumb, ICON_FA_FOLDER_OPEN, suggestions[i], false))
			gBrowserSelectedCategory = cat;
		ImGui::SetItemTooltip("%s", suggestions[i]);
		ImGui::PopID();
	}
	ImGui::EndChild();
}

// Compact list / tile switch. Keep its sizing and context options in one place
// so it can sit beside search instead of occupying a separate toolbar row.
static void
uiBrowserViewToggle(void)
{
	if(ImGui::Button(gBrowserThumbnailView ? ICON_FA_TABLE_COLUMNS "##browser_view" : ICON_FA_LIST "##browser_view"))
		gBrowserThumbnailView = !gBrowserThumbnailView;
	ImGui::SetItemTooltip(_("Switch between tiles and list. Right-click for tile size."));
	if(ImGui::BeginPopupContextItem("##browser_view_options")){
		if(ImGui::RadioButton(_("List"), !gBrowserThumbnailView))
			gBrowserThumbnailView = false;
		ImGui::SameLine();
		if(ImGui::RadioButton(_("Tiles"), gBrowserThumbnailView))
			gBrowserThumbnailView = true;
		if(gBrowserThumbnailView){
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
			ImGui::SliderFloat(_("Tile size"), &gBrowserTileSize, 4.0f, 14.0f, "%.0f");
		}
		ImGui::EndPopup();
	}
}

// Full-width search field. Ctrl+F focuses it. On model tabs, the view switch
// shares this row so it does not push the browser content down by itself.
static void
uiBrowserSearchBox(ImGuiTextFilter &filter, const char *id, const char *hint, bool showViewToggle = false)
{
	if(ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteFromRootWindow))
		ImGui::SetKeyboardFocusHere();
	float toggleWidth = showViewToggle ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
	// While it narrows the list, the field is tinted and has a clear button,
	// so it is plain why objects are missing.
	bool active = filter.InputBuf[0] != '\0';
	float clearWidth = active ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - toggleWidth - clearWidth);
	if(active) ImGui::PushStyleColor(ImGuiCol_Border, UiSemanticColor(UI_COLOR_WARNING));
	if(ImGui::InputTextWithHint(id, hint, filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf)))
		filter.Build();
	if(active){
		ImGui::PopStyleColor();
		ImGui::SameLine();
		char clearId[64];
		snprintf(clearId, sizeof(clearId), ICON_FA_XMARK "##clear%s", id);
		if(ImGui::Button(clearId, ImVec2(ImGui::GetFrameHeight(), 0))){
			filter.InputBuf[0] = '\0';
			filter.Build();
		}
		ImGui::SetItemTooltip("%s", _("Clear"));
	}
	if(showViewToggle){
		ImGui::SameLine();
		uiBrowserViewToggle();
	}
}

static bool
uiBrowserIsIdQuery(ImGuiTextFilter &filter)
{
	return ParseModelIdQuery(filter.InputBuf, nil);
}

// Name search; numbers, lists and ranges of numbers find models by ID.
static bool
uiBrowserMatches(ImGuiTextFilter &filter, ObjectDef *obj)
{
	static std::string lastQuery;
	static std::vector<std::pair<int,int>> ranges;
	static bool isIdQuery;
	if(lastQuery != filter.InputBuf){
		lastQuery = filter.InputBuf;
		ranges.clear();
		isIdQuery = ParseModelIdQuery(filter.InputBuf, &ranges);
	}
	if(isIdQuery){
		for(size_t i = 0; i < ranges.size(); i++)
			if(PawnModelId(obj->m_id) >= ranges[i].first && PawnModelId(obj->m_id) <= ranges[i].second)
				return true;
		return false;
	}
	return filter.PassFilter(obj->m_name);
}

static void
uiStartSampPlacement(int kind, int model)
{
	if(gBrushMode) ExitBrushMode();
	if(gPrefabPlaceMode) ExitPrefabPlaceMode();
	selectBrowserObject(model);
	SetSpawnElementKind(kind);
	gPlaceMode = true;
}

// Placement tools are grouped by what they add to the map.
static int
uiElementFeature(int kind)
{
	switch(kind){
	case SAMP_ELEMENT_ACTOR: case SAMP_ELEMENT_CLASS: case SAMP_ELEMENT_NPC: return BROWSER_CHARACTERS;
	case SAMP_ELEMENT_PATHPOINT: return BROWSER_PATHS;
	case SAMP_ELEMENT_PICKUP: case SAMP_ELEMENT_MAPICON: case SAMP_ELEMENT_CHECKPOINT: return BROWSER_MARKERS;
	case SAMP_ELEMENT_LABEL: case SAMP_ELEMENT_SOUND: return BROWSER_TEXT_AUDIO;
	default: return BROWSER_ZONES;
	}
}

static void
uiBrowserElementsTab(void)
{
	static int pickupModel = 1239;
	static int skins[] = { 7, 7, 7 }; // Independent choices for actor, class and NPC.
	struct Row { int kind; const char *icon; const char *help; const char *badge; };
	static const Row rows[] = {
		{ SAMP_ELEMENT_ACTOR, ICON_FA_USER, N_("Place a character with a skin and animation."), nil },
		{ SAMP_ELEMENT_CLASS, ICON_FA_LOCATION_DOT, N_("Add a player spawn point."), nil },
		{ SAMP_ELEMENT_NPC, ICON_FA_USER, N_("Place an NPC that can move and interact."), N_("open.mp only") },
		{ SAMP_ELEMENT_PATHPOINT, ICON_FA_LOCATION_CROSSHAIRS, N_("Build an NPC path, one point at a time."), N_("open.mp only") },
		{ SAMP_ELEMENT_PICKUP, ICON_FA_STAR, N_("Place an item players can collect."), nil },
		{ SAMP_ELEMENT_MAPICON, ICON_FA_MAP_PIN, N_("Add a marker to the radar."), nil },
		{ SAMP_ELEMENT_CHECKPOINT, ICON_FA_LOCATION_CROSSHAIRS, N_("Place a checkpoint or add it to a race."), N_("streamer only") },
		{ SAMP_ELEMENT_LABEL, ICON_FA_FONT, N_("Add text in the 3D world."), nil },
		{ SAMP_ELEMENT_SOUND, ICON_FA_VOLUME_HIGH, N_("Play a sound or audio stream near this point."), nil },
		{ SAMP_ELEMENT_GANGZONE, ICON_FA_SQUARE, N_("Mark a coloured area on the radar."), nil },
		{ SAMP_ELEMENT_AREA, ICON_FA_VECTOR_SQUARE, N_("Limit streamed elements to an area."), N_("streamer only") },
	};
	const Row *selected = nil;
	int choices = 0;
	for(const Row &r : rows){
		if(uiElementFeature(r.kind) != gBrowserElementFeature) continue;
		choices++;
		if(selected == nil || r.kind == gBrowserElementKind) selected = &r;
	}
	if(selected == nil) return;
	gBrowserElementKind = selected->kind;
	if(choices > 1){
		for(const Row &r : rows){
			if(uiElementFeature(r.kind) != gBrowserElementFeature) continue;
			char label[160];
			snprintf(label, sizeof(label), "%s  %s##place %s", r.icon,
				_(GetSampElementKindName(r.kind)), GetSampElementKindName(r.kind));
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
			bool clicked = ImGui::Selectable(label, gBrowserElementKind == r.kind, 0,
				ImVec2(0, ImGui::GetFontSize()*2.1f));
			ImGui::PopStyleVar();
			ImGui::SetItemTooltip("%s%s%s", _(r.help), r.badge ? "\n" : "", r.badge ? _(r.badge) : "");
			if(clicked && r.kind != gBrowserElementKind){
				if(gPlaceMode) SpawnExitPlaceMode();
				gBrowserElementKind = r.kind;
				selected = &r;
			}
			if(ImGui::BeginPopupContextItem()){
				int count = 0;
				for(CPtrNode *p = instances.first; p; p = p->next){
					ObjectInst *inst = (ObjectInst*)p->item;
					if(!inst->m_isDeleted && GetSampElementKind(inst) == r.kind) count++;
				}
				char countLabel[48];
				snprintf(countLabel, sizeof(countLabel), _("%d in map"), count);
				if(ImGui::MenuItem(countLabel, nil, false, count > 0)){
					ClearSelection();
					for(CPtrNode *p = instances.first; p; p = p->next){
						ObjectInst *inst = (ObjectInst*)p->item;
						if(!inst->m_isDeleted && GetSampElementKind(inst) == r.kind) inst->Select();
					}
					showInstanceWindow = true;
				}
				ImGui::EndPopup();
			}
		}
		ImGui::Spacing();
	}
	int kind = selected->kind;
	bool character = kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_NPC;
	int skinIndex = kind == SAMP_ELEMENT_ACTOR ? 0 : kind == SAMP_ELEMENT_CLASS ? 1 : 2;
	int model = character ? GetSampElementDisplayModel(kind, skins[skinIndex]) :
		kind == SAMP_ELEMENT_PICKUP ? pickupModel : GetSampElementProxyModel(kind);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	ImGui::BeginChild("##element_setup", ImVec2(0, 0),
		ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::TextUnformatted(_(GetSampElementKindName(kind)));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", _(selected->help));
	ImGui::PopStyleColor();
	ImGui::Spacing();
	bool changed = false;
	if(character || kind == SAMP_ELEMENT_PICKUP){
		float side = min(ImGui::GetFontSize()*6.0f, ImGui::GetContentRegionAvail().x*0.4f);
		rw::Texture *thumb = GetObjectThumbnailTexture(model);
		if(thumb) ImGui::Image((ImTextureID)thumb, ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
		else ImGui::Dummy(ImVec2(side, side));
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s", character ? _("Skin") : _("Model"));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(character) changed = uiSkinPicker("##place_skin", &skins[skinIndex]);
		else changed = uiNamedValueCombo("##place_pickup", &pickupModel, uiPickupModels, IM_ARRAYSIZE(uiPickupModels));
		ImGui::EndGroup();
	}else if(kind == SAMP_ELEMENT_PATHPOINT){
		char pathName[PAWN_OBJECT_NAME_LEN];
		snprintf(pathName, sizeof(pathName), "%s", GetSpawnPathName());
		ImGui::TextDisabled(_("Path"));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(uiNpcPathPicker("##place_path", pathName, sizeof(pathName), false)) SetSpawnPathName(pathName);
		std::vector<ObjectInst*> points;
		GetNpcPathPoints(GetSpawnPathName(), points);
		ImGui::TextDisabled(_("%d points"), (int)points.size());
	}else if(kind == SAMP_ELEMENT_CHECKPOINT){
		char race[PAWN_OBJECT_NAME_LEN];
		snprintf(race, sizeof(race), "%s", GetSpawnRaceName());
		ImGui::TextDisabled(_("Race"));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::InputTextWithHint("##place_race", _("none: plain checkpoints"), race, sizeof(race)) &&
		   (race[0] == '\0' || IsValidPawnObjectName(race))) SetSpawnRaceName(race);
	}
	model = character ? GetSampElementDisplayModel(kind, skins[skinIndex]) :
		kind == SAMP_ELEMENT_PICKUP ? pickupModel : GetSampElementProxyModel(kind);
	bool placing = gPlaceMode && GetSpawnElementKind() == kind;
	if(changed && placing) uiStartSampPlacement(kind, model);
	ImGui::Spacing();
	ImGui::Checkbox(_("Keep placing"), &gPlaceRepeat);
	ImGui::SetItemTooltip(_("Stay in placement after each click, as Shift+click does."));
	ImGui::Spacing();
	char action[96];
	snprintf(action, sizeof(action), "%s##element_place", placing ? _("Stop placing") : _("Place"));
	ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.3f));
	ImGui::BeginDisabled(GetObjectDef(model) == nil);
	if(ImGui::Button(action, ImVec2(-FLT_MIN, 0))){
		if(placing) SpawnExitPlaceMode();
		else uiStartSampPlacement(kind, model);
	}
	ImGui::EndDisabled();
	ImGui::PopStyleColor();
	if(placing) ImGui::TextWrapped(_("Click in the world. Esc stops."));
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

static void
uiDiscoveryModelResults(int *ids, int count, int selected)
{
	uiBrowserSelectedObject(selected);
	uiObjectResults(ids, count, selected);
}

static void
uiBrowserWindow(void)
{
	uiDefaultPanelLayout(PANEL_LEFT_TOP);
	uiNextPanelSizeConstraints(ImVec2(360, 360));
	if(gBrowserDiscoveryFocusPending){ ImGui::SetNextWindowFocus(); gBrowserDiscoveryFocusPending = false; }
	ImGui::Begin(LI(ICON_FA_MAGNIFYING_GLASS, "Object Browser"), &showBrowserWindow);
	uiFitItemsToPanel();

	int selId = GetSpawnObjectId();
	static int filtered[NUMOBJECTDEFS];
	int numFiltered = 0;

	if(BrowserDiscoveryActive()){
		if(BrowserDiscoveryShowsModels())
			uiBrowserViewToggle();
		BrowserDiscoveryDraw(uiDiscoveryModelResults, selId);
		ImGui::End();
		return;
	}

	// Docked in a wide, low strip (under the 3D view), the browser lays out
	// like a content browser: what to show in a column on the left, the
	// thumbnails filling the rest. In a tall column everything stacks.
	ImVec2 room = ImGui::GetContentRegionAvail();
	bool wide = room.x > room.y * 1.7f && room.x > ImGui::GetFontSize() * 34.0f;
	bool sideOpen = false, resultsOpen = false;
	if(wide){
		ImGui::BeginChild("##browser_side", ImVec2(ImGui::GetFontSize() * 15.0f, 0), ImGuiChildFlags_None);
		sideOpen = true;
	}
	auto toResults = [&](){
		if(!sideOpen || resultsOpen)
			return;
		ImGui::EndChild();
		ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.6f);
		// A quiet line between the two halves.
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		c.w *= 0.25f;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - ImGui::GetFontSize()*0.3f, p.y),
			ImVec2(p.x - ImGui::GetFontSize()*0.3f, p.y + ImGui::GetContentRegionAvail().y), ImGui::GetColorU32(c));
		ImGui::BeginChild("##browser_results", ImVec2(0, 0), ImGuiChildFlags_None);
		resultsOpen = true;
	};

	// group: the element feature for Elements, the shelf (objects, skins,
	// vehicles) for Categories.
	struct Feature { int tab, group; const char *name; };
	static const Feature features[] = {
		{ BROWSER_TAB_CATEGORIES, BROWSER_KIND_OBJECTS, N_("Objects") },
		{ BROWSER_TAB_CATEGORIES, BROWSER_KIND_SKINS, N_("Skins") },
		{ BROWSER_TAB_CATEGORIES, BROWSER_KIND_VEHICLES, N_("Vehicles") },
		{ BROWSER_TAB_ELEMENTS, BROWSER_CHARACTERS, N_("Characters") },
		{ BROWSER_TAB_ELEMENTS, BROWSER_PATHS, N_("Paths") },
		{ BROWSER_TAB_ELEMENTS, BROWSER_MARKERS, N_("Markers") },
		{ BROWSER_TAB_ELEMENTS, BROWSER_TEXT_AUDIO, N_("Text & audio") },
		{ BROWSER_TAB_ELEMENTS, BROWSER_ZONES, N_("Zones") },
		{ BROWSER_TAB_PREFABS, -1, N_("Prefabs") },
		{ BROWSER_TAB_FAVOURITES, -1, N_("Favourites") },
		{ BROWSER_TAB_IDE, -1, N_("Model files") },
	};
	auto featureActive = [](const Feature &feature){
		if(gBrowserActiveTab != feature.tab) return false;
		if(feature.tab == BROWSER_TAB_CATEGORIES) return feature.group == gBrowserKind;
		return feature.group < 0 || feature.group == gBrowserElementFeature;
	};
	const char *featureName = _("Objects");
	for(const Feature &feature : features)
		if(featureActive(feature))
			featureName = _(feature.name);
	ImGui::SetNextItemWidth(-FLT_MIN);
	if(ImGui::BeginCombo("##browser_source", featureName, ImGuiComboFlags_HeightLarge)){
		for(const Feature &feature : features){
			bool active = featureActive(feature);
			if(ImGui::Selectable(_(feature.name), active) && !active){
				if(gPlaceMode) SpawnExitPlaceMode();
				gBrowserActiveTab = feature.tab;
				if(feature.tab == BROWSER_TAB_CATEGORIES && feature.group != gBrowserKind){
					// Objects open on what was placed recently; skins and
					// vehicles, a short catalogue, on all of them.
					gBrowserKind = feature.group;
					gBrowserSelectedCategory = gBrowserKind == BROWSER_KIND_OBJECTS ? BROWSER_CATEGORY_RECENT : BROWSER_CATEGORY_ALL;
				}else if(feature.group >= 0)
					gBrowserElementFeature = feature.group;
			}
			// Objects, skins and vehicles first, then the map's other elements.
			if(feature.tab == BROWSER_TAB_CATEGORIES && feature.group == BROWSER_KIND_VEHICLES)
				ImGui::Separator();
		}
		ImGui::EndCombo();
	}
	ImGui::Spacing();
	if(gBrowserActiveTab == BROWSER_TAB_CATEGORIES)
		uiBrowserSearchBox(gBrowserCategoryFilter, "##search_objects", TI(ICON_FA_MAGNIFYING_GLASS, "Search"), true);
	else if(gBrowserActiveTab == BROWSER_TAB_FAVOURITES)
		uiBrowserSearchBox(gBrowserFavFilter, "##search_fav", TI(ICON_FA_MAGNIFYING_GLASS, "Search"), true);
	else if(gBrowserActiveTab == BROWSER_TAB_IDE)
		uiBrowserSearchBox(gBrowserIdeFilter, "##search_ide", TI(ICON_FA_MAGNIFYING_GLASS, "Search"), true);

	{
		// Outside Categories, the side column holds only the source and search.
		if(gBrowserActiveTab != BROWSER_TAB_CATEGORIES)
			toResults();
		if(gBrowserActiveTab == BROWSER_TAB_CATEGORIES){
			gBrowserActiveTab = BROWSER_TAB_CATEGORIES;
			if(gBrowserSelectedCategory >= 0 && !browserKindHasCategory(gBrowserKind, gBrowserSelectedCategory))
				gBrowserSelectedCategory = gBrowserKind == BROWSER_KIND_OBJECTS ? BROWSER_CATEGORY_RECENT : BROWSER_CATEGORY_ALL;

			uiBrowserSelectedObject(selId);
			// Category dropdown: the shelf's own categories only.
			const char *allLabel = gBrowserKind == BROWSER_KIND_SKINS ? _("People (skins)") :
				gBrowserKind == BROWSER_KIND_VEHICLES ? _("Vehicles") : _("All objects");
			char catLabel[128];
			if(gBrowserSelectedCategory >= 0 && gBrowserSelectedCategory < NUM_OBJ_CATEGORIES)
				snprintf(catLabel, sizeof(catLabel), "%s", objCategories[gBrowserSelectedCategory].name);
			else if(gBrowserSelectedCategory == BROWSER_CATEGORY_RECENT)
				snprintf(catLabel, sizeof(catLabel), TI(ICON_FA_CLOCK_ROTATE_LEFT, "Recently placed"));
			else
				snprintf(catLabel, sizeof(catLabel), "%s", allLabel);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if(ImGui::BeginCombo("##CatCombo", catLabel, ImGuiComboFlags_HeightLarge)){
				if(ImGui::Selectable(TI(ICON_FA_CLOCK_ROTATE_LEFT, "Recently placed"), gBrowserSelectedCategory == BROWSER_CATEGORY_RECENT))
					gBrowserSelectedCategory = BROWSER_CATEGORY_RECENT;
				if(ImGui::Selectable(allLabel, gBrowserSelectedCategory == BROWSER_CATEGORY_ALL))
					gBrowserSelectedCategory = BROWSER_CATEGORY_ALL;
				bool separated = false;
				static char lb[128];
				for(int c = 0; c < NUM_OBJ_CATEGORIES; c++){
					if(!browserKindHasCategory(gBrowserKind, c))
						continue;
					if(!separated){ ImGui::Separator(); separated = true; }
					buildCategoryLabel(c, lb, sizeof(lb));
					if(gBrowserKind == BROWSER_KIND_VEHICLES)	// one level, under Vehicles
						snprintf(lb, sizeof(lb), "%s", objCategories[c].name);
					bool isSel = (c == gBrowserSelectedCategory);
					if(ImGui::Selectable(lb, isSel)){
						gBrowserSelectedCategory = c;
					}
				}
				ImGui::EndCombo();
			}
			uiObjectFiltersPanel();
			toResults();

			// Searching from the recent list searches everything.
			bool showRecent = gBrowserSelectedCategory == BROWSER_CATEGORY_RECENT &&
				!gBrowserCategoryFilter.IsActive();
			// An ID search finds the model on whichever shelf it is.
			bool idQuery = uiBrowserIsIdQuery(gBrowserCategoryFilter);
			numFiltered = 0;
			if(showRecent){
				const int *recent;
				int numRecent = GetRecentObjects(&recent);
				for(int i = 0; i < numRecent; i++){
					ObjectDef *obj = GetObjectDef(recent[i]);
					if(obj && browserKindOf(obj) == gBrowserKind && ObjectPassesFilters(recent[i]))
						filtered[numFiltered++] = recent[i];
				}
				// Skins and vehicles have no starter cards: show them all.
				if(numFiltered == 0 && gBrowserKind != BROWSER_KIND_OBJECTS)
					showRecent = false;
			}
			if(!showRecent) for(int i = 0; i < NUMOBJECTDEFS; i++){
				ObjectDef *obj = GetObjectDef(i);
				if(obj == nil) continue;
				if(!idQuery && browserKindOf(obj) != gBrowserKind) continue;
				if(gBrowserSelectedCategory >= 0 && !idQuery){
					int cat = GetObjectCategory(i);
					if(cat < 0 || !isCategoryOrChild(cat, gBrowserSelectedCategory))
						continue;
				}
				if(!uiBrowserMatches(gBrowserCategoryFilter, obj)) continue;
				if(!ObjectPassesFilters(i)) continue;
				filtered[numFiltered++] = i;
			}
			if(!showRecent)
				SortObjectResults(filtered, numFiltered);
			// A useful first shelf, followed by the full catalogue. Explicit
			// categories and searches retain their normal result ordering.
			if(gBrowserKind == BROWSER_KIND_OBJECTS && gBrowserSelectedCategory == BROWSER_CATEGORY_ALL && !gBrowserCategoryFilter.IsActive()){
				const int starters[] = { 3555, 728, 1280, 1226, 1337, 1211, 1271, 2232, 3594, 1411, 1238 };
				int insertAt = 0;
				for(int id : starters)
					for(int i = insertAt; i < numFiltered; i++)
						if(filtered[i] == id){
							memmove(filtered + insertAt + 1, filtered + insertAt, (i - insertAt) * sizeof(int));
							filtered[insertAt++] = id;
							break;
						}
			}
			if(showRecent && numFiltered == 0 && !ObjectFiltersActive())
				uiBrowserStartSuggestions();
			else{
				ImGui::TextDisabled(gBrowserKind == BROWSER_KIND_SKINS ? _("%d skins") :
					gBrowserKind == BROWSER_KIND_VEHICLES ? _("%d vehicles") : _("%d objects"), numFiltered);
				uiObjectResults(filtered, numFiltered, selId);
			}

		}

		// === Favourites tab ===
		if(gBrowserActiveTab == BROWSER_TAB_FAVOURITES){
			gBrowserActiveTab = BROWSER_TAB_FAVOURITES;
			uiBrowserSelectedObject(selId);

			numFiltered = 0;
			for(int i = 0; i < NUMOBJECTDEFS; i++){
				if(!IsFavourite(i)) continue;
				ObjectDef *obj = GetObjectDef(i);
				if(obj == nil) continue;
				if(!uiBrowserMatches(gBrowserFavFilter, obj)) continue;
				filtered[numFiltered++] = i;
			}
			if(numFiltered == 0 && !gBrowserFavFilter.IsActive())
				ImGui::TextDisabled(_("No favourites yet. Pick an object and click %s to keep it here."), ICON_FA_STAR);
			else
				ImGui::TextDisabled(_("%d favourites"), numFiltered);
			uiObjectResults(filtered, numFiltered, selId);

		}

		// === Feature-specific map elements ===
		if(gBrowserActiveTab == BROWSER_TAB_ELEMENTS){
			gBrowserActiveTab = BROWSER_TAB_ELEMENTS;
			uiBrowserElementsTab();
		}

		// === Prefabs tab ===
		if(gBrowserActiveTab == BROWSER_TAB_PREFABS){
			gBrowserActiveTab = BROWSER_TAB_PREFABS;
			uiBrowserPrefabsTab();
		}

		// === IDE tab ===
		if(gBrowserActiveTab == BROWSER_TAB_IDE){
			gBrowserActiveTab = BROWSER_TAB_IDE;
			uiBrowserSelectedObject(selId);
			// Collect unique IDE file names
			static const char *ideFiles[512];
			static int numIdeFiles = 0;
			if(gBrowserIdeListDirty){
				numIdeFiles = 0;
				for(int i = 0; i < NUMOBJECTDEFS; i++){
					ObjectDef *obj = GetObjectDef(i);
					if(obj == nil || obj->m_file == nil) continue;
					bool found = false;
					for(int j = 0; j < numIdeFiles; j++)
						if(strcmp(ideFiles[j], obj->m_file->name) == 0){
							found = true; break;
						}
					if(!found && numIdeFiles < 512)
						ideFiles[numIdeFiles++] = obj->m_file->name;
				}
				gBrowserIdeListDirty = false;
			}

			// IDE dropdown
			const char *ideLabel = gBrowserSelectedIde[0] ? gBrowserSelectedIde : _("All IDE files");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if(ImGui::BeginCombo("##IdeCombo", ideLabel)){
				if(ImGui::Selectable(_("All IDE files"), gBrowserSelectedIde[0] == '\0'))
					gBrowserSelectedIde[0] = '\0';
				for(int j = 0; j < numIdeFiles; j++){
					bool isSel = strcmp(gBrowserSelectedIde, ideFiles[j]) == 0;
					if(ImGui::Selectable(ideFiles[j], isSel))
						snprintf(gBrowserSelectedIde, sizeof(gBrowserSelectedIde), "%s", ideFiles[j]);
				}
				ImGui::EndCombo();
			}

			numFiltered = 0;
			for(int i = 0; i < NUMOBJECTDEFS; i++){
				ObjectDef *obj = GetObjectDef(i);
				if(obj == nil) continue;
				if(gBrowserSelectedIde[0] != '\0' &&
				   (obj->m_file == nil || strcmp(obj->m_file->name, gBrowserSelectedIde) != 0))
					continue;
				if(!uiBrowserMatches(gBrowserIdeFilter, obj)) continue;
				filtered[numFiltered++] = i;
			}
			ImGui::TextDisabled(_("%d objects"), numFiltered);
			uiObjectResults(filtered, numFiltered, selId);

		}

		gBrowserTabRestorePending = false;
	}

	if(resultsOpen || sideOpen)
		ImGui::EndChild();
	ImGui::End();
}

// Marks an object in the 3D view, e.g. while its list row is hovered.
// ---- SA-MP element overlays ----

static ImU32
uiPawnColorToImU32(uint32 color)
{
	return IM_COL32((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

static ImDrawList*
uiWorldOverlayDrawList(void)
{
#ifdef IMGUI_HAS_VIEWPORT
	return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
#else
	return ImGui::GetBackgroundDrawList();
#endif
}

// SA-MP draws 3D labels centred on their position with a black outline, and
// {RRGGBB} switches the colour (alpha stays the label's).
static void
uiDrawPawnLabelText(ImDrawList *drawList, ImVec2 center, const char *text, uint32 color)
{
	struct Segment { std::string text; ImU32 color; };
	std::vector<std::vector<Segment>> lines(1);
	ImU32 current = uiPawnColorToImU32(color);
	std::string run;
	for(const char *p = text; ; p++){
		bool embed = p[0] == '{' && strspn(p + 1, "0123456789abcdefABCDEF") >= 6 && p[7] == '}';
		if(*p == '\0' || *p == '\n' || embed){
			if(!run.empty())
				lines.back().push_back({ run, current });
			run.clear();
			if(*p == '\0')
				break;
			if(*p == '\n')
				lines.push_back(std::vector<Segment>());
			else{
				uint32 rgb = (uint32)strtoul(std::string(p + 1, 6).c_str(), nil, 16);
				current = uiPawnColorToImU32((rgb << 8) | (color & 0xFF));
				p += 7;
			}
			continue;
		}
		run.push_back(*p);
	}
	float lineHeight = ImGui::GetTextLineHeight();
	float y = center.y - lineHeight * lines.size() * 0.5f;
	ImU32 outline = IM_COL32(0, 0, 0, (color & 0xFF));
	for(size_t l = 0; l < lines.size(); l++, y += lineHeight){
		float width = 0.0f;
		for(size_t i = 0; i < lines[l].size(); i++)
			width += ImGui::CalcTextSize(lines[l][i].text.c_str()).x;
		float x = center.x - width * 0.5f;
		for(size_t i = 0; i < lines[l].size(); i++){
			const char *segment = lines[l][i].text.c_str();
			static const ImVec2 offsets[] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
			for(int o = 0; o < 4; o++)
				drawList->AddText(ImVec2(x + offsets[o].x, y + offsets[o].y), outline, segment);
			drawList->AddText(ImVec2(x, y), lines[l][i].color, segment);
			x += ImGui::CalcTextSize(segment).x;
		}
	}
}

static void
uiDrawWorldTag(ImDrawList *drawList, ImVec2 at, const char *text, ImU32 color)
{
	ImVec2 size = ImGui::CalcTextSize(text);
	ImVec2 pad = ImVec2(4.0f, 2.0f);
	ImVec2 p0 = ImVec2(at.x - size.x * 0.5f - pad.x, at.y - size.y - pad.y * 2.0f);
	ImVec2 p1 = ImVec2(at.x + size.x * 0.5f + pad.x, at.y);
	drawList->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), 3.0f);
	drawList->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), color, text);
}

static bool
uiWorldToScreen(rw::V3d pos, ImVec2 *out)
{
	rw::V3d screen;
	float sw, sh;
	if(!Sprite::CalcScreenCoors(pos, &screen, &sw, &sh, false))
		return false;
	*out = EditorUiPoint(screen.x, screen.y);
	return true;
}

// Labels, tags and gang zone outlines for the SA-MP elements in view.
static void
uiDrawSampElementOverlays(void)
{
	if(!gRenderSampElements)
		return;
	ImDrawList *drawList = uiWorldOverlayDrawList();
	rw::V3d cam = TheCamera.m_position;
	int classIndex = 0;
	// Classes are numbered in creation order, as AddPlayerClass returns them.
	std::vector<ObjectInst*> elements;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
			elements.push_back(inst);
	}
	std::sort(elements.begin(), elements.end(), [](const ObjectInst *a, const ObjectInst *b){
		return a->m_id < b->m_id;
	});
	// Radar art makes hundreds of zones: then only selected ones get a tag.
	int numZones = 0;
	for(size_t i = 0; i < elements.size(); i++)
		if(GetObjectPawnProperties(elements[i])->elementKind == SAMP_ELEMENT_GANGZONE)
			numZones++;
	for(size_t i = 0; i < elements.size(); i++){
		ObjectInst *inst = elements[i];
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		int kind = properties->elementKind;
		int thisClass = kind == SAMP_ELEMENT_CLASS ? classIndex++ : -1;
		if(inst->m_editorHidden || PlayerViewHides(inst))
			continue;
		float dist = length(sub(inst->m_translation, cam));
		if(dist >= gStreamDistance) continue;
		bool selected = inst->m_selected != 0;
		ImU32 tagColor = selected ? IM_COL32(255, 220, 90, 255) : IM_COL32(220, 220, 220, 230);
		char tag[160];
		ImVec2 screen;
		// Text and tags go just above an editor icon, not over it.
		auto toScreen = [&](rw::V3d p, ImVec2 *out){
			if(!uiWorldToScreen(p, out))
				return false;
			if(ElementKindUsesIcon(kind))
				out->y -= 20.0f*GetUiScale();
			return true;
		};
		switch(kind){
		case SAMP_ELEMENT_LABEL: {
			float drawDistance = gStreamDistance;
			// As in game, unless it is selected.
			if((dist > drawDistance && !selected) || !toScreen(inst->m_translation, &screen))
				break;
			uiDrawPawnLabelText(drawList, screen, properties->labelText, properties->labelColor);
			break;
		}
		case SAMP_ELEMENT_GANGZONE: {
			float minX, minY, maxX, maxY;
			minX = inst->m_translation.x - properties->zoneSizeX * 0.5f;
			minY = inst->m_translation.y - properties->zoneSizeY * 0.5f;
			maxX = inst->m_translation.x + properties->zoneSizeX * 0.5f;
			maxY = inst->m_translation.y + properties->zoneSizeY * 0.5f;
			float z = inst->m_translation.z + 0.3f;
			uint32 c = properties->zoneColor;
			rw::RGBA line = { (uint8)(c >> 24), (uint8)(c >> 16), (uint8)(c >> 8), 255 };
			if(selected)
				line = { 255, 220, 90, 255 };
			RenderLine({ minX, minY, z }, { maxX, minY, z }, line, line);
			RenderLine({ maxX, minY, z }, { maxX, maxY, z }, line, line);
			RenderLine({ maxX, maxY, z }, { minX, maxY, z }, line, line);
			RenderLine({ minX, maxY, z }, { minX, minY, z }, line, line);
			// Hatching in the zone colour so the area reads at a glance.
			rw::RGBA hatch = { line.red, line.green, line.blue, (uint8)max(60, (int)(c & 0xFF)) };
			float span = (maxX - minX) + (maxY - minY);
			float step = max(4.0f, span / 24.0f);
			for(float d = step; d < span; d += step){
				rw::V3d a = { minX + min(d, maxX - minX), minY + max(0.0f, d - (maxX - minX)), z };
				rw::V3d b = { minX + max(0.0f, d - (maxY - minY)), minY + min(d, maxY - minY), z };
				RenderLine(a, b, hatch, hatch);
			}
			if((selected || numZones <= 64) && dist < gStreamDistance && toScreen(inst->m_translation, &screen)){
				snprintf(tag, sizeof(tag), _("Gang zone %.0f x %.0f"), properties->zoneSizeX, properties->zoneSizeY);
				uiDrawWorldTag(drawList, screen, tag, tagColor);
			}
			break;
		}
		case SAMP_ELEMENT_AREA: {
			DrawDynamicAreaOverlay(inst, selected);
			if((selected || dist < gStreamDistance) && uiWorldToScreen(inst->m_translation, &screen)){
				static const char *shapes[] = { N_("circle"), N_("cylinder"), N_("sphere"), N_("rectangle"), N_("cuboid"), N_("cube"), N_("polygon") };
				snprintf(tag, sizeof(tag), _("Area %s (%s, %d item(s))"), properties->name,
					_(shapes[clamp(properties->areaShape, 0, 6)]), CountItemsInArea(properties->name));
				uiDrawWorldTag(drawList, screen, tag, tagColor);
			}
			break;
		}
		case SAMP_ELEMENT_CHECKPOINT: {
			// The marker itself is a mesh (checkpoints.cpp); this is its tag.
			if(dist > gStreamDistance && !selected)
				break;
			bool race = properties->cpKind == SAMP_CHECKPOINT_RACE;
			const rw::V3d &c = inst->m_translation;
			float h = max(1.0f, properties->cpSize * 0.75f);
			rw::V3d above = c;
			above.z += race ? 3.5f : h + 0.5f;
			if((dist > gStreamDistance && !selected) || !(ElementKindUsesIcon(kind) ? toScreen(inst->m_translation, &screen) : uiWorldToScreen(above, &screen)))
				break;
			if(race){
				std::vector<ObjectInst*> points;
				GetRaceCheckpoints(properties->cpRace, points);
				int position = 0;
				for(size_t j = 0; j < points.size(); j++)
					if(points[j] == inst)
						position = (int)j + 1;
				// The type without its leading number ("1  Finish").
				const char *type = _(uiRaceCheckpointTypes[GetRaceCheckpointShownType(inst)]);
				type += strspn(type, "0123456789 ");
				snprintf(tag, sizeof(tag), "%s %d/%d  %s", properties->cpRace, position, (int)points.size(), type);
			}else
				snprintf(tag, sizeof(tag), _("Checkpoint  size %.1f"), properties->cpSize);
			uiDrawWorldTag(drawList, screen, tag, tagColor);
			break;
		}
		case SAMP_ELEMENT_SOUND: {
			// The distance it is heard at; a stream without a position has none.
			bool stream = properties->soundKind == SAMP_SOUND_STREAM;
			if(dist > gStreamDistance && !selected)
				break;
			if((!stream || properties->soundUsePos) && properties->soundRadius > 0.0f){
				rw::RGBA ring = selected ? rw::RGBA{ 255, 220, 90, 255 } : rw::RGBA{ 90, 200, 255, 220 };
				const rw::V3d &c = inst->m_translation;
				float r = properties->soundRadius;
				const int segments = 64;
				for(int k = 0; k < segments; k++){
					float a0 = (float)k / segments * 2.0f * (float)M_PI;
					float a1 = (float)(k + 1) / segments * 2.0f * (float)M_PI;
					RenderLine({ c.x + r * cosf(a0), c.y + r * sinf(a0), c.z + 0.3f },
						{ c.x + r * cosf(a1), c.y + r * sinf(a1), c.z + 0.3f }, ring, ring);
				}
			}
			rw::V3d above = inst->m_translation;
			above.z += 0.8f;
			if((dist > gStreamDistance && !selected) || !(ElementKindUsesIcon(kind) ? toScreen(inst->m_translation, &screen) : uiWorldToScreen(above, &screen)))
				break;
			if(stream)
				snprintf(tag, sizeof(tag), _("Audio stream  %s"), properties->soundUrl[0] ? properties->soundUrl : _("(no URL)"));
			else{
				const char *soundName = uiSoundIdName(properties->soundId);
				snprintf(tag, sizeof(tag), _("Sound %d  %s"), properties->soundId, soundName ? soundName : "");
			}
			uiDrawWorldTag(drawList, screen, tag, tagColor);
			break;
		}
		default: {
			if(dist > gStreamDistance && !selected)
				break;
			rw::V3d above = inst->m_translation;
			above.z += kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_CLASS ||
				kind == SAMP_ELEMENT_NPC ? 1.2f : 0.8f;
			if(!(ElementKindUsesIcon(kind) ? toScreen(inst->m_translation, &screen) : uiWorldToScreen(above, &screen)))
				break;
			switch(kind){
			case SAMP_ELEMENT_PICKUP:
				snprintf(tag, sizeof(tag), _("Pickup  type %d"), properties->pickupType);
				break;
			case SAMP_ELEMENT_ACTOR:
				snprintf(tag, sizeof(tag), _("Actor  skin %d"), properties->elementModel);
				break;
			case SAMP_ELEMENT_PATHPOINT: {
				std::vector<ObjectInst*> points;
				GetNpcPathPoints(properties->pathName, points);
				int position = 0;
				for(size_t j = 0; j < points.size(); j++)
					if(points[j] == inst)
						position = (int)j + 1;
				snprintf(tag, sizeof(tag), "%s %d", properties->pathName, position);
				break;
			}
			case SAMP_ELEMENT_NPC:
				// Drawn like the name tag over a player's head.
				snprintf(tag, sizeof(tag), _("NPC %s  skin %d"), properties->npcName,
					properties->elementModel);
				break;
			case SAMP_ELEMENT_CLASS:
				if(properties->classTeam >= 0)
					snprintf(tag, sizeof(tag), _("Class %d  skin %d  team %d"), thisClass,
						properties->elementModel, properties->classTeam);
				else
					snprintf(tag, sizeof(tag), _("Class %d  skin %d"), thisClass, properties->elementModel);
				break;
			case SAMP_ELEMENT_MAPICON:
				snprintf(tag, sizeof(tag), _("Map icon %d  %s"), properties->mapIconType,
					properties->mapIconType >= 0 && properties->mapIconType < 64 ?
					_(uiMapIconNames[properties->mapIconType]) : "");
				break;
			default:
				tag[0] = '\0';
				break;
			}
			if(properties->name[0] && strlen(tag) + strlen(properties->name) + 4 < sizeof(tag)){
				strcat(tag, "  ");
				strcat(tag, properties->name);
			}
			uiDrawWorldTag(drawList, screen, tag, tagColor);
			break;
		}
		}
	}
}

// NPC paths as lines from point to point, and a line from each NPC that
// follows a path to the point it starts at.
static void
uiDrawNpcPaths(void)
{
	if(!gRenderSampElements)
		return;
	std::vector<std::string> names;
	GetNpcPathNames(names);
	for(size_t i = 0; i < names.size(); i++){
		std::vector<ObjectInst*> points;
		GetNpcPathPoints(names[i].c_str(), points);
		bool selected = false;
		for(size_t j = 0; j < points.size(); j++)
			selected |= points[j]->m_selected != 0;
		rw::RGBA color = selected ? rw::RGBA{ 255, 220, 90, 255 } : rw::RGBA{ 90, 200, 255, 255 };
		for(size_t j = 0; j + 1 < points.size(); j++){
			rw::V3d a = points[j]->m_translation, b = points[j+1]->m_translation;
			a.z += 0.2f;
			b.z += 0.2f;
			RenderLine(a, b, color, color);
			// An arrowhead halfway, pointing the way NPCs walk.
			rw::V3d d = sub(b, a);
			float len = length(d);
			if(len < 0.5f)
				continue;
			d = scale(d, 1.0f / len);
			rw::V3d mid = add(a, scale(d, len * 0.5f));
			rw::V3d side = { -d.y * 0.4f, d.x * 0.4f, 0.0f };
			rw::V3d back = sub(mid, scale(d, 0.8f));
			RenderLine(mid, add(back, side), color, color);
			RenderLine(mid, sub(back, side), color, color);
		}
	}
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(inst->m_isDeleted || properties->elementKind != SAMP_ELEMENT_NPC || !properties->pathName[0])
			continue;
		std::vector<ObjectInst*> points;
		GetNpcPathPoints(properties->pathName, points);
		if(points.empty())
			continue;
		ObjectInst *start = properties->npcPathReversed ? points.back() : points.front();
		rw::RGBA color = { 90, 200, 255, 140 };
		RenderLine(inst->m_translation, start->m_translation, color, color);
	}
}

static void
uiMarkObjectInWorld(ObjectInst *inst)
{
	rw::V3d screen;
	float sw, sh;
	if(inst == nil || !Sprite::CalcScreenCoors(inst->m_translation, &screen, &sw, &sh, false))
		return;
	// Behind the panels, so a marker under a window does not cover its text.
#ifdef IMGUI_HAS_VIEWPORT
	ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
#else
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
#endif
	ImVec2 center = EditorUiPoint(screen.x, screen.y);
	ImU32 color = IM_COL32(255, 210, 60, 255);
	float radius = ImGui::GetFontSize() * 1.2f;
	drawList->AddCircle(center, radius, color, 32, 2.5f);
	drawList->AddLine(ImVec2(center.x - radius * 1.6f, center.y), ImVec2(center.x - radius * 0.6f, center.y), color, 2.0f);
	drawList->AddLine(ImVec2(center.x + radius * 0.6f, center.y), ImVec2(center.x + radius * 1.6f, center.y), color, 2.0f);
	drawList->AddLine(ImVec2(center.x, center.y - radius * 1.6f), ImVec2(center.x, center.y - radius * 0.6f), color, 2.0f);
	drawList->AddLine(ImVec2(center.x, center.y + radius * 0.6f), ImVec2(center.x, center.y + radius * 1.6f), color, 2.0f);
}

// Everything that ends up in the Pawn map, organised by group: click selects,
// the crosshair button flies the camera there, hovering marks it in the world.
// ---- Map Objects: an outliner ----
// Rows are rebuilt only when the map, the search, the filters or what is
// open change, and drawn through a clipper, so 20000 objects stay smooth.

struct UiOutlinerRow
{
	enum { HEADER, OBJECT, NOTE, TEXTDRAW } type;
	int depth;
	ObjectInst *inst;	// OBJECT; NOTE: the NPC it describes
	std::string key;	// HEADER: what keeps it open or closed
	std::string label;
	int count;	// HEADER
	bool dropTarget;	// HEADER of a group in the group view
	std::string group;	// ... and its group
	std::vector<ObjectInst*> members;	// HEADER: everything under it, open or not
	std::vector<ObjectInst*> refs;	// NOTE: what clicking it selects
	int textDraw;	// TEXTDRAW
	bool mapTextDraw;	// an old map's read-only one: textDraw indexes GetMapTextDraws()
	const char *problem;	// OBJECT: the first Map check problem, or nil
};

enum { UI_OUTLINER_BY_GROUP, UI_OUTLINER_BY_KIND };
// Kind filter: -1 all; SAMP element kinds; then these.
enum { UI_OUTLINER_VEHICLES = NUM_SAMP_ELEMENT_KINDS, UI_OUTLINER_TEXTDRAWS };

static struct {
	int mode = UI_OUTLINER_BY_GROUP;
	char search[64];
	int kind = -1;
	bool hiddenOnly, problemsOnly;
	std::set<std::string> closed;
	int closedVersion;
	std::vector<UiOutlinerRow> rows;
	int removedWorld, numObjects;
	// What the rows were built from.
	uint32 builtSeq = 0xFFFFFFFF;
	double builtTime;
	std::string builtKey;
	int anchor = -1;	// row of the last plain click, for Shift ranges
	int32 renaming = -1;	// instance whose variable is being typed
	char renameBuffer[PAWN_OBJECT_NAME_LEN];
	bool renameFocus;
	uint64 selectionSeen;	// to scroll to a selection made in the viewport
	bool selectionFromHere;
	bool openGroupPopup;
	bool hovered;	// the mouse is over the list, for the rename shortcut
	bool scrollToRenaming;
} gOutliner;

static void uiRequestGroupSelection(void) { showMapObjectsWindow = true; gOutliner.openGroupPopup = true; }

static void
uiOutlinerStartRename(ObjectInst *inst)
{
	showMapObjectsWindow = true;	// from the 3D view's menu it may be closed
	gOutliner.renaming = inst->m_id;
	snprintf(gOutliner.renameBuffer, sizeof(gOutliner.renameBuffer), "%s", GetObjectPawnProperties(inst)->name);
	gOutliner.renameFocus = true;
	gOutliner.scrollToRenaming = true;
}

static uint64
uiSelectionSignature(void)
{
	uint64 h = 1469598103934665603ull;
	for(CPtrNode *p = selection.first; p; p = p->next)
		h = (h ^ (uint64)((ObjectInst*)p->item)->m_id) * 1099511628211ull;
	return h;
}

static int
uiOutlinerKindOf(const ObjectInst *inst)
{
	return inst->m_isVehicle ? UI_OUTLINER_VEHICLES : GetSampElementKind(inst);
}

static bool
uiOutlinerMatches(ObjectInst *inst, const std::vector<std::pair<int,int>> &idRanges, bool idQuery,
	const std::unordered_map<int32, const char*> &problems)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	int kind = uiOutlinerKindOf(inst);
	if(gOutliner.kind >= 0 && gOutliner.kind != kind)
		return false;
	if(gOutliner.hiddenOnly && !IsInstanceHidden(inst))
		return false;
	if(gOutliner.problemsOnly && !problems.count(inst->m_id))
		return false;
	const char *search = gOutliner.search;
	if(!search[0])
		return true;
	if(idQuery){
		for(size_t i = 0; i < idRanges.size(); i++)
			if(inst->m_objectId >= idRanges[i].first && inst->m_objectId <= idRanges[i].second)
				return true;
		return false;
	}
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	return ImStristr(properties->name, nil, search, nil) || ImStristr(properties->labelText, nil, search, nil) ||
		ImStristr(inst->m_isVehicle ? "Vehicle" : GetSampElementKindName(properties->elementKind), nil, search, nil) ||
		ImStristr(inst->m_isVehicle ? _("Vehicle") : _(GetSampElementKindName(properties->elementKind)), nil, search, nil) ||
		ImStristr(properties->group, nil, search, nil) || ImStristr(properties->comment, nil, search, nil) ||
		ImStristr(properties->npcName, nil, search, nil) || ImStristr(properties->pathName, nil, search, nil) ||
		(obj && ImStristr(obj->m_name, nil, search, nil));
}

// A header and, when it is open, its rows.
static void
uiOutlinerAddHeader(const std::string &key, const std::string &label, int depth,
	const std::vector<ObjectInst*> &members, bool dropTarget = false, const std::string &group = "")
{
	UiOutlinerRow row = {};
	row.type = UiOutlinerRow::HEADER;
	row.depth = depth;
	row.key = key;
	row.label = label;
	row.count = (int)members.size();
	row.members = members;
	row.dropTarget = dropTarget;
	row.group = group;
	row.textDraw = -1;
	gOutliner.rows.push_back(row);
}

static bool uiOutlinerOpen(const std::string &key) { return !gOutliner.closed.count(key); }

// notes: the lines under it (an area's items, an NPC's path...); left out
// where the rows around it say the same.
static void
uiOutlinerAddObject(ObjectInst *inst, int depth, const std::unordered_map<int32, const char*> &problems,
	bool notes = true)
{
	UiOutlinerRow row = {};
	row.type = UiOutlinerRow::OBJECT;
	row.depth = depth;
	row.inst = inst;
	row.textDraw = -1;
	char label[160];
	const ObjectPawnProperties *own = GetObjectPawnProperties(inst);
	if(!inst->m_isVehicle && own->elementKind == SAMP_ELEMENT_NPC && !own->name[0])
		snprintf(label, sizeof(label), _("NPC %s"), own->npcName);	// its player name, not an ID
	else
		uiObjectLabel(inst, label, sizeof(label));
	row.label = label;
	if(GetAttachParent(inst))
		row.label += "  " ICON_FA_LINK;
	std::unordered_map<int32, const char*>::const_iterator problem = problems.find(inst->m_id);
	row.problem = problem != problems.end() ? problem->second : nil;
	gOutliner.rows.push_back(row);
	if(!notes)
		return;
	const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
	auto note = [&](const std::string &text, const std::vector<ObjectInst*> &refs){
		UiOutlinerRow n = {};
		n.type = UiOutlinerRow::NOTE;
		n.depth = depth + 1;
		n.inst = inst;
		n.label = text;
		n.refs = refs;
		n.textDraw = -1;
		gOutliner.rows.push_back(n);
	};
	// Dynamic areas: an area lists what is shown only inside it, an item its area.
	if(!inst->m_isVehicle && p->elementKind == SAMP_ELEMENT_AREA && p->name[0]){
		std::vector<ObjectInst*> items;
		for(CPtrNode *q = instances.first; q; q = q->next){
			ObjectInst *item = (ObjectInst*)q->item;
			if(!item->m_isDeleted && !item->m_isVehicle && strcmp(GetObjectPawnProperties(item)->areaName, p->name) == 0)
				items.push_back(item);
		}
		if(!items.empty()){
			char text[64];
			snprintf(text, sizeof(text), _("%d item(s) only inside it"), (int)items.size());
			note(text, items);
		}
	}else if(!inst->m_isVehicle && p->areaName[0]){
		ObjectInst *area = FindAreaByName(p->areaName);
		char text[96];
		snprintf(text, sizeof(text), area ? _("only inside %s") : _("only inside %s (missing)"), p->areaName);
		note(text, area ? std::vector<ObjectInst*>{ area } : std::vector<ObjectInst*>{});
	}
	// An NPC's vehicle, path, nodes or recording, one line each.
	if(inst->m_isVehicle || p->elementKind != SAMP_ELEMENT_NPC)
		return;
	if(ObjectInst *vehicle = GetNpcVehicle(inst)){
		char name[160];
		uiObjectLabel(vehicle, name, sizeof(name));
		static const char *seats[] = { N_("driver"), N_("front passenger"), N_("back left"), N_("back right") };
		char text[224];
		snprintf(text, sizeof(text), _("in %s (%s)"), name, _(seats[clamp(p->npcSeat, 0, 3)]));
		note(text, { vehicle });
	}
	if(p->npcRecording[0]){
		char text[96];
		snprintf(text, sizeof(text), _("plays %s.rec"), p->npcRecording);
		note(text, {});
	}else if(p->npcPlaysNode){
		char text[64];
		snprintf(text, sizeof(text), _("walks NODES%d.DAT from point %d"), p->npcNode, p->npcNodePoint);
		note(text, {});
	}else if(p->pathName[0]){
		std::vector<ObjectInst*> points;
		GetNpcPathPoints(p->pathName, points);
		char text[96];
		snprintf(text, sizeof(text), _("follows %s (%d points)"), p->pathName, (int)points.size());
		note(text, points);
	}
}

static void
uiOutlinerRebuild(void)
{
	gOutliner.rows.clear();
	gOutliner.removedWorld = 0;
	std::vector<std::pair<int,int>> idRanges;
	bool idQuery = gOutliner.search[0] && ParseModelIdQuery(gOutliner.search, &idRanges);
	// Map check problems (errors and warnings) by instance, from the last check.
	std::unordered_map<int32, const char*> problems;
	if(gMapCheckDone)
		for(size_t i = 0; i < gMapCheck.items.size(); i++)
			if(gMapCheck.items[i].severity != MAPCHECK_INFO)
				for(int id : gMapCheck.items[i].instances)
					problems.emplace(id, gMapCheck.items[i].text.c_str());

	std::vector<ObjectInst*> objects;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!PawnObjectNeedsExport(inst))
			continue;
		if(inst->m_isDeleted){
			gOutliner.removedWorld++;
			continue;
		}
		if(uiOutlinerMatches(inst, idRanges, idQuery, problems))
			objects.push_back(inst);
	}
	// Oldest first, matching the saved file.
	std::sort(objects.begin(), objects.end(), [](const ObjectInst *a, const ObjectInst *b){
		return a->m_id < b->m_id;
	});
	gOutliner.numObjects = (int)objects.size();

	if(gOutliner.mode == UI_OUTLINER_BY_GROUP){
		std::map<std::string, std::vector<ObjectInst*>> groups;	// sorted; "" (ungrouped) first
		for(ObjectInst *inst : objects)
			groups[GetObjectPawnProperties(inst)->group].push_back(inst);
		for(auto &g : groups){
			std::string key = "g:" + g.first;
			uiOutlinerAddHeader(key, g.first.empty() ? std::string(_("Ungrouped")) : g.first, 0, g.second, true, g.first);
			if(uiOutlinerOpen(key))
				for(ObjectInst *inst : g.second)
					uiOutlinerAddObject(inst, 1, problems);
		}
	}else{
		// Kinds, with paths and races as their own headers in point order.
		struct Category { int kind; const char *name; };
		static const Category categories[] = {
			{ SAMP_ELEMENT_OBJECT, N_("Objects") }, { UI_OUTLINER_VEHICLES, N_("Vehicles") },
			{ SAMP_ELEMENT_ACTOR, N_("Actors") }, { SAMP_ELEMENT_CLASS, N_("Player classes") },
			{ SAMP_ELEMENT_NPC, N_("NPCs") }, { SAMP_ELEMENT_PATHPOINT, N_("NPC paths") },
			{ SAMP_ELEMENT_CHECKPOINT, N_("Checkpoints and races") }, { SAMP_ELEMENT_PICKUP, N_("Pickups") },
			{ SAMP_ELEMENT_LABEL, N_("3D text labels") }, { SAMP_ELEMENT_GANGZONE, N_("Gang zones") },
			{ SAMP_ELEMENT_MAPICON, N_("Map icons") }, { SAMP_ELEMENT_SOUND, N_("Sounds") },
			{ SAMP_ELEMENT_AREA, N_("Dynamic areas") },
		};
		std::unordered_set<ObjectInst*> shown(objects.begin(), objects.end());
		for(const Category &c : categories){
			std::vector<ObjectInst*> members;
			for(ObjectInst *inst : objects)
				if(uiOutlinerKindOf(inst) == c.kind)
					members.push_back(inst);
			if(members.empty())
				continue;
			std::string key = std::string("k:") + c.name;
			uiOutlinerAddHeader(key, _(c.name), 0, members);
			if(!uiOutlinerOpen(key))
				continue;
			if(c.kind == SAMP_ELEMENT_PATHPOINT || c.kind == SAMP_ELEMENT_CHECKPOINT){
				// Plain checkpoints first, then each path or race by name.
				std::map<std::string, bool> names;
				for(ObjectInst *inst : members){
					const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
					if(c.kind == SAMP_ELEMENT_PATHPOINT)
						names[p->pathName] = true;
					else if(p->cpKind == SAMP_CHECKPOINT_RACE)
						names[p->cpRace] = true;
					else
						uiOutlinerAddObject(inst, 1, problems);
				}
				for(auto &n : names){
					std::vector<ObjectInst*> ordered, inOrder;
					if(c.kind == SAMP_ELEMENT_PATHPOINT)
						GetNpcPathPoints(n.first.c_str(), ordered);
					else
						GetRaceCheckpoints(n.first.c_str(), ordered);
					for(ObjectInst *inst : ordered)
						if(shown.count(inst))
							inOrder.push_back(inst);
					std::string sub = key + "/" + n.first;
					char subLabel[96];
					snprintf(subLabel, sizeof(subLabel), c.kind == SAMP_ELEMENT_PATHPOINT ? _("Path %s") : _("Race %s"),
						n.first.empty() ? _("(no name)") : n.first.c_str());
					uiOutlinerAddHeader(sub, subLabel, 1, inOrder);
					if(uiOutlinerOpen(sub))
						for(ObjectInst *inst : inOrder)
							uiOutlinerAddObject(inst, 2, problems);
				}
				continue;
			}
			if(c.kind == SAMP_ELEMENT_AREA){
				// Each area with the items shown only inside it.
				for(ObjectInst *area : members){
					const char *name = GetObjectPawnProperties(area)->name;
					std::vector<ObjectInst*> group = { area };
					for(ObjectInst *inst : objects)
						if(inst != area && !inst->m_isVehicle && name[0] &&
						   strcmp(GetObjectPawnProperties(inst)->areaName, name) == 0)
							group.push_back(inst);
					std::string sub = key + "/" + std::to_string(area->m_id);
					char label[160];
					uiObjectLabel(area, label, sizeof(label));
					uiOutlinerAddHeader(sub, label, 1, group);
					if(uiOutlinerOpen(sub))
						for(ObjectInst *inst : group)
							uiOutlinerAddObject(inst, 2, problems, false);
				}
				continue;
			}
			for(ObjectInst *inst : members)
				uiOutlinerAddObject(inst, 1, problems);
		}
	}
	// TextDraws are not in the world; they list by name and open TextDraw
	// mode. The file's come first, then an old map's read-only ones.
	bool textDraws = (gOutliner.kind < 0 || gOutliner.kind == UI_OUTLINER_TEXTDRAWS) &&
		!gOutliner.hiddenOnly && !gOutliner.problemsOnly;
	struct TdShown { int index; bool map; };
	std::vector<TdShown> tdShown;
	for(int list = 0; list < 2 && textDraws; list++){
		const std::vector<TextDraw> &tds = list ? GetMapTextDraws() : GetTextDraws();
		for(size_t i = 0; i < tds.size(); i++)
			if(!gOutliner.search[0] || idQuery || ImStristr(tds[i].name, nil, gOutliner.search, nil) ||
			   ImStristr(tds[i].text.c_str(), nil, gOutliner.search, nil))
				tdShown.push_back({ (int)i, list == 1 });
	}
	if(!tdShown.empty()){
		uiOutlinerAddHeader("t:", _("TextDraws"), 0, {});
		gOutliner.rows.back().count = (int)tdShown.size();
		if(uiOutlinerOpen("t:"))
			for(const TdShown &shown : tdShown){
				const TextDraw &td = (shown.map ? GetMapTextDraws() : GetTextDraws())[shown.index];
				UiOutlinerRow row = {};
				row.type = UiOutlinerRow::TEXTDRAW;
				row.depth = 1;
				row.textDraw = shown.index;
				row.mapTextDraw = shown.map;
				std::string text = td.text.substr(0, 32);
				std::replace(text.begin(), text.end(), '\n', ' ');
				row.label = std::string(ICON_FA_FONT " ") + (td.name[0] ? td.name : _("textdraw")) + "  " + text +
					(shown.map ? _("  (in the map, read-only)") : "");
				gOutliner.rows.push_back(row);
			}
	}
}

// Selects inst from a click on its row: Ctrl toggles, Shift adds the range.
static void
uiOutlinerClick(int rowIndex)
{
	ObjectInst *inst = gOutliner.rows[rowIndex].inst;
	ImGuiIO &io = ImGui::GetIO();
	gOutliner.selectionFromHere = true;
	if(io.KeyShift && gOutliner.anchor >= 0 && gOutliner.anchor < (int)gOutliner.rows.size()){
		if(!io.KeyCtrl)
			ClearSelection();
		int a = min(gOutliner.anchor, rowIndex), b = max(gOutliner.anchor, rowIndex);
		for(int i = a; i <= b; i++)
			if(gOutliner.rows[i].type == UiOutlinerRow::OBJECT)
				gOutliner.rows[i].inst->Select();
		return;
	}
	if(io.KeyCtrl){
		if(inst->m_selected) inst->Deselect(); else inst->Select();
	}else{
		ClearSelection();
		inst->Select();
	}
	gOutliner.anchor = rowIndex;
}

static void
uiOutlinerSetGroup(const std::vector<ObjectInst*> &insts, const char *group)
{
	UndoBeginGroup();
	for(ObjectInst *inst : insts){
		ObjectPawnProperties properties = *GetObjectPawnProperties(inst);
		if(strcmp(properties.group, group) == 0)
			continue;
		snprintf(properties.group, sizeof(properties.group), "%s", group);
		SetObjectPawnProperties(inst, properties);
	}
	UndoEndGroup();
}

static void
uiSelectedOrThis(ObjectInst *inst, std::vector<ObjectInst*> &out)
{
	out.clear();
	if(inst->m_selected)
		uiSelectedInstances(out);
	else
		out.push_back(inst);
}

// Right-click on a row: the selection, or this one when it isn't selected.
static void
uiOutlinerObjectMenu(ObjectInst *inst)
{
	uiConvertWorldObjectAction(inst, true);
	uiReplayParticlesAction(inst, true);
	if(ImGui::MenuItem(_("Find instances"))) uiOpenObjectDiscovery(inst->m_objectId, false);
	if(ImGui::MenuItem(_("Find similar models"))) uiOpenObjectDiscovery(inst->m_objectId, true);
	ImGui::Separator();
	if(!inst->m_isVehicle && GetSampElementKind(inst) == SAMP_ELEMENT_OBJECT && IsPawnObjectEditable(inst)){
		if(!inst->m_selected){
			gOutliner.selectionFromHere = true;
			ClearSelection();
			inst->Select();
		}
		int selectedCount = 0;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *selected = (ObjectInst*)p->item;
			if(selected && !selected->m_isDeleted && !selected->m_isVehicle &&
			   GetSampElementKind(selected) == SAMP_ELEMENT_OBJECT && IsPawnObjectEditable(selected))
				selectedCount++;
		}
		const char *action = selectedCount > 1 ? _("Attach selected to target") : _("Attach to target");
		if(ImGui::MenuItem(action))
			BeginAttachmentTargetPick();
		ImGui::Separator();
	}
	if(!IsInstInIplMapDocument(inst)){
		if(ImGui::MenuItem(_("Focus"))) inst->JumpTo();
		return;
	}

	if(!inst->m_selected){
		gOutliner.selectionFromHere = true;
		ClearSelection();
		inst->Select();
	}
	if(ImGui::MenuItem(TI(ICON_FA_EYE_SLASH, "Hide"), ShortcutLabel(SC_HIDE_SELECTION))) uiRunShortcut(SC_HIDE_SELECTION);
	if(ImGui::MenuItem(_("Isolate"), ShortcutLabel(SC_ISOLATE_SELECTION))) uiRunShortcut(SC_ISOLATE_SELECTION);
	if(ImGui::MenuItem(_("Focus"), ShortcutLabel(SC_FOCUS_SELECTION))) uiRunShortcut(SC_FOCUS_SELECTION);
	ImGui::Separator();
	if(ImGui::MenuItem(_("Rename variable"), ShortcutLabel(SC_RENAME)))
		uiOutlinerStartRename(inst);
	if(ImGui::MenuItem(_("Group selected..."), ShortcutLabel(SC_GROUP_SELECTION))) uiRequestGroupSelection();
	if(ImGui::MenuItem(_("Duplicate"), ShortcutLabel(SC_DUPLICATE))) uiRunShortcut(SC_DUPLICATE);
	if(ImGui::MenuItem(TI(ICON_FA_TRASH, "Delete"), ShortcutLabel(SC_DELETE))) uiRunShortcut(SC_DELETE);
}

// The same menu on a right-click in the 3D view (handleTool opens it).
static int gViewportMenuInstance = -1;
static bool gOpenViewportMenu;

bool
OpenViewportObjectMenu(ObjectInst *inst)
{
	if(inst == nil || inst->m_isDeleted)
		return false;
	if(!inst->m_selected && IsInstInIplMapDocument(inst)){
		ClearSelection();
		inst->Select();
	}
	gViewportMenuInstance = inst->m_id;
	gOpenViewportMenu = true;
	return true;
}

static void
uiViewportObjectMenu(void)
{
	if(gOpenViewportMenu){
		ImGui::OpenPopup("##viewport_object");
		gOpenViewportMenu = false;
	}
	if(!ImGui::BeginPopup("##viewport_object"))
		return;
	ObjectInst *inst = GetInstanceByID(gViewportMenuInstance);
	if(inst && !inst->m_isDeleted)
		uiOutlinerObjectMenu(inst);
	else
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void
uiOutlinerHeaderMenu(const UiOutlinerRow &row, bool *openRename, char *renameFrom, char *renameTo, size_t size)
{
	if(ImGui::MenuItem(_("Select all"))){
		gOutliner.selectionFromHere = true;
		ClearSelection();
		for(ObjectInst *inst : row.members)
			inst->Select();
	}
	if(ImGui::MenuItem(TI(ICON_FA_EYE_SLASH, "Hide all")))
		for(ObjectInst *inst : row.members)
			SetInstanceHidden(inst, true);
	if(ImGui::MenuItem(TI(ICON_FA_EYE, "Show all")))
		for(ObjectInst *inst : row.members)
			SetInstanceHidden(inst, false);
	if(row.dropTarget && !row.group.empty() && ImGui::MenuItem(_("Rename group..."))){
		snprintf(renameFrom, size, "%s", row.group.c_str());
		snprintf(renameTo, size, "%s", row.group.c_str());
		*openRename = true;
	}
}

static void
uiMapObjectsList(void)
{
	// Toolbar: view, search and filters.
	int mode = gOutliner.mode;
	if(ImGui::RadioButton(_("Groups"), mode == UI_OUTLINER_BY_GROUP)) gOutliner.mode = UI_OUTLINER_BY_GROUP;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Kinds"), mode == UI_OUTLINER_BY_KIND)) gOutliner.mode = UI_OUTLINER_BY_KIND;
	// The kind filter goes on its own row when the panel is too narrow for it.
	if(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x -
	   ImGui::GetStyle().ItemSpacing.x > ImGui::GetFontSize() * 8.0f)
		ImGui::SameLine();
	const char *kindName = gOutliner.kind < 0 ? _("All kinds") : gOutliner.kind == UI_OUTLINER_VEHICLES ? _("Vehicles") :
		gOutliner.kind == UI_OUTLINER_TEXTDRAWS ? _("TextDraws") : _(GetSampElementKindName(gOutliner.kind));
	ImGui::SetNextItemWidth(-1.0f);
	if(ImGui::BeginCombo("##kind", kindName)){
		if(ImGui::Selectable(_("All kinds"), gOutliner.kind < 0)) gOutliner.kind = -1;
		for(int k = 0; k <= UI_OUTLINER_TEXTDRAWS; k++){
			const char *name = k == UI_OUTLINER_VEHICLES ? _("Vehicles") : k == UI_OUTLINER_TEXTDRAWS ? _("TextDraws") :
				k == SAMP_ELEMENT_OBJECT ? _("Objects") : _(GetSampElementKindName(k));
			if(ImGui::Selectable(name, gOutliner.kind == k)) gOutliner.kind = k;
		}
		ImGui::EndCombo();
	}
	// Search, then two filter toggles.
	auto toggle = [](const char *label, bool *on, const char *tip){
		// On, it hides rows: drawn in the warning colour.
		if(*on){
			ImVec4 w = UiSemanticColor(UI_COLOR_WARNING);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(w.x, w.y, w.z, 0.30f));
			ImGui::PushStyleColor(ImGuiCol_Text, w);
		}
		if(ImGui::Button(label))
			*on = !*on;
		if(*on)
			ImGui::PopStyleColor(2);
		ImGui::SetItemTooltip("%s", tip);
	};
	float toggles = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f +
		ImGui::GetStyle().FramePadding.x * 4.0f;
	ImGui::SetNextItemWidth(-toggles);
	// The long hint when it fits, else just "Search".
	const char *searchHint = _("Search name, model, ID, kind, variable");
	if(ImGui::CalcTextSize(searchHint).x + ImGui::GetStyle().FramePadding.x*2.0f > ImGui::GetContentRegionAvail().x - toggles)
		searchHint = _("Search");
	bool searching = gOutliner.search[0] != '\0';
	if(searching) ImGui::PushStyleColor(ImGuiCol_Border, UiSemanticColor(UI_COLOR_WARNING));
	ImGui::InputTextWithHint("##map_objects_search", searchHint,
		gOutliner.search, sizeof(gOutliner.search));
	if(searching) ImGui::PopStyleColor();
	ImGui::SetItemTooltip(_("Names, models, kinds, groups and comments; or IDs like 1337, 19000-19100."));
	ImGui::SameLine();
	toggle(ICON_FA_EYE_SLASH "##hiddenonly", &gOutliner.hiddenOnly, _("Only hidden objects."));
	ImGui::SameLine();
	toggle(ICON_FA_TRIANGLE_EXCLAMATION "##problemsonly", &gOutliner.problemsOnly, gMapCheckDone ?
		_("Only objects with a Map check problem.") : _("Only objects with a Map check problem. Runs Map check first."));
	if(gOutliner.problemsOnly && !gMapCheckDone){
		RunMapCheck(gPawnSaveFormat, &gMapCheck, true);
		gMapCheckDone = true;
		gOutliner.builtKey.clear();
	}

	// Rebuild when what the rows show may have changed. Gizmo and brush drags
	// change the sequence every frame, so while a button is held they wait.
	char key[256];
	snprintf(key, sizeof(key), "%d|%s|%d|%d|%d|%d|%d|%d|%u|%d|%zu", gOutliner.mode, gOutliner.search, gOutliner.kind,
		gOutliner.hiddenOnly, gOutliner.problemsOnly, gOutliner.closedVersion, CountHiddenInstances(),
		gMapCheckDone ? (int)gMapCheck.items.size() : -1, TextDrawsChangeSeq(), (int)GetMapTextDraws().size(),
		(size_t)instances.first);
	uint32 seq = GetLatestChangeSeq();
	double now = ImGui::GetTime();
	bool dragging = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
	if(gOutliner.builtKey != key || (seq != gOutliner.builtSeq && (!dragging || now - gOutliner.builtTime > 0.25))){
		uiOutlinerRebuild();
		gOutliner.builtKey = key;
		gOutliner.builtSeq = seq;
		gOutliner.builtTime = now;
		if(gOutliner.anchor >= (int)gOutliner.rows.size())
			gOutliner.anchor = -1;
	}
	if(gOutliner.removedWorld)
		ImGui::TextDisabled(_("%d shown, GTA buildings removed: %d"), gOutliner.numObjects, gOutliner.removedWorld);
	else
		ImGui::TextDisabled(_("%d shown"), gOutliner.numObjects);

	// A selection made in the viewport: open its header and scroll to it.
	uint64 signature = uiSelectionSignature();
	int scrollTo = -1;
	if(signature != gOutliner.selectionSeen){
		gOutliner.selectionSeen = signature;
		if(!gOutliner.selectionFromHere && selection.first){
			ObjectInst *first = (ObjectInst*)selection.first->item;
			for(size_t i = 0; i < gOutliner.rows.size() && scrollTo < 0; i++){
				const UiOutlinerRow &row = gOutliner.rows[i];
				if(row.type == UiOutlinerRow::OBJECT && row.inst == first)
					scrollTo = (int)i;
				else if(row.type == UiOutlinerRow::HEADER && !uiOutlinerOpen(row.key) &&
				        std::find(row.members.begin(), row.members.end(), first) != row.members.end()){
					gOutliner.closed.erase(row.key);
					gOutliner.closedVersion++;
					gOutliner.selectionSeen = 0;	// look again once it is open
				}
			}
		}
		gOutliner.selectionFromHere = false;
	}

	static char renameFrom[PAWN_OBJECT_GROUP_LEN];
	static char renameTo[PAWN_OBJECT_GROUP_LEN];
	bool openRename = false;
	ImGui::BeginChild("##map_objects_tree");
	gOutliner.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	if(gOutliner.scrollToRenaming){
		gOutliner.scrollToRenaming = false;
		for(size_t i = 0; i < gOutliner.rows.size(); i++)
			if(gOutliner.rows[i].type == UiOutlinerRow::OBJECT && gOutliner.rows[i].inst->m_id == gOutliner.renaming)
				scrollTo = (int)i;
	}
	float rowHeight = ImGui::GetTextLineHeightWithSpacing();
	if(scrollTo >= 0){
		float y = scrollTo * rowHeight;
		if(y < ImGui::GetScrollY() || y > ImGui::GetScrollY() + ImGui::GetWindowHeight() - rowHeight * 2.0f)
			ImGui::SetScrollY(max(0.0f, y - ImGui::GetWindowHeight() / 3.0f));
	}
	float indent = ImGui::GetFontSize() * 1.1f;
	float startX = ImGui::GetCursorPosX();
	ImGuiListClipper clipper;
	clipper.Begin((int)gOutliner.rows.size(), rowHeight);
	if(scrollTo >= 0)
		clipper.IncludeItemByIndex(scrollTo);
	while(clipper.Step())
		for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
			UiOutlinerRow &row = gOutliner.rows[i];
			ImGui::PushID(i);
			ImGui::SetCursorPosX(startX + row.depth * indent);
			if(row.type == UiOutlinerRow::HEADER){
				bool open = uiOutlinerOpen(row.key);
				char label[PAWN_OBJECT_GROUP_LEN + 48];
				snprintf(label, sizeof(label), "%s (%d)###%s", row.label.c_str(), row.count, row.key.c_str());
				ImGui::SetNextItemOpen(open);
				bool nowOpen = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen |
					ImGuiTreeNodeFlags_SpanAvailWidth);
				if(nowOpen != open){
					if(nowOpen) gOutliner.closed.erase(row.key); else gOutliner.closed.insert(row.key);
					gOutliner.closedVersion++;
				}
				if(row.dropTarget){
					ImGui::SetItemTooltip(_("Drop objects here to move them into this group."));
					if(ImGui::BeginDragDropTarget()){
						if(const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("HAUMEA_OUTLINER")){
							ObjectInst *dragged = GetInstanceByID(*(const int32*)payload->Data);
							if(dragged){
								std::vector<ObjectInst*> moved;
								uiSelectedOrThis(dragged, moved);
								uiOutlinerSetGroup(moved, row.group.c_str());
							}
						}
						ImGui::EndDragDropTarget();
					}
				}
				if(ImGui::BeginPopupContextItem("##header")){
					uiOutlinerHeaderMenu(row, &openRename, renameFrom, renameTo, sizeof(renameFrom));
					ImGui::EndPopup();
				}
			}else if(row.type == UiOutlinerRow::OBJECT){
				ObjectInst *inst = row.inst;
				// Hide in the viewport (editor-only, never saved).
				bool hidden = IsInstanceHidden(inst);
				if(ImGui::SmallButton(hidden ? ICON_FA_EYE_SLASH "##hide" : ICON_FA_EYE "##hide"))
					SetInstanceHidden(inst, !hidden);
				ImGui::SetItemTooltip("%s", hidden ? _("Hidden in the viewport. Click to show it.") :
					_("Hide in the viewport. Only the editor hides it; the map keeps it."));
				ImGui::SameLine();
				if(row.problem){
					ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), ICON_FA_TRIANGLE_EXCLAMATION);
					ImGui::SetItemTooltip("%s", row.problem);
					ImGui::SameLine();
				}
				if(gOutliner.renaming == inst->m_id){
					if(gOutliner.renameFocus){
						ImGui::SetKeyboardFocusHere();
						gOutliner.renameFocus = false;
					}
					ImGui::SetNextItemWidth(-1.0f);
					bool enter = ImGui::InputTextWithHint("##rename", _("variable (empty: automatic)"),
						gOutliner.renameBuffer, sizeof(gOutliner.renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
					if(enter){
						const char *name = gOutliner.renameBuffer;
						if(name[0] && !IsValidPawnObjectName(name))
							Toast(TOAST_SELECTION, _("\"%s\" isn't a Pawn variable name"), name);
						else if(name[0] && FindObjectByPawnName(name, inst))
							Toast(TOAST_SELECTION, _("Another object is already called %s"), name);
						else{
							ObjectPawnProperties properties = *GetObjectPawnProperties(inst);
							snprintf(properties.name, sizeof(properties.name), "%s", name);
							SetObjectPawnProperties(inst, properties);
						}
						gOutliner.renaming = -1;
					}else if(ImGui::IsItemDeactivated())
						gOutliner.renaming = -1;	// Esc or a click elsewhere
				}else{
					if(hidden)
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					bool clicked = ImGui::Selectable(row.label.c_str(), inst->m_selected,
						ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAvailWidth);
					if(hidden)
						ImGui::PopStyleColor();
					if(clicked){
						uiOutlinerClick(i);
						if(ImGui::IsMouseDoubleClicked(0))
							FocusSelection();
					}
					if(ImGui::IsItemHovered())
						uiMarkObjectInWorld(inst);
					const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
					if(properties->comment[0])
						ImGui::SetItemTooltip("%s", properties->comment);
					if(ImGui::BeginDragDropSource()){
						int32 id = inst->m_id;
						ImGui::SetDragDropPayload("HAUMEA_OUTLINER", &id, sizeof(id));
						int count = inst->m_selected ? uiSelectionCount() : 1;
						if(count > 1)
							ImGui::Text(_("%d objects"), count);
						else
							ImGui::TextUnformatted(row.label.c_str());
						ImGui::EndDragDropSource();
					}
					if(ImGui::BeginPopupContextItem("##object")){
						uiOutlinerObjectMenu(inst);
						ImGui::EndPopup();
					}
				}
			}else if(row.type == UiOutlinerRow::NOTE){
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				std::string label = "\xc2\xb7 " + row.label;	// a middle dot, in every font
				if(ImGui::Selectable(label.c_str(), false) && !row.refs.empty()){
					gOutliner.selectionFromHere = true;
					ClearSelection();
					for(ObjectInst *ref : row.refs)
						ref->Select();
				}
				ImGui::PopStyleColor();
				if(!row.refs.empty())
					ImGui::SetItemTooltip(_("Click to select it."));
			}else if(row.mapTextDraw){
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if(ImGui::Selectable(row.label.c_str(), false))
					EnterTextDrawMode();
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip(_("Saved with the map as it is. Move it to a textdraw file to edit it:\n"
					"TextDraws > Move to a separate file."));
			}else{
				if(ImGui::Selectable(row.label.c_str(), false))
					SelectTextDraw(row.textDraw);
				ImGui::SetItemTooltip(_("Opens it in TextDraw mode."));
			}
			ImGui::PopID();
		}
	clipper.End();
	if(gOutliner.rows.empty())
		ImGui::TextDisabled("%s", gOutliner.search[0] || gOutliner.kind >= 0 || gOutliner.hiddenOnly || gOutliner.problemsOnly ?
			_("Nothing matches.") : _("Nothing placed yet. Pick an object in the Object Browser and click Place."));
	ImGui::EndChild();

	if(openRename)
		ImGui::OpenPopup(L("Rename group"));
	if(ImGui::BeginPopupModal(L("Rename group"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::InputText(_("Name"), renameTo, sizeof(renameTo));
		if(ImGui::Button(_("Rename"), ImVec2(120, 0))){
			std::vector<ObjectInst*> members;
			for(CPtrNode *p = instances.first; p; p = p->next){
				ObjectInst *inst = (ObjectInst*)p->item;
				if(inst->m_pawnProperties && strcmp(inst->m_pawnProperties->group, renameFrom) == 0)
					members.push_back(inst);
			}
			uiOutlinerSetGroup(members, renameTo);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// "Group selected": a new or existing group for the whole selection.
	static char groupName[PAWN_OBJECT_GROUP_LEN];
	if(gOutliner.openGroupPopup){
		gOutliner.openGroupPopup = false;
		if(selection.first){
			snprintf(groupName, sizeof(groupName), "%s",
				GetObjectPawnProperties((ObjectInst*)selection.first->item)->group);
			ImGui::OpenPopup(L("Group selected"));
		}else
			Toast(TOAST_SELECTION, _("Select what to group first"));
	}
	if(ImGui::BeginPopupModal(L("Group selected"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		uiGroupPicker(_("Group"), groupName, sizeof(groupName));
		ImGui::SetItemTooltip(_("Pick a group or type a new name. Empty: ungrouped."));
		bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
		if(ImGui::Button(_("Group"), ImVec2(120, 0)) || enter){
			std::vector<ObjectInst*> selected;
			uiSelectedInstances(selected);
			uiOutlinerSetGroup(selected, groupName);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel"), ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

static void
uiMapObjectsWindow(void)
{
	uiDefaultPanelLayout(PANEL_RIGHT_BOTTOM);
	uiNextPanelSizeConstraints();
	if(ImGui::Begin(LI(ICON_FA_LAYER_GROUP, "Map Objects"), &showMapObjectsWindow)){
		uiFitItemsToPanel();
		RadarArtLayersOutliner();
		uiMapObjectsList();
	}
	ImGui::End();
}

static void
uiDiffWindow(void)
{
	uiNextPanelSizeConstraints();
	ImGui::Begin(LI(ICON_FA_CODE_COMPARE, "Changes for Pawn Export"), &showDiffWindow);
	uiFitItemsToPanel();

	// Count changes by category
	int numAdded = 0, numDeleted = 0, numMoved = 0, numRotated = 0, numModel = 0, numMaterial = 0, numRestored = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!IsPawnObjectSelectable(inst))
			continue;
		int flags = GetInstanceDiffFlags(inst);
		if(flags & DIFF_ADDED)    numAdded++;
		if(flags & DIFF_DELETED)  numDeleted++;
		if(flags & DIFF_MOVED)    numMoved++;
		if(flags & DIFF_ROTATED)  numRotated++;
		if(flags & DIFF_MODEL)    numModel++;
		if(flags & DIFF_MATERIAL) numMaterial++;
		if(flags & DIFF_RESTORED) numRestored++;
	}

	int total = numAdded + numDeleted + numMoved + numRotated + numModel + numMaterial + numRestored;
	if(total == 0){
		ImGui::TextDisabled(_("No changes requiring Pawn export."));
		ImGui::End();
		return;
	}

	if(numAdded + numDeleted + numMoved + numRotated + numMaterial > 0)
		ImGui::Text(_("%d added, %d deleted, %d moved, %d rotated, %d material"), numAdded, numDeleted, numMoved, numRotated, numMaterial);
	if(numModel > 0)
		ImGui::TextDisabled(_("Model ID changes: %d"), numModel);
	if(numRestored > 0)
		ImGui::SameLine(), ImGui::Text(_(", %d restored"), numRestored);
	ImGui::Separator();

	// Filter buttons (bitmask — 0 = show all)
	if(ImGui::RadioButton(_("All"), gDiffFilter == 0)) gDiffFilter = 0;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Added"), gDiffFilter == DIFF_ADDED)) gDiffFilter = DIFF_ADDED;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Deleted"), gDiffFilter == DIFF_DELETED)) gDiffFilter = DIFF_DELETED;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Moved"), gDiffFilter == DIFF_MOVED)) gDiffFilter = DIFF_MOVED;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Rotated"), gDiffFilter == DIFF_ROTATED)) gDiffFilter = DIFF_ROTATED;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Model ID"), gDiffFilter == DIFF_MODEL)) gDiffFilter = DIFF_MODEL;
	ImGui::SameLine();
	if(ImGui::RadioButton(_("Material"), gDiffFilter == DIFF_MATERIAL)) gDiffFilter = DIFF_MATERIAL;
	if(numRestored > 0){
		ImGui::SameLine();
		if(ImGui::RadioButton(_("Restored"), gDiffFilter == DIFF_RESTORED)) gDiffFilter = DIFF_RESTORED;
	}

	// Collect changed instances into temp array for sorting
	ObjectInst *changed[4096];
	int numChanged = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!IsPawnObjectSelectable(inst))
			continue;
		int flags = GetInstanceDiffFlags(inst);
		if(flags == 0) continue;
		if(gDiffFilter != 0 && !(flags & gDiffFilter)) continue;
		if(numChanged < 4096)
			changed[numChanged++] = inst;
	}

	// Sort by m_changeSeq descending (most recent first)
	for(int i = 0; i < numChanged - 1; i++)
		for(int j = i + 1; j < numChanged; j++)
			if(changed[j]->m_changeSeq > changed[i]->m_changeSeq){
				ObjectInst *tmp = changed[i];
				changed[i] = changed[j];
				changed[j] = tmp;
			}

	ImGui::BeginChild("DiffList", ImVec2(0, 0), true);
	for(int i = 0; i < numChanged; i++){
		ObjectInst *inst = changed[i];
		int flags = GetInstanceDiffFlags(inst);
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		const char *name = obj ? obj->m_name : "???";

		// Build prefix string from flags
		char prefix[8] = "";
		ImVec4 color = ImVec4(1,1,1,1);
		if(flags & DIFF_ADDED){
			strcat(prefix, "+");
			if(flags & DIFF_MATERIAL) strcat(prefix, "T");
			if(flags & DIFF_PROPERTIES) strcat(prefix, "P");
			color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
		}else if(flags & DIFF_DELETED){
			strcat(prefix, "-");
			color = UiSemanticColor(UI_COLOR_ERROR);
		}else if(flags & DIFF_RESTORED){
			strcat(prefix, "U");
			color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
		}else{
			if(flags & DIFF_MOVED)  strcat(prefix, "M");
			if(flags & DIFF_ROTATED) strcat(prefix, "R");
			if(flags & DIFF_MODEL)   strcat(prefix, "I");
			if(flags & DIFF_MATERIAL) strcat(prefix, "T");
			if(flags & DIFF_PROPERTIES) strcat(prefix, "P");
			color = ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
			if(flags == DIFF_ROTATED)
				color = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
		}

		char buf[256];
		if(flags & DIFF_MODEL){
			snprintf(buf, sizeof(buf), _("%-3s %-20s  Model ID %d -> %d"), prefix, name,
				PawnModelId(inst->m_savedObjectId), PawnModelId(inst->m_objectId));
		}else if(flags & DIFF_MOVED){
			float dist = length(sub(inst->m_translation, inst->m_savedTranslation));
			snprintf(buf, sizeof(buf), "%-3s %-20s  %.1f, %.1f, %.1f  (%.1fm)",
				prefix, name,
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				dist);
		}else if(flags & DIFF_ROTATED){
			float dot = GetQuaternionSimilarity(inst->m_rotation, inst->m_savedRotation);
			float angleDeg = 2.0f * acosf(dot) * (180.0f / 3.14159265f);
			snprintf(buf, sizeof(buf), _("%-3s %-20s  %.1f, %.1f, %.1f  (%.1f deg)"),
				prefix, name,
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				angleDeg);
		}else if(flags & DIFF_MATERIAL){
			snprintf(buf, sizeof(buf), _("%-3s %-20s  material override(s)"), prefix, name);
		}else{
			snprintf(buf, sizeof(buf), "%-3s %-20s  %.1f, %.1f, %.1f",
				prefix, name,
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z);
		}

		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::PushID(inst);
		if(ImGui::Selectable(buf)){
			inst->Select();
		}
		ImGui::PopID();
		ImGui::PopStyleColor();

		if(ImGui::IsItemHovered()){
			inst->m_highlight = HIGHLIGHT_HOVER;
			if(ImGui::IsMouseDoubleClicked(0))
				inst->JumpTo();
		}
	}
	ImGui::EndChild();

	ImGui::End();
}

bool
InPawnIdeMode(void)
{
 return showPawnCodeWindow && !InTextDrawMode();
}

// Workspace navigation shares the menu bar; panel tabs keep their own docks.
static void
uiWorkspaceNavigation(void)
{
	bool hintChanged = false;
	if(InTextDrawMode()){
		showPawnCodeWindow = false;
		if(gShowTextDrawsHint){ gShowTextDrawsHint = false; hintChanged = true; }
	}
	if(InPawnIdeMode() && gShowCodeHint){ gShowCodeHint = false; hintChanged = true; }
	const char *labels[] = { L("Map"), L("TextDraws"), L("Code") };
	for(int i = 0; i < 3; i++){
		int mode = InTextDrawMode() ? 1 : (InPawnIdeMode() ? 2 : 0);
		bool selected = mode == i;
		ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		bool clicked = ImGui::Button(labels[i]);
		ImGui::PopStyleColor(4);
		if(clicked){
			if(i == 1){
				showPawnCodeWindow = false;
				EnterTextDrawMode();
				if(gShowTextDrawsHint){ gShowTextDrawsHint = false; hintChanged = true; }
			}else{
				ExitTextDrawMode();
				showPawnCodeWindow = i == 2;
				if(i == 2 && gShowCodeHint){ gShowCodeHint = false; hintChanged = true; }
			}
		}
		if(i == 1 || i == 2){
			ImVec2 anchor((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, ImGui::GetItemRectMax().y);
			if(i == 1){
				gTextDrawsHintAnchor = anchor;
				ImGui::SetItemTooltip(_("TextDraw mode: edit the textdraw file on a canvas instead of the world."));
			}else{
				gCodeHintAnchor = anchor;
				ImGui::SetItemTooltip("%s", ShortcutLabel(SC_WINDOW_PAWN_CODE));
			}
		}
	}
	if(hintChanged) saveSaveSettings();
}


static void
uiNewWorkspaceHint(const char *window, const char *dismiss, ImVec2 anchor, bool &visible, float offset)
{
	if(!visible) return;
	static double appeared = -1.0;
	if(appeared < 0.0) appeared = ImGui::GetTime();
	const double age = ImGui::GetTime() - appeared;
	// Five small jumps, then settle. The hint remains until used or dismissed.
	float jump = age < 6.0 ? std::abs(std::sin(age * 3.141592653589793 / 1.2)) * 4.0f : 0.0f;
	KeepNextEditorOverlayOnMainWindow();
	ImGui::SetNextWindowPos(ImVec2(anchor.x + offset, anchor.y + 12.0f - jump),
		ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	const ImU32 yellow = IM_COL32(255, 213, 79, 255);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, yellow);
	ImGui::Begin(window, nil, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking);
	ImGui::TextColored(ImVec4(0.20f, 0.14f, 0.02f, 1.0f), "%s", _("New!"));
	ImGui::SameLine(0.0f, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.14f, 0.02f, 0.60f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.14f, 0.02f, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.14f, 0.02f, 0.18f));
	if(ImGui::SmallButton(dismiss)){
		visible = false;
		saveSaveSettings();
	}
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar();
	ImGui::SetItemTooltip("%s", _("Dismiss"));
	ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	ImDrawList *shadow = ImGui::GetBackgroundDrawList();
	shadow->AddRectFilled(ImVec2(pos.x + 1, pos.y + 2), ImVec2(pos.x + size.x + 1, pos.y + size.y + 2),
		IM_COL32(0, 0, 0, 45), 12.0f);
	ImVec2 tip(anchor.x, anchor.y + 2.0f);
	ImVec2 left(tip.x - 5.0f, pos.y + 3.0f), right(tip.x + 5.0f, pos.y + 3.0f);
	ImGui::GetForegroundDrawList()->AddTriangleFilled(tip, right, left, yellow);
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}


static void
uiWorkspaceNewHints(void)
{
	if((!gShowTextDrawsHint && !gShowCodeHint) || !isSA() || InTextDrawMode() || InPawnIdeMode() || StartScreenOpen() ||
	   ImGui::GetIO().AppFocusLost || ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		return;
	// Spread the balloons slightly when the buttons are close, keeping both
	// tails anchored to their own workspace without overlapping the labels.
	float width = ImGui::CalcTextSize(_("New!")).x + ImGui::CalcTextSize(ICON_FA_XMARK).x + 26.0f;
	float offset = std::max(0.0f, (width + 12.0f - (gCodeHintAnchor.x - gTextDrawsHintAnchor.x)) * 0.5f);
	uiNewWorkspaceHint("###TextDrawsNew", ICON_FA_XMARK "###DismissTextDrawsHint", gTextDrawsHintAnchor, gShowTextDrawsHint, -offset);
	uiNewWorkspaceHint("###CodeNew", ICON_FA_XMARK "###DismissCodeHint", gCodeHintAnchor, gShowCodeHint, offset);
}


// Objects and removed buildings in the map, recounted when the scene changes.
static void
countMapContents(int *objects, int *removed)
{
	static uint32 countedSeq = 0xFFFFFFFFu;
	static int cachedObjects, cachedRemoved;
	uint32 seq = GetLatestChangeSeq();
	if(seq != countedSeq){
		cachedObjects = cachedRemoved = 0;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(!PawnObjectNeedsExport(inst))
				continue;
			if(inst->m_isDeleted)
				cachedRemoved++;
			else
				cachedObjects++;
		}
		countedSeq = seq;
	}
	*objects = cachedObjects;
	*removed = cachedRemoved;
}

// Bottom bar: what mode the editor is in, what to do next, and the map state.
static void
uiStatusBar(void)
{
	ImGuiIO &io = ImGui::GetIO();
	// A side bar, so panels and the dock space stop above it.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 5.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
	ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, uiStatusBarHeight(),
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
	{
		ImVec2 lo = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddLine(lo, ImVec2(lo.x + ImGui::GetWindowWidth(), lo.y), ImGui::GetColorU32(ImGuiCol_Separator));
	}

	int selected = 0;
	for(CPtrNode *p = selection.first; p; p = p->next)
		selected++;
	ObjectDef *spawn = GetObjectDef(GetSpawnObjectId());
	const char *mode;
	ImVec4 modeColor;
	char hint[512];
	if(InPawnIdeMode()){
		mode = "PAWN";
		modeColor = UiSemanticColor(UI_COLOR_INFO);
		snprintf(hint, sizeof(hint), _("Edit Pawn · Ctrl+S saves · Ctrl+F searches."));
	}else if(InTextDrawMode()){
		mode = _("TEXTDRAW");
		modeColor = UiSemanticColor(UI_COLOR_WARNING);
		snprintf(hint, sizeof(hint), _("Drag to move · drag a corner to resize · %s stops."), uiKey(SC_CANCEL));
	}else if(gBrushMode && spawn){
		mode = _("BRUSH");
		modeColor = UiSemanticColor(UI_COLOR_WARNING);
		snprintf(hint, sizeof(hint), _("Click or drag to paint · %s stops."), uiKey(SC_CANCEL));
	}else if(gPrefabPlaceMode){
		mode = _("PREFAB");
		modeColor = UiSemanticColor(UI_COLOR_INFO);
		snprintf(hint, sizeof(hint), _("Click to place · Shift+click repeats · %s stops."), uiKey(SC_CANCEL));
	}else if(gPlaceMode && spawn){
		mode = _("PLACE");
		modeColor = UiSemanticColor(UI_COLOR_SUCCESS);
		snprintf(hint, sizeof(hint), _("Click to place · Shift+click repeats · %s stops."), uiKey(SC_CANCEL));
	}else if(selected > 0){
		mode = _("SELECT");
		modeColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		snprintf(hint, sizeof(hint), _("%d selected · Drag gizmo to move or rotate · %s deletes."),
			selected, uiKey(SC_DELETE));
	}else{
		mode = _("READY");
		modeColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		snprintf(hint, sizeof(hint), _("%s Browse · %s Map objects · %s Save · %s Shortcuts"),
			uiKey(SC_WINDOW_BROWSER), uiKey(SC_WINDOW_MAP_OBJECTS), uiKey(SC_SAVE_MAP), uiKey(SC_WINDOW_SHORTCUTS));
	}
	// Left: the mode as a badge, then what to do next.
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImVec2 t = ImGui::CalcTextSize(mode);
		float padX = 5.0f;
		ImDrawList *draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(ImVec2(p.x, p.y - 1.0f), ImVec2(p.x + t.x + padX*2.0f, p.y + t.y + 1.0f),
			ImGui::GetColorU32(ImVec4(modeColor.x, modeColor.y, modeColor.z, 0.18f)), 2.0f);
		ImGui::SetCursorScreenPos(ImVec2(p.x + padX, p.y));
		ImGui::TextColored(modeColor, "%s", mode);
		ImGui::SameLine(0.0f, padX + ImGui::GetFontSize()*0.6f);
	}

	int objects, removed;
	countMapContents(&objects, &removed);
	char removedText[32] = "";
	if(removed > 0)
		snprintf(removedText, sizeof(removedText), _(", %d removed"), removed);
	char objectsText[64], countText[32];
	snprintf(countText, sizeof(countText), _("%d objects"), objects);
	snprintf(objectsText, sizeof(objectsText), "%s%s", countText, removedText);
	bool unsaved = mapHasUnsavedChanges();
	const char *savedText = unsaved ? _("Unsaved changes") : _("Saved");
	char fps[32];
	snprintf(fps, sizeof(fps), _("%.0f FPS"), io.Framerate);

	// Hidden objects (H / Shift+H), with a way back.
	int hidden = CountHiddenInstances();
	char hiddenText[48];
	snprintf(hiddenText, sizeof(hiddenText), TI(ICON_FA_EYE_SLASH, "%d hidden"), hidden);
	// Errors the user hasn't seen in the Log yet.
	int newErrors;
	{
		std::lock_guard<std::mutex> lock(gLogMutex);
		newErrors = showLogWindow ? 0 : gLogErrors - gLogSeenErrors;
	}
	char errorsText[48];
	snprintf(errorsText, sizeof(errorsText), TI(ICON_FA_TRIANGLE_EXCLAMATION, "%d errors"), newErrors);

	// Right: segments split by thin lines, measured first so the hint can give way.
	float gap = ImGui::GetFontSize() * 1.2f;
	float right = ImGui::CalcTextSize(currentMapName()).x + ImGui::GetFontSize()*0.5f + ImGui::CalcTextSize(savedText).x +
		gap + ImGui::CalcTextSize(objectsText).x + gap + ImGui::CalcTextSize(fps).x;
	if(newErrors > 0)
		right += ImGui::CalcTextSize(errorsText).x + gap;
	if(hidden > 0)
		right += ImGui::CalcTextSize(hiddenText).x + ImGui::CalcTextSize(_("Show all")).x +
			ImGui::GetStyle().FramePadding.x*2.0f + ImGui::GetStyle().ItemSpacing.x + gap;
	float rightX = ImGui::GetWindowContentRegionMax().x - right;
	float hintRoom = rightX - ImGui::GetCursorPosX() - gap;
	if(ImGui::CalcTextSize(hint).x <= hintRoom)
		ImGui::TextDisabled("%s", hint);
	else
		ImGui::Dummy(ImVec2(0, 0));
	ImGui::SetItemTooltip("%s", hint);

	auto divider = [&](void){
		ImGui::SameLine(0.0f, gap*0.5f);
		ImVec2 p = ImGui::GetCursorScreenPos();
		float h = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h*0.15f), ImVec2(p.x, p.y + h*0.85f), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.5f));
		ImGui::Dummy(ImVec2(1.0f, h));
		ImGui::SameLine(0.0f, gap*0.5f);
	};
	ImGui::SameLine(max(ImGui::GetCursorPosX() + gap, rightX));
	bool first = true;
	if(newErrors > 0){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), "%s", errorsText);
		if(ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::SetItemTooltip("%s", _("Something went wrong: click for the Log"));
		if(ImGui::IsItemClicked())
			showLogWindow = true;
		first = false;
	}
	if(hidden > 0){
		if(!first) divider();
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", hiddenText);
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Show all")))
			ShowAllInstances();
		ImGui::SetItemTooltip(_("Show every hidden object (%s)"), uiKey(SC_SHOW_ALL));
		first = false;
	}
	if(!first) divider();
	ImGui::TextUnformatted(currentMapName());
	ImGui::SameLine(0.0f, ImGui::GetFontSize()*0.5f);
	ImGui::TextColored(unsaved ? UiSemanticColor(UI_COLOR_WARNING) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", savedText);
	divider();
	ImGui::TextDisabled("%s", objectsText);
	divider();
	ImGui::TextDisabled("%s", fps);
	ImGui::End();
}

// What the player view leaves out, over the top of the 3D view.
static void
uiPlayerViewOverlay(void)
{
	if(!gPlayerView || !isSA())
		return;
	const PlayerViewStats &s = GetPlayerViewStats();
	ImGuiIO &io = ImGui::GetIO();
	KeepNextEditorOverlayOnMainWindow();
	ImGui::SetNextWindowPos(EditorUiPoint(io.DisplaySize.x * 0.5f, ImGui::GetFrameHeight() * 2.6f),
		ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.75f);
	if(ImGui::Begin("##player_view_overlay", nil, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav)){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), TI(ICON_FA_EYE, "Player view"));
		ImGui::SameLine();
		if(s.streamer)
			ImGui::Text(_("%d objects visible / %d streamed (of %d in stream range, limit 1000)"),
				s.objectsVisible, s.objectsStreamed, s.objectsInRange);
		else
			ImGui::Text(_("%d objects visible / %d created (native, limit 1000)"), s.objectsVisible, s.objectsStreamed);
		ImGui::TextDisabled(_("%d vehicles, %d actors streamed  |  virtual world %d, interior %d"), s.vehiclesStreamed,
			s.actorsStreamed, gPlayerViewWorld, currentArea);
		if(s.otherWorld)
			ImGui::TextDisabled(_("%d element(s) are in another virtual world or interior"), s.otherWorld);
		if(s.outsideArea)
			ImGui::TextDisabled(_("%d item(s) only show inside a dynamic area the camera is outside"), s.outsideArea);
	}
	ImGui::End();
}

// Shown on an empty map until the user places something or hides it.
static void
uiStartGuide(void)
{
	int objects, removed;
	countMapContents(&objects, &removed);
	// Only for a map that is still new: undoing back to an empty map isn't a fresh start.
	if(!gShowStartGuide || objects > 0 || removed > 0 || gPlaceMode || gBrushMode || StartScreenOpen() ||
	   gPrefabPlaceMode || !isSA() || MapHasUndoHistory())
		return;
	ImGuiIO &io = ImGui::GetIO();
	KeepNextEditorOverlayOnMainWindow();
	ImGui::SetNextWindowPos(EditorUiPoint(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
		ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.92f);
	if(!ImGui::Begin(L("Start mapping"), nil, ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)){
		ImGui::End();
		return;
	}
	ImGui::TextDisabled(_("Choose an object, then click the world to place it."));
	const char *workspace = GetWorkspaceDir();
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorHovered));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive));
	if(ImGui::Button(TI(ICON_FA_MAGNIFYING_GLASS, "Browse objects")))
		showBrowserWindow = true;
	ImGui::PopStyleColor(3);
	ImGui::SetItemTooltip(_("Pick an object, then click in the world to place it."));
	ImGui::SameLine();
	if(ImGui::Button(TI(ICON_FA_FOLDER_OPEN, "Open a map...")))
		requestMapAction(MAP_ACTION_OPEN);
	if(!workspace[0]){
		ImGui::SameLine();
		if(ImGui::Button(TI(ICON_FA_GLOBE, "Workspace...")))
			gShowWorkspaceWindow = true;
	}
	ImGui::SameLine();
	if(ImGui::Button(TI(ICON_FA_CIRCLE_QUESTION, "Help")))
		showHelpWindow = true;
	ImGui::SameLine();
	if(ImGui::Button(_("Hide"))){
		gShowStartGuide = false;
		saveSaveSettings();
	}
	ImGui::SetItemTooltip(_("Help > Getting started shows it again."));
	ImGui::End();
}

static int
uiSelectionCount(void)
{
	int count = 0;
	for(CPtrNode *p = selection.first; p; p = p->next) count++;
	return count;
}

static void
uiExitPlacementModes(void)
{
	if(gPlaceMode) SpawnExitPlaceMode();
	if(gPrefabPlaceMode) ExitPrefabPlaceMode();
	if(gBrushMode) ExitBrushMode();
}

// Scrolls the panel under the mouse, or the focused one, by most of a page.
static void
uiScrollPanel(float direction)
{
	ImGuiContext &g = *ImGui::GetCurrentContext();
	ImGuiWindow *window = g.HoveredWindow ? g.HoveredWindow : g.NavWindow;
	while(window && window->ScrollMax.y <= 0.0f && (window->Flags & ImGuiWindowFlags_ChildWindow))
		window = window->ParentWindow;
	if(window == nil || window->ScrollMax.y <= 0.0f)
		return;
	float page = window->InnerRect.GetHeight() * 0.8f;
	ImGui::SetScrollY(window, ImClamp(window->Scroll.y + direction * page, 0.0f, window->ScrollMax.y));
}

// What each entry of the shortcut table does, also run from the Edit menu.
// Arrow keys over the 3D view: the selection one move step, along the map
// axis closest to where the camera looks (so layouts stay on the grid).
// One undo step per press.
static void
uiNudgeSelection(int action)
{
	if(selection.first == nil)
		return;
	ObjectInst *leader = (ObjectInst*)selection.first->item;
	if(leader == nil || !IsPawnObjectEditable(leader))
		return;
	float step = gGizmoSnapTranslate > 0.0f ? gGizmoSnapTranslate : 1.0f;
	rw::V3d look = sub(TheCamera.m_target, TheCamera.m_position);
	rw::V3d forward = fabsf(look.x) > fabsf(look.y) ? rw::makeV3d(look.x > 0 ? 1.0f : -1.0f, 0, 0) :
		rw::makeV3d(0, look.y > 0 ? 1.0f : -1.0f, 0);
	rw::V3d right = rw::makeV3d(forward.y, -forward.x, 0);
	rw::V3d delta = rw::makeV3d(0, 0, 0);
	switch(action){
	case SC_NUDGE_LEFT: delta = scale(right, -step); break;
	case SC_NUDGE_RIGHT: delta = scale(right, step); break;
	case SC_NUDGE_FORWARD: delta = scale(forward, step); break;
	case SC_NUDGE_BACK: delta = scale(forward, -step); break;
	case SC_NUDGE_UP: delta = rw::makeV3d(0, 0, step); break;
	case SC_NUDGE_DOWN: delta = rw::makeV3d(0, 0, -step); break;
	}
	finishTransformPanelEdit();
	std::vector<UndoTransform> transforms;
	if(!CaptureObjectTransformTargets(leader, true, transforms))
		return;
	PreviewObjectTransformTargets(leader, transforms, add(leader->m_translation, delta),
		leader->m_rotation, UNDO_TRANSFORM_POS);
	CommitObjectTransformTargets(transforms);
}

static void
uiRunShortcut(int action)
{
 int count;
 if(InPawnIdeMode()){
  if(action == SC_SAVE_MAP) PawnEditorRequest(PAWN_EDITOR_SAVE);
  else if(action == SC_SAVE_MAP_AS) PawnEditorRequest(PAWN_EDITOR_SAVE_AS);
  else if(action == SC_NEW_MAP) PawnEditorRequest(PAWN_EDITOR_NEW);
  else if(action == SC_OPEN_MAP) PawnEditorRequest(PAWN_EDITOR_OPEN);
  else if(action == SC_WINDOW_PAWN_CODE) showPawnCodeWindow = false;
  return;
 }
	// TextDraw mode takes the file keys and leaves out map editing.
	if(action != SC_UNDO && action != SC_REDO && TextDrawModeShortcut(action))
		return;
	switch(action){
	case SC_NEW_MAP: requestMapAction(MAP_ACTION_NEW); break;
	case SC_OPEN_MAP: requestMapAction(MAP_ACTION_OPEN); break;
	case SC_SAVE_MAP: savePawnMap(); break;
	case SC_SAVE_MAP_AS: gOpenSavePawnAs = true; break;
	case SC_EXPORT_PREFAB:
		if(selection.first)
			gOpenExportPrefab = true;
		break;
	case SC_IMPORT_PREFAB: gOpenImportPrefab = true; break;
	case SC_TEST_IN_GAME:
		uiMapCheckBefore("testing");
		TestServerRunMap(gPawnSaveFormat);
		showTestServerWindow = true;
		break;

	case SC_UNDO: uiUndo(); break;
	case SC_REDO: uiRedo(); break;
	case SC_COPY:
		finishTransformPanelEdit();
		count = uiSelectionCount();
		CopySelected();
		if(count > 0)
			ToastFor(TOAST_COPY_PASTE, 6.0f, _("Copied %d instance(s) | %s: paste at camera | %s: paste in place"),
				min(count, MAX_BATCH_OBJECTS), ShortcutLabel(SC_PASTE), ShortcutLabel(SC_PASTE_IN_PLACE));
		break;
	case SC_CUT:
		finishTransformPanelEdit();
		count = uiSelectionCount();
		CutSelected();
		if(count > 0)
			Toast(TOAST_COPY_PASTE, _("Cut %d instance(s)"), min(count, MAX_BATCH_OBJECTS));
		break;
	case SC_PASTE:
	case SC_PASTE_IN_PLACE:
		finishTransformPanelEdit();
		count = action == SC_PASTE_IN_PLACE ? PasteClipboardInPlace() : PasteClipboard();
		if(count > 0)
			Toast(TOAST_COPY_PASTE, action == SC_PASTE_IN_PLACE ?
				_("Pasted %d instance(s) at original position") : _("Pasted %d instance(s)"), count);
		break;
	case SC_DUPLICATE:
		finishTransformPanelEdit();
		count = MapToolsDuplicate(nil);
		if(count > 0)
			Toast(TOAST_COPY_PASTE, _("Duplicated %d instance(s)"), count);
		break;
	case SC_RENAME:
		// Map Objects: the variable of the selection, typed in its row.
		if(gOutliner.hovered && selection.first)
			uiOutlinerStartRename((ObjectInst*)selection.first->item);
		break;
	case SC_GROUP_SELECTION:
		uiRequestGroupSelection();
		break;
	case SC_DELETE:
		finishTransformPanelEdit();
		count = uiSelectionCount();
		if(count > 0){
			DeleteSelected();
			Toast(TOAST_DELETE, _("Deleted %d instance(s)"), min(count, MAX_BATCH_OBJECTS));
		}
		break;
	case SC_CANCEL:
		if(gBrushMode)
			ExitBrushMode();
		else if(gPrefabPlaceMode)
			ExitPrefabPlaceMode();
		else if(gPlaceMode)
			SpawnExitPlaceMode();
		else if(selection.first && !gGizmoUsing){
			// Nothing to leave: Esc lets go of the selection, to see the area.
			finishTransformPanelEdit();
			ClearSelection();
		}
		break;

	case SC_GIZMO_MOVE:
		gGizmoEnabled = true;
		gGizmoMode = GIZMO_TRANSLATE;
		break;
	case SC_GIZMO_ROTATE:
		if(gPrefabPlaceMode)
			break;	// Q/E turn the prefab being placed instead
		gGizmoEnabled = true;
		gGizmoMode = GIZMO_ROTATE;
		break;
	case SC_GIZMO_SPACE:
		if(selection.first)
			gGizmoSpace = gGizmoSpace == GIZMO_LOCAL ? GIZMO_WORLD : GIZMO_LOCAL;
		break;
	case SC_SNAP_TO_GROUND:
	case SC_ALIGN_TO_GROUND:
		finishTransformPanelEdit();
		SnapSelectedToGround(action == SC_ALIGN_TO_GROUND);
		break;
	case SC_OBJECT_SNAP:
		gObjectSnapMode = !gObjectSnapMode;
		Toast(TOAST_SELECTION, "%s", gObjectSnapMode ? _("Object snap mode enabled") : _("Object snap mode disabled"));
		break;
	case SC_FOCUS_SELECTION: FocusSelection(); break;
	case SC_NUDGE_LEFT: case SC_NUDGE_RIGHT: case SC_NUDGE_FORWARD:
	case SC_NUDGE_BACK: case SC_NUDGE_UP: case SC_NUDGE_DOWN:
		uiNudgeSelection(action);
		break;
	case SC_GO_TO_COORDINATES: gOpenGoToCoordinates = true; break;
	case SC_COPY_CAMERA_POSITION: uiCopyCameraPosition(); break;

	case SC_OUTLINE_OBJECTS: gOutlineMapObjects = !gOutlineMapObjects; break;
	case SC_VIEWER_CAMERA: gUseViewerCam = !gUseViewerCam; break;
	case SC_PLAYER_VIEW:
		gPlayerView = !gPlayerView;
		Toast(TOAST_SELECTION, "%s", gPlayerView ? _("Player view on: what a player at the camera would see") : _("Player view off"));
		break;
	case SC_HIDE_SELECTION:
		count = HideSelected();
		if(count > 0)
			Toast(TOAST_SELECTION, _("Hid %d instance(s) | %s: show all"), count, uiKey(SC_SHOW_ALL));
		break;
	case SC_ISOLATE_SELECTION:
		count = IsolateSelected();
		if(count > 0)
			Toast(TOAST_SELECTION, _("Isolated the selection, %d other instance(s) hidden | %s: show all"),
				count, uiKey(SC_SHOW_ALL));
		break;
	case SC_SHOW_ALL:
		count = ShowAllInstances();
		if(count > 0)
			Toast(TOAST_SELECTION, _("Showing %d hidden instance(s)"), count);
		break;
	case SC_FOV_IN: TheCamera.m_fov = ClampCameraFov(TheCamera.m_fov - gFovWheelStep); break;
	case SC_FOV_OUT: TheCamera.m_fov = ClampCameraFov(TheCamera.m_fov + gFovWheelStep); break;
	case SC_FOV_RESET: TheCamera.m_fov = CAMERA_FOV_DEFAULT; break;
	case SC_FLY_FASTER:
	case SC_FLY_SLOWER:
		gFlySpeed = clamp(gFlySpeed * (action == SC_FLY_FASTER ? 1.25f : 0.8f), 0.1f, 70.0f);
		Toast(TOAST_SELECTION, _("Fly speed %.2f"), gFlySpeed);
		saveSaveSettings();
		break;
	case SC_UI_SCALE_UP: StepUiScale(1.0f); break;
	case SC_UI_SCALE_DOWN: StepUiScale(-1.0f); break;
	case SC_UI_SCALE_RESET: SetUiScale(1.0f); break;
	case SC_SCROLL_UP: uiScrollPanel(-1.0f); break;
	case SC_SCROLL_DOWN: uiScrollPanel(1.0f); break;

	case SC_WINDOW_BROWSER:
		showBrowserWindow ^= 1;
		if(!showBrowserWindow)
			uiExitPlacementModes();
		break;
	case SC_WINDOW_OBJECT_INFO: showInstanceWindow ^= 1; break;
	case SC_WINDOW_MAP_OBJECTS: showMapObjectsWindow ^= 1; break;
	case SC_WINDOW_TOOLS: showToolsWindow ^= 1; break;
	case SC_WINDOW_VIEW: showViewWindow ^= 1; break;
	case SC_WINDOW_TIME_WEATHER: showTimeWeatherWindow ^= 1; break;
	case SC_WINDOW_CAMERA: showEditorWindow ^= 1; break;
	case SC_WINDOW_CHANGES: showDiffWindow ^= 1; break;
	case SC_WINDOW_PAWN_CODE: ExitTextDrawMode(); showPawnCodeWindow ^= 1; break;
	case SC_WINDOW_TEXTURES: showTextureBrowserWindow ^= 1; break;
	case SC_WINDOW_NODES:
		if(isSA())
			showNodesWindow ^= 1;
		break;
	case SC_WINDOW_TERMINAL: showTerminalWindow ^= 1; break;
	case SC_WINDOW_SHORTCUTS: showShortcutsWindow ^= 1; break;
	case SC_WINDOW_LOG: showLogWindow ^= 1; break;
	case SC_WINDOW_PREFERENCES: showPreferencesWindow ^= 1; break;
	case SC_WINDOW_MAP_CHECK: showMapCheckWindow ^= 1; break;
	}
}

void
gui(void)
{
	static bool show_another_window = false;
	static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	static bool camloaded = false;

	if(!camloaded){
		loadCamSettings();
		loadSaveSettings();
		// Legacy Pawn-layer state is intentionally not part of the open.mp
		// workflow. Keep old sessions on the single normal editing layer.
		gPawnEditMode = false;
		gPawnWorldDeleteMode = false;
		camloaded = true;
	}
	gCopyableTextCopiedThisFrame = false;
	gTransformPanelDrawnThisFrame = false;


	Path::guiHoveredNode = nil;
	{
		// The document starts untitled; reopen the last map once the world is up.
		static bool startupMapHandled;
		if(!startupMapHandled){
			startupMapHandled = true;
			// SA-MP's fonts and the free stand-ins, into the user's fonts folder.
			InstallBundledMaterialFonts();
			// The workspace's 0.3.DL / open.mp custom models, map or not.
			LoadCustomModelsForMap(nil, std::string());
			gPawnSavePath[0] = '\0';
			gPawnLastSavedChangeSeq = GetLatestChangeSeq();
			// Developer check: --pawn-roundtrip <in.pwn> <out.pwn> [<in> <out>...]
			// opens each map in turn, saves it to its other file and quits, so
			// a test loads the game once for many maps. A later pair may read
			// what an earlier one wrote.
			// --open <map.pwn> opens that map instead of the last one.
			const char *roundtripIn = nil, *openPath = nil;
			std::vector<std::pair<const char*, const char*>> roundtrips;
			const char *selftestIn = nil, *selftestOut = nil, *profileIn = nil;
			const char *textDrawsIn = nil, *textDrawsOut = nil;
			for(int i = 1; i + 2 < sk::args.argc; i++)
				if(strcmp(sk::args.argv[i], "--pawn-roundtrip") == 0){
					roundtripIn = sk::args.argv[i+1];
					for(int k = i + 1; k + 1 < sk::args.argc && strncmp(sk::args.argv[k], "--", 2) != 0 &&
					    strncmp(sk::args.argv[k+1], "--", 2) != 0; k += 2)
						roundtrips.push_back({ sk::args.argv[k], sk::args.argv[k+1] });
				}else if(strcmp(sk::args.argv[i], "--textdraw-roundtrip") == 0){
					// The same for a textdraw file (the textdraws of any Pawn file).
					textDrawsIn = sk::args.argv[i+1];
					textDrawsOut = sk::args.argv[i+2];
				}else if(strcmp(sk::args.argv[i], "--undo-selftest") == 0){
					selftestIn = sk::args.argv[i+1];
					selftestOut = sk::args.argv[i+2];
				}else if(strcmp(sk::args.argv[i], "--profile") == 0){
					profileIn = sk::args.argv[i+1];
					selftestOut = sk::args.argv[i+2];
				}
			const char *categoriesOut = nil, *metaOut = nil;
			for(int i = 1; i + 1 < sk::args.argc; i++)
				if(strcmp(sk::args.argv[i], "--open") == 0)
					openPath = sk::args.argv[i+1];
				else if(strcmp(sk::args.argv[i], "--dump-categories") == 0)
					categoriesOut = sk::args.argv[i+1];
				else if(strcmp(sk::args.argv[i], "--dump-model-meta") == 0)
					metaOut = sk::args.argv[i+1];
			// --dump-zone-settings out.txt: the zones' start-up population as
			// read from the game's main.scm (tests).
			for(int i = 1; i + 1 < sk::args.argc; i++)
				if(strcmp(sk::args.argv[i], "--dump-zone-settings") == 0){
					bool written = SAPaths::DumpZoneSettings(sk::args.argv[i+1]);
					printf("dump-zone-settings: %s\n", written ? sk::args.argv[i+1] : "failed");
					fflush(stdout);
					sk::globals.quit = 1;
				}
			// --material-text-png out.png text materialSize font fontSize bold alignment:
			// one SetObjectMaterialText texture, white on transparent (tests).
			for(int i = 1; i + 7 < sk::args.argc; i++)
				if(strcmp(sk::args.argv[i], "--material-text-png") == 0){
					ObjectMaterialOverride m = {};
					m.textEnabled = true;
					snprintf(m.text, sizeof(m.text), "%s", sk::args.argv[i+2]);
					m.materialSize = atoi(sk::args.argv[i+3]);
					snprintf(m.font, sizeof(m.font), "%s", sk::args.argv[i+4]);
					m.fontSize = atoi(sk::args.argv[i+5]);
					m.bold = atoi(sk::args.argv[i+6]) != 0;
					m.alignment = atoi(sk::args.argv[i+7]);
					m.textColor = { 255, 255, 255, 255 };
					m.backgroundColor = { 0, 0, 0, 0 };
					std::string font;
					DescribeMaterialFont(m.font, m.bold, &font);
					rw::Image *image = RenderMaterialTextImage(m);
					bool written = image != nil;
					if(image){
						rw::writePNG(image, sk::args.argv[i+1]);
						image->destroy();
					}
					printf("material-text-png: %s (%s)\n", written ? sk::args.argv[i+1] : "failed", font.c_str());
					fflush(stdout);
					sk::globals.quit = 1;
				}
			// --textdraw-colours text RRGGBBAA: each line's colours (tests).
			for(int i = 1; i + 2 < sk::args.argc; i++)
				if(strcmp(sk::args.argv[i], "--textdraw-colours") == 0){
					TextDraw td;
					InitTextDraw(&td);
					td.text = sk::args.argv[i+1];
					td.colour = (unsigned int)strtoul(sk::args.argv[i+2], nil, 16);
					printf("textdraw-colours:\n%s\n", DescribeTextDrawColours(td).c_str());
					fflush(stdout);
					sk::globals.quit = 1;
				}
			if(sk::globals.quit){
			}else if(metaOut){
				// Every model's facts, for the agent's index and coverage checks.
				ReadAllModelMetaDetails();
				printf("dump-model-meta: %d model(s) to %s\n", DumpModelMeta(metaOut), metaOut);
				fflush(stdout);
				sk::globals.quit = 1;
			}else if(categoriesOut){
				// Developer check: every model with its category, for coverage reports.
				printf("dump-categories: %d model(s) to %s\n", DumpObjectCategories(categoriesOut), categoriesOut);
				fflush(stdout);
				sk::globals.quit = 1;
			}else if(textDrawsIn){
				std::string error;
				bool done = OpenTextDrawFile(textDrawsIn, &error) && SaveTextDrawFile(textDrawsOut, &error);
				printf("textdraw-roundtrip: %s -> %s: %d textdraw(s)%s%s\n", textDrawsIn, textDrawsOut,
					done ? (int)GetTextDraws().size() : -1, done ? "" : ", ", done ? "" : error.c_str());
				fflush(stdout);
				sk::globals.quit = 1;
			}else if(selftestIn || profileIn){
				// Developer self-tests (selftest.h): report to a file, then quit.
				FILE *report = fopen(selftestOut, "w");
				if(report == nil)
					report = stdout;
				auto start = std::chrono::steady_clock::now();
				bool opened = openPawnMap(selftestIn ? selftestIn : profileIn);
				double openSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				if(!opened)
					fprintf(report, "FAIL open: could not open the map\n");
				else if(selftestIn){
					RunUndoSelfTest(report);
					sk::globals.quit = 1;
				}else{
					RunProfile(report, openSeconds);
					gProfileReport = report;
					gProfileFrames = 0;
				}
				if(selftestIn && report != stdout)
					fclose(report);
				if(!opened)
					sk::globals.quit = 1;
			}else if(openPath && !roundtripIn){
				if(openPawnMap(openPath))
					OfferMoveMapTextDraws(openPath);
			}
			else if(roundtripIn){
				for(auto &roundtrip : roundtrips){
					bool opened = openPawnMap(roundtrip.first);
					int skipped = 0;
					int saved = opened ? ExportPawnCode(roundtrip.second,
						(PawnCodeFormat)gPawnSaveFormat, false, &skipped) : -1;
					printf("pawn-roundtrip: %s -> %s: %d operation(s), %d skipped on save\n",
						roundtrip.first, roundtrip.second, saved, skipped);
					fflush(stdout);
				}
				sk::globals.quit = 1;
			}else if(StartScreenWantedAtLaunch()){
				ShowStartScreen();	// it opens the map the user picks
			}else if(gReopenLastMap && gNumRecentMaps > 0 && isSA()){
				if(openPawnMap(gRecentMaps[0]))
					OfferMoveMapTextDraws(gRecentMaps[0]);
			}
		}
	}
	if(gProfileFrames >= 0){
		static std::chrono::steady_clock::time_point last;
		static double total, worst;
		auto now = std::chrono::steady_clock::now();
		if(gProfileFrames > 30){	// after the first frames settle
			double ms = std::chrono::duration<double, std::milli>(now - last).count();
			total += ms;
			worst = max(worst, ms);
		}
		last = now;
		if(++gProfileFrames == 330){
			fprintf(gProfileReport, "frame avg %.2f ms, worst %.2f ms (300 frames)\n", total / 300.0, worst);
			if(gProfileReport != stdout)
				fclose(gProfileReport);
			sk::globals.quit = 1;
		}
	}
	if(gWindowCloseRequested){
		gWindowCloseRequested = false;
		requestMapAction(MAP_ACTION_QUIT);
	}
	updateMapWindowTitle();
	uiSnapPanels();
	uiMainmenu();
	uiCommandBar();
	uiWorkspaceWindow();
	uiStatusBar();
	// The tools moved to the main toolbar and the 3D view's overlay.
	uiPanelStrip();
#ifdef IMGUI_HAS_DOCK
	uiDockSpace();
#endif
	if(!InTextDrawMode() && !InPawnIdeMode()){
		uiStartGuide();
		uiPlayerViewOverlay();
		uiDrawSampElementOverlays();
		uiDrawNpcPaths();
	}
	uiUnsavedMapPopup();
	uiGoToCoordinatesPopup();
	DrawStartScreen(true);
	// TODO: restore when the Destroy Entire Map workflow is finished.
	UpdaterDrawGui();
	CrashReportsDrawBanner();
	autosaveTick();

	// While looking around, W/A/S/D fly and no key is a shortcut (W/E/R
	// don't switch the gizmo).
	if(InPawnIdeMode()) PawnEditorDraw();
	PawnEditorTick();
	DiscordPresenceTick();
	ShortcutsBeginFrame(gCopyableTextCopiedThisFrame || gCameraLooking || InPawnIdeMode());
	for(int action = 0; action < SC_NUM_ACTIONS; action++)
		if(ShortcutPressed(action))
			uiRunShortcut(action);
	UpdateMaterialIndexView();
	if(InTextDrawMode() || InPawnIdeMode()){
		// Map editing is off: nothing selected or being placed.
		uiExitPlacementModes();
		if(selection.first)
			ClearSelection();
	}

	// TextDraw mode keeps only its own panel and the editor's general windows:
	// the map's panels go, and their docks give the canvas the room.
	const bool mapPanels = !InTextDrawMode() && !InPawnIdeMode();
	if(mapPanels && showTimeWeatherWindow){
		uiNextPanelSizeConstraints();
		ImGui::Begin(LI(ICON_FA_CLOUD_SUN, "Time & Weather"), &showTimeWeatherWindow);
		uiFitItemsToPanel();
		uiTimeWeather();
		ImGui::End();
	}

	if(mapPanels && showHistoryWindow)
		uiHistoryWindow();

	if(mapPanels && showViewWindow){
		ImGui::SetNextWindowSize(ImVec2(460.0f, 640.0f), ImGuiCond_FirstUseEver);
		uiNextPanelSizeConstraints();
		ImGui::Begin(LI(ICON_FA_EYE, "View"), &showViewWindow);
		uiFitItemsToPanel();
		uiView();
		ImGui::End();
	}

	if(mapPanels && showToolsWindow) uiToolsWindow();
	if(showLicensesWindow) DrawLicensesWindow(&showLicensesWindow);
	if(mapPanels && showMapCheckWindow)
		uiMapCheckWindow();
	if(showPreferencesWindow)
		uiPreferencesWindow();
	else if(gShortcutCaptureAction >= 0){
		gShortcutCaptureAction = -1;
		ShortcutsSetCapturing(false);
	}

	{
		static SAPaths::Node *prevSaNode = nil;
		gSaNodeJustSelected = SAPaths::selectedNode != nil && SAPaths::selectedNode != prevSaNode;
		prevSaNode = SAPaths::selectedNode;
		if(gSaNodeJustSelected)
			showInstanceWindow = true;
	}
	if(mapPanels && showInstanceWindow) uiInstWindow();
	uiAttachmentTargetPickWindow();
	if(IsAttachmentWorkspaceActive()) uiAttachmentWorkspaceWindow();

	if(mapPanels && showEditorWindow) uiEditorWindow();

	if(mapPanels && showDiffWindow) uiDiffWindow();

	if(mapPanels && showMapObjectsWindow) uiMapObjectsWindow();

	if(mapPanels && showBrowserWindow){
		uiBrowserWindow();
		// ImGui X button can set showBrowserWindow to false
		if(!showBrowserWindow){
			if(gPlaceMode) SpawnExitPlaceMode();
			if(gPrefabPlaceMode) ExitPrefabPlaceMode();
			if(gBrushMode) ExitBrushMode();
		}
	}
	if(mapPanels){
		uiTextureBrowserWindow();
		SAPaths::DrawNodesWindow(&showNodesWindow);
	}
	BrowserDiscoveryHighlight(mapPanels && showBrowserWindow);
	MapToolsLineOverlay(mapPanels);
	CoordinatePinOverlay(mapPanels);
	MultiplayerOverlay(mapPanels);
	LayoutPathsOverlay(mapPanels);
	SAPaths::UpdateNodesUndo();
	DrawTextDrawsWindow();
	DrawTextDrawsOverlay();
	DrawAddCustomModelDialog();
	if(showTestServerWindow) TestServerWindow(&showTestServerWindow, gPawnSaveFormat);
	if(mapPanels && showInteriorsWindow) InteriorsWindow(&showInteriorsWindow);
	uiWorkspaceNewHints();
	DrawNamakaWindow(&showNamakaWindow);	// also runs the updater's queued Namaka update
	DrawMultiplayerWindow(&showMultiplayerWindow);
	{
		// A new shell starts beside the open map, else in the home folder.
		std::string folder = gPawnSavePath;
		size_t slash = folder.find_last_of("/\\");
		folder = slash == std::string::npos ? std::string() : folder.substr(0, slash);
		if(folder.empty()){
			const char *home = getenv("USERPROFILE");
			if(home == nil)
				home = getenv("HOME");
			folder = home ? home : "";
		}
		DrawTerminalWindow(&showTerminalWindow, folder.c_str());
	}



		if(showHelpWindow) uiHelpWindow();
		if(showShortcutsWindow) uiKeyboardShortcutsWindow();
	if(showLogWindow) drawLogWindow(&showLogWindow);

	// Place mode overlay
	if(gPlaceMode && GetSpawnObjectId() >= 0){
		ObjectDef *obj = GetObjectDef(GetSpawnObjectId());
		if(obj){

			ImGuiIO &placeIO = ImGui::GetIO();
			if(!placeIO.WantCaptureMouse){
				rw::V3d hitPos, hitNormal;
				if(GetPlacementSurfaceHit(&hitPos, &hitNormal)){
					rw::V3d centerPos = hitPos;
					centerPos.z += GetElementPlacementBaseOffset(GetSpawnElementKind(), GetSpawnObjectId());

					rw::V3d centerScreen;
					float csw, csh;
					if(Sprite::CalcScreenCoors(centerPos, &centerScreen, &csw, &csh, false)){
						ImDrawList *dl = EditorForegroundDrawList();
						ImU32 outer = IM_COL32(80, 230, 130, 240);
						ImU32 inner = IM_COL32(80, 230, 130, 70);
						float r = 14.0f * csw;
						if(r < 7.0f) r = 7.0f;
						if(r > 52.0f) r = 52.0f;
						ImVec2 c = EditorUiPoint(centerScreen.x, centerScreen.y);
						dl->AddCircleFilled(c, r, inner, 32);
						dl->AddCircle(c, r, outer, 32, 1.6f);
						dl->AddCircleFilled(c, 4.0f, outer, 12);

						rw::V3d normalEnd = add(hitPos, scale(hitNormal, 2.0f));
						normalEnd.z += GetElementPlacementBaseOffset(GetSpawnElementKind(), GetSpawnObjectId());
						rw::V3d normalScreen;
						float nsw, nsh;
						if(Sprite::CalcScreenCoors(normalEnd, &normalScreen, &nsw, &nsh, false))
							dl->AddLine(c, EditorUiPoint(normalScreen.x, normalScreen.y), outer, 2.0f);
					}
				}
			}
		}
	}

	// Prefab placement overlay
	if(gPrefabPlaceMode){

		ImGuiIO &prefabIO = ImGui::GetIO();
		if(!prefabIO.WantCaptureMouse){
			rw::V3d hitPos, hitNormal;
			if(GetPlacementSurfaceHit(&hitPos, &hitNormal)){
				rw::V3d centerScreen;
				float csw, csh;
				if(Sprite::CalcScreenCoors(hitPos, &centerScreen, &csw, &csh, false)){
					ImDrawList *dl = EditorForegroundDrawList();
					ImU32 outer = IM_COL32(80, 180, 255, 240);
					ImU32 inner = IM_COL32(80, 180, 255, 60);
					float r = 18.0f * csw;
					if(r < 8.0f) r = 8.0f;
					if(r > 64.0f) r = 64.0f;
					ImVec2 c = EditorUiPoint(centerScreen.x, centerScreen.y);
					dl->AddCircleFilled(c, r, inner, 32);
					dl->AddCircle(c, r, outer, 32, 1.8f);
					dl->AddCircleFilled(c, 4.0f, outer, 12);
				}
			}
		}
	}

	// Brush mode overlay + on-surface preview marker
	if(gBrushMode && GetSpawnObjectId() >= 0){
		ObjectDef *obj = GetObjectDef(GetSpawnObjectId());
		if(obj){

			// On-surface preview — only if not over an ImGui panel
			ImGuiIO &brushIO = ImGui::GetIO();
			if(!brushIO.WantCaptureMouse){
				rw::V3d bHit, bNormal;
				if(GetBrushSurfaceHit(&bHit, &bNormal)){
					rw::V3d centerPos = bHit;
					centerPos.z += GetElementPlacementBaseOffset(GetSpawnElementKind(), GetSpawnObjectId());
					centerPos.z += gBrushZOffset;

					ImDrawList *dl = EditorForegroundDrawList();
					ImU32 outer = IM_COL32(240, 190, 50, 230);
					ImU32 inner = IM_COL32(255, 220, 100, 70);

					// Center marker: fixed-size crosshair in screen space
					rw::V3d centerScreen;
					float csw, csh;
					if(Sprite::CalcScreenCoors(centerPos, &centerScreen, &csw, &csh, false)){
						float cr = 4.0f;
						dl->AddCircleFilled(EditorUiPoint(centerScreen.x, centerScreen.y), cr, outer, 12);
					}

					// World-radius disc: sample points around the brush circle in world XY,
					// ground-snap each so it hugs the terrain, project to screen and stroke.
					if(gBrushRadius > 0.01f){
						const int segs = 48;
						ImVec2 pts[segs + 1];
						int validPts = 0;
						bool continuous = true;
						for(int i = 0; i <= segs; i++){
							float theta = (float)i / (float)segs * 6.28318530718f;
							rw::V3d wp = bHit;
							wp.x += gBrushRadius * cosf(theta);
							wp.y += gBrushRadius * sinf(theta);
							rw::V3d snapped = wp;
							GetGroundPlacementSurface(wp, &snapped, nil, true);
							// lift slightly to avoid z-fighting with ground
							snapped.z += 0.05f;

							rw::V3d sp;
							float sw, sh;
							if(Sprite::CalcScreenCoors(snapped, &sp, &sw, &sh, false))
								pts[validPts++] = EditorUiPoint(sp.x, sp.y);
							else
								continuous = false;
						}
						if(validPts > 2){
							dl->AddPolyline(pts, validPts, outer, continuous ? ImDrawFlags_Closed : 0, 2.0f);
							// faint inner shade so the disc reads as a filled region
							dl->AddConvexPolyFilled(pts, validPts, inner);
						}
					}else{
						// radius 0: small screen-space ring around the single-placement point
						if(Sprite::CalcScreenCoors(centerPos, &centerScreen, &csw, &csh, false)){
							float r = 14.0f * csw;
							if(r < 6.0f) r = 6.0f;
							if(r > 64.0f) r = 64.0f;
							dl->AddCircle(EditorUiPoint(centerScreen.x, centerScreen.y), r, outer, 32, 1.5f);
						}
					}
				}
			}
		}
	}


	if(gTransformPanel.editKind != TRANSFORM_PANEL_NONE && !gTransformPanelDrawnThisFrame)
		finishTransformPanelEdit();

	uiDrawPanelSnapGuides();
#ifdef IMGUI_HAS_DOCK
	uiDrawDockTabUnderlines();
#endif
	KeycastFrame();
	RadarFrame();
	uiToasts();
	gResetPanelLayout = false;

	gSettingsAutosaveSeconds += ImGui::GetIO().DeltaTime;
	if(gSettingsAutosaveSeconds >= 1.0f){
		saveSaveSettings();
		gSettingsAutosaveSeconds = 0.0f;
	}

//	uiTest();
}

// The map document for other front ends (the agent bridge). They behave like
// the File menu without its prompts, so callers check for unsaved work first.
bool
OpenPawnMapFile(const char *path)
{
	if(path && path[0])
		leaveJoinedSession(true);
	return path && path[0] && openPawnMap(path);
}

void
NewPawnMapDocument(void)
{
	leaveJoinedSession(false);
	newPawnMap();
}

// The open map as Pawn in its own format, to path, leaving its own path and
// saved state alone (a session's server autosave).
bool
ExportOpenPawnMap(const char *path)
{
	int skipped = 0;
	return path && ExportPawnCode(path, (PawnCodeFormat)gPawnSaveFormat, false, &skipped) >= 0;
}

bool
SavePawnMapTo(const char *path, int format)
{
	char oldPath[sizeof(gPawnSavePath)];
	snprintf(oldPath, sizeof(oldPath), "%s", gPawnSavePath);
	int oldFormat = gPawnSaveFormat;
	if(path && path[0])
		snprintf(gPawnSavePath, sizeof(gPawnSavePath), "%s", path);
	if(format == PAWN_CODE_NATIVE || format == PAWN_CODE_STREAMER)
		gPawnSaveFormat = format;
	if(gPawnSavePath[0] && savePawnMap())
		return true;
	snprintf(gPawnSavePath, sizeof(gPawnSavePath), "%s", oldPath);
	gPawnSaveFormat = oldFormat;
	return false;
}

void
SaveEditorSettings(void)
{
	saveSaveSettings();
}

bool
PickPawnFile(char *dst, size_t size, bool save, const char *suggested)
{
	return pickPawnFileDialog(dst, size, save, suggested);
}

const std::vector<std::string>&
GetRecentMapPaths(void)
{
	static std::vector<std::string> paths;
	paths.clear();
	for(int i = 0; i < gNumRecentMaps; i++)
		paths.push_back(gRecentMaps[i]);
	return paths;
}

const char*
GetPawnMapPath(void)
{
	return gPawnSavePath;
}

int
GetPawnMapFormat(void)
{
	return gPawnSaveFormat;
}

void
SetPawnMapFormat(int format)
{
	if(format == PAWN_CODE_NATIVE || format == PAWN_CODE_STREAMER)
		gPawnSaveFormat = format;
}

bool
PawnMapHasUnsavedChanges(void)
{
	return mapHasUnsavedChanges();
}

// Merges a map file into the open map like File > Merge (Pawn, Texture
// Studio .db, MTA:SA .map, IPL); GetLastPawnImportReport says what came in.
int
MergeMapFile(const char *path, int *skipped)
{
	return path && path[0] ? importMapFile(path, skipped) : -1;
}
