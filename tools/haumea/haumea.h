#ifdef _WIN32
#include <windows.h>	// necessary for the moment
#endif

#include <rw.h>
#include <skeleton.h>
#include "imgui/ImGuizmo.h"
#include <memory>
#include <vector>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <string>
#include <map>

#include <rwgta.h>
#define PS2

//using namespace std;
using rw::int8;
using rw::uint8;
using rw::int16;
using rw::uint16;
using rw::int32;
using rw::uint32;
using rw::int64;
using rw::uint64;
using rw::float32;
using rw::bool32;
using rw::uintptr;
typedef unsigned int uint;


// SA-MP 0.3.DL / open.mp custom models (custommodels.cpp). AddSimpleModel's
// negative IDs -1000 to -30000 live at CUSTOM_MODEL_BASE + 1000 to + 30000
// in the editor; Pawn code always has the negative ID. AddCharModel skins
// keep their IDs (20001-30000).
#define CUSTOM_MODEL_BASE 40000
int PawnModelId(int editorId);
int EditorModelId(int pawnId);
bool IsCustomModel(int editorId);
void ListCustomSimpleModels(std::vector<int> &pawnIds);	// AddSimpleModel's, -1000 to -30000
void LoadCustomModelsForMap(const char *mapPath, const std::string &source);
const char *GetCustomModelsDir(void);	// the server's models folder, or nil
const char *GetCustomModelDffPath(int editorId);
const char *GetCustomModelTxdPath(int txdSlot);
// The name SA-MP knows a custom TXD by (its file name without .txd, as in
// artconfig.txt; SetObjectMaterial uses it), and the slot of that name. The
// editor's own slot names (~custom0...) only keep them apart from stock TXDs.
const char *GetCustomModelTxdName(int txdSlot);
int FindCustomModelTxdSlot(const char *name);
// Server artwork TXDs can be browsed without importing or placing a 3D model.
void ListServerTextureFiles(std::vector<std::string> &files);
int LoadServerTextureTxd(const std::string &relativePath);
int ServerTextureModelId(const std::string &relativePath); // 0 if no literal registration was found
std::string ServerTexturePathForModel(int pawnId);
bool MapUsesCustomModels(void);
void SetupCustomModelCollision(struct ObjectDef *obj, rw::Clump *clump);
void OpenAddCustomModelDialog(const char *droppedPath);	// nil, or a dropped DFF/TXD
void DrawAddCustomModelDialog(void);
const char *GetCustomModelsMapPath(void);
// Multiplayer: the custom models as artconfig.txt lines naming files by
// their own names, sorted, with those files (name -> path)...
std::string CustomModelsSessionList(std::map<std::string, std::string> &files);
// ...and a session's, its files in dir: the ones not here yet are added.
int AddSessionCustomModels(const std::string &dir, const std::string &artconfig);
// Models a map uses that were old modloader/Haumea imports, moved into the
// server's models folder as custom models on request.
int FindLegacyImports(const std::vector<int> &unknownIds, std::vector<int> &legacyIds);
bool MigrateLegacyImports(const std::vector<int> &ids, std::string &err);
int RemapLegacyModel(int id);	// the custom model an old import became, or -1

// SA-MP vehicle extras (vehiclemods.cpp)
#define VEHICLE_PLATE_LEN 33	// SetVehicleNumberPlate takes up to 32 characters
#define VEHICLE_MOD_SLOTS 16	// GetVehicleComponentType's slots
uint32 GetSampVehicleColour(int index);	// 0xRRGGBB00, index 0-255
bool IsVehicleComponentValid(int vehicleModel, int component);
int GetVehicleComponentSlot(int component);	// -1 for none
const char *GetVehicleComponentSlotName(int slot);
int GetVehiclePaintjobCount(int vehicleModel);	// 0-3
void ApplyEditorVehiclePlate(rw::Clump *clump, const char *plateText);
void AddEditorVehicleComponents(rw::Clump *clump, const uint16 *mods);
void ApplyEditorVehiclePaintjob(rw::Clump *clump, int vehicleModel, int paintjob);

uint32 NextRwObjectSerial(void);	// objectinst.cpp

struct ObjectInst;

#include "sapaths.h"
#include "textdraws.h"
#include "checkpoints.h"
void RenderAtomicTinted(rw::Atomic *atomic, rw::RGBA colour);	// renderer.cpp, alpha blended
#include "startscreen.h"

#ifdef RWHALFPIXEL
#define HALFPX (0.5f)
#else
#define HALFPX (0.0f)
#endif

#include "Pad.h"
#include "camera.h"
#include "collision.h"

void panic(const char *fmt, ...);
void debug(const char *fmt, ...);
void log(const char *fmt, ...);
void SetupStyle(void);
// 0 and 1 are Haumea's own; the rest are palettes (style.cpp).
enum { UI_THEME_DARK, UI_THEME_LIGHT, UI_THEME_FIRST_PALETTE };
enum { UI_COLOR_SUCCESS, UI_COLOR_WARNING, UI_COLOR_ERROR, UI_COLOR_INFO };
int GetUiTheme(void);
void SetUiTheme(int theme);
int NumUiThemes(void);
const char *GetUiThemeName(int theme);	// in the UI's language
bool UiThemeIsLight(void);	// dark text on a light background
rw::RGBA UiThemePreviewBackground(void);	// behind model previews and thumbnails
ImVec4 UiSemanticColor(int color);
void SetupFonts(void);
bool GetEditorRootDirectory(char *dir, size_t size);
bool GetGameRootDirectory(char *dir, size_t size);
bool BuildPath(char *dst, size_t size, const char *dir, const char *name);
bool EnsureParentDirectoriesForPath(const char *path);
bool GetHaumeaDataDirectory(char *dir, size_t size);
bool GetHaumeaDataPath(char *dst, size_t size, const char *name);
FILE *fopenHaumeaDataRead(const char *name, const char *legacyName = nil);
FILE *fopenHaumeaDataWrite(const char *name);
void setHotReloadTracePath(const char *path);
void hotReloadTrace(const char *fmt, ...);
void addToLogWindow(const char *fmt, va_list args);
void OpenLogFile(bool append);	// append: a renderer hand-over's second process
void LoadInitialEditorWindowState(int *width, int *height);
void LoadInitialAntialiasingSettings(void);
// Windows ships haumea.exe (OpenGL) and haumea-d3d9.exe (Direct3D 9); the
// setting says which one runs, and a start where OpenGL failed goes to Direct3D 9.
enum { RENDERER_AUTO, RENDERER_OPENGL, RENDERER_D3D9 };
extern int gRendererChoice;
extern bool gRendererGlFailed;	// OpenGL didn't start on this PC: Automatic uses Direct3D 9
void LoadInitialRendererSettings(void);
bool RendererChoiceAvailable(void);	// both exes are there
void ApplyInitialEditorWindowState(void);
// Startup splash window and its growth into the editor window (GLFW builds).
#define HAUMEA_SPLASH_WIDTH 880
#define HAUMEA_SPLASH_HEIGHT 500
bool BeginSplashWindow(int width, int height);
bool GetInitialEditorWindowRect(int *x, int *y, int *width, int *height, bool *canPosition);
void GetEditorWindowRect(int *x, int *y, int *width, int *height);
void SetEditorWindowRect(int x, int y, int width, int height, bool setPosition);
void SetEditorWindowDecorated(bool decorated);
void ShowEditorWindow(void);
void HideEditorWindow(void);
void SyncEditorWindowSize(void);
void ExpandSplashWindow(void);
void UpdateEditorWindowState(void);
void OnEditorWindowResized(int width, int height);
void SaveEditorSettingsNow(void);
bool RestartHaumea(void);
// Restart with this run's arguments (--agent-socket, --game...), e.g. after an update.
bool RestartHaumeaKeepingArgs(void);
// At startup, before any window: replace this process with a new start of the
// executable on disk, same arguments. Returns only if that failed.
void ExecHaumeaInPlace(void);
bool GetHaumeaExecutablePath(char *path, size_t size);

char *getPath(const char *path);
FILE *fopen_ci(const char *path, const char *mode);
bool doesFileExist(const char *path);
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
template <typename T> T min(T a, T b) { return a < b ? a : b; }
template <typename T> T max(T a, T b) { return a > b ? a : b; }
template <typename T> T clamp(T v, T min, T max) { return v<min ? min : v>max ? max : v; }
template <typename T> T sq(T a) { return a*a; }

void plCapturePad(int arg);
void plUpdatePad(CControllerState *state);

void ConvertTxd(rw::TexDictionary *txd);

extern float timeStep;
extern float avgTimeStep;

#define DEGTORAD(d) (d/180.0f*PI)

struct Ray {
	rw::V3d start;
	rw::V3d dir;
};
inline bool
SphereIntersect(const CSphere &sph, const Ray &ray)
{
	rw::V3d diff = sub(ray.start, sph.center);
	float a = dot(ray.dir,ray.dir);
	float b = 2*dot(ray.dir, diff);
	float c = dot(diff,diff) - sq(sph.radius);

	float discr = sq(b) - 4*a*c;
	return discr > 0.0f;
}
bool IntersectRayTriangle(const Ray &ray, rw::V3d a, rw::V3d b, rw::V3d c, float *t);
bool IntersectRaySphere(const Ray &ray, const CSphere &sphere, float *t);
bool IntersectRayColModel(const Ray &worldRay, ObjectInst *inst, rw::V3d *hitPos);

//
// Options
//

extern bool gRenderOnlyLod;
extern bool gRenderOnlyHD;
extern bool gRenderBackground;
extern bool gRenderWater;
extern bool gRenderPostFX;
extern bool gEnableFog;	// follows gFogMode
// One preview distance for every object; map export properties stay separate.
enum { FOG_GAME, FOG_LIGHT, FOG_OFF };
extern int gFogMode;
extern float gStreamDistance;
extern bool gVSync;
extern int gFrameLimit; // zero is unlimited
extern bool gFocusMyMap;
bool IsUserMapObject(ObjectInst *inst);
extern bool gEnableTimecycleBoxes;
extern bool gUseBlurAmb;
extern bool gOverrideBlurAmb;
extern bool gNoTimeCull;
extern bool gNoAreaCull;
extern bool gDoBackfaceCulling;
extern int gSelectionHighlightOpacity;
extern bool gOutlineMapObjects;
extern bool gOutlineSelection;
extern bool gPlayAnimations;
extern bool gUseViewerCam;
extern bool gDrawTarget;
extern bool gFlyAcceleration;
extern float gFlySpeed;
extern float gFlyFastMul;
extern float gFlySlowMul;
extern float gFovWheelStep;
// Mouse button used for first-person camera look while dragging: 1=LMB,
// 2=MMB, 3=RMB. RMB is the default; stationary clicks keep their tool actions.
extern int gCameraLookButton;
extern bool gCameraLooking;	// the look button is held after a press in the view
void SetLookCursorLocked(bool locked);	// hide and hold the cursor while looking
void SetInstIplFilterKey(ObjectInst *inst, const char *sceneName);

// non-rendering things
extern bool gRenderCollision;
extern bool gRenderBounds;
extern bool gRenderZones;
extern bool gRenderMapZones;
extern bool gRenderNavigZones;
extern bool gRenderInfoZones;
extern bool gRenderCullZones;
extern bool gRenderAttribZones;
extern bool gRenderLegacyPedPaths;
extern bool gRenderLegacyCarPaths;
extern bool gRenderSaPedPaths;
extern bool gRenderSaPedPathWalkers;
extern bool gRenderSaCarPaths;
extern bool gRenderSaCarPathTraffic;
extern bool gRenderSaAreaGrid;
extern bool gRenderLightEffects;
extern bool gRenderEffects;
extern bool gRenderTimecycleBoxes;
extern int gSaPedPathWalkerCount;
extern int gSaCarPathTrafficCount;
extern float gSaCarPathTrafficSpeedScale;
extern bool gSaCarPathTrafficFreezeRoutes;
extern bool gRenderSaCarPathParkedCars;
extern int gSaCarPathParkedCarCount;
extern uint32 gRequestedAASamples;

// SA postfx
extern int  gColourFilter;
extern bool gRadiosity;

// SA building pipe
extern int gBuildingPipeSwitch;
extern float gDayNightBalance;
extern float gWetRoadEffect;

// Neo stuff
extern float gNeoLightMapStrength;


// These don't necessarily match the game's values, roughly double of SA PC
enum {
	MODELNAMELEN = 30,
	NUMOBJECTDEFS = 70001,	// CUSTOM_MODEL_BASE + 30000 custom models
	NUMTEXDICTS = 10000,
	NUMCOLS = 510,
	NUMSCENES = 80,
	NUMIPLS = 512,
	NUMCDIMAGES = 100,
	NUMTCYCBOXES = 64,

	NUMWATERVERTICES = 4000,
	NUMWATERQUADS = 1000,
	NUMWATERTRIS = 1000,
	NUMZONES = 500,	// for each type
};

enum {
	MAX_PAWN_MATERIALS = 16,
	PAWN_TEXTURE_NAME_LEN = 32,
	PAWN_MATERIAL_TEXT_LEN = 256,
	PAWN_MATERIAL_FONT_LEN = 64,
};

struct ObjectMaterialOverride
{
	bool active;
	bool hasTexture;
	int sourceModel;
	int txdSlot;
	char txdName[MODELNAMELEN];
	char textureName[PAWN_TEXTURE_NAME_LEN];
	rw::RGBA color;

	bool textEnabled;
	char text[PAWN_MATERIAL_TEXT_LEN];
	char font[PAWN_MATERIAL_FONT_LEN];
	int fontSize;
	bool bold;
	rw::RGBA textColor;
	rw::RGBA backgroundColor;
	int alignment;
	int materialSize;
};

enum {
	PAWN_OBJECT_NAME_LEN = 32,	// Pawn identifiers are limited to 31 characters
	PAWN_OBJECT_GROUP_LEN = 48,
	PAWN_OBJECT_COMMENT_LEN = 128,
};

// What a placed instance stands for in the Pawn map. Objects and vehicles
// are what they look like; the others are SA-MP elements drawn with a stand-in
// model (labels, zones, icons) or a skin (actors, player classes).
enum SampElementKind {
	SAMP_ELEMENT_OBJECT,
	SAMP_ELEMENT_PICKUP,	// CreatePickup / CreateDynamicPickup
	SAMP_ELEMENT_LABEL,	// Create3DTextLabel / CreateDynamic3DTextLabel
	SAMP_ELEMENT_ACTOR,	// CreateActor / CreateDynamicActor
	SAMP_ELEMENT_CLASS,	// AddPlayerClass(Ex): a spawn point
	SAMP_ELEMENT_GANGZONE,	// GangZoneCreate
	SAMP_ELEMENT_MAPICON,	// SetPlayerMapIcon / CreateDynamicMapIcon
	SAMP_ELEMENT_NPC,	// NPC_Create + NPC_Spawn (open.mp only)
	SAMP_ELEMENT_PATHPOINT,	// NPC_AddPointToPath: one point of an NPC path (open.mp only)
	SAMP_ELEMENT_SOUND,	// PlayAudioStreamForPlayer / PlayerPlaySound near a place
	SAMP_ELEMENT_CHECKPOINT,	// CreateDynamicCP / CreateDynamicRaceCP (streamer only)
	SAMP_ELEMENT_AREA,	// CreateDynamicCircle .. CreateDynamicPolygon (streamer only)
	NUM_SAMP_ELEMENT_KINDS
};

// ObjectPawnProperties::npcPathRepeat
enum { NPC_PATH_ONCE, NPC_PATH_LOOP, NPC_PATH_BACK_AND_FORTH };

enum {
	PAWN_LABEL_TEXT_LEN = 1024,	// Create3DTextLabel's limit
	PAWN_CLASS_WEAPONS = 3,
	PAWN_NPC_NAME_LEN = 25,	// MAX_PLAYER_NAME + 1
	PAWN_NPC_RECORDING_LEN = 64,
	PAWN_ANIM_NAME_LEN = 36,
	PAWN_SOUND_URL_LEN = 256,
	PAWN_AREA_POINTS = 32,	// polygon corners of a dynamic area
};

// A dynamic area's shape (ObjectPawnProperties::areaShape), around its position.
enum {
	SAMP_AREA_CIRCLE,	// CreateDynamicCircle: areaRadius, every height
	SAMP_AREA_CYLINDER,	// CreateDynamicCylinder: areaRadius, areaHeight up from the position
	SAMP_AREA_SPHERE,	// CreateDynamicSphere: areaRadius
	SAMP_AREA_RECTANGLE,	// CreateDynamicRectangle: areaSize x and y, every height
	SAMP_AREA_CUBOID,	// CreateDynamicCuboid: areaSize, z up from the position
	SAMP_AREA_CUBE,	// CreateDynamicCube: the same box, the other name
	SAMP_AREA_POLYGON,	// CreateDynamicPolygon: areaPoints (offsets), areaHeight up (0: every height)
	NUM_SAMP_AREA_SHAPES
};

// Checkpoints: a plain one, or one of a race that points at the next.
enum {
	SAMP_CHECKPOINT_NORMAL,	// CreateDynamicCP
	SAMP_CHECKPOINT_RACE,	// CreateDynamicRaceCP
};
// A race: from first to last, or round and round (cpRaceMode). The race
// checkpoint types after the client's 0-8: Normal until the end, then Finish
// (as haumea_races.inc does).
enum { SAMP_RACE_POINT_TO_POINT, SAMP_RACE_CIRCUIT };
enum { SAMP_RACE_CP_AUTO_GROUND = 9, SAMP_RACE_CP_AUTO_AIR = 10, SAMP_RACE_MAX_LAPS = 999 };

// How a gang zone is shown (ObjectPawnProperties::zoneMode).
enum {
	SAMP_ZONE_GLOBAL,	// GangZoneCreate, shown in _ShowForPlayer (1024 in all)
	SAMP_ZONE_PLAYER,	// CreatePlayerGangZone for every player (open.mp only, 1024 a player)
	SAMP_ZONE_STREAMED,	// per-player zones near the player, by streamer areas (open.mp only)
	NUM_SAMP_ZONE_MODES
};

// What a sound place plays.
enum {
	SAMP_SOUND_STREAM,	// PlayAudioStreamForPlayer, started in _ShowForPlayer
	SAMP_SOUND_GAME,	// PlayerPlaySound when a player walks in
};

// SA-MP object settings besides transform and materials. The Pawn map is the
// document, so every field round-trips through it: the name is the object's
// variable, distances/world are call arguments, the attachment is an
// AttachObjectToObject call, and group/comment live in a trailing comment.
struct ObjectPawnProperties
{
	char name[PAWN_OBJECT_NAME_LEN];	// empty: automatic handle when needed
	char group[PAWN_OBJECT_GROUP_LEN];
	char comment[PAWN_OBJECT_COMMENT_LEN];
	float drawDistance;	// CreateObject DrawDistance / streamer drawdistance; 0 = default
	float streamDistance;	// streamer streamdistance; 0 = STREAMER_OBJECT_SD
	int virtualWorld;	// streamer worldid; -1 = every world
	bool forceWorldReplacement;	// explicitly remove/recreate a GTA original, even at its original pose
	int32 attachParent;	// ObjectInst::m_id of the parent, or -1
	rw::V3d attachOffset;	// in the parent's frame
	rw::V3d attachRotation;	// degrees, SA-MP Euler order
	bool attachSyncRotation;
	int attachBone;	// SA-MP player bone ID (1-18), or -1 for object/vehicle attachment
	int attachSlot;	// SetPlayerAttachedObject slot (0-9)
	rw::V3d attachScale;	// SetPlayerAttachedObject scale
	uint32 attachColor1, attachColor2;	// SetPlayerAttachedObject material colours (ARGB)
	int attachSkin;	// preview skin for the generic player target
	// MoveObject / MoveDynamicObject once the object is made: it glides by
	// moveOffset (world axes, from where it stands, so it follows the object)
	// at moveSpeed m/s and, with moveRotates, turns to moveRotation.
	bool moveEnabled;
	rw::V3d moveOffset;
	rw::V3d moveRotation;	// degrees, SA-MP Euler order
	bool moveRotates;
	float moveSpeed;	// metres a second

	// SA-MP element settings; elementKind is SAMP_ELEMENT_OBJECT for objects
	// and vehicles. The world, draw and stream distances above apply too.
	int elementKind;
	int pickupType;
	char labelText[PAWN_LABEL_TEXT_LEN];
	uint32 labelColor;	// Pawn order, 0xRRGGBBAA
	bool labelTestLOS;
	int elementModel;	// pickup model, or actor/class skin (drawn as skin 7 when it has no model, e.g. CJ)
	bool actorInvulnerable;
	float actorHealth;
	int classTeam;	// -1: AddPlayerClass without a team
	int classWeapons[PAWN_CLASS_WEAPONS];
	int classAmmo[PAWN_CLASS_WEAPONS];
	float zoneSizeX, zoneSizeY;	// gang zone extent around the position
	uint32 zoneColor;	// 0xRRGGBBAA, used by GangZoneShowForPlayer
	int zoneMode;	// SAMP_ZONE_GLOBAL .. SAMP_ZONE_STREAMED
	int mapIconType;
	uint32 mapIconColor;	// 0xRRGGBBAA
	int mapIconStyle;	// MAPICON_LOCAL .. MAPICON_GLOBAL_CHECKPOINT
	// NPCs also use elementModel (skin), actorHealth and actorInvulnerable.
	char npcName[PAWN_NPC_NAME_LEN];	// the player name NPC_Create gives it
	float npcArmour;
	int npcWeapon;
	int npcAmmo;
	// A path point's path, or the path an NPC walks (NPC_MoveByPath); the
	// path's Pawn variable, empty for none.
	char pathName[PAWN_OBJECT_NAME_LEN];
	int pathOrder;	// points are added to the path in this order
	float pathStopRange;	// NPC_AddPointToPath stopRange
	int npcMoveType;	// NPC_MOVE_TYPE_WALK .. NPC_MOVE_TYPE_AUTO
	float npcMoveSpeed;	// < 0: NPC_MOVE_SPEED_AUTO
	bool npcPathReversed;
	// What an NPC does at its path's end: NPC_PATH_ONCE stops, LOOP walks it
	// again, BACK_AND_FORTH walks it back (a generated OnNPCFinishMovePath helper).
	int npcPathRepeat;
	// NPC_StartPlayback: a recording in npcmodes/recordings, without .rec.
	// Playing one stops any path movement, so an NPC does one or the other.
	char npcRecording[PAWN_NPC_RECORDING_LEN];
	bool npcRecordingAtNpc;	// start the route where the NPC stands
	// NPC_PutInVehicle: the vehicle it sits in (ObjectInst::m_id, -1 on foot)
	// and the seat (0 driver, 1-3 passengers). A driver recording needs one.
	int32 npcVehicle;
	int npcSeat;
	// NPC_PlayNode: walk the game's path nodes (NODES<npcNode>.DAT) from
	// point npcNodePoint, with npcMoveType/npcMoveSpeed.
	bool npcPlaysNode;
	int npcNode;	// 0-63
	int npcNodePoint;
	float npcNodeRadius;
	bool npcNodeSetAngle;
	// ApplyActorAnimation / NPC_ApplyAnimation for actors and NPCs; no
	// animation while animName is empty.
	char animLib[PAWN_ANIM_NAME_LEN];
	char animName[PAWN_ANIM_NAME_LEN];
	float animDelta;
	bool animLoop, animLockX, animLockY, animFreeze;
	int animTime;	// ms, 0 = until stopped
	// Sound places: an audio stream heard within soundRadius of the place
	// (when soundUsePos), or a game sound played to a player walking in.
	int soundKind;	// SAMP_SOUND_STREAM / SAMP_SOUND_GAME
	char soundUrl[PAWN_SOUND_URL_LEN];
	int soundId;	// PlayerPlaySound soundid
	float soundRadius;
	bool soundUsePos;	// streams: off plays it everywhere at full volume
	bool soundStopOnLeave;	// game sounds: play sound 0 (stop) when the player walks out
	// Checkpoints (streamer only). A race checkpoint points at the next one
	// of its race (cpRace, in cpOrder); the last points nowhere.
	int cpKind;	// SAMP_CHECKPOINT_NORMAL / SAMP_CHECKPOINT_RACE
	float cpSize;
	int cpRaceType;	// CreateDynamicRaceCP type: 0 normal .. 8
	char cpRace[PAWN_OBJECT_NAME_LEN];
	int cpOrder;
	// The whole race's settings, the same on each of its checkpoints
	// (SetRaceSettings). Playable races are saved for haumea_races.inc
	// (MapRace_Create), one player at a time, instead of as fixed markers.
	int cpRaceMode;	// SAMP_RACE_POINT_TO_POINT / SAMP_RACE_CIRCUIT
	int cpRaceLaps;	// circuits: 1 to SAMP_RACE_MAX_LAPS
	bool cpRacePlayable;
	// Dynamic areas (streamer only). The shape sits around the position;
	// a rectangle or cuboid is areaSize wide, centred in x and y, its z
	// going up from the position.
	int areaShape;	// SAMP_AREA_CIRCLE ..
	float areaRadius;
	rw::V3d areaSize;
	float areaHeight;
	int areaPointCount;
	float areaPoints[PAWN_AREA_POINTS][2];	// polygon corners, x and y from the position
	// Any streamer item: shown only to players inside this area (its Pawn
	// variable), the streamer's areaid. Empty for everywhere.
	char areaName[PAWN_OBJECT_NAME_LEN];
	// Streamer items in interior 0 (m_area): written as interiorid 0, only in
	// interior 0, instead of -1, every interior. Set when a map said 0.
	bool interiorZeroOnly;
};

#define LODDISTANCE (300.0f)

// SA-MP exposes a much larger default editing world than GTA SA's native
// water.dat coordinate range. Keep both limits explicit so editor-only
// workspace visuals do not get confused with game file limits.
static const int SA_MP_WORLD_BOUND = 20000;
static const int SA_GAME_WATER_BOUND = 3000;

#include "Rect.h"
#include "PtrNode.h"
#include "PtrList.h"

struct CRGBA
{
	uint8 r, g, b, a;
};

#include "timecycle.h"
#include "Sprite.h"

namespace Zones
{
void CreateZone(const char *name, int type, CBox box, int level, const char *text);
void Render(void);
void AddAttribZone(CBox box, int flags, int wantedLevelDrop);
void AddAttribZone(rw::V3d pos, float s1x, float s1y,
	float s2x, float s2y, float zmin, float zmax, int flags);
void AddMirrorAttribZone(rw::V3d pos, float s1x, float s1y,
	float s2x, float s2y, float zmin, float zmax,
	int flags, rw::Plane mirror);
void RenderAttribZones(void);
void RenderCullZones(void);
// SA's navigation zones (info.zon): the smallest one around a point, or -1,
// and a zone's GXT key (several zones share one name).
int FindNavigZone(rw::V3d p);
const char *GetNavigZoneText(int zone);
// The map zone (map.zon) level at a point: 1 Los Santos, 2 San Fierro,
// 3 Las Venturas, 0 elsewhere.
int GetMapZoneLevel(rw::V3d p);
}

// Game

enum eGame
{
	GAME_NA,
	GAME_III,
	GAME_VC,
	GAME_SA,
	GAME_LCS,
	GAME_VCS
};
using rw::PLATFORM_NULL;
using rw::PLATFORM_PS2;
using rw::PLATFORM_XBOX;
enum {
	PLATFORM_PC = rw::PLATFORM_D3D8,
	PLATFORM_PSP = 10
};
extern int gameversion;
extern int gameplatform;
inline bool isIII(void) { return gameversion == GAME_III; }
inline bool isVC(void) { return gameversion == GAME_VC; }
inline bool isSA(void) { return gameversion == GAME_SA; }

struct WeatherInfo;

struct Params
{
	int map;
	rw::V3d initcampos;
	rw::V3d initcamtarg;
	int numAreas;
	const char **areaNames;

	int objFlagset;

	int timecycle;
	int numHours;
	int numWeathers;
	int extraColours;	// weather ID where extra colours start
	int numExtraColours;	// number of extra colour blocks
	WeatherInfo *weatherInfo;
	int background;
	int daynightPipe;

	int water;
	const char *waterTex;
	rw::V2d waterStart, waterEnd;	// waterpro

	int alphaRefDefault;	// preset value
	int alphaRef;		// the regular one we want to use for rendering
	bool ps2AlphaTest;	// emulate PS2 alpha test

	bool backfaceCull;
	bool txdFallbackGeneric;

	int neoWorldPipe;

	int leedsPipe;

	bool checkColModels;
	int maxNumColSpheres;
	int maxNumColBoxes;
	int maxNumColTriangles;
};
extern Params params;

extern int gameTxdSlot;

extern int currentHour, currentMinute;
extern int extraColours;
extern int currentArea;

struct WeatherInfo
{
	const char *name;
	int flags;
};

namespace Weather
{
enum {
	Sunny = 0x01,
	Foggy = 0x02,
	Extrasunny = 0x04,
};

extern int oldWeather, newWeather;
extern float interpolation;
extern float cloudCoverage;
extern float foggyness;
extern float extraSunnyness;

void Update(void);
};

// TODO, this is a stub
struct GameFile
{
	char *name;
	char *sourcePath;  // physical disk path for I/O, or nil for base game files
	int8 custom;	// a map's file (pawnPathLooksCustom), -1 until asked
	// types of files we want to have eventually:
	//	IDE Definition
	//	IPL Scene
	//	COL ?
	//	IMG ?
};
GameFile *NewGameFile(char *path);

enum SaveDestination
{
	SAVE_DESTINATION_ORIGINAL_FILES = 0,
	SAVE_DESTINATION_MODLOADER,
};
extern SaveDestination gSaveDestination;

bool IsHourInRange(int h1, int h2);
void FindVersion(void);
void LoadGame(void);
void Idle(void);
void DefinedState(void);

// Gizmo
enum GizmoMode {
	GIZMO_TRANSLATE,
	GIZMO_ROTATE
};
enum GizmoSpace {
	GIZMO_LOCAL,
	GIZMO_WORLD
};
enum GizmoPivot { GIZMO_PIVOT_ACTIVE, GIZMO_PIVOT_CENTRE, GIZMO_PIVOT_INDIVIDUAL, GIZMO_PIVOT_CUSTOM };
extern int gGizmoPivot;
extern rw::V3d gGizmoCustomPivot;
void ComputeGizmoRotation(const rw::V3d &position, const rw::Quat &rotation,
	const rw::Quat &leaderBefore, const rw::Quat &leaderAfter, const rw::V3d &pivot,
	bool individual, bool local, rw::V3d *newPosition, rw::Quat *newRotation);
extern int gGizmoMode;
extern int gGizmoSpace;
extern bool gGizmoEnabled;
extern bool gGizmoHovered;
extern bool gGizmoUsing;
extern bool gPlaceSnapToObjects;
extern bool gPlaceSnapToGround;
// Object snap for moving objs from startPositions by delta (selection-wide).
bool FindSelectionSnapDelta(const std::vector<ObjectInst*> &objs, const std::vector<rw::V3d> &startPositions,
	const rw::V3d &delta, rw::V3d *outDelta);
extern bool gDragFollowGround;
extern bool gDragAlignToSurface;

// Brush tool (paint objects onto surfaces)
extern bool gBrushMode;
extern float gBrushZOffset;         // signed; added to surface Z (negative sinks object into ground)
extern bool gBrushAlignToSurface;   // rotate placed objects to match surface normal
extern float gBrushYawMin;          // degrees; yaw applied is uniform random in [min,max]
extern float gBrushYawMax;          // degrees; min==max means no randomization (fixed angle)
extern float gBrushSpacing;         // min world-space distance between drag-painted placements (meters)
extern float gBrushRadius;          // brush disc radius (m); 0 = single placement at hit
extern int gBrushCount;             // number of objects per click (scatter count when radius > 0)
extern float gBrushDelayMs;         // min time between drag-paint bursts (ms); 0 = disabled
rw::Quat BuildGroundAlignedRotationFromRotation(const rw::Quat &sourceRotation, rw::V3d groundNormal);
bool GetBrushSurfaceHit(rw::V3d *hitPos, rw::V3d *hitNormal);
void EnterBrushMode(int objectId);
void ExitBrushMode(void);
extern bool gGizmoSnap;
extern float gGizmoSnapAngle;
extern float gGizmoSnapTranslate;
extern bool gObjectSnapMode;
extern float gObjectSnapDistance;

// Rect-select (marquee selection)
extern bool gRectSelectActive;	// true when LMB is consumed by rect-select (blocks camera)

// Practical cap for expensive selection operations. The selection list itself
// is uncapped, but operations that allocate temporary buffers stop here.
#define MAX_BATCH_OBJECTS 20000

// Undo/Redo
enum UndoType {
	UNDO_MOVE,
	UNDO_ROTATE,
	UNDO_DELETE,
	UNDO_PASTE,
	UNDO_TRANSFORM_BATCH,
	UNDO_MATERIAL,
	UNDO_PROPERTIES,
	UNDO_WORLD_CONVERSION,
	UNDO_VEHICLE,
	UNDO_MODEL_ID,
	UNDO_NODES,
	UNDO_RADAR_ART,	// the radar art layers (radarart.cpp)
	UNDO_LAYOUT_PATHS,
};

enum UndoTransformFlags {
	UNDO_TRANSFORM_POS = 1,
	UNDO_TRANSFORM_ROT = 2,
};

struct UndoTransform {
	ObjectInst *inst;
	bool oldDirty;
	rw::V3d oldPos;
	rw::V3d newPos;
	rw::Quat oldRot;
	rw::Quat newRot;
	uint8 flags;
};

// A vehicle's own settings, for undo.
struct VehicleState
{
	bool isStatic;
	int color1, color2;
	int respawnDelay;
	int paintjob;
	char plate[VEHICLE_PLATE_LEN];
	uint16 mods[VEHICLE_MOD_SLOTS];
};
VehicleState GetVehicleState(const struct ObjectInst *inst);
void SetVehicleState(struct ObjectInst *inst, const VehicleState &state);

struct UndoAction {
	int type;
	int group;	// non-zero: undone and redone together with its neighbours
	// For MOVE/ROTATE: single instance
	ObjectInst *inst;
	rw::V3d oldPos;
	rw::Quat oldRot;
	rw::V3d newPos;
	rw::Quat newRot;
	// For MOVE: LOD that was also moved
	ObjectInst *lodInst;
	rw::V3d lodOldPos;
	rw::V3d lodNewPos;
	// For DELETE: list of deleted instances (inst + LOD cascade)
	std::vector<ObjectInst*> deletedInsts;
	int numDeleted;
	// For PASTE: list of pasted instances (to delete on undo)
	std::vector<ObjectInst*> pastedInsts;
	int numPasted;
	// For batch transform actions (snap to ground, etc.)
	std::vector<UndoTransform> transforms;
	int numTransforms;
	// For one material-slot edit
	int materialSlot;
	ObjectMaterialOverride oldMaterial;
	ObjectMaterialOverride newMaterial;
	// For one object-properties edit
	ObjectPawnProperties oldProperties;
	ObjectPawnProperties newProperties;
	std::vector<ObjectInst*> convertedWorldObjects;
	// For a vehicle edit (colours, plate, paintjob, parts)
	VehicleState oldVehicle;
	VehicleState newVehicle;
	int oldModelId, newModelId;
	// For a Nodes tool edit: the areas it changed (SAPaths' own copies)
	std::vector<int> nodeAreas;
	std::vector<std::shared_ptr<void>> nodesBefore;
	std::vector<std::shared_ptr<void>> nodesAfter;
	// For radar art: the layers before and after
	std::shared_ptr<void> radarBefore, radarAfter;
};

void UndoRecordMove(ObjectInst *inst, rw::V3d oldPos, ObjectInst *lodInst, rw::V3d lodOldPos);
void UndoRecordRotate(ObjectInst *inst, rw::Quat oldRot);
void UndoRecordDelete(ObjectInst **insts, int num);
void UndoRecordPaste(ObjectInst **insts, int num);
void UndoRecordTransformBatch(UndoTransform *transforms, int num);
void UndoRecordMaterial(ObjectInst *inst, int slot, const ObjectMaterialOverride &oldMaterial);
void UndoRecordProperties(ObjectInst *inst, const ObjectPawnProperties &oldProperties);
void UndoRecordVehicle(ObjectInst *inst, const VehicleState &before);	// after is the vehicle now
void UndoRecordModelId(ObjectInst *inst, int oldModelId);
int GetUndoPosition(void);	// undo steps recorded so far, for tests
bool MapHasUndoHistory(void);	// any step to undo or redo since the map was opened
void UndoRecordNodes(const std::vector<int> &areas, const std::vector<std::shared_ptr<void>> &before,
	const std::vector<std::shared_ptr<void>> &after);
void UndoRecordRadarArt(const std::shared_ptr<void> &before, const std::shared_ptr<void> &after);
void UndoRecordLayoutPaths(const std::shared_ptr<void> &before, const std::shared_ptr<void> &after);
void SetLayoutObjectState(ObjectInst *inst, const rw::V3d &position, const rw::Quat &rotation, bool deleted);
void Undo(void);
void Redo(void);
// Actions recorded between these are undone as one step (they nest).
void UndoBeginGroup(void);
void UndoEndGroup(void);
void ResetUndoHistory(void);
void UndoForgetInstance(ObjectInst *inst);	// someone else changed it (multiplayer)
void UndoForgetKind(int type);	// someone else replaced that list (UNDO_RADAR_ART...)

rw::V3d GetObjectRotationDegrees(const rw::Quat &rotation);
// An instance's rotation in degrees: the map file's own numbers while the
// rotation is unchanged, else GetObjectRotationDegrees.
rw::V3d GetInstanceRotationDegrees(const ObjectInst *inst);
bool InstanceUsesSourceDegrees(const ObjectInst *inst);
// Stored (conjugated) ObjectInst rotation for a rendered basis matrix.
rw::Quat QuatFromMatrix(const rw::Matrix &matrix);
rw::Quat MakeObjectRotationDegrees(const rw::V3d &degrees);
rw::Quat ApplyObjectRotationDelta(const rw::Quat &startRotation,
	const rw::V3d &degrees, bool worldSpace);
bool CaptureObjectTransformTargets(ObjectInst *leader, bool includeSelection,
	std::vector<UndoTransform> &transforms, bool *capped = nil);
void PreviewObjectTransformTargets(ObjectInst *leader,
	std::vector<UndoTransform> &transforms, const rw::V3d &leaderPos,
	const rw::Quat &leaderRot, uint8 requestedFlags);
int CommitObjectTransformTargets(std::vector<UndoTransform> &transforms);

// Copy/Paste
void CopySelected(void);
void CutSelected(void);
int PasteClipboard(void);
int PasteClipboardInPlace(void);
// A copy of the selection where it is, leaving the clipboard alone.
int DuplicateSelected(void);
// Object Info's "Edit all selected": edits go to every selected element that
// has the field (materials: the same slot), as one undo step.
bool GetBatchEditSelection(void);
void SetBatchEditSelection(bool on);
// Material slots of an instance's model (0 before it is loaded).
int GetInstanceMaterialCount(ObjectInst *inst);
// Hide and isolate, editor-only: hidden instances are not drawn or
// clickable in the viewport and nothing about them is saved.
bool IsInstanceHidden(ObjectInst *inst);
void SetInstanceHidden(ObjectInst *inst, bool hidden);
int HideSelected(void);
int IsolateSelected(void);	// hides everything that is not selected
int ShowAllInstances(void);
int CountHiddenInstances(void);
// Points the camera at the selection and moves back until it fits.
void FocusSelection(void);
// UI scale, from Preferences, Ctrl+wheel and the shortcut keys; saved.
void SetUiScale(float scale);
float GetUiScale(void);
void StepUiScale(float steps);

// Prefabs
int ExportPrefab(const char *path);
// Where a saved prefab is held when placed.
enum { PREFAB_ORIGIN_CENTRE, PREFAB_ORIGIN_FIRST, PREFAB_ORIGIN_LOWEST };
int ExportPrefabEx(const char *path, int origin, const char *tags);	// tags: comma-separated, or nil
int ImportPrefab(const char *path);
int ImportPrefabAt(const char *path, rw::V3d spawnPos);
// Turned heading degrees about Z; group (or nil) is set on everything placed.
int ImportPrefabAtEx(const char *path, rw::V3d spawnPos, float heading, const char *group);
struct PrefabInfo
{
	std::string tags;
	int instances = 0, elements = 0, vehicles = 0;
	long long mtime = 0;
};
bool ReadPrefabInfo(const char *path, PrefabInfo *info);
// The prefab library: Mine (the config folder), Workspace, Starter (shipped
// with the editor) and the old game-folder location, with subfolders.
struct PrefabListing
{
	std::string path, name;
	std::string folder;	// "Mine", "Mine/Roads", "Starter", ...
	PrefabInfo info;
};
void ListPrefabs(std::vector<PrefabListing> &out);
// Where Save Prefab writes: Mine or the workspace's prefabs/, plus a subfolder.
std::string PrefabSaveDir(bool workspace, const char *folder);
extern float gPrefabPlaceHeading;	// degrees, turned with Q/E or the wheel while placing
extern bool gPrefabPlaceRepeat, gPrefabPlaceAsGroup;
extern bool gPrefabPlaceMode;
void EnterPrefabPlaceMode(const char *path);
void ExitPrefabPlaceMode(void);
const char *GetPrefabPlacePath(void);
void RenderPrefabPlacementGhost(const char *path, rw::V3d spawnPos, float heading);
int ExportSelectedDffs(const char *dir, int *numFailed);
int ExportSelectedTxds(const char *dir, int *numFailed);

// Pawn object code
enum PawnCodeFormat {
	PAWN_CODE_NATIVE,
	PAWN_CODE_STREAMER,
};
int ExportPawnCode(const char *path, PawnCodeFormat format, bool selectedOnly,
	int *numSkipped = nil);
int BuildPawnCode(const char *pathHint, PawnCodeFormat format, bool selectedOnly,
	std::string &code, int *numSkipped = nil);
int ImportPawnCode(const char *path, int *numSkipped = nil);
// keepRotationNumbers: the source is a map's own Pawn, whose rx, ry, rz are
// written back as they are while unchanged (not for converted maps).
int ImportPawnSource(const std::string &source, const char *path, int *numSkipped = nil,
	bool keepRotationNumbers = false);
// Texture Studio maps (SavedMaps/*.db), converted to Pawn calls and imported.
bool BuildTextureStudioPawn(const char *path, std::string &source, std::string &error);
int ImportTextureStudioMap(const char *path, int *numSkipped = nil);
// What the last ImportPawnCode call did, for explaining skipped content.
struct PawnImportReport
{
	int placed;
	int removed;
	int unreadableCalls;	// object calls whose arguments could not be read
	int unknownModelObjects;	// objects using a model the editor has not loaded
	std::vector<int> unknownModels;
	int unmatchedRemovals;	// RemoveBuildingForPlayer with no matching world object
	int unresolvedLinks;	// material/attach/interior calls on unknown handles
	std::vector<std::string> notes;	// what another tool's map had that SA-MP can't show
};
void AddPawnImportNote(const std::string &note);
// How SA-MP 0.3.7 resolves a slot's SetObjectMaterial: what it shows, or why
// the picked texture won't show. Model -1 keeps the texture (colour only),
// model 0 is SA-MP's blanktex whatever the names; otherwise the model's TXD
// is loaded and the texture is looked up by TXD name.
enum MaterialTextureCheck {
	MATERIAL_TEXTURE_OK,
	MATERIAL_TEXTURE_NOTE,	// shows only if another model already loaded the TXD
	MATERIAL_TEXTURE_WARNING,	// the texture won't show
	MATERIAL_TEXTURE_ERROR,	// the client ignores the whole call
};
int CheckMaterialTexture(const ObjectMaterialOverride &material, char *buf, size_t size,
	bool translated = false);	// true: in the UI's language
// MTA:SA .map and IPL text maps (mapimport.cpp), opened like Texture Studio's .db.
bool IsForeignMapFile(const char *path);
int ImportForeignMap(const char *path, int *numSkipped = nil);
bool LastForeignMapUsesStreamer(void);
const PawnImportReport &GetLastPawnImportReport(void);
void ClearLastPawnImportReport(void);
bool IsPawnWorldObject(ObjectInst *inst);
extern bool gPawnEditMode;
extern bool gPawnWorldDeleteMode;
bool IsPawnObjectSelectable(ObjectInst *inst);
bool IsPawnObjectEditable(ObjectInst *inst);
bool CanChangeInstanceModelId(ObjectInst *inst);
bool ChangeInstanceModelId(ObjectInst *inst, int editorModelId, bool recordUndo = true);
bool PawnObjectNeedsExport(const ObjectInst *inst);
int ConvertWorldObjectsToMapObjects(const std::vector<ObjectInst*> &objects);
void SetPawnEditMode(bool enabled);
void SetPawnWorldDeleteMode(bool enabled);
int SelectAllPawnObjects(void);
int SelectAllWorldObjectsForDeletion(void);

ObjectMaterialOverride *GetObjectMaterialOverride(ObjectInst *inst, int slot);
const ObjectMaterialOverride *GetObjectMaterialOverride(const ObjectInst *inst, int slot);
bool HasObjectMaterialOverrides(const ObjectInst *inst);
void InitObjectMaterialOverride(ObjectMaterialOverride *material, int sourceModel = 0);
void SetObjectMaterialOverride(ObjectInst *inst, int slot,
	const ObjectMaterialOverride &material, bool recordUndo = true);
void ClearObjectMaterialOverride(ObjectInst *inst, int slot, bool recordUndo = true);
void PreviewObjectMaterialOverride(ObjectInst *inst, int slot, const ObjectMaterialOverride &material);
// --ui-script <file> (uiscript.cpp): scripted input for UI tests
void UiScriptTick(void);
void UiScriptAfterRender(void);
void UiScriptNoteToast(const char *text);
bool UiScriptActive(void);
// Real input events are dropped while a UI script drives the editor.
bool UiScriptDropsEvent(sk::Event e);
void UiScriptRestateMouse(void);	// d3d9: the script's pointer over the backend's
// Texture Browser (texbrowser.cpp)
void DrawTextureBrowserWindow(bool *open);
// Help > About > Licenses (licenses.cpp)
void DrawLicensesWindow(bool *open);
struct TextureSearchResult { int model; std::string txd, texture; bool samp, custom; int tstudioIndex;
	int tiles; };	// tiles: 0 not, 1 left to right, 2 top to bottom, 3 both
int SearchTextures(const char *query, int limit, std::vector<TextureSearchResult> &out);	// indexes everything first
// Texture Studio's texture list (tstudio.cpp): Texture Studio maps store
// textures as indices into it, so its order matters. The workspace's own
// alltextures.pwn when the server has Texture Studio, else the bundled copy;
// *source says which. Index 0 is "no texture".
struct TStudioTexture { int model; const char *txd, *texture; };
const TStudioTexture *TStudioTextureList(int *count, std::string *source = nil);
void InitObjectPawnProperties(ObjectPawnProperties *properties);
// Never nil: instances without their own properties share the defaults.
const ObjectPawnProperties *GetObjectPawnProperties(const ObjectInst *inst);
bool ObjectPawnPropertiesAreDefault(const ObjectPawnProperties &properties);
void SetObjectPawnProperties(ObjectInst *inst, const ObjectPawnProperties &properties,
	bool recordUndo = true);
bool IsValidPawnObjectName(const char *name);
bool IsValidNpcName(const char *name);
ObjectInst *FindNpcByName(const char *name, const ObjectInst *except = nil);
void MakeUniqueNpcName(const char *base, char *out, size_t outSize);
// Dynamic areas (streamer only): the area with that Pawn variable, a free
// area variable name, and the kinds that can be limited to an area (areaid).
ObjectInst *FindAreaByName(const char *name);
void MakeUniqueAreaName(const char *base, char *out, size_t outSize);
bool SampElementTakesArea(int kind);
void ApplyInstanceAnimation(ObjectInst *inst);	// from its actor/NPC properties
ObjectInst *FindObjectByPawnName(const char *name, const ObjectInst *except = nil);
// Attaches keeping the child's current world transform; false on a cycle.
bool AttachObjectInstance(ObjectInst *child, ObjectInst *parent, bool syncRotation = true,
	int playerBone = 2, const rw::V3d *playerScale = nil, bool recordUndo = true);
void DetachObjectInstance(ObjectInst *child);
ObjectInst *GetAttachParent(const ObjectInst *inst);
bool BeginAttachmentTargetPick(void);
bool AttachmentTargetPickActive(void);
void CancelAttachmentTargetPick(void);
int CompleteAttachmentTargetPick(ObjectInst *target);
void GetAttachmentTargetPickOptions(int *bone, rw::V3d *scale);
void SetAttachmentTargetPickOptions(int bone, const rw::V3d &scale);
bool GetAttachmentPlayerBoneTransform(const ObjectInst *parent, int bone, rw::Matrix *matrix);
bool RebaseObjectAttachment(ObjectInst *child);
enum { PAWN_ATTACH_PLAYER = -2, PAWN_ATTACH_NONE = -1 };
bool IsPlayerAttachmentTarget(const ObjectInst *inst);
bool IsPlayerAttachedObject(const ObjectInst *inst);
bool AttachObjectToPlayer(ObjectInst *child, int previewSkin = 1, bool recordUndo = true);
bool IsAttachmentWorkspaceActive(void);
bool AttachmentWorkspaceHidesWorld(void);
bool AttachmentWorkspaceShowsInstance(const ObjectInst *inst);
enum AttachmentWorkspaceCameraView {
	ATTACHMENT_VIEW_FOCUS,
	ATTACHMENT_VIEW_FRONT,
	ATTACHMENT_VIEW_BACK,
	ATTACHMENT_VIEW_LEFT,
	ATTACHMENT_VIEW_RIGHT,
	ATTACHMENT_VIEW_TOP
};
void OpenAttachmentWorkspace(ObjectInst *target, bool voidView);
void OpenPlayerAttachmentWorkspace(int skin, bool voidView);
void CloseAttachmentWorkspace(void);
void SetAttachmentWorkspaceVoid(bool voidView);
void SetAttachmentWorkspaceCameraView(int view);
void UpdateAttachmentWorkspace(void);
void SetAttachmentWorkspacePlayerSkin(int skin);
int GetAttachmentWorkspacePlayerSkin(void);
ObjectInst *GetAttachmentWorkspacePlayerPreview(void);
ObjectInst *GetAttachmentWorkspaceTarget(void);
// The vehicle an NPC sits in (npcVehicle), or nil on foot or when it was deleted.
ObjectInst *GetNpcVehicle(const ObjectInst *npc);
// Once per frame, after recordings move vehicles: seats NPCs in their vehicles.
void UpdateNpcVehicleSeats(void);
// Moves attached objects with their parents; called once per frame.
void UpdateObjectAttachments(void);
// Generated SA-MP text texture for a slot (cached; owned by the cache).
rw::Texture *GetObjectMaterialTextPreview(const ObjectMaterialOverride &material);
// Re-renders every material text texture, e.g. after the font list changed.
void RefreshMaterialTextTextures(void);
// Show material indexes: the selected objects show each slot's number, white
// on green; nothing is saved. UpdateMaterialIndexView runs every frame.
extern bool gShowMaterialIndexes;
void UpdateMaterialIndexView(void);

// Toast notifications
enum ToastCategory {
	TOAST_UNDO_REDO,
	TOAST_DELETE,
	TOAST_COPY_PASTE,
	TOAST_SAVE,
	TOAST_SELECTION,
	TOAST_SPAWN,
	TOAST_NUM_CATEGORIES
};
void Toast(ToastCategory cat, const char *fmt, ...);
void ToastFor(ToastCategory cat, float duration, const char *fmt, ...);
// A small rounded pill for what only some servers have ("open.mp only",
// "streamer only", "0.3.DL"); the tooltip says why. Same look in every panel.
void UiBadge(const char *text, const char *tooltip = nil);
// ImGui::Combo over a table of N_() names, drawn in the user's language.
bool UiCombo(const char *label, int *current, const char *const *items, int count);
// Right-click in the 3D view: Map Objects' menu for inst (selecting it first
// when it isn't selected). False for what Map Objects doesn't list.
bool OpenViewportObjectMenu(ObjectInst *inst);
void OpenObjectInspector(void);

// Diff viewer flags (bitmask — an instance can be both moved and rotated)
enum DiffFlags {
	DIFF_ADDED    = 1,
	DIFF_DELETED  = 2,
	DIFF_MOVED    = 4,
	DIFF_ROTATED  = 8,
	DIFF_RESTORED = 16,
	DIFF_MATERIAL = 32,
	DIFF_PROPERTIES = 64,
	DIFF_MODEL = 128,
};
int GetInstanceDiffFlags(ObjectInst *inst);
float GetQuaternionSimilarity(const rw::Quat &a, const rw::Quat &b);
void StampChangeSeq(ObjectInst *inst);
uint32 BumpChangeSeq(void);
uint32 GetLatestChangeSeq(void);
bool LogicalPathEquals(const char *a, const char *b);

// Object Spawner
extern bool gPlaceMode;
extern bool gPlaceRepeat;
void InitLodLookup(void);
void SpawnPlaceObject(rw::V3d position, const rw::Quat *orientation = nil);
int SpawnPlaceObjectNoUndo(rw::V3d position, const rw::Quat *orientation,
	ObjectInst **outInsts, int outCapacity);
void SpawnExitPlaceMode(void);
int GetSpawnObjectId(void);
void SetSpawnObjectId(int id);
// SA-MP element the next placement creates; SetSpawnObjectId resets it to an
// object (or an actor for a skin).
void SetSpawnElementKind(int kind);
// The path new path points go into.
void SetSpawnPathName(const char *name);
const char *GetSpawnPathName(void);
// Checkpoints placed next go on the end of this race; "" places plain ones.
void SetSpawnRaceName(const char *name);
const char *GetSpawnRaceName(void);
// Path points of a path, in order.
void GetNpcPathPoints(const char *pathName, std::vector<ObjectInst*> &out);
void GetNpcPathNames(std::vector<std::string> &out);
// A race's checkpoints in order, and the one after a race checkpoint (nil for the last).
void GetRaceCheckpoints(const char *raceName, std::vector<ObjectInst*> &out);
// The next checkpoint; a circuit's last leads back to its first.
ObjectInst *GetNextRaceCheckpoint(const ObjectInst *inst);
// The type a player sees on a checkpoint shown on its own (a marker): the
// automatic types become Finish on a race's last checkpoint (a circuit's
// first, where laps end) and Normal before; air ones likewise.
int GetRaceCheckpointShownType(const ObjectInst *inst);
// Mode, laps and playable on every checkpoint of the race, as one undo step.
void SetRaceSettings(const char *raceName, int mode, int laps, bool playable);
// haumea_races.inc, written to path; false if it can't be.
bool WriteHaumeaRacesInclude(const char *path);
int GetSpawnElementKind(void);
// Stand-in model used to draw and pick elements that have no model.
int GetSampElementProxyModel(int kind);
const char *GetSampElementKindName(int kind);
int GetSampElementKind(const ObjectInst *inst);
// The model that draws an element's model or skin.
int GetSampElementDisplayModel(int kind, int elementModel);
// Defaults for a new element of this kind (keeps name, group and comment).
void InitSampElementProperties(ObjectPawnProperties *properties, int kind);
void SetCustomPlacementIpl(const char *logicalPath, const char *sourcePath, bool addToDat);
GameFile *GetOrCreateCurrentPlacementIplFile(void);
int GetLodForObject(int id);
int SnapSelectedToGround(bool alignRotation);
// forStanding: the ground a car or a ped stands on, skipping thin props
// (lamp posts, signs, fences, bollards) that the highest surface would hit.
bool GetGroundPlacementSurface(rw::V3d pos, rw::V3d *hitPos, rw::V3d *hitNormal = nil, bool ignoreSelection = false,
	bool forStanding = false);
// Vehicles, peds and the elements drawn as peds or markers stand on the ground.
bool ModelStandsOnGround(int modelId, int elementKind);
bool InstStandsOnGround(const ObjectInst *inst);
bool GetPlacementSurfaceHit(rw::V3d *hitPos, rw::V3d *hitNormal);
rw::V3d GetPlacementPosition(void);
float GetPlacementBaseOffset(int objectId);

// Object Browser categories & favourites
void InitObjectCategories(void);
int GetObjectCategory(int modelId);
void SetObjectCategory(int modelId, int category);
void LoadFavourites(void);
void SaveFavourites(void);
enum { MAX_RECENT_OBJECTS = 48 };
void LoadRecentObjects(void);
void NoteRecentObject(int id);
int GetRecentObjects(const int **ids);
bool IsFavourite(int id);
void ToggleFavourite(int id);

// 3D Preview
void InitPreviewRenderer(void);
void ShutdownPreviewRenderer(void);
void RenderPreviewObject(int objectId);
bool RenderModelPreviewToRaster(int objectId, rw::V3d rotDegrees, float zoom, int colour1, int colour2,
	rw::RGBA background, rw::Raster *colorRaster, rw::Raster *depthRaster);
bool CaptureObjectPreviewPng(int objectId, const char *path, int size, float angle,
	char *error = nil, size_t errorSize = 0);
void RenderPlacementGhost(int objectId, rw::V3d position, rw::Quat rotation, rw::RGBA color);
void RenderRequestedObjectThumbnails(void);
rw::Texture *GetObjectThumbnailTexture(int objectId);
void RenderRequestedPrefabThumbnails(void);
rw::Texture *GetPrefabThumbnailTexture(const char *path);
extern rw::Texture *gPreviewTexture;
void HandleCustomImportDrop(const char *path);

// Game Data structures

void AddCdImage(const char *path);
void InitCdImages(void);
void RefreshCdImageMappings(void);
uint8 *ReadFileFromImage(int i, int *size);
GameFile *GetGameFileFromImage(int i);
const char *GetCdImageLogicalName(int i);
const char *GetCdImageSourcePath(int i);
bool GetCdImageEntrySourcePath(int i, char *outSourcePath, size_t outSourcePathSize);
bool GetPreviousCdImageEntryIndex(int i, int *outIndex);
bool WriteFileToImage(int i, uint8 *data, int size);
bool BuildModloaderImageEntryExportPath(int i, char *dst, size_t size);
void RequestObject(int id);
void LoadAllRequestedObjects(void);


struct TxdDef
{
	char name[MODELNAMELEN];
	rw::TexDictionary *txd;
	int parentId;
	int32 imageIndex;
};
extern rw::TexDictionary *defaultTxd;
void RegisterTexStorePlugin(void);
TxdDef *GetTxdDef(int i);
int FindTxdSlot(const char *name);
int AddTxdSlot(const char *name);
bool RemoveTxdSlot(int i);
int GetNumTxdSlots(void);
int GetTxdTextureCount(int i);
rw::Texture *GetTxdTexture(int i, int textureIndex);
rw::Texture *FindTextureInTxd(int i, const char *name);
void TxdPush(void);
void TxdPop(void);
bool IsTxdLoaded(int i);
void CreateTxd(int i);
void LoadTxd(int i);
void LoadTxd(int i, const char *path);
void TxdMakeCurrent(int i);
void TxdSetParent(const char *child, const char *parent);
void SetTxdLookupContext(const char *objectName, int txdSlot);


struct ColFileHeader
{
	uint32 fourcc;
	uint32 modelsize;
	char name[24];
};

struct ColDef
{
	char name[MODELNAMELEN];
	int32 imageIndex;
};
ColDef *GetColDef(int i);
int AddColSlot(const char *name);
void LoadCol(int slot);
void LoadAllCollisions(void);

// One class for all map objects
struct ObjectDef
{
	enum Type {
		ATOMIC,
		CLUMP
	};

	int m_id;	// our own id
	char m_name[MODELNAMELEN];
	int m_txdSlot;
	int m_type;
	CColModel *m_colModel;
	bool m_gotChildCol;
	int m_pedPathIndex;
	int m_carPathIndex;
	int m_effectIndex;
	int m_numEffects;

	// flags
	bool m_normalCull;	// only III
	bool m_noFade;
	bool m_drawLast;
	bool m_additive;
	bool m_isSubway;	// only III?
	bool m_ignoreLight;
	bool m_noZwrite;
	// VC
	bool m_wetRoadReflection;
	bool m_noShadows;
	bool m_ignoreDrawDist;	// needs a better name perhaps
	bool m_isCodeGlass;
	bool m_isArtistGlass;
	// SA Base
	bool m_noBackfaceCulling;
	// SA Atomic
	bool m_dontCollideWithFlyer;
	bool m_isGarageDoor;
	bool m_isDamageable;
	bool m_isTree;
	bool m_isPalmTree;
	bool m_isTag;
	bool m_noCover;
	bool m_wetOnly;
	// SA Clump
	bool m_isDoor;

	// atomic info
	int m_numAtomics;
	float m_drawDist[3];
	rw::Atomic *m_atomics[3];
	// time objects
	bool m_isTimed;
	int m_timeOn, m_timeOff;
	bool m_isVehicle;
	char m_vehicleType[16];	// vehicles.ide: car, bike, bmx, boat, plane, heli, train, trailer, ...
	char m_vehicleClass[16];	// vehicles.ide: normal, executive, worker, motorbike, ...
	bool m_isPed;	// a skin from peds.ide or SA-MP's IDE

	// clump info
	rw::Clump *m_clump;
	char m_animname[MODELNAMELEN];
	void *m_animAsset;

	bool m_cantLoad;
	bool m_hasPreRendered;
	int32 m_imageIndex;
	float m_minDrawDist;
	bool m_isBigBuilding;
	bool m_isHidden;
	ObjectDef *m_relatedModel;
	ObjectDef *m_relatedTimeModel;

	GameFile *m_file;

	float GetLargestDrawDist(void);
	rw::Atomic *GetAtomicForDist(float dist);
	bool IsLoaded(void);
	void LoadAtomic(void);
	void LoadClump(void);
	void Load(void);
	void SetAtomic(int n, rw::Atomic *atomic);
	void SetClump(rw::Clump *clump);
	void CantLoad(void);
	void SetupBigBuilding(int first, int last);
	void SetFlags(int flags);
};
ObjectDef *AddObjectDef(int id);
void RemoveObjectDef(int id);
ObjectDef *GetObjectDef(int id);
ObjectDef *GetObjectDef(const char *name, int *id);
bool CreateObjectPreviewRwObject(int id, rw::Atomic **atomicOut, rw::Clump **clumpOut);


struct FileObjectInstance
{
	rw::V3d position;
	rw::Quat rotation;
	int objectId;
	int area;
	int lod;
};

struct ObjectInst
{
	rw::V3d m_translation;
	rw::Quat m_rotation;
	// The rx, ry, rz a map file gave, and the rotation they made: shown and
	// saved as written while m_rotation is still that one (several triples
	// give the same rotation, e.g. 180,90,0 and 0,-90,180). Doubles, so a
	// written -89.999961 isn't read back as float32's -89.99996185.
	double m_sourceDegrees[3];
	rw::Quat m_sourceRotation;
	bool m_hasSourceDegrees;
	// cached form of the above
	rw::Matrix m_matrix;
	// Drawn the way SA's IPL loader places it: a rotation with |x| and |y| at
	// most 0.05 keeps only its heading. Script objects (CreateObject) always
	// get the full rotation, so only instances read from or saved to an IPL.
	bool m_iplRotation;
	int m_objectId;
	int m_savedObjectId;
	int m_area;

	void *m_rwObject;
	// Changes with every m_rwObject made or destroyed, never repeating: what
	// holds on to parts of the model (anim.cpp) checks it, as a new model can
	// sit where a freed one was and look the same by its address.
	uint32 m_rwObjectSerial = 0;
	bool m_isBigBuilding;
	uint16 m_scanCode;

	// SA only
	int m_lodId;
	int m_iplSlot;
	ObjectInst *m_lod;
	int m_numChildren;	// hq versions
	int m_numChildrenRendered;
	// SA flags
	bool m_isUnimportant;
	bool m_isUnderWater;
	bool m_isTunnel;
	bool m_isTunnelTransition;

	// additional stuff
	int32 m_id;	// to identify when picking
	uint32 m_editorDocumentId;	// stable only within the map's XML document
	int m_selected;
	uint32 m_selectSeq;	// when it was selected: the earliest is the gizmo's pivot
	int m_highlight;	// various ways to highlight this object
	bool m_isDeleted;	// marked for deletion (commented out in IPL)
	bool m_editorHidden;	// hidden in the viewport by the user; editor-only, never saved
	uint8 m_playerView;	// player view's verdict this frame (playerview.cpp)
	bool m_isAdded;		// newly created text IPL instance not yet saved to disk
	bool m_isDirty;		// position/rotation was modified since the last save
	bool m_gameEntityExists;	// hot reload believes this instance currently exists in the running game
	bool m_isVehicle;
	bool m_vehicleStatic;
	int m_vehicleColor1;
	int m_vehicleColor2;
	int m_vehicleRespawnDelay;
	int m_vehiclePaintjob;	// ChangeVehiclePaintjob's value + 1, 0 for none
	char m_vehiclePlate[VEHICLE_PLATE_LEN];	// SetVehicleNumberPlate, empty for none
	uint16 m_vehicleMods[VEHICLE_MOD_SLOTS];	// AddVehicleComponent by slot, 0 for none
	uint8 *m_vehicleColorRoles;
	int m_numVehicleColorRoles;
	rw::V3d m_origTranslation;	// position the game currently has (for hot reload)
	rw::Quat m_origRotation;	// rotation the game currently has (for hot reload)
	// saved state (updated when IPL is written to disk)
	rw::V3d m_savedTranslation;
	rw::Quat m_savedRotation;
	bool m_savedStateValid;		// false for instances that have never been saved
	bool m_wasSavedDeleted;		// deletion state at last save (text IPL only)
	uint32 m_changeSeq;		// monotonic counter for chronological diff ordering
	int m_iplIndex;		// index of this instance within its IPL file (for save)
	int32 m_imageIndex;	// IMG directory index (for binary IPL save), -1 if text IPL
	int m_binInstIndex;	// index within binary IPL instance array
	char m_iplFilterKey[256];
	void *m_animState;
	float m_animTime;

	// Instance-local SA-MP/Pawn material overrides. The base DFF geometry is
	// shared by ObjectDef instances and is never mutated by these edits.
	ObjectMaterialOverride *m_materialOverrides;
	bool m_materialGeometryApplied;
	bool m_showingMaterialIndexes;	// Show material indexes: drawn with the index overrides
	rw::Geometry *m_materialBaseGeometry;
	rw::Geometry *m_materialCustomGeometry;
	// SA-MP object properties; nil means all defaults.
	ObjectPawnProperties *m_pawnProperties;
	// Last transform written by UpdateObjectAttachments, to tell parent
	// movement apart from the user moving the attached object itself.
	bool m_attachApplied;
	rw::V3d m_attachAppliedPos;
	rw::Quat m_attachAppliedRot;

	GameFile *m_file;

	~ObjectInst();

	void UpdateMatrix(void);
	void *CreateRwObject(void);
	void Init(FileObjectInstance *fi);
	void SetupBigBuilding(void);
	CRect GetBoundRect(void);
	bool IsOnScreen(void);
	void PreRender(void);
	void SetBaseGeometry(rw::Geometry *geometry);
	void RefreshMaterialOverrides(void);
	void refreshMaterialGeometry(void);
	void RefreshVehicleColors(void);
	void RefreshVehicleUpgrades(void);
	void RefreshVehiclePlate(void);

	void JumpTo(void);
	void Select(void);
	void Deselect(void);
	void Delete(void);
	void Undelete(void);
};
extern CPtrList instances;
extern CPtrList selection;
ObjectInst *GetInstanceByID(int32 id);
int32 pick(void);
ObjectInst *AddInstance(void);
// XML restores individual entities; it must not automatically create a LOD.
ObjectInst *SpawnDocumentInstance(int model, const rw::V3d &position, const rw::Quat &rotation);
void RestoreDocumentTransform(ObjectInst *inst, const rw::V3d &position, const rw::Quat &rotation, bool deleted);
void BuildInstanceMatrix(rw::Matrix *matrix, const rw::Quat &rotation, const rw::V3d &translation, bool iplRotation);
void ClearSelection(void);
void DeleteSelected(void);
int DeleteAllInstances(void);
int ResetPawnMap(void);
// RemoveBuildingForPlayer(playerid, -1, x, y, z, radius) from an imported map:
// every world building (HD and LOD) within radius of the point, as the SA-MP
// client compares a building's position with the sphere. Kept as written and
// exported the same way, not as one removal per building.
struct PawnAreaRemoval { float x, y, z, radius; };
const std::vector<PawnAreaRemoval> &GetPawnAreaRemovals(void);
void SetPawnAreaRemovals(const std::vector<PawnAreaRemoval> &removals);	// multiplayer
// Whether a world building's original position is inside one of them.
bool InsidePawnAreaRemoval(const ObjectInst *inst);
void RemoveInstFromSectors(ObjectInst *inst);

// Optional single-IPL document focus. The whole GTA world remains loaded as
// visual context, but only the selected IPL family is editable and saved.
bool IsIplMapDocumentOpen(void);
bool IsIplMapDocumentExternal(void);
bool IsExternalIplMapLogicalPath(const char *logicalPath);
const char *GetIplMapDocumentLogicalPath(void);
const char *GetIplMapDocumentPhysicalPath(void);
bool IsInstInIplMapDocument(const ObjectInst *inst);
void SetIplMapDocument(const char *logicalPath, const char *physicalPath, bool external);
void CloseIplMapDocument(void);
// Opens a dropped .pwn as the map (asking about unsaved changes first).
void HandleMapFileDrop(const char *path);
// The Pawn map document, for the agent bridge: File > Open/New/Save without
// prompts. SavePawnMapTo takes nil for the open map's path and -1 for its format.
bool OpenPawnMapFile(const char *path);
int MergeMapFile(const char *path, int *skipped);
void NewPawnMapDocument(void);
bool SavePawnMapTo(const char *path, int format);
bool ExportOpenPawnMap(const char *path);	// a copy to path; the map keeps its own path
const char *GetPawnMapPath(void);	// empty while untitled
const std::vector<std::string> &GetRecentMapPaths(void);	// File > Open Recent, newest first
void SaveEditorSettings(void);
bool PickPawnFile(char *dst, size_t size, bool save, const char *suggested);	// the native dialog
int GetPawnMapFormat(void);	// PawnCodeFormat
void SetPawnMapFormat(int format);	// what Save writes, e.g. the multiplayer host's
bool PawnMapHasUnsavedChanges(void);



enum EffectType {
	FX_LIGHT,
	FX_PARTICLE,
	FX_LOOKATPOINT,
	FX_PEDQUEUE,
	FX_SUNGLARE,
	FX_INTERIOR,
	FX_ENTRYEXIT,
	FX_ROADSIGN,
	FX_TRIGGERPOINT,
	FX_COVERPOINT,
	FX_ESCALATOR
};

enum FlareType {
        FLARE_NONE,
        FLARE_SUN,
        FLARE_HEADLIGHTS
};

// III and VC for now
struct Effect {
	int id;
	rw::V3d pos;
	rw::RGBA col;
	int type;
	struct Light {
		float lodDist;
		float size;
		float coronaSize;
		float shadowSize;
		int flashiness;
		int reflection;
		int lensFlareType;
		int shadowAlpha;
		int flags;
		char coronaTex[32];
		char shadowTex[32];
	};
	struct Particle {
		int particleType;
		char name[24];
		rw::V3d dir;
		float size;
	};
	struct LookAtPoint {
		rw::V3d dir;
		int type;
		int probability;
	};
	struct PedQueue {
		rw::V3d queueDir;
		rw::V3d useDir;
		rw::V3d forwardDir;
		int type;
		int interest;
		int lookAt;
		int flags;
		char scriptName[8];
	};
	struct Interior {
		int type;
		int group;
		float width;
		float depth;
		float height;
		float rot;
	};
	struct EntryExit {
		float enterAngle;
		float radiusX;
		float radiusY;
		rw::V3d exitPos;
		float exitAngle;
		int areaCode;
		int flags;
		int extraColor;
		int openTime;
		int shutTime;
		int extraFlags;
		char title[8];
	};
	struct Roadsign {
		float width;
		float height;
		float rotX;
		float rotY;
		float rotZ;
		int flags;
		char text[4][16];
	};
	struct TriggerPoint {
		int index;
	};
	struct CoverPoint {
		float dirX;
		float dirY;
		int usage;
	};
	struct Escalator {
		rw::V3d bottom;
		rw::V3d top;
		rw::V3d end;
		bool goingUp;
	};
	union {
		Light light;
		Particle prtcl;
		LookAtPoint look;
		PedQueue queue;
		Interior interior;
		EntryExit entryExit;
		Roadsign roadsign;
		TriggerPoint triggerPoint;
		CoverPoint coverPoint;
		Escalator escalator;
		// glare has no extra data
	};

	void JumpTo(ObjectInst *inst);
};

namespace Effects {
extern Effect *hoveredEffect, *guiHoveredEffect;
extern Effect *selectedEffect;
void AddEffect(Effect e);
Effect *GetEffect(int idx);
void RenderLights(void);
void Render(void);
}


enum PathType {
	PedPath,
	CarPath,
	WaterPath
};

#define LaneWidth 5.0f
struct PathNode {
	int type;
	int link;
	int linkType;
	float x, y, z;
	float width;
	int lanesIn, lanesOut;
	// VC
	int speed;
	int flags;
	float density;
	// SA
	int special;

	enum Type {
		NodeNone = 0,
		NodeExternal,
		NodeInternal
	};
	enum Flags {
		NodeDisabled = 1,
		NodeRoadBlock = 2,
		NodeBetweenLevels = 4,
		NodeUnderBridge = 8
	};

	bool water;
	int lanesInX, lanesOutX;

	int idx;
	int tabId;
	int objId;
	// for internal nodes
	int numLinks;
	int links[12];

	float laneOffset(void) {
		if(lanesInX == 0)
			return 0.5f - 0.5f*lanesOutX;
		if(lanesOutX == 0)
			return 0.5f - 0.5f*lanesInX;
		return 0.5f + width/(2.0f*LaneWidth);
	}
	float laneOffsetIII(void) {
		if(lanesInX == 0)
			return 0.5f - 0.5f*lanesOutX;
		if(lanesOutX == 0)
			return 0.5f - 0.5f*lanesInX;
		return 0.5f;
	}
	void JumpTo(ObjectInst *inst);
	bool isDetached(void);
};

namespace Path {
extern PathNode *hoveredNode, *guiHoveredNode;
extern PathNode *selectedNode;
void AddNode(PathType type, int id, PathNode node);
PathNode *GetPedNode(int base, int i);
PathNode *GetCarNode(int base, int i);
PathNode *GetDetachedPedNode(int base, int i);
PathNode *GetDetachedCarNode(int base, int i);
void RenderPedPaths(void);
void RenderCarPaths(void);
}


// World/sectors

struct Sector
{
	CPtrList buildings;
	CPtrList buildings_overlap;
	CPtrList bigbuildings;
	CPtrList bigbuildings_overlap;
};
extern int numSectorsX, numSectorsY;
extern CRect worldBounds;
extern Sector *sectors;
extern Sector outOfBoundsSector;
void InitSectors(void);
Sector *GetSector(int ix, int iy);
//Sector *GetSector(float x, float y);
int GetSectorIndexX(float x);
int GetSectorIndexY(float x);
bool IsInstInBounds(ObjectInst *inst);
void InsertInstIntoSectors(ObjectInst *inst);


struct IplDef
{
	char name[MODELNAMELEN];
	int instArraySlot;

	int32 imageIndex;
};
int AddInstArraySlot(int n);
ObjectInst **GetInstArray(int i);
int GetInstArraySize(int i);
IplDef *GetIplDef(int i);
int AddIplSlot(const char *name);
void LoadIpl(int i, const char *sceneName = nil);

// File Loader

namespace FileLoader
{

struct BinaryIplSaveResult
{
	int numSavedImages;
	int32 savedImages[256];
	int numFailedImages;
	int32 failedImages[256];
	int numBlockedEmptyDeletes;
	int numFailedFiles;
};

struct AutomaticBackupResult
{
	bool createdSnapshot;
	bool hadWarnings;
	int numTextFiles;
	int numBinaryFiles;
	int numErrors;
	char snapshotPath[1024];
};

struct AutomaticBackupSnapshot
{
	char name[128];
	char path[1024];
};

struct AutomaticBackupRestoreResult
{
	bool restored;
	int numFiles;
	int numErrors;
};

extern GameFile *currentFile;

struct DatDesc
{
	char name[5];
	void (*handler)(char *line);

	static void *get(DatDesc *desc, const char *name);
};

char *LoadLine(FILE *f);
void LoadLevel(const char *filename, float progressFrom = 0.0f, float progressTo = 0.0f);
void LoadVehicleDefinitions(void);
void LoadObjectTypes(const char *filename);
void LoadScene(const char *filename, bool loadRelatedStreaming = true);
int GetLoadedSceneCount(void);
const char *GetLoadedScenePath(int index);
void LoadCollisionFile(const char *path);
rw::TexDictionary *LoadTexDictionary(const char *path);
BinaryIplSaveResult SaveScene(const char *filename);
BinaryIplSaveResult SaveBinaryIpls(const int32 *skipImages = nil, int numSkipImages = 0);
AutomaticBackupResult CreateAutomaticBackup(const char *rootDir, int keepCount);
int ListAutomaticBackupSnapshots(const char *rootDir, AutomaticBackupSnapshot *snapshots, int maxCount);
AutomaticBackupRestoreResult RestoreAutomaticBackup(const char *snapshotDir);
}

// Rendering

enum HighlightStyle
{
	HIGHLIGHT_NONE,
	HIGHLIGHT_FILTER,
	HIGHLIGHT_SELECTION,
	HIGHLIGHT_HOVER,
	HIGHLIGHT_MATCH,
};

struct SceneGlobals {
	rw::World *world;
	rw::Camera *camera;
};
extern rw::Light *pAmbient, *pDirect;
extern rw::Texture *whiteTex;
extern SceneGlobals Scene;
extern CCamera TheCamera;

bool32 instWhite(int type, uint8 *dst, uint32 numVertices, uint32 stride);

void myRenderCB(rw::Atomic *atomic);
// Atomics whose prelighting a material colour removed (renderer.cpp).
void LitObjectRenderCB(rw::Atomic *atomic);

// Neo World pipeline
extern rw::ObjPipeline *neoWorldPipe;
void MakeNeoWorldPipe(void);

// SA DN building pipeline
bool IsBuildingPipeAttached(rw::Atomic *atm);
void SetupBuildingPipe(rw::Atomic *atm);
void UpdateDayNightBalance(void);
// this should perhaps not be public
void GetBuildingEnvMatrix(rw::Atomic *atomic, rw::Frame *envframe, rw::RawMatrix *envmat);
extern rw::ObjPipeline *buildingPipe;
extern rw::ObjPipeline *buildingDNPipe;
void MakeCustomBuildingPipelines(void);

// Leeds building pipeline
extern rw::ObjPipeline *leedsPipe;
void MakeLeedsPipe(void);

void RegisterPipes(void);
void RenderInit(void);
void BuildRenderList(void);
void RenderOpaque(void);
void RenderTransparent(void);
void RenderEverything(void);
void RenderAttachmentWorkspaceGrid(void);
void RenderAttachmentBoneMarker(void);
void RenderAttachmentWorkspacePreview(void);
bool IntersectRayColModelDetailed(const Ray &ray, ObjectInst *inst, rw::V3d *hitPos, rw::V3d *hitNormal);
ObjectInst *GetVisibleInstUnderRay(const Ray &ray, rw::V3d *hitPos = nil, float *hitT = nil);
void ForEachVisibleInst(void (*fn)(ObjectInst *inst, void *data), void *data);

// Debug Render
void RenderLine(rw::V3d v1, rw::V3d v2, rw::RGBA c1, rw::RGBA c2);
void RenderWireBoxVerts(rw::V3d *verts, rw::RGBA col);
void RenderWireBox(CBox *box, rw::RGBA col, rw::Matrix *xform);
void RenderSphereAsWireBox(CSphere *sphere, rw::RGBA col, rw::Matrix *xform);
void RenderSphereAsCross(CSphere *sphere, rw::RGBA col, rw::Matrix *xform);
void RenderWireSphere(CSphere *sphere, rw::RGBA col, rw::Matrix *xform);
void RenderWireTriangle(rw::V3d *v1, rw::V3d *v2, rw::V3d *v3, rw::RGBA col, rw::Matrix *xform);
void RenderAxesWidget(rw::V3d pos, rw::V3d x, rw::V3d y, rw::V3d z);

void RenderEverythingCollisions(void);
void RenderDebugLines(void);
void SplashProgress(float progress, const char *status);
void SplashKeepAlive(void);
extern ImFont *gCodeFont;
extern ImFont *gTerminalFont;	// the same face, with what terminals print beyond Latin
extern ImFont *gTitleFont;
void RenderLinesNow(const rw::V3d *points, int numPoints, rw::RGBA col);
// Selected objects that move (MoveObject): a line to where they go, a ghost
// there, and one gliding between at their speed.
extern bool gPreviewObjectMoves;
void RenderObjectMovePreviews(void);
void RenderObjectOutlines(void);


void RenderPostFX(void);


namespace WaterLevel
{
	struct WaterVertex {
		rw::V3d pos;
		rw::V2d speed;
		float waveunk, waveheight;
	};
	struct WaterQuad {
		int indices[4];
		int flags;	// bit 0: visible, bit 1: limited depth
	};
	struct WaterTri {
		int indices[3];
		int flags;
	};

	void Initialise(void);
	void Render(void);
	// The water's surface at x, y (SA: water.dat, and the sea beyond it);
	// false where there is none.
	bool GetWaterHeight(float x, float y, float *z);
};

namespace Clouds
{
	extern float CloudRotation;

	void Init(void);
	// III and VC
	void RenderBackground(int16 topred, int16 topgreen, int16 topblue,
		int16 botred, int16 botgreen, int16 botblue, int16 alpha);
	void RenderHorizon(void);

	void RenderLowClouds(void);
	void RenderFluffyClouds(void);
	// SA
	void RenderSkyPolys(void);
}


//
// GUI
//

void gui(void);
void uiShowCdImages(void);

// UI coordinates are desktop-relative with multi-viewports, client-relative otherwise.
inline ImVec2 EditorUiPoint(float x, float y)
{
	ImVec2 origin = ImGui::GetMainViewport()->Pos;
	return ImVec2(origin.x + x, origin.y + y);
}
inline ImDrawList *EditorForegroundDrawList(void)
{
#ifdef IMGUI_HAS_VIEWPORT
	return ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
#else
	return ImGui::GetForegroundDrawList();
#endif
}
inline void KeepNextEditorOverlayOnMainWindow(void)
{
#ifdef IMGUI_HAS_VIEWPORT
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
#endif
}
#if defined(_WIN32) && defined(RW_D3D9)
void RenderDetachedEditorWindows(void);
void InitializeEditorWindowDpi(void);
void BringEditorPanelsHome(void);
#endif
