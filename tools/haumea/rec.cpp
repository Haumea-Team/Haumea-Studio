// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "icons.h"
#include "rec.h"
#include "workspace.h"
#include "anim.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ctype.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif

// NPC recordings, read like open.mp's NPCRecordManager::parseRecordFile and
// played like NPCPlayback::process. The server sends frame i once i's
// timestamp has passed and the client smooths towards it; in the viewport
// that smoothing is a lerp/slerp to the next frame.

#define REC_HEADER_SIZE 8
#define REC_DRIVER_FRAME_SIZE (4 + 63)
#define REC_ONFOOT_FRAME_SIZE (4 + 68)
// The client snaps rather than smooths across jumps this long.
#define REC_SNAP_DISTANCE 30.0f
#define REC_VEL_TO_KMH 180.0f	// units per 1/50 s, x 50 x 3.6

namespace {

struct Reader
{
	const uint8 *p;
	uint8 u8(void) { return *p++; }
	uint16 u16(void) { uint16 v = p[0] | p[1]<<8; p += 2; return v; }
	uint32 u32(void) { uint32 v = p[0] | p[1]<<8 | p[2]<<16 | (uint32)p[3]<<24; p += 4; return v; }
	float f32(void) { uint32 u = u32(); float f; memcpy(&f, &u, 4); return f; }
	rw::V3d v3(void) { rw::V3d v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
	// stored w, x, y, z
	rw::Quat quat(void) { rw::Quat q; q.w = f32(); q.x = f32(); q.y = f32(); q.z = f32(); return q; }
};

void
readDriverFrame(Reader &r, RecFrame *f)
{
	f->vehicleId = r.u16();
	f->lr = r.u16();
	f->ud = r.u16();
	f->keys = r.u16();
	f->rot = r.quat();
	f->pos = r.v3();
	f->vel = r.v3();
	f->vehicleHealth = r.f32();
	f->health = r.u8();
	f->armour = r.u8();
	f->weaponKey = r.u8();
	f->siren = r.u8();
	f->gear = r.u8();
	f->trailerId = r.u16();
	f->extra = r.u32();
}

void
readOnFootFrame(Reader &r, RecFrame *f)
{
	f->lr = r.u16();
	f->ud = r.u16();
	f->keys = r.u16();
	f->pos = r.v3();
	f->rot = r.quat();
	f->health = r.u8();
	f->armour = r.u8();
	f->weaponKey = r.u8();
	f->specialAction = r.u8();
	f->vel = r.v3();
	f->surfOffset = r.v3();
	f->surfId = r.u16();
	f->animId = r.u16();
	f->animFlags = r.u16();
}

float
dist(rw::V3d a, rw::V3d b)
{
	return length(sub(a, b));
}

rw::V3d
recLerp(rw::V3d a, rw::V3d b, float t)
{
	return add(a, scale(sub(b, a), t));
}

rw::Quat
recSlerp(rw::Quat a, rw::Quat b, float t)
{
	float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
	if(d < 0.0f){
		d = -d;
		b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
	}
	float wa, wb;
	if(d > 0.9995f){
		wa = 1.0f - t;
		wb = t;
	}else{
		float th = acosf(d);
		float s = sinf(th);
		wa = sinf((1.0f-t)*th) / s;
		wb = sinf(t*th) / s;
	}
	rw::Quat q;
	q.x = wa*a.x + wb*b.x;
	q.y = wa*a.y + wb*b.y;
	q.z = wa*a.z + wb*b.z;
	q.w = wa*a.w + wb*b.w;
	float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
	if(n > 0.0f){
		q.x /= n; q.y /= n; q.z /= n; q.w /= n;
	}else
		q = a;
	return q;
}

}

const char*
RecTypeName(int type)
{
	return type == REC_DRIVER ? _("Driver") : type == REC_ONFOOT ? _("On foot") : _("Unknown");
}

bool
RecLoad(const char *path, RecFile *rec)
{
	rec->frames.clear();
	rec->type = REC_NONE;
	rec->version = 0;
	rec->trailingBytes = 0;
	rec->distance = 0.0f;
	strncpy(rec->path, path ? path : "", sizeof(rec->path)-1);
	rec->path[sizeof(rec->path)-1] = '\0';

	FILE *f = path ? fopen(path, "rb") : nil;
	if(f == nil)
		return false;
	std::vector<uint8> data;
	uint8 buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf+n);
	fclose(f);
	if(data.size() < REC_HEADER_SIZE)
		return false;

	Reader r = { data.data() };
	rec->version = r.u32();
	int type = (int32)r.u32();
	if(type != REC_DRIVER && type != REC_ONFOOT)
		return false;
	rec->type = type;

	size_t frameSize = type == REC_DRIVER ? REC_DRIVER_FRAME_SIZE : REC_ONFOOT_FRAME_SIZE;
	size_t body = data.size() - REC_HEADER_SIZE;
	size_t count = body / frameSize;
	rec->trailingBytes = (int)(body - count*frameSize);
	rec->frames.resize(count);
	for(size_t i = 0; i < count; i++){
		RecFrame *fr = &rec->frames[i];
		memset(fr, 0, sizeof(*fr));
		fr->time = r.u32();
		if(type == REC_DRIVER)
			readDriverFrame(r, fr);
		else
			readOnFootFrame(r, fr);
		if(i > 0)
			rec->distance += dist(fr->pos, rec->frames[i-1].pos);
	}
	return true;
}

float
RecDuration(const RecFile &rec)
{
	return rec.frames.empty() ? 0.0f : rec.frames.back().time / 1000.0f;
}

bool
RecSampleAt(const RecFile &rec, float t, RecSample *out)
{
	memset(out, 0, sizeof(*out));
	out->type = rec.type;
	out->vehicleModel = -1;
	if(rec.frames.empty())
		return false;

	// Last frame whose timestamp has passed; before the first one the
	// server has sent nothing yet, so show where playback will begin.
	double ms = t * 1000.0;
	int n = (int)rec.frames.size();
	int lo = 0, hi = n;
	while(lo < hi){
		int mid = (lo + hi) / 2;
		if(rec.frames[mid].time <= ms)
			lo = mid + 1;
		else
			hi = mid;
	}
	int i = max(lo - 1, 0);
	const RecFrame &a = rec.frames[i];

	out->frame = i;
	out->pos = a.pos;
	out->rot = a.rot;
	out->vel = a.vel;
	if(i+1 < n && ms > a.time){
		const RecFrame &b = rec.frames[i+1];
		uint32 span = b.time - a.time;
		if(b.time > a.time && dist(a.pos, b.pos) < REC_SNAP_DISTANCE){
			float k = clamp((float)((ms - a.time) / span), 0.0f, 1.0f);
			out->pos = recLerp(a.pos, b.pos, k);
			out->rot = recSlerp(a.rot, b.rot, k);
			out->vel = recLerp(a.vel, b.vel, k);
		}
	}

	out->health = a.health;
	out->armour = a.armour;
	out->weapon = a.weaponKey & 0x3F;
	out->keys = a.keys;
	out->lr = (int16)a.lr;
	out->ud = (int16)a.ud;
	out->onFoot = rec.type == REC_ONFOOT;
	if(rec.type == REC_DRIVER){
		out->vehicleId = a.vehicleId;
		out->vehicleHealth = a.vehicleHealth;
		out->siren = a.siren != 0;
		out->gear = a.gear;
		memcpy(&out->trainSpeed, &a.extra, 4);
	}else{
		out->specialAction = a.specialAction;
		out->animId = a.animId;
		out->animFlags = a.animFlags;
	}
	return true;
}

// ---- Viewport player ----

namespace {

struct RecPlayer
{
	int32 instId;
	RecFile rec;
	bool loaded;
	char name[256];	// recording name the file was loaded for
	float time;
	bool playing;
	bool loop;
	bool showRoute;
	bool startAtInst;
	// The instance's own transform, put back on stop.
	bool applied;
	rw::V3d origPos;
	rw::Quat origRot;
	// What the player last wrote, to notice the user moving the instance.
	rw::V3d lastPos;
	rw::Quat lastRot;
	int lastUiFrame;
	// On-foot recordings animate the skin; its own animation is put back on stop.
	bool animated;
	bool hadAnim;
	AnimPlayback ownAnim;
	char animLib[32], animName[32];	// what the recording plays now; empty: idle
};

std::vector<RecPlayer*> players;
char recFolder[1024];
bool recFolderLoaded;
// A folder the user chose; otherwise the workspace's npcmodes/recordings.
bool recFolderCustom;
char serverHint[1024];

const char *REC_FOLDER_SETTING = "rec_folder.txt";

RecPlayer*
findPlayer(int32 instId)
{
	for(RecPlayer *p : players)
		if(p->instId == instId)
			return p;
	return nil;
}

RecPlayer*
getPlayer(ObjectInst *inst)
{
	RecPlayer *p = findPlayer(inst->m_id);
	if(p)
		return p;
	p = new RecPlayer();
	p->instId = inst->m_id;
	p->loop = true;
	p->showRoute = true;
	p->lastUiFrame = -1;
	players.push_back(p);
	return p;
}

void
setInstTransform(ObjectInst *inst, rw::V3d pos, rw::Quat rot)
{
	RemoveInstFromSectors(inst);
	inst->m_translation = pos;
	inst->m_rotation = rot;
	inst->UpdateMatrix();
	if(inst->m_rwObject){
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if(obj){
			rw::Frame *f = obj->m_type == ObjectDef::ATOMIC ?
				((rw::Atomic*)inst->m_rwObject)->getFrame() :
				((rw::Clump*)inst->m_rwObject)->getFrame();
			f->transform(&inst->m_matrix, rw::COMBINEREPLACE);
		}
	}
	InsertInstIntoSectors(inst);
}

bool
sameTransform(ObjectInst *inst, rw::V3d pos, rw::Quat rot)
{
	return inst->m_translation.x == pos.x && inst->m_translation.y == pos.y &&
		inst->m_translation.z == pos.z &&
		inst->m_rotation.x == rot.x && inst->m_rotation.y == rot.y &&
		inst->m_rotation.z == rot.z && inst->m_rotation.w == rot.w;
}

// open.mp's NPC_StartPlayback with a start position moves the whole route
// so its first frame lands there.
rw::V3d
routeOffset(RecPlayer *p, ObjectInst *inst)
{
	rw::V3d off = { 0.0f, 0.0f, 0.0f };
	if(p->startAtInst && !p->rec.frames.empty())
		off = sub(p->applied ? p->origPos : inst->m_translation, p->rec.frames[0].pos);
	return off;
}

void
restore(RecPlayer *p, ObjectInst *inst)
{
	if(p->applied && inst && sameTransform(inst, p->lastPos, p->lastRot))
		setInstTransform(inst, p->origPos, p->origRot);
	if(p->animated && inst){
		if(p->hadAnim)
			SetInstanceAnimation(inst, p->ownAnim.lib, p->ownAnim.name, p->ownAnim.delta,
				p->ownAnim.loop, p->ownAnim.lockX, p->ownAnim.lockY, p->ownAnim.freeze,
				p->ownAnim.timeMs);
		else
			ClearInstanceAnimation(inst);
	}
	p->animated = false;
	p->applied = false;
	p->playing = false;
}

// The animation the recorded player had, or walk/jog/sprint from its speed
// when the recording has none the editor knows. While paused it stays as is.
void
animateSample(RecPlayer *p, ObjectInst *inst, const RecSample &s)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || !obj->m_isPed)
		return;
	if(!p->animated){
		p->hadAnim = GetInstanceAnimation(inst, &p->ownAnim);
		p->animLib[0] = p->animName[0] = '\0';
		p->animated = true;
	}
	char lib[32] = "", name[32] = "";
	if(!AnimFromSampId(s.animId, lib, sizeof(lib), name, sizeof(name)) ||
	   AnimFindInLibrary(AnimFindLibrary(lib), name) < 0){
		// vel is in GTA units per 1/50 s
		float speed = sqrtf(s.vel.x*s.vel.x + s.vel.y*s.vel.y) * 50.0f;
		strcpy(lib, "PED");
		if(speed >= 7.0f) strcpy(name, "sprint_civi");
		else if(speed >= 2.5f) strcpy(name, "run_player");
		else if(speed >= 0.3f) strcpy(name, "WALK_player");
		else lib[0] = '\0';	// standing
	}
	if(rw::strcmp_ci(lib, p->animLib) == 0 && rw::strcmp_ci(name, p->animName) == 0)
		return;
	strcpy(p->animLib, lib);
	strcpy(p->animName, name);
	if(lib[0] == '\0' || !SetInstanceAnimation(inst, lib, name, 4.1f, true, false, false, false))
		ClearInstanceAnimation(inst);
}

void
applySample(RecPlayer *p, ObjectInst *inst)
{
	RecSample s;
	if(!p->loaded || !RecSampleAt(p->rec, p->time, &s))
		return;
	if(!p->applied){
		p->origPos = inst->m_translation;
		p->origRot = inst->m_rotation;
		p->applied = true;
	}
	rw::V3d pos = add(s.pos, routeOffset(p, inst));
	setInstTransform(inst, pos, s.rot);
	p->lastPos = inst->m_translation;
	p->lastRot = inst->m_rotation;
	if(p->rec.type == REC_ONFOOT && !inst->m_isVehicle)
		animateSample(p, inst, s);
}

void
drawRoute(RecPlayer *p, ObjectInst *inst)
{
	const std::vector<RecFrame> &fr = p->rec.frames;
	if(fr.size() < 2)
		return;
	rw::V3d off = routeOffset(p, inst);
	rw::RGBA col = p->rec.type == REC_DRIVER ?
		rw::RGBA{ 255, 170, 40, 255 } : rw::RGBA{ 80, 200, 255, 255 };
	rw::RGBA done = { col.red, col.green, col.blue, 110 };
	uint32 now = (uint32)(p->time * 1000.0f);
	size_t step = max<size_t>(1, fr.size() / 20000);
	size_t prev = 0;
	for(size_t i = step; i < fr.size(); i += step){
		size_t j = min(i, fr.size()-1);
		if(dist(fr[prev].pos, fr[j].pos) < REC_SNAP_DISTANCE*step){
			rw::RGBA c = fr[j].time <= now ? done : col;
			RenderLine(add(fr[prev].pos, off), add(fr[j].pos, off), c, c);
		}
		prev = j;
	}
	// a cross where the route starts
	rw::V3d s = add(fr[0].pos, off);
	rw::RGBA green = { 60, 230, 90, 255 };
	RenderLine({ s.x-1.0f, s.y, s.z }, { s.x+1.0f, s.y, s.z }, green, green);
	RenderLine({ s.x, s.y-1.0f, s.z }, { s.x, s.y+1.0f, s.z }, green, green);
	RenderLine({ s.x, s.y, s.z-1.0f }, { s.x, s.y, s.z+1.0f }, green, green);
}

void
removePlayer(size_t i)
{
	delete players[i];
	players.erase(players.begin() + i);
}

// ---- Recordings folder ----

bool
isDirectory(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

void
trimEnd(char *s)
{
	size_t n = strlen(s);
	while(n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
		s[--n] = '\0';
}

bool
isSeparator(char c)
{
	return c == '/' || c == '\\';
}

// Walks up from path looking for a server directory with npcmodes/recordings.
bool
findRecordingsFolder(const char *path, char *dst, size_t size)
{
	std::string dir = path ? path : "";
	while(!dir.empty() && isSeparator(dir.back()))
		dir.pop_back();
	for(int depth = 0; depth < 8 && !dir.empty(); depth++){
		std::string candidate = dir + "/npcmodes/recordings";
		if(isDirectory(candidate.c_str())){
			snprintf(dst, size, "%s", candidate.c_str());
			return true;
		}
		size_t slash = dir.find_last_of("/\\");
		if(slash == std::string::npos)
			break;
		dir.resize(slash);
	}
	return false;
}

void
saveFolderSetting(void)
{
	// An emptied field goes back to following the workspace.
	recFolderCustom = recFolder[0] != '\0';
	FILE *f = fopenHaumeaDataWrite(REC_FOLDER_SETTING);
	if(f == nil)
		return;
	fprintf(f, "%s\n", recFolder);
	fclose(f);
}

// The default folder: the workspace's npcmodes/recordings, or one found
// around the server hint.
void
followWorkspace(void)
{
	static std::string lastDefault = "\x01";
	if(recFolderCustom)
		return;
	char folder[1024] = "";
	if(GetWorkspaceDir()[0])
		snprintf(folder, sizeof(folder), "%s", WorkspacePath("npcmodes/recordings").c_str());
	else if(serverHint[0])
		findRecordingsFolder(serverHint, folder, sizeof(folder));
	// Only when the default moves, so typing in the field is not undone.
	if(lastDefault != folder){
		lastDefault = folder;
		snprintf(recFolder, sizeof(recFolder), "%s", folder);
	}
}

void
loadFolderSetting(void)
{
	if(!recFolderLoaded){
		recFolderLoaded = true;
		FILE *f = fopenHaumeaDataRead(REC_FOLDER_SETTING);
		if(f){
			if(fgets(recFolder, sizeof(recFolder), f) == nil)
				recFolder[0] = '\0';
			trimEnd(recFolder);
			fclose(f);
		}
		recFolderCustom = recFolder[0] != '\0';
	}
	followWorkspace();
}

bool
hasRecExtension(const char *name)
{
	size_t n = strlen(name);
	return n > 4 && (name[n-4] == '.') &&
		tolower(name[n-3]) == 'r' && tolower(name[n-2]) == 'e' && tolower(name[n-1]) == 'c';
}

// Names without .rec, sorted.
void
listRecordings(const char *dir, std::vector<std::string> &names)
{
	names.clear();
	if(dir == nil || dir[0] == '\0')
		return;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((std::string(dir) + "\\*.rec").c_str(), &entry);
	if(handle != INVALID_HANDLE_VALUE){
		do{
			if(!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && hasRecExtension(entry.cFileName))
				names.push_back(std::string(entry.cFileName, strlen(entry.cFileName)-4));
		}while(FindNextFileA(handle, &entry));
		FindClose(handle);
	}
#else
	DIR *d = opendir(dir);
	if(d){
		dirent *ent;
		while((ent = readdir(d)) != nil)
			if(hasRecExtension(ent->d_name))
				names.push_back(std::string(ent->d_name, strlen(ent->d_name)-4));
		closedir(d);
	}
#endif
	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b){
#ifdef _WIN32
		return _stricmp(a.c_str(), b.c_str()) < 0;
#else
		return strcasecmp(a.c_str(), b.c_str()) < 0;
#endif
	});
}

void
formatTime(char *dst, size_t size, float t)
{
	int ms = (int)(t * 1000.0f + 0.5f);
	snprintf(dst, size, "%d:%02d.%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
}

bool
loadForName(RecPlayer *p, ObjectInst *inst, const char *name)
{
	restore(p, inst);
	p->loaded = false;
	p->time = 0.0f;
	snprintf(p->name, sizeof(p->name), "%s", name);
	if(name[0] == '\0')
		return false;
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.rec", recFolder, name);
	p->loaded = RecLoad(path, &p->rec);
	if(p->loaded && !p->rec.frames.empty())
		p->time = p->rec.frames[0].time / 1000.0f;
	return p->loaded;
}

}

int
RecTypeOfName(const char *name)
{
	if(name == nil || name[0] == '\0')
		return REC_NONE;
	loadFolderSetting();
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.rec", recFolder, name);
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return REC_NONE;
	// The header: uint32 version, int32 type.
	uint8 header[8];
	bool ok = fread(header, 1, sizeof(header), f) == sizeof(header);
	fclose(f);
	int type = ok ? (int)(header[4] | header[5] << 8 | header[6] << 16 | (uint32)header[7] << 24) : REC_NONE;
	return type == REC_DRIVER || type == REC_ONFOOT ? type : REC_NONE;
}

void
RecPlayerUpdate(void)
{
	float dt = ImGui::GetIO().DeltaTime;
	int frame = ImGui::GetFrameCount();
	for(size_t i = 0; i < players.size(); ){
		RecPlayer *p = players[i];
		ObjectInst *inst = GetInstanceByID(p->instId);
		if(inst == nil || inst->m_isDeleted){
			if(inst)
				restore(p, inst);
			removePlayer(i);
			continue;
		}
		// The user moved or rotated it: keep their edit as its own transform.
		if(p->applied && !sameTransform(inst, p->lastPos, p->lastRot)){
			p->applied = false;
			p->playing = false;
		}
		if(p->playing && p->loaded){
			float duration = RecDuration(p->rec);
			p->time += dt;
			if(p->time > duration){
				if(p->loop && duration > 0.0f)
					p->time = fmodf(p->time, duration);
				else{
					p->time = duration;
					p->playing = false;
				}
			}
			applySample(p, inst);
		}
		// Panels shown in the last couple of frames draw their route.
		bool shown = p->lastUiFrame >= frame - 2;
		if(p->loaded && p->showRoute && (shown || p->applied))
			drawRoute(p, inst);
		// Forget players nobody looks at and that are not posing anything.
		if(!shown && !p->applied){
			removePlayer(i);
			continue;
		}
		i++;
	}
}

bool
RecPlayerIsActive(ObjectInst *inst)
{
	RecPlayer *p = inst ? findPlayer(inst->m_id) : nil;
	return p && p->applied;
}

void
RecPlayerStop(ObjectInst *inst)
{
	RecPlayer *p = inst ? findPlayer(inst->m_id) : nil;
	if(p)
		restore(p, inst);
}

void
RecPlayerStopAll(void)
{
	for(RecPlayer *p : players)
		restore(p, GetInstanceByID(p->instId));
}

void
RecSetServerHint(const char *path)
{
	snprintf(serverHint, sizeof(serverHint), "%s", path ? path : "");
	if(recFolderLoaded)
		followWorkspace();
}

bool
uiRecPlayer(ObjectInst *inst, char *recName, size_t size)
{
	if(inst == nil || recName == nil || size == 0)
		return false;
	loadFolderSetting();
	RecPlayer *p = getPlayer(inst);
	p->lastUiFrame = ImGui::GetFrameCount();
	bool changed = false;

	ImGui::PushID("RecPlayer");

	// Folder
	static std::vector<std::string> names;
	static char listedFolder[1024];
	static int listedFrame = -1000;
	bool relist = strcmp(listedFolder, recFolder) != 0 ||
		ImGui::GetFrameCount() - listedFrame > 120;
	char folderId[64];
	UiFieldLabel(_("Folder"), folderId, sizeof(folderId));
	float browseWidth = ImGui::GetFrameHeight();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x);
	if(ImGui::InputTextWithHint("##folder", _("npcmodes/recordings folder"), recFolder, sizeof(recFolder),
			ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()){
		saveFolderSetting();
		relist = true;
	}
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip(_("Folder with the server's .rec files; empty: the workspace's npcmodes/recordings"));
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_FOLDER_OPEN "##browse", ImVec2(browseWidth, 0))){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), "Recordings folder", recFolder)){
			snprintf(recFolder, sizeof(recFolder), "%s", picked);
			saveFolderSetting();
			relist = true;
		}
	}
	ImGui::SetItemTooltip("%s", _("Browse..."));
	if(relist){
		listRecordings(recFolder, names);
		snprintf(listedFolder, sizeof(listedFolder), "%s", recFolder);
		listedFrame = ImGui::GetFrameCount();
	}

	// Recording
	const char *preview = recName[0] ? recName : _("(none)");
	if(UiBeginCombo(_("Recording"), preview, ImGuiComboFlags_HeightLarge)){
		if(ImGui::Selectable(_("(none)"), recName[0] == '\0') && recName[0]){
			recName[0] = '\0';
			changed = true;
		}
		for(const std::string &n : names){
			bool sel = strcmp(n.c_str(), recName) == 0;
			if(ImGui::Selectable(n.c_str(), sel) && !sel){
				snprintf(recName, size, "%s", n.c_str());
				changed = true;
			}
			if(sel)
				ImGui::SetItemDefaultFocus();
		}
		if(names.empty())
			ImGui::TextDisabled("%s", recFolder[0] ? _("No .rec files in this folder") : _("Pick the recordings folder first"));
		ImGui::EndCombo();
	}

	if(strcmp(p->name, recName) != 0)
		loadForName(p, inst, recName);

	if(recName[0] == '\0'){
		ImGui::PopID();
		return changed;
	}
	if(!p->loaded){
		ImVec4 bad = UiSemanticColor(UI_COLOR_ERROR);
		if(p->rec.type == REC_NONE && doesFileExist(p->rec.path))
			ImGui::TextColored(bad, _("Not an NPC recording (unknown type)"));
		else
			ImGui::TextColored(bad, _("%s.rec is not in the recordings folder"), recName);
		if(ImGui::Button(TI(ICON_FA_ROTATE, "Reload"), ImVec2(-FLT_MIN, 0)))
			loadForName(p, inst, recName);
		ImGui::PopID();
		return changed;
	}

	float duration = RecDuration(p->rec);
	char durText[32];
	formatTime(durText, sizeof(durText), duration);
	ImGui::TextDisabled(_("%s, %d frames, %s, %.0f m"), RecTypeName(p->rec.type),
		(int)p->rec.frames.size(), durText, p->rec.distance);
	if(p->rec.type == REC_DRIVER && !inst->m_isVehicle)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("A driver recording: the NPC must be in a vehicle"));
	else if(p->rec.type == REC_ONFOOT && inst->m_isVehicle)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("An on-foot recording on a vehicle"));
	if(p->rec.trailingBytes)
		ImGui::TextDisabled(_("%d trailing bytes ignored"), p->rec.trailingBytes);

	// Transport: Play and Stop share the row, the options below them.
	ImGui::Spacing();
	float half = floorf((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f);
	if(ImGui::Button(p->playing ? _("Pause") : _("Play"), ImVec2(half, 0))){
		if(!p->playing && p->time >= duration && !p->rec.frames.empty())
			p->time = p->rec.frames[0].time / 1000.0f;
		p->playing = !p->playing;
		if(p->playing)
			applySample(p, inst);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!p->applied);
	if(ImGui::Button(_("Stop"), ImVec2(half, 0))){
		restore(p, inst);
		p->time = p->rec.frames.empty() ? 0.0f : p->rec.frames[0].time / 1000.0f;
	}
	ImGui::EndDisabled();
	if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(_("Stop and put the element back where it was"));
	auto checkboxWidth = [](const char *label){
		return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nil, true).x;
	};
	UiCheckbox(_("Loop"), &p->loop);
	UiSameLineIfFits(checkboxWidth(_("Route")));
	UiCheckbox(_("Route"), &p->showRoute);
	UiSameLineIfFits(checkboxWidth(_("From here")));
	bool startAtInst = p->startAtInst;
	if(UiCheckbox(_("From here"), &startAtInst)){
		p->startAtInst = startAtInst;
		if(p->applied)
			applySample(p, inst);
	}
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip(_("Move the route so it starts at the element, like NPC_StartPlayback\n"
			"with a start position. Off plays it where it was recorded."));

	// Timeline
	ImGui::Spacing();
	char timeText[32], label[80];
	formatTime(timeText, sizeof(timeText), p->time);
	snprintf(label, sizeof(label), "%s / %s", timeText, durText);
	ImGui::SetNextItemWidth(-FLT_MIN);
	float t = p->time;
	if(ImGui::SliderFloat("##timeline", &t, 0.0f, duration, label, ImGuiSliderFlags_NoInput)){
		p->time = t;
		applySample(p, inst);
	}
	if(ImGui::IsItemActive())
		p->playing = false;

	RecSample s;
	if(RecSampleAt(p->rec, p->time, &s)){
		ImGui::TextDisabled(_("Frame %d  %.1f km/h  Health %.0f  Armour %.0f  Weapon %d"),
			s.frame, length(s.vel) * REC_VEL_TO_KMH, s.health, s.armour, s.weapon);
		if(s.onFoot)
			ImGui::TextDisabled(_("Keys 0x%X  Special action %d  Animation %d"), s.keys, s.specialAction, s.animId);
		else
			ImGui::TextDisabled(_("Keys 0x%X  Vehicle ID %d  Vehicle health %.0f%s"), s.keys,
				s.vehicleId, s.vehicleHealth, s.siren ? (std::string("  ") + _("Siren")).c_str() : "");
	}
	ImGui::PopID();
	return changed;
}
