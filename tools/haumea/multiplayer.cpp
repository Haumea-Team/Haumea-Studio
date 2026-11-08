// SPDX-License-Identifier: GPL-2.0-or-later
// Multiplayer editing (multiplayer.h).
//
// A relay keeps the session (mprelay.cpp): this editor's own when hosting,
// or haumea-server's. Every editor, the host's too, sends it its changes,
// and it relays every change, the sender's included, to all in one order.
// A change is an element's whole state (MpElementHead plus the editor's own
// property, vehicle and material structs), found by comparing each element
// with what was last sent or received, so every way of editing is covered,
// undo included, and a new property field travels without changes here.
// The map's lists (textdraws, area removals, radar art, format) go whole
// when they change, as channels.
//
// Two people changing one element at once: an editor ignores the others'
// changes to an element while the relay hasn't echoed its own yet, since the
// relay puts its own after them, so everyone ends with the same state.
#include "mpprotocol.h"	// first: winsock2.h before windows.h
#include "mprelay.h"
#include "haumea.h"
#include "upnp.h"
#include "multiplayer.h"
#include "mpaccounts.h"
#include <deque>
#include "imgui_internal.h"
#include "radarart.h"
#include "sapaths.h"
#include "documentxml.h"
#include "coordinatepin.h"
#include "layoutpaths.h"
#include "textdraws.h"
#include "workspace.h"
#include "encoding.h"
#include "icons.h"
#include "i18n.h"

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ctype.h>
#include <cmath>
#include <string.h>
#include <sys/stat.h>

static const double MP_SEND_INTERVAL = 0.05;
static const double MP_PRESENCE_INTERVAL = 0.1;
// Others arrive ten times a second; their markers follow, closing most of the
// gap in this time. A long jump (Go to, an agent starting somewhere else)
// takes longer, so it reads as flying there instead of a flicker.
static const float MP_FOLLOW_TIME = 0.12f;
static const float MP_GLIDE_TIME = 0.35f;
static const float MP_GLIDE_GAP = 50.0f;
static const double MP_CHANNEL_INTERVAL = 0.5;

/*
 * Session state
 */

namespace {
struct Peer
{
	uint32 id;
	char name[MP_NAME_LEN];
	rw::V3d camera, target;	// as they last sent them
	rw::V3d shownCamera, shownTarget;	// where we draw them: following those smoothly
	int area;
	std::vector<uint32> selection;	// element ids
	bool agent;	// an AI agent's editor, as the relay says
	int role;	// in this room (MP_ROLE_*)
	char owner[MP_NAME_LEN];	// an agent's: the account it acts for
	int ping;	// ms, as it measured it; 0 unknown
};
// A chat message as it came; peer 0 is the session itself.
struct ChatLine
{
	uint32 seq;	// ours, in arrival order (the agent bridge reads from one on)
	uint32 peer;
	char name[MP_NAME_LEN];
	bool agent;
	long long time;
	rw::V3d camera, target;
	int area;	// -1: no place to go to
	std::string text;
};
// A server we joined, or keep to join again.
struct ServerEntry
{
	bool favourite;
	std::string address, name, password;	// password only for a favourite that keeps it
	int port;
	int kind = 0;	// how we signed in there
	std::string account, room;
	std::string pin;	// the server's fingerprint the first time: another one later means trouble
};
}
static std::deque<ChatLine> gChat;
static uint32 gChatSeq;
static bool gChatHistory;	// the lines a joining editor gets come before the snapshot ends
static bool gAsAgent;	// we're an agent's editor (--background): the others see a tag
static uint32 gOffers;	// what the relay offers (MP_OFFERS_*)
static bool gReplacing;	// our map is going to replace the session's
static std::vector<ServerEntry> gServers;
static bool gServersLoaded;
// Signing in (protocol 5).
static MultiplayerJoinRequest gJoin;
static uint8_t gNonce[MP_NONCE_LEN];
static bool gAccountsMode;	// the server has accounts and rooms
static std::string gFingerprint;	// its identity, as people compare it ("" for none: an editor hosting)
static int gRole = 2;	// ours where we are (MP_ROLE_*)
static std::string gRoomName;
struct LobbyRoom
{
	std::string name, people;
	bool locked;
	int count, role;
};
static std::vector<LobbyRoom> gLobby;
static int gAccountRole;	// ours on the server, as the lobby says (an admin may make rooms)
static int gPingMs = -1;
static double gLastPing;
static std::string gAdminText, gAdminData;
static bool gAdminOk;
static unsigned int gAdminSeq;
static std::string gAgentToken;
static bool gAgentTokenAsked;

static int gState = MP_OFF;	// joining: MP_OFF, MP_CONNECTING or MP_JOINED
static bool gHosting;	// the relay is this editor's own (mprelay.h)
static MpConnection gRelay;
static bool gRelayConnected;	// the TCP connection is up
static double gConnectStarted;
static uint32 gSelf;	// our peer id, from the relay
static uint32 gNextElement;
static char gName[MP_NAME_LEN];
static char gPassword[MP_PASSWORD_LEN];
static char gSessionName[MP_NAME_LEN];	// the host's name, or haumea-server's --name
static char gAddress[256];
static int gPort;
static std::string gStatus;	// the last thing that happened, for the window
static std::map<uint32, Peer> gPeers;	// the others
static double gLastSend, gLastPresence, gLastPresenceChange, gLastThorough;
static std::string gLastPresenceState;
static int gMissingModels;	// elements whose model this game doesn't have

// Elements by the editor's instance id (m_id, never reused) and by their
// session-wide element id (the creating peer's id in the top bits).
static std::unordered_map<int32, uint32> gElementOf;
static std::unordered_map<uint32, int32> gInstanceOf;
static std::unordered_map<int32, std::string> gBaseline;	// what everyone last had
static std::unordered_map<int32, int> gPending;	// our changes not echoed yet
static std::unordered_map<int32, uint32> gSeenChange;	// m_changeSeq when last compared
static std::unordered_set<int32> gChangedByOthers;	// last changed by someone else
static double gLastSweep;
static std::unordered_map<uint32, std::string> gUnresolved;	// waiting for a parent, vehicle or LOD
static std::unordered_map<std::string, ObjectInst*> gWorldIndex;
static std::string gChannelBaseline[MP_CHANNEL_COUNT];
static std::unordered_set<std::string> gHaveFiles;
static std::string gReceivedModels;	// the session's custom models as they came	// keys the relay has (we sent or got them)
static double gLastChannels;

static const ImU32 peerColours[] = {
	IM_COL32(80, 170, 255, 255), IM_COL32(255, 120, 80, 255), IM_COL32(110, 220, 110, 255),
	IM_COL32(240, 200, 60, 255), IM_COL32(210, 120, 240, 255), IM_COL32(60, 220, 210, 255),
	IM_COL32(255, 110, 170, 255), IM_COL32(200, 200, 200, 255),
};

static ImU32
peerColour(uint32 peer)
{
	return peerColours[(peer + 7) % (sizeof(peerColours) / sizeof(peerColours[0]))];
}

static rw::RGBA
peerRgba(uint32 peer)
{
	ImU32 c = peerColour(peer);
	rw::RGBA rgba = { (uint8)(c >> IM_COL32_R_SHIFT), (uint8)(c >> IM_COL32_G_SHIFT),
		(uint8)(c >> IM_COL32_B_SHIFT), 255 };
	return rgba;
}

static void clearElements(void);
static void receiveChat(const std::string &payload);
static void sendMapFile(int reason, const char *path);
static void rememberServer(void);
static void loadServers(void);
static void receiveRooms(const std::string &payload);
static void lobbyPanel(std::string *error);
static void adminPanel(void);

static void
resetSession(void)
{
	MpCloseSocket(gRelay.socket);
	gRelay = MpConnection();
	if(gHosting){
		MpRelayStop();
		UpnpClosePort();
	}
	gHosting = false;
	gRelayConnected = false;
	gState = MP_OFF;
	gSelf = 0;
	gNextElement = 0;
	gPassword[0] = '\0';
	gSessionName[0] = '\0';
	gPeers.clear();
	gChat.clear();
	gOffers = 0;
	gReplacing = false;
	gLobby.clear();
	gRoomName.clear();
	gRole = 2;
	gAccountsMode = false;
	gFingerprint.clear();
	gPingMs = -1;
	gAgentToken.clear();
	gAgentTokenAsked = false;
	clearElements();
	gLastPresenceState.clear();
	gLastThorough = 0.0;
	gMissingModels = 0;
}

// What ties the open map to the session's elements, for a session joined anew
// or a map that replaces it.
static void
clearElements(void)
{
	gElementOf.clear();
	gInstanceOf.clear();
	gBaseline.clear();
	gPending.clear();
	gSeenChange.clear();
	gChangedByOthers.clear();
	gUnresolved.clear();
	gWorldIndex.clear();
	for(std::string &baseline : gChannelBaseline)
		baseline.clear();
	gHaveFiles.clear();
	gReceivedModels.clear();
}

/*
 * Elements
 */


static ObjectInst*
instanceOf(uint32 element)
{
	auto it = gInstanceOf.find(element);
	return it == gInstanceOf.end() ? nil : GetInstanceByID(it->second);
}

// Two peers may give one GTA building an id each before they hear of the
// other's: the relay keeps the first and sends it back in place of the
// other, so everyone takes the relay's.
static uint32
bindElement(ObjectInst *inst, uint32 element, bool fromRelay)
{
	gInstanceOf[element] = inst->m_id;
	auto it = gElementOf.find(inst->m_id);
	if(it == gElementOf.end() || fromRelay)
		gElementOf[inst->m_id] = element;
	return gElementOf[inst->m_id];
}

static uint32
elementOf(ObjectInst *inst, bool assign)
{
	auto it = gElementOf.find(inst->m_id);
	if(it != gElementOf.end())
		return it->second;
	if(!assign || gSelf == 0)
		return 0;
	gNextElement = (gNextElement + 1) & MP_ELEMENT_MASK;
	return bindElement(inst, gSelf << MP_PEER_SHIFT | gNextElement, false);
}

static std::string
worldKey(const char *file, int iplIndex, int imageIndex, int binInstIndex)
{
	char numbers[48];
	snprintf(numbers, sizeof(numbers), "|%d|%d|%d", iplIndex, imageIndex, binInstIndex);
	return std::string(file) + numbers;
}

static ObjectInst*
findWorldInstance(const std::string &file, const MpElementHead &h)
{
	if(gWorldIndex.empty())
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_file && inst->m_file->name && IsPawnWorldObject(inst))
				gWorldIndex[worldKey(inst->m_file->name, inst->m_iplIndex, inst->m_imageIndex,
					inst->m_binInstIndex)] = inst;
		}
	auto it = gWorldIndex.find(worldKey(file.c_str(), h.iplIndex, h.imageIndex, h.binInstIndex));
	return it == gWorldIndex.end() ? nil : it->second;
}

// An element of the map: the map's own, or a GTA building it changed.
// Deleted ones of the map's own were never seen by the others.
static bool
isMapElement(ObjectInst *inst, bool thorough)
{
	if(IsRadarArtStandIn(inst))
		return false;
	if(IsUserMapObject(inst))
		return !inst->m_isDeleted;
	if(!thorough && !inst->m_isDeleted && !inst->m_isDirty && !inst->m_pawnProperties &&
	   !inst->m_materialOverrides)
		return false;
	return PawnObjectNeedsExport(inst) || inst->m_pawnProperties;
}

static std::string
encodeElement(ObjectInst *inst, uint32 element)
{
	MpElementHead h;
	memset(&h, 0, sizeof(h));
	ObjectPawnProperties props;
	memcpy(&props, GetObjectPawnProperties(inst), sizeof(props));
	bool world = IsPawnWorldObject(inst);
	h.element = element;
	if(!world && inst->m_lod)
		h.lod = elementOf(inst->m_lod, true);
	if(props.attachParent == PAWN_ATTACH_PLAYER)
		h.flags |= EL_ATTACH_PLAYER;
	else if(ObjectInst *parent = props.attachParent >= 0 ? GetInstanceByID(props.attachParent) : nil)
		h.attachParent = elementOf(parent, true);
	if(ObjectInst *vehicle = props.npcVehicle >= 0 ? GetInstanceByID(props.npcVehicle) : nil)
		h.npcVehicle = elementOf(vehicle, true);
	props.attachParent = PAWN_ATTACH_NONE;
	props.npcVehicle = -1;
	h.model = PawnModelId(inst->m_objectId);
	h.area = inst->m_area;
	if(world){
		h.flags |= EL_WORLD;
		h.iplIndex = inst->m_iplIndex;
		h.imageIndex = inst->m_imageIndex;
		h.binInstIndex = inst->m_binInstIndex;
		h.fileLength = inst->m_file && inst->m_file->name ? (uint32)strlen(inst->m_file->name) : 0;
	}
	if(inst->m_isDeleted) h.flags |= EL_DELETED;
	if(inst->m_isVehicle) h.flags |= EL_VEHICLE;
	if(inst->m_hasSourceDegrees){
		h.flags |= EL_SOURCE_DEGREES;
		memcpy(h.degrees, inst->m_sourceDegrees, sizeof(h.degrees));
		memcpy(h.sourceRotation, &inst->m_sourceRotation, sizeof(h.sourceRotation));
	}
	memcpy(h.position, &inst->m_translation, sizeof(h.position));
	memcpy(h.rotation, &inst->m_rotation, sizeof(h.rotation));
	std::string materials;
	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
		const ObjectMaterialOverride *m = GetObjectMaterialOverride((const ObjectInst*)inst, slot);
		if(m && (m->active || m->textEnabled)){
			h.materials |= 1u << slot;
			MpPut(materials, *m);
		}
	}
	std::string out;
	MpPut(out, h);
	if(h.fileLength)
		out.append(inst->m_file->name, h.fileLength);
	MpPut(out, props);
	if(inst->m_isVehicle)
		MpPut(out, GetVehicleState(inst));
	return out + materials;
}

static void
linkLod(ObjectInst *inst, ObjectInst *lod)
{
	if(inst->m_lod == lod)
		return;
	// As ApplyMapXML relinks a restored LOD.
	if(inst->m_lod && inst->m_lod->m_numChildren > 0)
		--inst->m_lod->m_numChildren;
	inst->m_lod = lod;
	inst->m_lodId = lod ? lod->m_iplIndex : -1;
	if(lod){
		++lod->m_numChildren;
		if(!lod->m_isBigBuilding)
			lod->SetupBigBuilding();
	}
}

static bool
finiteWithin(const float *values, int count, float limit)
{
	for(int i = 0; i < count; i++)
		if(!std::isfinite(values[i]) || fabsf(values[i]) > limit)
			return false;
	return true;
}

// Makes the element here what the blob says. Returns false while something
// it points at (parent, vehicle, LOD) isn't here yet.
static bool
applyElement(const std::string &blob)
{
	MpReader r(blob);
	MpElementHead h;
	std::string file;
	ObjectPawnProperties props;
	VehicleState vehicle = {};
	ObjectMaterialOverride materials[MAX_PAWN_MATERIALS];
	if(!r.get(&h) || h.element == 0 || h.fileLength > 1024)
		return true;
	file.resize(h.fileLength);
	if(h.fileLength) r.bytes(&file[0], h.fileLength);
	r.get(&props);
	if(h.flags & EL_VEHICLE) r.get(&vehicle);
	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++)
		if(h.materials & (1u << slot)) r.get(&materials[slot]);
	if(!r.ok || !finiteWithin(h.position, 3, 1e6f) || !finiteWithin(h.rotation, 4, 2.0f) ||
	   !finiteWithin(h.sourceRotation, 4, 2.0f))
		return true;
	for(double d : h.degrees)
		if(!std::isfinite(d))
			return true;
	vehicle.plate[sizeof(vehicle.plate) - 1] = '\0';

	bool world = (h.flags & EL_WORLD) != 0, deleted = (h.flags & EL_DELETED) != 0;
	rw::V3d position;
	rw::Quat rotation;
	memcpy(&position, h.position, sizeof(position));
	memcpy(&rotation, h.rotation, sizeof(rotation));
	ObjectInst *inst = instanceOf(h.element);
	if(inst == nil && world)
		inst = findWorldInstance(file, h);
	bool spawned = false;
	if(inst == nil && !world && !deleted){
		inst = SpawnDocumentInstance(EditorModelId(h.model), position, rotation);
		spawned = inst != nil;
		if(inst == nil && gMissingModels++ == 0)
			Toast(TOAST_SPAWN, _("This game doesn't have model %d, which the session uses"), h.model);
	}
	if(inst == nil)
		return true;	// a building this game doesn't have, or deleted before we saw it
	bindElement(inst, h.element, true);
	// Someone else's change: undoing ours on it would throw theirs away, and
	// an agent rolling back its own edits leaves it alone.
	if(spawned || encodeElement(inst, h.element) != blob){
		UndoForgetInstance(inst);
		gChangedByOthers.insert(inst->m_id);
	}

	bool resolved = true;
	auto reference = [&resolved](uint32 element) -> ObjectInst* {
		ObjectInst *target = element ? instanceOf(element) : nil;
		if(element && target == nil)
			resolved = false;
		return target;
	};
	ObjectInst *parent = reference(h.attachParent), *npcVehicle = reference(h.npcVehicle);
	ObjectInst *lod = world ? inst->m_lod : reference(h.lod);
	props.attachParent = h.flags & EL_ATTACH_PLAYER ? PAWN_ATTACH_PLAYER :
		parent && parent != inst ? parent->m_id : PAWN_ATTACH_NONE;
	props.npcVehicle = npcVehicle ? npcVehicle->m_id : -1;
	SetObjectPawnProperties(inst, props, false);
	int model = EditorModelId(h.model);
	if(model != inst->m_objectId && !inst->m_isVehicle)
		ChangeInstanceModelId(inst, model, false);
	inst->m_area = h.area;
	if(memcmp(&inst->m_translation, &position, sizeof(position)) != 0 ||
	   memcmp(&inst->m_rotation, &rotation, sizeof(rotation)) != 0 || inst->m_isDeleted != deleted){
		if(deleted)
			inst->Deselect();
		RestoreDocumentTransform(inst, position, rotation, deleted);
	}
	inst->m_hasSourceDegrees = (h.flags & EL_SOURCE_DEGREES) != 0;
	if(inst->m_hasSourceDegrees){
		memcpy(inst->m_sourceDegrees, h.degrees, sizeof(h.degrees));
		memcpy(&inst->m_sourceRotation, h.sourceRotation, sizeof(h.sourceRotation));
	}
	if(inst->m_isVehicle && (h.flags & EL_VEHICLE)){
		VehicleState current = GetVehicleState(inst);
		if(memcmp(&current, &vehicle, sizeof(vehicle)) != 0)
			SetVehicleState(inst, vehicle);
	}
	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
		const ObjectMaterialOverride *current = GetObjectMaterialOverride((const ObjectInst*)inst, slot);
		if(h.materials & (1u << slot))
			SetObjectMaterialOverride(inst, slot, materials[slot], false);
		else if(current && (current->active || current->textEnabled))
			ClearObjectMaterialOverride(inst, slot, false);
	}
	if(!world)
		linkLod(inst, lod);
	gBaseline[inst->m_id] = encodeElement(inst, elementOf(inst, false));
	gSeenChange[inst->m_id] = inst->m_changeSeq;
	return resolved;
}

static uint32
blobElement(const std::string &blob)
{
	MpElementHead h;
	MpReader r(blob);
	return r.get(&h) ? h.element : 0;
}

static void
receiveElement(uint32 origin, const std::string &blob)
{
	MpElementHead h;
	MpReader r(blob);
	if(!r.get(&h))
		return;
	uint32 element = h.element;
	ObjectInst *inst = instanceOf(element);
	if(inst == nil && (h.flags & EL_WORLD) && h.fileLength <= 1024){
		// A building we gave another id: the relay's is the one.
		std::string file(h.fileLength, '\0');
		if(h.fileLength) r.bytes(&file[0], h.fileLength);
		if(r.ok && (inst = findWorldInstance(file, h)) != nil && gElementOf.count(inst->m_id))
			bindElement(inst, element, true);
	}
	if(inst){
		auto pending = gPending.find(inst->m_id);
		if(origin == gSelf){
			if(pending != gPending.end() && --pending->second <= 0)
				gPending.erase(pending);
			return;
		}
		// The relay's own word (origin 0: a viewer's edit it didn't keep) is the
		// session's, whatever we sent: ours won't come back.
		if(origin == 0 && pending != gPending.end()){
			gPending.erase(pending);
			pending = gPending.end();
		}
		// Ours comes after it: the relay puts everything in one order.
		if(pending != gPending.end())
			return;
	}
	if(origin == gSelf)
		return;
	gUnresolved.erase(element);
	if(!applyElement(blob))
		gUnresolved[element] = blob;
}

static void
retryUnresolved(void)
{
	for(auto it = gUnresolved.begin(); it != gUnresolved.end();){
		std::string blob = it->second;
		if(applyElement(blob))
			it = gUnresolved.erase(it);
		else
			++it;
	}
}

/*
 * Channels: the map's lists, each sent whole when it changes.
 */

static bool
readBytes(const std::string &path, std::string *bytes)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return false;
	bytes->clear();
	char buffer[65536];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
		bytes->append(buffer, n);
	fclose(f);
	return true;
}

static bool
writeBytes(const std::string &path, const std::string &bytes)
{
	if(!EnsureParentDirectoriesForPath(path.c_str()))
		return false;
	std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	if(f && fclose(f) != 0)
		ok = false;
#ifdef _WIN32
	if(ok)
		remove(path.c_str());
#endif
	if(!ok || rename(temporary.c_str(), path.c_str()) != 0){
		remove(temporary.c_str());
		return false;
	}
	return true;
}

// Files the session's channels name, by key, and the session's custom models
// made of them, in the config folder.
static std::string
sessionPath(const char *folder, const std::string &name)
{
	return std::string(GetConfigDir()) + "/multiplayer/" + folder + "/" + name;
}

// A file's key, worked out again only when its size or time changes.
static std::string
fileKey(const std::string &path)
{
	struct Known { long long size, time; std::string key; };
	static std::unordered_map<std::string, Known> known;
	struct stat st;
	if(stat(path.c_str(), &st) != 0)
		return "";
	Known &k = known[path];
	if(k.key.empty() || k.size != (long long)st.st_size || k.time != (long long)st.st_mtime){
		std::string bytes;
		if(!readBytes(path, &bytes))
			return "";
		k.size = (long long)st.st_size;
		k.time = (long long)st.st_mtime;
		k.key = MpFileKey(bytes.data(), bytes.size());
	}
	return k.key;
}

// Custom models: "file <key> <name>" for each file, then the artconfig.txt lines.
static std::string
customModelsState(std::vector<std::pair<std::string, std::string>> *files)
{
	std::map<std::string, std::string> paths;
	std::string list = CustomModelsSessionList(paths);
	std::string out;
	for(const auto &file : paths){
		std::string key = fileKey(file.second);
		if(key.empty())
			continue;
		out += "file " + key + " " + file.first + "\n";
		if(files)
			files->emplace_back(key, file.second);
	}
	return out + list;
}

static void
applyCustomModels(const std::string &data)
{
	std::string list;
	size_t at = 0;
	while(at < data.size()){
		size_t end = data.find('\n', at);
		std::string line = data.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = end == std::string::npos ? data.size() : end + 1;
		if(line.compare(0, 5, "file ") != 0){
			list += line + "\n";
			continue;
		}
		size_t space = line.find(' ', 5);
		if(space == std::string::npos)
			continue;
		std::string key = line.substr(5, space - 5), name = line.substr(space + 1);
		if(name.empty() || name.find_first_of("/\\:") != std::string::npos || name[0] == '.')
			continue;	// a file name, nothing more
		std::string bytes, target = sessionPath("models", name);
		if(fileKey(target) == key || !readBytes(sessionPath("files", key), &bytes))
			continue;
		if(!writeBytes(target, bytes))
			log("Multiplayer: could not write %s\n", target.c_str());
	}
	int added = AddSessionCustomModels(sessionPath("models", "").c_str(), list);
	if(added > 0)
		log("Multiplayer: %d custom model(s) from the session\n", added);
}

static void
receiveFile(const std::string &payload)
{
	std::string key, bytes;
	if(!MpReadFilePayload(payload, &key, &bytes))
		return;
	gHaveFiles.insert(key);
	if(!writeBytes(sessionPath("files", key), bytes))
		log("Multiplayer: could not keep the session's file %s\n", key.c_str());
}

// 3D paths name their objects by document id, which is this editor's own:
// on the way they name them by element id.
static void
layoutPathRefs(EditorXML::Node &root, bool toElements)
{
	unsigned next = 0;
	std::unordered_map<unsigned, ObjectInst*> byDocument;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		next = std::max(next, inst->m_editorDocumentId);
		if(inst->m_editorDocumentId)
			byDocument[inst->m_editorDocumentId] = inst;
	}
	auto documentId = [&](ObjectInst *inst) {
		if(inst->m_editorDocumentId == 0)
			byDocument[inst->m_editorDocumentId = ++next] = inst;
		return inst->m_editorDocumentId;
	};
	if(toElements)
		// Every placed object, so the paths write theirs.
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(IsUserMapObject(inst) && !inst->m_isDeleted)
				documentId(inst);
		}
	// An object that isn't here (yet) is left out: the path keeps the rest.
	for(EditorXML::Node &path : root.children){
		std::vector<EditorXML::Node> kept;
		for(EditorXML::Node &object : path.children){
			double ref = 0;
			if(object.name == "object" && object.number("ref", &ref)){
				ObjectInst *inst = nil;
				if(toElements){
					auto it = byDocument.find((unsigned)ref);
					inst = it == byDocument.end() ? nil : it->second;
				}else
					inst = instanceOf((uint32)ref);
				if(inst == nil)
					continue;
				object.set("ref", toElements ? elementOf(inst, true) : documentId(inst));
			}
			kept.push_back(std::move(object));
		}
		path.children = std::move(kept);
	}
}

// A Nodes area, worked out again only after node edits; one nobody edited
// here stays as the session has it (its baseline).
static bool gNodesStale = true;

static std::string
nodesState(int area)
{
	static uint32 serial;
	static std::string states[64];
	static bool shared[64];
	if(gNodesStale || serial != SAPaths::NodesEditSerial()){
		gNodesStale = false;
		serial = SAPaths::NodesEditSerial();
		for(int i = 0; i < 64; i++){
			std::vector<uint8> data;
			shared[i] = SAPaths::GetSharedAreaData(i, data);
			states[i].assign(data.begin(), data.end());
		}
	}
	return shared[area] ? states[area] : gChannelBaseline[MP_CHANNEL_NODES + area];
}

static std::string
channelState(int channel)
{
	if(channel >= MP_CHANNEL_NODES)
		return nodesState(channel - MP_CHANNEL_NODES);
	std::string out;
	switch(channel){
	case MP_CHANNEL_META:
		MpPut(out, (int32)GetPawnMapFormat());
		MpPut(out, (int32)GetPawnTextEncoding());
		break;
	case MP_CHANNEL_CUSTOM_MODELS:
		out = customModelsState(nil);
		break;
	case MP_CHANNEL_TEXTDRAWS:
		out = EditorXML::Serialize(BuildTextDrawXML(GetMapTextDraws(), {}, PAWN_TEXT_CP1252));
		break;
	case MP_CHANNEL_REMOVALS:
		for(const PawnAreaRemoval &removal : GetPawnAreaRemovals())
			MpPut(out, removal);
		break;
	case MP_CHANNEL_RADAR_ART:
		for(const RadarArtLayer &layer : GetRadarArtLayers()){
			out.append(layer.group, sizeof(layer.group));
			MpPut(out, (int32)layer.zoneMode);
			MpPut(out, (uint32)layer.zones.size());
			for(const RadarArtZone &zone : layer.zones)
				MpPut(out, zone);
		}
		break;
	case MP_CHANNEL_TEXTDRAW_FILE:
		MpPut(out, (int32)GetTextDrawCodeTarget());
		out += EditorXML::Serialize(BuildTextDrawXML(GetTextDraws(), GetTextDrawGuides(), PAWN_TEXT_CP1252));
		break;
	case MP_CHANNEL_PINS:
		out = EditorXML::Serialize(WriteCoordinatePinXML());
		break;
	case MP_CHANNEL_LAYOUT_PATHS: {
		EditorXML::Node root = WriteLayoutPathsXML();
		layoutPathRefs(root, true);
		out = EditorXML::Serialize(root);
		break;
	}
	}
	return out;
}

static void
applyChannel(int channel, const std::string &data)
{
	MpReader r(data);
	switch(channel){
	case MP_CHANNEL_META: {
		int32 format = 0, encoding = 0;
		if(r.get(&format) && r.get(&encoding)){
			SetPawnMapFormat(format);
			if(encoding == PAWN_TEXT_CP1252 || encoding == PAWN_TEXT_UTF8)
				SetPawnTextEncoding((PawnTextEncoding)encoding);
		}
		break;
	}
	case MP_CHANNEL_TEXTDRAWS: {
		EditorXML::Node document;
		std::vector<TextDraw> textdraws;
		std::vector<TextDrawGuide> guides;
		std::string error;
		if(!data.empty() && (!EditorXML::Parse(data, &document, &error) ||
		   !ReadTextDrawXML(document, true, textdraws, guides, &error))){
			log("Multiplayer: unreadable textdraws: %s\n", error.c_str());
			return;
		}
		GetMapTextDraws() = std::move(textdraws);
		BumpChangeSeq();
		break;
	}
	case MP_CHANNEL_REMOVALS: {
		std::vector<PawnAreaRemoval> removals;
		PawnAreaRemoval removal;
		while(r.left >= sizeof(removal) && r.get(&removal))
			if(finiteWithin(&removal.x, 4, 1e6f))
				removals.push_back(removal);
		SetPawnAreaRemovals(removals);
		break;
	}
	case MP_CHANNEL_RADAR_ART: {
		std::vector<RadarArtLayer> layers;
		while(r.ok && r.left > 0){
			RadarArtLayer layer;
			int32 mode = 0;
			uint32 count = 0;
			r.bytes(layer.group, sizeof(layer.group));
			r.get(&mode);
			r.get(&count);
			if(!r.ok || count > r.left / sizeof(RadarArtZone))
				return;
			layer.group[sizeof(layer.group) - 1] = '\0';
			layer.zoneMode = mode;
			layer.zones.resize(count);
			for(RadarArtZone &zone : layer.zones)
				r.get(&zone);
			layers.push_back(std::move(layer));
		}
		if(r.ok){
			SetRadarArtLayers(layers, false);
			UndoForgetKind(UNDO_RADAR_ART);
		}
		break;
	}
	case MP_CHANNEL_CUSTOM_MODELS:
		applyCustomModels(data);
		gReceivedModels = data;
		break;
	case MP_CHANNEL_TEXTDRAW_FILE: {
		int32 target = 0;
		EditorXML::Node document;
		std::vector<TextDraw> textdraws;
		std::vector<TextDrawGuide> guides;
		std::string error;
		if(!r.get(&target) || !EditorXML::Parse(r.rest(), &document, &error) ||
		   !ReadTextDrawXML(document, true, textdraws, guides, &error)){
			log("Multiplayer: unreadable textdraw file: %s\n", error.c_str());
			return;
		}
		GetTextDraws() = std::move(textdraws);
		GetTextDrawGuides() = std::move(guides);
		if(target == TEXTDRAW_CODE_SAMP || target == TEXTDRAW_CODE_OPENMP)
			SetTextDrawCodeTarget((TextDrawCodeTarget)target);
		TextDrawsReplacedByOthers();
		break;
	}
	case MP_CHANNEL_PINS: {
		EditorXML::Node root;
		std::string error;
		if(!EditorXML::Parse(data, &root, &error) || !ReadCoordinatePinXML(root, &error, true))
			log("Multiplayer: unreadable alignment pins: %s\n", error.c_str());
		break;
	}
	case MP_CHANNEL_LAYOUT_PATHS: {
		EditorXML::Node root;
		std::string error;
		if(EditorXML::Parse(data, &root, &error)){
			layoutPathRefs(root, false);
			if(ReadLayoutPathsXML(root, &error, true)){
				UndoForgetKind(UNDO_LAYOUT_PATHS);
				break;
			}
		}
		log("Multiplayer: unreadable 3D paths: %s\n", error.c_str());
		break;
	}
	}
	if(channel >= MP_CHANNEL_NODES){
		if(SAPaths::SetSharedAreaData(channel - MP_CHANNEL_NODES, std::vector<uint8>(data.begin(), data.end())))
			UndoForgetKind(UNDO_NODES);
		gNodesStale = true;
	}
	gChannelBaseline[channel] = channelState(channel);
}

static void
sendToRelay(uint8 type, const std::string &payload)
{
	if(gState == MP_JOINED || gState == MP_CONNECTING)
		MpQueue(gRelay, MpMakeFrame(type, payload));
}

// Sends the lists in [first, end) that changed; silent only takes note of them.
static void
sendChannels(bool silent, int first = 1, int end = MP_CHANNEL_COUNT)
{
	for(int channel = first; channel < end; channel++){
		std::string state = channelState(channel);
		if(state == gChannelBaseline[channel])
			continue;
		gChannelBaseline[channel] = state;
		if(silent)
			continue;
		if(channel == MP_CHANNEL_CUSTOM_MODELS){
			// The files first, each once: the relay keeps them for everyone.
			std::vector<std::pair<std::string, std::string>> files;
			customModelsState(&files);
			for(const auto &file : files){
				std::string bytes;
				if(gHaveFiles.count(file.first) || !readBytes(file.second, &bytes))
					continue;
				if(bytes.size() > MP_MAX_UNPACKED - 64){
					log("Multiplayer: %s is too big to share\n", file.second.c_str());
					continue;
				}
				sendToRelay(MSG_FILE, MpFilePayload(file.first, bytes));
				gHaveFiles.insert(file.first);
			}
		}
		sendToRelay(MSG_CHANNEL, MpChannelPayload(gSelf, (uint8)channel, state));
	}
}

/*
 * Sending
 */

static void
publishElement(ObjectInst *inst, const std::string &blob)
{
	sendToRelay(MSG_ELEMENT, MpFromPeer(gSelf, blob));
	gPending[inst->m_id]++;
	gChangedByOthers.erase(inst->m_id);
}

// Sends the element when it changed since it was last sent; silent only
// takes note of it.
static void
noteElement(ObjectInst *inst, bool silent)
{
	gSeenChange[inst->m_id] = inst->m_changeSeq;
	std::string state = encodeElement(inst, elementOf(inst, true));
	std::string &baseline = gBaseline[inst->m_id];
	if(state == baseline)
		return;
	baseline = state;
	if(!silent)
		publishElement(inst, state);
}

// Sends what changed since the last time; silent only takes note of it.
static void
sendChanges(bool silent)
{
	bool thorough = MpNow() - gLastThorough > 5.0;
	if(thorough)
		gLastThorough = MpNow();
	// What new elements need goes before them, what refers to them after.
	bool channels = MpNow() - gLastChannels > MP_CHANNEL_INTERVAL || silent;
	if(channels){
		gLastChannels = MpNow();
		sendChannels(silent, 1, MP_CHANNEL_AFTER_ELEMENTS);
	}
	// Encoding every element every time is too slow for big maps: only what
	// was stamped as changed or is selected (being dragged), and all of them
	// now and then for edits that don't stamp.
	bool sweep = silent || MpNow() - gLastSweep > 1.0;
	if(sweep)
		gLastSweep = MpNow();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		bool known = gElementOf.count(inst->m_id) != 0;
		if(!known && !isMapElement(inst, thorough))
			continue;
		auto seen = gSeenChange.find(inst->m_id);
		if(known && !sweep && !inst->m_selected && seen != gSeenChange.end() && seen->second == inst->m_changeSeq)
			continue;
		noteElement(inst, silent);
	}
	if(channels)
		sendChannels(silent, MP_CHANNEL_AFTER_ELEMENTS);
}

// Into an empty session: the whole map, in its order, then every list.
static void
seedSession(void)
{
	gBaseline.clear();
	gSeenChange.clear();
	for(std::string &baseline : gChannelBaseline)
		baseline = "\x01";	// never a list's state: all of them go
	sendChannels(false, 1, MP_CHANNEL_AFTER_ELEMENTS);
	// In the order they were made, which the joining editors make them in
	// too: Pawn is written in the instance list's order.
	std::vector<ObjectInst*> elements;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(isMapElement(inst, true))
			elements.push_back(inst);
	}
	std::sort(elements.begin(), elements.end(), [](ObjectInst *a, ObjectInst *b) { return a->m_id < b->m_id; });
	for(ObjectInst *inst : elements)
		noteElement(inst, false);
	sendChannels(false, MP_CHANNEL_AFTER_ELEMENTS);
	gLastThorough = gLastSweep = gLastChannels = MpNow();
	sendToRelay(MSG_SEED_DONE, "");
}

static std::string
presencePayload(uint32 peer, const char *name, const rw::V3d &camera, const rw::V3d &target, int area,
	const std::vector<uint32> &selection)
{
	std::string payload;
	MpPut(payload, peer);
	payload.append(name, MP_NAME_LEN);
	MpPut(payload, camera);
	MpPut(payload, target);
	MpPut(payload, (int32)area);
	MpPut(payload, (uint32)selection.size());
	for(uint32 element : selection)
		MpPut(payload, element);
	MpPut(payload, (uint16_t)(gPingMs > 0 ? std::min(gPingMs, 65535) : 0));
	return payload;
}

static bool
readPresence(const std::string &payload, Peer *peer)
{
	MpReader r(payload);
	uint32 count = 0;
	int32 area = 0;
	r.get(&peer->id);
	r.bytes(peer->name, MP_NAME_LEN);
	r.get(&peer->camera);
	r.get(&peer->target);
	r.get(&area);
	r.get(&count);
	if(!r.ok || count > MP_MAX_SELECTION)
		return false;
	char name[MP_NAME_LEN];
	memcpy(name, peer->name, MP_NAME_LEN);
	name[MP_NAME_LEN - 1] = '\0';
	MpCopyName(peer->name, name);
	peer->area = area;
	peer->selection.resize(count);
	for(uint32 &element : peer->selection)
		r.get(&element);
	// Put in by the relay: agent, role, whose agent, ping.
	char agent = 0, role = 2;
	uint16_t ping = 0;
	memset(peer->owner, 0, sizeof(peer->owner));
	if(r.ok && r.left >= 2 + MP_NAME_LEN + 2){
		r.get(&agent);
		r.get(&role);
		r.bytes(peer->owner, MP_NAME_LEN);
		r.get(&ping);
		peer->owner[MP_NAME_LEN - 1] = '\0';
	}
	peer->agent = agent != 0;
	peer->role = role;
	peer->ping = ping;
	return r.ok;
}

static bool gPresencePoseSet;
static rw::V3d gPresenceCamera, gPresenceTarget;

void
MultiplayerSetPresencePose(const rw::V3d &camera, const rw::V3d &target)
{
	gPresencePoseSet = true;
	gPresenceCamera = camera;
	gPresenceTarget = target;
}

static std::string
ownPresence(void)
{
	std::vector<uint32> selected;
	for(CPtrNode *p = selection.first; p && (int)selected.size() < MP_MAX_SELECTION; p = p->next)
		if(uint32 element = elementOf((ObjectInst*)p->item, false))
			selected.push_back(element);
	return presencePayload(gSelf, gName, gPresencePoseSet ? gPresenceCamera : TheCamera.m_position,
		gPresencePoseSet ? gPresenceTarget : TheCamera.m_target, currentArea, selected);
}

static void
sendPresence(void)
{
	double t = MpNow();
	if(t - gLastPresence < MP_PRESENCE_INTERVAL)
		return;
	std::string payload = ownPresence();
	if(payload == gLastPresenceState && t - gLastPresenceChange < MP_KEEPALIVE)
		return;
	gLastPresence = gLastPresenceChange = t;
	gLastPresenceState = payload;
	sendToRelay(MSG_PRESENCE, payload);
}

/*
 * Joining: a relay, our own when hosting
 */

static void
relayLog(const char *line)
{
	log("Multiplayer relay: %s\n", line);
}

namespace {
struct Candidate
{
	sockaddr_storage address;
	socklen_t length;
};
}
static std::vector<Candidate> gCandidates;	// addresses left to try
static double gCandidateStarted;
static const double MP_CANDIDATE_TIMEOUT = 4.0;	// then the next address

// Starts connecting to the next address; false when none is left.
static bool
connectNext(void)
{
	MpCloseSocket(gRelay.socket);
	while(!gCandidates.empty()){
		Candidate c = gCandidates.front();
		gCandidates.erase(gCandidates.begin());
		gRelay.socket = socket(c.address.ss_family, SOCK_STREAM, IPPROTO_TCP);
		if(gRelay.socket != MP_NO_SOCKET && MpSetNonBlocking(gRelay.socket) &&
		   (connect(gRelay.socket, (sockaddr*)&c.address, c.length) == 0 || MpWouldBlock())){
			MpTuneSocket(gRelay.socket);
			gCandidateStarted = MpNow();
			return true;
		}
		MpCloseSocket(gRelay.socket);
	}
	return false;
}

static bool
joinRelay(const char *address, int port, const char *password, const char *name, std::string *error)
{
	if(!isSA() || IsIplMapDocumentOpen()){
		*error = _("Multiplayer edits Pawn maps of San Andreas.");
		return false;
	}
	if(password == nil || strlen(password) >= MP_PASSWORD_LEN){
		*error = _("The password is too long.");
		return false;
	}
	// Every address the name has, IPv6 and IPv4, tried in the order the
	// system prefers until one answers.
	char service[16];
	snprintf(service, sizeof(service), "%d", port);
	addrinfo hints = {}, *found = nil;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	std::string host = address;
	if(host.size() > 2 && host.front() == '[' && host.back() == ']')
		host = host.substr(1, host.size() - 2);	// [2001:db8::1]
	if(!MpStartNetwork() || getaddrinfo(host.c_str(), service, &hints, &found) != 0 || found == nil){
		*error = _("Could not find that address.");
		return false;
	}
	gRelay = MpConnection();
	gCandidates.clear();
	for(addrinfo *a = found; a; a = a->ai_next){
		Candidate c = {};
		memcpy(&c.address, a->ai_addr, std::min((size_t)a->ai_addrlen, sizeof(c.address)));
		c.length = (socklen_t)a->ai_addrlen;
		gCandidates.push_back(c);
	}
	freeaddrinfo(found);
	if(!connectNext()){
		*error = _("Could not connect.");
		return false;
	}
	snprintf(gAddress, sizeof(gAddress), "%s", address);
	snprintf(gPassword, sizeof(gPassword), "%s", password);
	MpCopyName(gName, name);
	gPort = port;
	gState = MP_CONNECTING;
	gConnectStarted = gRelay.heard = MpNow();
	// The relay speaks first (MSG_CHALLENGE).
	gStatus.clear();
	return true;
}

bool
MultiplayerJoin(const char *address, int port, const char *password, const char *name, std::string *error)
{
	if(gState != MP_OFF || gHosting)
		MultiplayerLeave();
	gAsAgent = IsBackgroundWorker();
	gJoin = MultiplayerJoinRequest();
	// A copied address or password often brings a space or line end along,
	// unseen in the hidden field: it would count as a wrong password.
	auto trimmed = [](const char *text){
		std::string s = text ? text : "";
		size_t first = s.find_first_not_of(" \t\r\n");
		size_t last = s.find_last_not_of(" \t\r\n");
		return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
	};
	std::string cleanAddress = trimmed(address), cleanPassword = trimmed(password);
	return joinRelay(cleanAddress.c_str(), port, cleanPassword.c_str(), name, error);
}

// This computer's public IPv6 address, if it has one: what a route to the
// internet starts from (connecting a UDP socket sends nothing).
static std::string
publicIpv6(void)
{
	MpSocket s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if(s == MP_NO_SOCKET)
		return "";
	sockaddr_in6 to = {};
	to.sin6_family = AF_INET6;
	to.sin6_port = htons(53);
	inet_pton(AF_INET6, "2001:4860:4860::8888", &to.sin6_addr);
	sockaddr_in6 local = {};
	socklen_t length = sizeof(local);
	char text[64] = "";
	if(connect(s, (sockaddr*)&to, sizeof(to)) == 0 && getsockname(s, (sockaddr*)&local, &length) == 0 &&
	   (local.sin6_addr.s6_addr[0] & 0xe0) == 0x20)	// global unicast, 2000::/3
		inet_ntop(AF_INET6, &local.sin6_addr, text, sizeof(text));
	MpCloseSocket(s);
	return text;
}
static std::string gIpv6;	// while hosting

bool
MultiplayerHost(int port, const char *name, std::string *error, bool openRouterPort, bool thisComputerOnly)
{
	if(gState != MP_OFF || gHosting)
		MultiplayerLeave();
	resetSession();
	if(!isSA() || IsIplMapDocumentOpen()){
		*error = _("Multiplayer edits Pawn maps of San Andreas.");
		return false;
	}
	static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	std::random_device random;
	char code[7];
	for(int i = 0; i < 6; i++)
		code[i] = letters[random() % (sizeof(letters) - 1)];
	code[6] = '\0';
	MpRelayOptions options;
	options.port = port;
	if(thisComputerOnly)
		options.bindAddress = "127.0.0.1";
	options.password = code;
	options.passwordIgnoresCase = true;
	MpCopyName(gName, name);
	options.name = gName;
	options.log = relayLog;
	std::string relayError;
	if(!MpRelayStart(options, &relayError)){
		char text[128];
		snprintf(text, sizeof(text), _("Port %d is in use or not allowed."), port);
		*error = text;
		return false;
	}
	gHosting = true;
	gJoin = MultiplayerJoinRequest();
	// Our own map seeds our own relay, as the first editor into haumea-server's does.
	if(!joinRelay("127.0.0.1", MpRelayPort(), code, name, error)){
		resetSession();
		return false;
	}
	if(openRouterPort && !thisComputerOnly)
		UpnpOpenPort(MpRelayPort());
	gIpv6 = thisComputerOnly ? "" : publicIpv6();
	log("Multiplayer: hosting on port %d\n", MpRelayPort());
	return true;
}

static void
lostRelay(const char *why)
{
	gStatus = why;
	ToastFor(TOAST_SAVE, 6.0f, "%s", why);
	log("Multiplayer: %s\n", why);
	resetSession();
}

// 1 connected, 0 not yet, -1 refused or unreachable.
static int
socketConnected(MpSocket socket)
{
	fd_set writable, failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(socket, &writable);
	FD_SET(socket, &failed);
	timeval zero = {0, 0};
	if(select((int)socket + 1, nil, &writable, &failed, &zero) <= 0)
		return 0;
	int error = 0;
	socklen_t length = sizeof(error);
	getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&error, &length);
	return error != 0 || FD_ISSET(socket, &failed) ? -1 : 1;
}

static void
welcomed(const MpFrame &frame)
{
	MpReader r(frame.payload);
	char name[MP_NAME_LEN];
	uint32 counted = 0;
	char seed = 0;
	if(!r.get(&gSelf) || !r.get(&counted) || !r.get(&seed) || !r.bytes(name, sizeof(name)) ||
	   gSelf == 0 || gSelf > MP_MAX_PEER_ID){
		lostRelay(_("The session sent something this version can't read."));
		return;
	}
	name[MP_NAME_LEN - 1] = '\0';
	MpCopyName(gSessionName, name);
	gOffers = 0;
	if(r.left >= 4)
		r.get(&gOffers);
	char role = 2;
	if(r.left >= 1)
		r.get(&role);
	gRole = role;
	uint32 roomSize = 0;
	gRoomName.clear();
	if(r.left >= 4 && r.get(&roomSize) && roomSize <= r.left){
		gRoomName.assign(r.at, roomSize);
		r.at += roomSize;
		r.left -= roomSize;
	}
	gNextElement = counted;
	gWorldIndex.clear();
	if(seed){
		// An empty session, or one our map replaces: ours becomes it.
		seedSession();
		gState = MP_JOINED;
		if(gReplacing)
			Toast(TOAST_SAVE, _("Your map is now everyone's in %s"), gSessionName);
		else if(!gHosting)
			Toast(TOAST_SAVE, _("Your map started the session on %s"), gSessionName);
		gReplacing = false;
		rememberServer();
		log("Multiplayer: seeded %s:%d as peer %u\n", gAddress, gPort, gSelf);
		return;
	}
	// The session's map replaces ours, and its textdraw file ours (untitled:
	// saving it never writes over our own file).
	gChatHistory = true;
	gChat.clear();
	NewPawnMapDocument();
	NewTextDrawFile();
	ResetUndoHistory();
}

// The relay's key and password salt: ours back, then everything is sealed,
// our hello first.
static void
challenged(const MpFrame &frame)
{
	MpReader r(frame.payload);
	uint32 magic = 0, protocol = 0;
	uint8_t salt[MP_SALT_LEN], relayKey[MP_KEY_LEN], passwordKey[MP_KEY_LEN];
	char caseless = 0;
	r.get(&magic);
	r.get(&protocol);
	r.bytes(salt, sizeof(salt));
	r.bytes(relayKey, sizeof(relayKey));
	r.get(&caseless);
	char mode = 0;
	uint8_t identity[32] = {}, signature[64] = {};
	r.get(&mode);
	r.bytes(identity, sizeof(identity));
	r.bytes(signature, sizeof(signature));
	if(!r.ok || magic != MP_MAGIC){
		lostRelay(_("That isn't a Haumea session."));
		return;
	}
	gAccountsMode = mode != 0;
	static const uint8_t none[32] = {};
	bool hasIdentity = memcmp(identity, none, sizeof(identity)) != 0;
	gFingerprint = hasIdentity ? MpFingerprint(identity) : "";
	if(hasIdentity){
		// It signed what we sent and what it answers with: someone in the middle couldn't.
		std::string signedPart(MP_IDENTITY_CONTEXT);
		signedPart.append((const char*)gNonce, sizeof(gNonce));
		signedPart.append((const char*)salt, sizeof(salt));
		signedPart.append((const char*)relayKey, sizeof(relayKey));
		signedPart += mode;
		if(crypto_eddsa_check(signature, identity, (const uint8_t*)signedPart.data(), signedPart.size()) != 0){
			lostRelay(_("The server's signature didn't check out: something may be between you and it. Not joined."));
			return;
		}
	}
	// The server we know by its fingerprint: another one now isn't it.
	if(!gServersLoaded)
		loadServers();
	for(const ServerEntry &e : gServers)
		if(e.address == gAddress && e.port == gPort && !e.pin.empty() && e.pin != gFingerprint){
			char why[400];
			snprintf(why, sizeof(why), _("This server isn't the one you joined before (its fingerprint was %s, now %s). "
				"Something may be between you and it. If its owner set it up again, forget it in Servers and join again."),
				e.pin.c_str(), gFingerprint.empty() ? _("none") : gFingerprint.c_str());
			lostRelay(why);
			return;
		}
	if(protocol != MP_PROTOCOL){
		lostRelay(_("The session runs a different Haumea version: update both to the same one."));
		return;
	}
	std::string password = gPassword;
	if(gJoin.kind == MP_LOGIN_INVITE || gJoin.kind == MP_LOGIN_AGENT){
		size_t dash = password.find('-');
		password = dash == std::string::npos ? std::string() : password.substr(dash + 1);
	}
	if(caseless)
		for(char &ch : password)
			ch = (char)toupper((unsigned char)ch);
	MpPasswordKey(password, salt, passwordKey);
	MpNewKeyPair(gRelay);
	sendToRelay(MSG_KEY, std::string((const char*)gRelay.publicKey, MP_KEY_LEN));
	MpSeal(gRelay, false, relayKey, gRelay.publicKey, passwordKey);
	crypto_wipe(passwordKey, sizeof(passwordKey));
	std::string hello;
	MpPut(hello, MP_MAGIC);
	MpPut(hello, MP_PROTOCOL);
	MpPut(hello, (uint32)sizeof(ObjectPawnProperties));
	MpPut(hello, (uint32)sizeof(ObjectMaterialOverride));
	MpPut(hello, (uint32)sizeof(VehicleState));
	MpPut(hello, (int32)params.map);
	hello.append(gName, MP_NAME_LEN);
	hello += (char)(gAsAgent ? 1 : 0);
	for(const std::string *s : { &gJoin.room, &gJoin.roomPassword, &gJoin.newName, &gJoin.newPassword, &gJoin.owner }){
		MpPut(hello, (uint32)s->size());
		hello += *s;
	}
	sendToRelay(MSG_HELLO, hello);
}

static void
updateClient(void)
{
	if(!gRelayConnected){
		int connected = socketConnected(gRelay.socket);
		if(connected < 0 || MpNow() - gCandidateStarted > MP_CANDIDATE_TIMEOUT){
			// This address doesn't answer: the next one, if any.
			if(!connectNext()){
				lostRelay(connected < 0 ? _("Could not connect to the session.") : _("The session didn't answer."));
				return;
			}
			gConnectStarted = gRelay.heard = MpNow();
			return;
		}
		if(connected == 0){
			if(MpNow() - gConnectStarted > MP_CONNECT_TIMEOUT)
				lostRelay(_("The session didn't answer."));
			return;
		}
		gRelayConnected = true;
		gRelay.heard = MpNow();
		// How we sign in, in the clear: the server answers with that one's challenge.
		std::string login, who;
		if(gJoin.kind == MP_LOGIN_ACCOUNT)
			who = gJoin.account;
		else if(gJoin.kind == MP_LOGIN_INVITE || gJoin.kind == MP_LOGIN_AGENT)
			who = std::string(gPassword).substr(0, std::string(gPassword).find('-'));
		MpRandom(gNonce, sizeof(gNonce));
		MpPut(login, MP_MAGIC);
		MpPut(login, MP_PROTOCOL);
		login += (char)gJoin.kind;
		MpPut(login, (uint32)who.size());
		login += who;
		login.append((const char*)gNonce, sizeof(gNonce));
		sendToRelay(MSG_LOGIN, login);
	}
	std::vector<MpFrame> frames;
	bool alive = MpReceive(gRelay, frames);
	for(const MpFrame &frame : frames){
		if(gState == MP_OFF)
			return;
		switch(frame.type){
		case MSG_REJECT:
			lostRelay((std::string(_("Refused: ")) + frame.payload.substr(0, 200)).c_str());
			return;
		case MSG_CHALLENGE:
			if(!gRelay.sealed)
				challenged(frame);
			break;
		case MSG_WELCOME:
			welcomed(frame);
			break;
		case MSG_CHANNEL:
			if(gSelf && frame.payload.size() >= 5){
				uint32 origin;
				memcpy(&origin, frame.payload.data(), 4);
				uint8 channel = (uint8)frame.payload[4];
				if(origin != gSelf && channel > 0 && channel < MP_CHANNEL_COUNT)
					applyChannel(channel, frame.payload.substr(5));
			}
			break;
		case MSG_ELEMENT:
			if(gSelf && frame.payload.size() > 4){
				uint32 origin;
				memcpy(&origin, frame.payload.data(), 4);
				receiveElement(origin, frame.payload.substr(4));
			}
			break;
		case MSG_PRESENCE: {
			Peer peer;
			if(readPresence(frame.payload, &peer) && peer.id != gSelf){
				auto known = gPeers.find(peer.id);
				if(known == gPeers.end() && gState == MP_JOINED)
					ToastFor(TOAST_SAVE, 4.0f, _("%s joined the session"), peer.name);
				// Their first real position, or another interior: no flight there.
				bool follow = known != gPeers.end() && known->second.area == peer.area &&
					rw::length(known->second.camera) > 0.001f;
				peer.shownCamera = follow ? known->second.shownCamera : peer.camera;
				peer.shownTarget = follow ? known->second.shownTarget : peer.target;
				gPeers[peer.id] = peer;
			}
			break;
		}
		case MSG_FILE:
			if(gSelf)
				receiveFile(frame.payload);
			break;
		case MSG_CHAT:
			if(gSelf)
				receiveChat(frame.payload);
			break;
		case MSG_ROOMS:
			receiveRooms(frame.payload);
			break;
		case MSG_NOTICE:
			Toast(TOAST_SAVE, "%s", frame.payload.substr(0, 300).c_str());
			gStatus = frame.payload.substr(0, 300);
			break;
		case MSG_ADMIN_REPLY: {
			MpReader r(frame.payload);
			char ok = 0;
			uint32 size = 0;
			r.get(&ok);
			std::string text, data;
			if(r.get(&size) && size <= r.left){ text.assign(r.at, size); r.at += size; r.left -= size; }
			if(r.get(&size) && size <= r.left){ data.assign(r.at, size); r.at += size; r.left -= size; }
			if(gAgentTokenAsked && ok && text == "an agent token for a day"){
				gAgentToken = data;
				gAgentTokenAsked = false;
				break;
			}
			gAdminOk = ok != 0;
			gAdminText = text;
			gAdminData = data;
			gAdminSeq++;
			if(!ok)
				Toast(TOAST_SAVE, "%s", text.c_str());
			break;
		}
		case MSG_PONG:
			if(frame.payload.size() >= sizeof(double)){
				double sent;
				memcpy(&sent, frame.payload.data(), sizeof(sent));
				gPingMs = (int)((MpNow() - sent) * 1000.0 + 0.5);
			}
			break;
		case MSG_SAVE_REQUEST:
			if(gSelf && gState == MP_JOINED)
				sendMapFile(1, nullptr);
			break;
		case MSG_RESET:
			if(gSelf){
				// Someone's map replaces the session's: it follows, as when joining.
				clearElements();
				gState = MP_CONNECTING;
				gChatHistory = true;
				gChat.clear();
				Toast(TOAST_SAVE, _("%s is loading another map for everyone..."), frame.payload.substr(0, 40).c_str());
			}
			break;
		case MSG_PEER_LEFT: {
			uint32 peer;
			MpReader r(frame.payload);
			if(r.get(&peer) && gPeers.count(peer)){
				ToastFor(TOAST_SAVE, 4.0f, _("%s left the session"), gPeers[peer].name);
				gPeers.erase(peer);
			}
			break;
		}
		case MSG_SNAPSHOT_DONE:
			if(gSelf && gState == MP_CONNECTING){
				retryUnresolved();
				sendChanges(true);
				// Our own custom models, beyond the session's, go to everyone.
				gChannelBaseline[MP_CHANNEL_CUSTOM_MODELS] = gReceivedModels;
				gChangedByOthers.clear();
				gState = MP_JOINED;
				gChatHistory = false;
				rememberServer();
				Toast(TOAST_SAVE, _("Joined %s's session"), gSessionName);
				log("Multiplayer: joined %s:%d as peer %u\n", gAddress, gPort, gSelf);
			}
			break;
		}
	}
	if(gState == MP_OFF)
		return;	// a frame ended it, and said why: that stays the reason
	if(!alive){
		lostRelay(gState == MP_JOINED ? _("The session ended.") : _("The session closed the connection."));
		return;
	}
	if(!MpFlush(gRelay) || MpNow() - gRelay.heard > MP_TIMEOUT)
		lostRelay(_("Lost the connection to the session."));
}

/*
 * Rooms, admin, ping (servers with accounts)
 */

static void
receiveRooms(const std::string &payload)
{
	MpReader r(payload);
	uint32 count = 0;
	r.get(&count);
	std::vector<LobbyRoom> rooms;
	auto text = [&](std::string *out){
		uint32 size = 0;
		if(!r.get(&size) || size > r.left)
			return false;
		out->assign(r.at, size);
		r.at += size;
		r.left -= size;
		return true;
	};
	for(uint32 i = 0; i < count && r.ok && i < 1000; i++){
		LobbyRoom room;
		char locked = 0, role = 0;
		uint32 people = 0;
		if(!text(&room.name) || !r.get(&locked) || !r.get(&people) || !r.get(&role) || !text(&room.people))
			break;
		room.locked = locked != 0;
		room.count = (int)people;
		room.role = role;
		rooms.push_back(room);
	}
	char mine = 0;
	if(r.ok && r.left)
		r.get(&mine);
	gLobby = rooms;
	gAccountRole = mine;
	if(gState == MP_CONNECTING && gSelf == 0){
		gState = MP_LOBBY;
		rememberServer();
	}
}

bool
MultiplayerEnterRoom(const char *room, const char *roomPassword)
{
	if(gState != MP_LOBBY || room == nil || room[0] == '\0')
		return false;
	std::string payload;
	std::string name = room, password = roomPassword ? roomPassword : "";
	MpPut(payload, (uint32)name.size());
	payload += name;
	MpPut(payload, (uint32)password.size());
	payload += password;
	gJoin.room = name;
	gState = MP_CONNECTING;
	sendToRelay(MSG_ENTER, payload);
	return MpFlush(gRelay);
}

bool
MultiplayerAdmin(int op, const char *a, const char *b, const char *c)
{
	if(gState != MP_JOINED && gState != MP_LOBBY)
		return false;
	std::string payload(1, (char)op);
	for(const char *s : { a, b, c }){
		std::string v = s ? s : "";
		MpPut(payload, (uint32)v.size());
		payload += v;
	}
	sendToRelay(MSG_ADMIN, payload);
	return MpFlush(gRelay);
}

std::string
MultiplayerAgentToken(void)
{
	if(!gAgentToken.empty() || !gAccountsMode)
		return gAgentToken;
	if(!gAgentTokenAsked && MultiplayerAdmin(MP_ADMIN_AGENT_TOKEN))
		gAgentTokenAsked = true;
	return "";
}

// Every two seconds: how long the server takes to answer.
static void
sendPing(void)
{
	double now = MpNow();
	if(now - gLastPing < 2.0)
		return;
	gLastPing = now;
	sendToRelay(MSG_PING, std::string((const char*)&now, sizeof(now)));
}

std::string
MultiplayerJoinInfoJson(void)
{
	auto text = [](const std::string &s){
		std::string out = "\"";
		for(unsigned char ch : s){
			if(ch == '"' || ch == '\\') { out += '\\'; out += (char)ch; }
			else if(ch >= 32) out += (char)ch;
		}
		return out + "\"";
	};
	bool in = gState == MP_JOINED || gState == MP_HOSTING;
	// With accounts the agents get a token instead (multiplayer_agent_token): no password goes out.
	std::string code = in && !gAccountsMode ? std::string(gPassword) : std::string();
	return std::string("\"in_session\":") + (in ? "true" : "false") +
		",\"hosting\":" + (gHosting ? "true" : "false") +
		",\"accounts\":" + (gAccountsMode ? "true" : "false") +
		",\"address\":" + text(gHosting ? "127.0.0.1" : gAddress) +
		",\"port\":" + std::to_string(GetMultiplayerPort()) +
		",\"room\":" + text(gRoomName) +
		",\"name\":" + text(gName) +
		",\"code\":" + text(code);
}

bool
MultiplayerJoinWith(const MultiplayerJoinRequest &join, std::string *error)
{
	if(gState != MP_OFF || gHosting)
		MultiplayerLeave();
	gAsAgent = IsBackgroundWorker() || join.kind == MP_LOGIN_AGENT;
	gJoin = join;
	std::string name = join.kind == MP_LOGIN_ACCOUNT ? join.account : join.kind == MP_LOGIN_INVITE ? join.newName : join.name;
	auto trimmed = [](const std::string &text){
		size_t first = text.find_first_not_of(" \t\r\n");
		size_t last = text.find_last_not_of(" \t\r\n");
		return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
	};
	gJoin.address = trimmed(join.address);
	gJoin.account = trimmed(join.account);
	gJoin.room = trimmed(join.room);
	gJoin.newName = trimmed(join.newName);
	std::string password = trimmed(join.password);
	if(join.kind == MP_LOGIN_INVITE && gJoin.newPassword.size() < 8){
		*error = _("Choose a password of 8 characters or more.");
		return false;
	}
	return joinRelay(gJoin.address.c_str(), join.port, password.c_str(), name.c_str(), error);
}

/*
 * Chat
 */

static void
receiveChat(const std::string &payload)
{
	MpReader r(payload);
	ChatLine line;
	char agent = 0;
	int64_t when = 0;
	float where[6];
	int32 area = -1;
	uint32 size = 0;
	r.get(&line.peer);
	r.bytes(line.name, MP_NAME_LEN);
	r.get(&agent);
	r.get(&when);
	r.bytes(where, sizeof(where));
	r.get(&area);
	r.get(&size);
	if(!r.ok || size > MP_MAX_CHAT || size > r.left)
		return;
	line.text.assign(r.at, size);
	line.name[MP_NAME_LEN - 1] = '\0';
	line.agent = agent != 0;
	line.time = when;
	line.camera = { where[0], where[1], where[2] };
	line.target = { where[3], where[4], where[5] };
	line.area = area;
	line.seq = ++gChatSeq;
	gChat.push_back(line);
	while(gChat.size() > (size_t)MP_CHAT_HISTORY)
		gChat.pop_front();
	if(!gChatHistory && line.peer != gSelf)
		ToastFor(TOAST_SAVE, 6.0f, "%s%s: %s", line.name, line.agent ? " [AI]" : "", line.text.substr(0, 120).c_str());
}

bool
MultiplayerSay(const char *text)
{
	if(gState != MP_JOINED || text == nil)
		return false;
	std::string said = text;
	size_t first = said.find_first_not_of(" \t\r\n");
	if(first == std::string::npos)
		return false;
	said = said.substr(first, MP_MAX_CHAT);
	std::string payload;
	rw::V3d camera = gPresencePoseSet ? gPresenceCamera : TheCamera.m_position;
	rw::V3d target = gPresencePoseSet ? gPresenceTarget : TheCamera.m_target;
	float where[6] = { camera.x, camera.y, camera.z, target.x, target.y, target.z };
	payload.append((const char*)where, sizeof(where));
	MpPut(payload, (int32)currentArea);
	MpPut(payload, (uint32)said.size());
	payload += said;
	sendToRelay(MSG_CHAT, payload);
	return MpFlush(gRelay);
}

std::string
MultiplayerChatJson(unsigned int since)
{
	auto text = [](const std::string &s){
		std::string out = "\"";
		for(unsigned char ch : s){
			if(ch == '"' || ch == '\\') { out += '\\'; out += (char)ch; }
			else if(ch >= 32) out += (char)ch;
		}
		return out + "\"";
	};
	std::string json = "\"self\":" + std::to_string(gSelf) + ",\"name\":" + text(gName) + ",\"messages\":[";
	bool first = true;
	for(const ChatLine &line : gChat){
		if(line.seq <= since)
			continue;
		char head[160];
		snprintf(head, sizeof(head), "%s{\"seq\":%u,\"peer\":%u,\"agent\":%s,\"mine\":%s,\"time\":%lld,\"name\":",
			first ? "" : ",", line.seq, line.peer, line.agent ? "true" : "false",
			line.peer == gSelf ? "true" : "false", line.time);
		json += head + text(line.name) + ",\"text\":" + text(line.text) + "}";
		first = false;
	}
	return json + "],\"last\":" + std::to_string(gChatSeq);
}

/*
 * The map as Pawn, for a relay that keeps it
 */

static std::string
openMapName(void)
{
	std::string path = GetPawnMapPath();
	size_t slash = path.find_last_of("/\\");
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if(dot != std::string::npos)
		name = name.substr(0, dot);
	return name.empty() ? std::string("session") : name;
}

// reason 0: someone saved it (path: the file they saved); 1: the relay's autosave.
static void
sendMapFile(int reason, const char *path)
{
	if(!(gOffers & MP_OFFERS_MAPS) || gState != MP_JOINED)
		return;
	std::string pawn, exported;
	if(path)
		readBytes(path, &pawn);
	else{
		const char *folder = getenv("TMPDIR");
#ifdef _WIN32
		if(folder == nil) folder = getenv("TEMP");
#endif
		exported = std::string(folder ? folder : "/tmp") + "/haumea-session-" + std::to_string((long long)gSelf) + "-" +
			std::to_string((long long)MpNow()) + ".pwn";
		if(ExportOpenPawnMap(exported.c_str()))
			readBytes(exported, &pawn);
		remove(exported.c_str());
	}
	if(pawn.empty() || pawn.size() > MP_MAX_MAP_FILE)
		return;
	std::string payload(1, (char)reason);
	std::string name = openMapName();
	MpPut(payload, (uint32)name.size());
	payload += name;
	MpPut(payload, (uint32)pawn.size());
	payload += pawn;
	sendToRelay(MSG_MAP_FILE, payload);
}

void
MultiplayerMapSaved(const char *path)
{
	sendMapFile(0, path);
}

/*
 * Our map, everyone's
 */

bool
MultiplayerReplacingMap(void)
{
	return gReplacing;
}

bool
MultiplayerLoadForEveryone(const char *path, const char *adminPassword, std::string *error)
{
	if(gState != MP_JOINED || !(gOffers & MP_OFFERS_REPLACE)){
		*error = _("This session doesn't let a map replace its own.");
		return false;
	}
	// Nothing of the old map goes to the session while the new one opens.
	gReplacing = true;
	clearElements();
	gState = MP_CONNECTING;
	if(!OpenPawnMapFile(path)){
		gReplacing = false;
		*error = _("Could not open that map: you left the session with the map you had.");
		MultiplayerLeave();
		return false;
	}
	std::string payload;
	std::string admin = adminPassword ? adminPassword : "";
	MpPut(payload, (uint32)admin.size());
	payload += admin;
	sendToRelay(MSG_REPLACE, payload);
	MpFlush(gRelay);
	return true;
}

/*
 * Servers we joined, and favourites
 */

static std::string
serversPath(void)
{
	char path[1024];
	return GetHaumeaDataPath(path, sizeof(path), "multiplayer_servers.txt") ? std::string(path) : std::string();
}

static void
loadServers(void)
{
	gServersLoaded = true;
	gServers.clear();
	std::string bytes;
	if(!readBytes(serversPath(), &bytes))
		return;
	size_t at = 0;
	while(at < bytes.size()){
		size_t end = bytes.find('\n', at);
		std::string line = bytes.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = end == std::string::npos ? bytes.size() : end + 1;
		std::vector<std::string> fields;
		size_t from = 0;
		while(true){
			size_t tab = line.find('\t', from);
			fields.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
			if(tab == std::string::npos) break;
			from = tab + 1;
		}
		if(fields.size() < 4 || fields[1].empty())
			continue;
		ServerEntry entry;
		entry.favourite = fields[0] == "favourite";
		entry.address = fields[1];
		entry.port = atoi(fields[2].c_str());
		entry.name = fields[3];
		entry.password = fields.size() > 4 && entry.favourite ? fields[4] : "";
		if(fields.size() > 8){
			entry.kind = atoi(fields[5].c_str());
			entry.account = fields[6];
			entry.room = fields[7];
			entry.pin = fields[8];
		}
		if(entry.port > 0 && entry.port < 65536)
			gServers.push_back(entry);
	}
}

static void
saveServers(void)
{
	if(SettingsAreReadOnly())
		return;
	std::string out;
	for(const ServerEntry &e : gServers)
		out += std::string(e.favourite ? "favourite" : "recent") + "\t" + e.address + "\t" + std::to_string(e.port) +
			"\t" + e.name + "\t" + (e.favourite ? e.password : "") + "\t" + std::to_string(e.kind) + "\t" + e.account +
			"\t" + e.room + "\t" + e.pin + "\n";
	std::string path = serversPath();
	if(path.empty())
		return;
	writeBytes(path, out);
#ifndef _WIN32
	chmod(path.c_str(), 0600);	// it can hold passwords
#endif
}

static bool gRememberPassword;

// A session we got into: first in the list (favourites keep their place).
static void
rememberServer(void)
{
	if(gHosting || gAsAgent || gAddress[0] == '\0' || gJoin.kind == MP_LOGIN_AGENT)
		return;
	if(!gServersLoaded)
		loadServers();
	ServerEntry entry;
	entry.favourite = false;
	entry.address = gAddress;
	entry.name = gSessionName[0] ? gSessionName : gAddress;
	entry.port = gPort;
	// An invite made an account: from now on that's how we sign in.
	entry.kind = gJoin.kind == MP_LOGIN_INVITE ? MP_LOGIN_ACCOUNT : gJoin.kind;
	entry.account = gJoin.kind == MP_LOGIN_INVITE ? gJoin.newName : gJoin.account;
	entry.room = gRoomName;
	entry.pin = gFingerprint;
	for(size_t i = 0; i < gServers.size(); i++)
		if(gServers[i].address == entry.address && gServers[i].port == entry.port){
			entry.favourite = gServers[i].favourite;
			entry.password = gServers[i].password;
			gServers.erase(gServers.begin() + i);
			break;
		}
	if(entry.favourite)
		entry.password = gRememberPassword ? (gJoin.kind == MP_LOGIN_INVITE ? gJoin.newPassword : std::string(gPassword)) : "";
	gServers.insert(gServers.begin(), entry);
	// Ten that aren't favourites at most.
	int recent = 0;
	for(size_t i = 0; i < gServers.size();){
		if(!gServers[i].favourite && ++recent > 10)
			gServers.erase(gServers.begin() + i);
		else
			i++;
	}
	saveServers();
}

/*
 * The rest of the editor
 */

void
MultiplayerLeave(void)
{
	if(gState != MP_OFF)
		MpFlush(gRelay);
	if(gState != MP_OFF || gHosting)
		log("Multiplayer: left the session\n");
	resetSession();
}

// Every frame: each marker a little closer to where its person last was.
static void
followPeers(void)
{
	static double last;
	double now = MpNow();
	float dt = (float)std::min(now - last, 0.25);
	last = now;
	for(auto &it : gPeers){
		Peer &peer = it.second;
		rw::V3d gap = rw::sub(peer.camera, peer.shownCamera);
		float time = rw::length(gap) > MP_GLIDE_GAP ? MP_GLIDE_TIME : MP_FOLLOW_TIME;
		float step = 1.0f - expf(-dt / time);
		peer.shownCamera = rw::add(peer.shownCamera, rw::scale(gap, step));
		peer.shownTarget = rw::add(peer.shownTarget, rw::scale(rw::sub(peer.target, peer.shownTarget), step));
	}
}

void
MultiplayerUpdate(void)
{
	if(gHosting)
		MpRelayUpdate();
	if(gState == MP_OFF)
		return;
	updateClient();
	if(gState == MP_LOBBY || gState == MP_JOINED){
		sendPing();
		MpFlush(gRelay);
	}
	if(gState != MP_JOINED)
		return;
	followPeers();
	if(!gUnresolved.empty())
		retryUnresolved();
	if(MpNow() - gLastSend >= MP_SEND_INTERVAL){
		gLastSend = MpNow();
		sendChanges(false);
	}
	sendPresence();
	if(!MpFlush(gRelay))
		lostRelay(_("Lost the connection to the session."));
	else if(gHosting)
		MpRelayUpdate();	// what we just sent, to the others this frame
}

bool
MultiplayerChangedByOthers(const ObjectInst *inst)
{
	return gState == MP_JOINED && gChangedByOthers.count(inst->m_id) != 0;
}

int
GetMultiplayerState(void)
{
	if(gHosting && gState != MP_OFF)
		return MP_HOSTING;
	return gState;
}

int GetMultiplayerPort(void) { return gHosting ? MpRelayPort() : gPort; }
const char *GetMultiplayerCode(void) { return gHosting ? gPassword : ""; }
int GetMultiplayerPeerCount(void) { return (int)gPeers.size(); }

static bool
projectPoint(const rw::V3d &point, ImVec2 *out)
{
	rw::V3d screen;
	float w, h;
	if(!Sprite::CalcScreenCoors(point, &screen, &w, &h, false))
		return false;
	*out = ImVec2(screen.x, screen.y);
	return true;
}

// Their name as shown, with a tag when an AI agent is behind it.
static std::string
shownName(const char *name, bool agent, const char *owner = nullptr)
{
	if(!agent)
		return name;
	std::string shown = std::string(name) + " [AI]";
	if(owner && owner[0])
		shown += std::string(" \xC2\xB7 via ") + owner;
	return shown;
}

// A name over the map: on a dark plate, readable over any road or wall.
static void
peerLabel(ImDrawList *draw, ImVec2 at, ImU32 colour, const char *name)
{
	float ui = GetUiScale();
	ImVec2 size = ImGui::CalcTextSize(name);
	draw->AddRectFilled(ImVec2(at.x - 4 * ui, at.y - 2 * ui), ImVec2(at.x + size.x + 4 * ui, at.y + size.y + 2 * ui),
		IM_COL32(0, 0, 0, 170), 3 * ui);
	draw->AddText(at, colour, name);
}

// The 3D view on screen: the dock space's middle, else the whole window.
static void
mapViewRect(ImVec2 *min, ImVec2 *max)
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	*min = vp->WorkPos;
	*max = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
	ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaDockSpace"));
	if(central && central->Size.x > 32.0f && central->Size.y > 32.0f){
		*min = central->Pos;
		*max = ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y);
	}
}

// A line that shows on any background: dark under, colour over.
static void
strongLine(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour, float width)
{
	draw->AddLine(a, b, IM_COL32(0, 0, 0, 190), width + 2.5f);
	draw->AddLine(a, b, colour, width);
}

static ImU32
withAlpha(ImU32 colour, int alpha)
{
	return (colour & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// Someone's camera: a dot where they are and the cone of what they see; at
// the edge of the view, pointing their way, when they're out of it.
static void
drawPeerCamera(ImDrawList *draw, const Peer &peer, ImU32 colour, float ui, ImVec2 viewMin, ImVec2 viewMax)
{
	rw::V3d cam = peer.shownCamera;
	rw::V3d forward = rw::sub(peer.shownTarget, cam);
	if(rw::length(forward) < 0.001f)
		forward = { 0.0f, 1.0f, 0.0f };
	forward = rw::normalize(forward);
	rw::V3d zaxis = { 0.0f, 0.0f, 1.0f };
	rw::V3d right = rw::cross(forward, zaxis);
	right = rw::length(right) < 0.001f ? rw::V3d{ 1.0f, 0.0f, 0.0f } : rw::normalize(right);
	rw::V3d up = rw::cross(right, forward);

	ImVec2 at;
	bool seen = projectPoint(cam, &at) && at.x >= viewMin.x && at.x <= viewMax.x && at.y >= viewMin.y && at.y <= viewMax.y;
	if(!seen){
		// Out of view: an arrow on the view's edge, towards them.
		rw::V3d viewAt = TheCamera.m_at;
		rw::V3d viewRight = rw::normalize(rw::cross(viewAt, TheCamera.m_up));
		rw::V3d viewUp = rw::cross(viewRight, viewAt);
		rw::V3d toPeer = rw::sub(cam, TheCamera.m_position);
		float dx = rw::dot(toPeer, viewRight), dy = -rw::dot(toPeer, viewUp);
		float len = sqrtf(dx * dx + dy * dy);
		if(len < 0.001f)
			return;
		dx /= len;
		dy /= len;
		ImVec2 centre((viewMin.x + viewMax.x) * .5f, (viewMin.y + viewMax.y) * .5f);
		float inset = 28 * ui;
		float hx = (viewMax.x - viewMin.x) * .5f - inset, hy = (viewMax.y - viewMin.y) * .5f - inset;
		float t = fminf(fabsf(dx) > 1e-4f ? hx / fabsf(dx) : 1e9f, fabsf(dy) > 1e-4f ? hy / fabsf(dy) : 1e9f);
		ImVec2 tip(centre.x + dx * t, centre.y + dy * t);
		ImVec2 side(-dy, dx);
		float s = 15 * ui;
		ImVec2 p1(tip.x + dx * s, tip.y + dy * s);
		ImVec2 p2(tip.x - dx * s * .6f + side.x * s * .8f, tip.y - dy * s * .6f + side.y * s * .8f);
		ImVec2 p3(tip.x - dx * s * .6f - side.x * s * .8f, tip.y - dy * s * .6f - side.y * s * .8f);
		draw->AddTriangleFilled(p1, p2, p3, colour);
		draw->AddTriangle(p1, p2, p3, IM_COL32(0, 0, 0, 220), 2.0f * ui);
		std::string shown = shownName(peer.name, peer.agent, peer.owner);
		ImVec2 size = ImGui::CalcTextSize(shown.c_str());
		ImVec2 label(tip.x - dx * (s + size.x * .5f) - size.x * .5f, tip.y - dy * (s + size.y) - size.y * .5f);
		peerLabel(draw, label, colour, shown.c_str());
		return;
	}

	// The cone: four corners a few metres ahead, as a camera's view.
	ImVec2 corner[4];
	bool cone = true;
	static const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
	for(int i = 0; i < 4; i++){
		rw::V3d p = rw::add(rw::add(cam, rw::scale(forward, 5.0f)),
			rw::add(rw::scale(right, 2.4f * sx[i]), rw::scale(up, 1.6f * sy[i])));
		cone &= projectPoint(p, &corner[i]);
	}
	if(cone){
		draw->AddQuadFilled(corner[0], corner[1], corner[2], corner[3], withAlpha(colour, 60));
		for(int i = 0; i < 4; i++){
			draw->AddTriangleFilled(at, corner[i], corner[(i + 1) % 4], withAlpha(colour, 28));
			strongLine(draw, at, corner[i], colour, 1.8f * ui);
			strongLine(draw, corner[i], corner[(i + 1) % 4], colour, 1.8f * ui);
		}
	}
	float r = 8.5f * ui;
	draw->AddCircleFilled(at, r + 2.5f * ui, IM_COL32(0, 0, 0, 220));
	draw->AddCircleFilled(at, r, colour);
	draw->AddCircle(at, r - 2.0f * ui, IM_COL32(255, 255, 255, 230), 0, 1.6f * ui);
	peerLabel(draw, ImVec2(at.x + r + 6 * ui, at.y - ImGui::GetFontSize() * .5f), colour, shownName(peer.name, peer.agent, peer.owner).c_str());
}

// A selection's box, in thick lines over the map.
static void
drawSelectionBox(ImDrawList *draw, const CBox &box, const rw::Matrix &matrix, ImU32 colour, float ui)
{
	ImVec2 p[8];
	for(int i = 0; i < 8; i++){
		rw::V3d local = { i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y, i & 4 ? box.max.z : box.min.z };
		rw::V3d world;
		rw::V3d::transformPoints(&world, &local, 1, &matrix);
		if(!projectPoint(world, &p[i]))
			return;	// partly behind the camera
	}
	static const int edges[12][2] = { {0,1}, {2,3}, {4,5}, {6,7}, {0,2}, {1,3}, {4,6}, {5,7}, {0,4}, {1,5}, {2,6}, {3,7} };
	for(const auto &e : edges)
		strongLine(draw, p[e[0]], p[e[1]], colour, 2.2f * ui);
}

void
MultiplayerOverlay(bool enabled)
{
	if(!enabled || gState != MP_JOINED || gPeers.empty())
		return;
#ifdef IMGUI_HAS_VIEWPORT
	ImDrawList *draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
#else
	ImDrawList *draw = ImGui::GetBackgroundDrawList();
#endif
	float ui = GetUiScale();
	ImVec2 viewMin, viewMax;
	mapViewRect(&viewMin, &viewMax);
	for(const auto &it : gPeers){
		const Peer &peer = it.second;
		ImU32 colour = peerColour(peer.id);
		if(peer.area == currentArea)
			drawPeerCamera(draw, peer, colour, ui, viewMin, viewMax);
		for(uint32 element : peer.selection){
			ObjectInst *inst = instanceOf(element);
			if(inst == nil || inst->m_isDeleted || inst->m_area != currentArea)
				continue;
			ObjectDef *obj = GetObjectDef(inst->m_objectId);
			if(obj && obj->m_colModel)
				drawSelectionBox(draw, obj->m_colModel->boundingBox, inst->m_matrix, colour, ui);
			ImVec2 at;
			if(element == peer.selection[0] && projectPoint(inst->m_translation, &at))
				peerLabel(draw, ImVec2(at.x + 6 * ui, at.y + 6 * ui), colour, shownName(peer.name, peer.agent, peer.owner).c_str());
		}
	}
}

static void
goToPeer(const Peer &peer)
{
	rw::V3d dir = rw::sub(peer.target, peer.camera);
	if(rw::length(dir) < 0.001f)
		return;
	TheCamera.m_position = peer.camera;
	TheCamera.m_target = peer.target;
	TheCamera.m_at = rw::normalize(dir);
}

static const char *
roleLabel(int role)
{
	switch(role){
	case MP_ROLE_VIEWER: return _("viewer");
	case MP_ROLE_BUILDER: return _("builder");
	case MP_ROLE_ADMIN: return _("admin");
	case MP_ROLE_OWNER: return _("owner");
	}
	return "";
}

// Green when quick, yellow when noticeable, red when slow.
static ImVec4
pingColour(int ms)
{
	return UiSemanticColor(ms < 80 ? UI_COLOR_SUCCESS : ms < 200 ? UI_COLOR_WARNING : UI_COLOR_ERROR);
}

// Signed in to a server with rooms: which one.
static void
lobbyPanel(std::string *error)
{
	ImGui::Text(_("Signed in to %s"), gAddress);
	if(gPingMs >= 0){
		ImGui::SameLine();
		ImGui::TextDisabled(_("%d ms"), gPingMs);
	}
	if(!gFingerprint.empty()){
		ImGui::TextDisabled(_("Server fingerprint: %s"), gFingerprint.c_str());
		ImGui::SetItemTooltip(_("Haumea remembers it, and won't join if another server answers here later."));
	}
	ImGui::SeparatorText(_("Rooms"));
	static char roomPassword[MP_PASSWORD_LEN];
	if(gLobby.empty())
		ImGui::TextDisabled(_("No rooms yet."));
	for(const LobbyRoom &room : gLobby){
		ImGui::PushID(room.name.c_str());
		if(ImGui::Button(_("Join")))
			MultiplayerEnterRoom(room.name.c_str(), roomPassword);
		ImGui::SameLine();
		ImGui::Text("%s%s", room.locked ? "\xF0\x9F\x94\x92 " : "", room.name.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled(_("%d in, you'd be a %s"), room.count, roleLabel(room.role));
		if(!room.people.empty())
			ImGui::SetItemTooltip("%s", room.people.c_str());
		ImGui::PopID();
	}
	bool anyLocked = false;
	for(const LobbyRoom &room : gLobby)
		anyLocked |= room.locked;
	if(anyLocked){
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		ImGui::InputText(_("Room password"), roomPassword, sizeof(roomPassword), ImGuiInputTextFlags_Password);
		ImGui::SetItemTooltip(_("For a locked room (admins don't need it)."));
	}
	if(gAccountRole >= MP_ROLE_ADMIN){
		static char newRoom[33];
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		ImGui::InputTextWithHint("##new room", _("new room's name"), newRoom, sizeof(newRoom));
		ImGui::SameLine();
		ImGui::BeginDisabled(newRoom[0] == '\0');
		if(ImGui::Button(_("Make room")))
			MultiplayerEnterRoom(newRoom, "");
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Your open map becomes the new room's."));
	}
	if(ImGui::Button(_("Refresh")))
		MultiplayerAdmin(MP_ADMIN_ROOMS);
	ImGui::SameLine();
	if(ImGui::Button(_("Leave")))
		MultiplayerLeave();
	(void)error;
}

// What admins do from the panel: invites, the room's lock, what agents may do here.
static void
adminPanel(void)
{
	if(!ImGui::CollapsingHeader(_("Admin")))
		return;
	static int inviteRole = 1, inviteUses = 1, inviteDays = 7;
	const char *roles[] = { roleLabel(MP_ROLE_VIEWER), roleLabel(MP_ROLE_BUILDER), roleLabel(MP_ROLE_ADMIN) };
	int roleCount = gRole >= MP_ROLE_OWNER ? 3 : 2;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
	ImGui::Combo(_("Invite as"), &inviteRole, roles, roleCount);
	inviteRole = clamp(inviteRole, 0, roleCount - 1);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
	ImGui::InputInt(_("Uses"), &inviteUses);
	inviteUses = clamp(inviteUses, 1, 100);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
	ImGui::InputInt(_("Days"), &inviteDays);
	inviteDays = clamp(inviteDays, 1, 365);
	static unsigned int inviteSeq;
	if(ImGui::Button(_("Make an invite code"))){
		MultiplayerAdmin(MP_ADMIN_INVITE, MpRoleName(inviteRole + MP_ROLE_VIEWER), std::to_string(inviteUses).c_str(),
			std::to_string(inviteDays).c_str());
		inviteSeq = gAdminSeq + 1;
	}
	if(inviteSeq && gAdminSeq >= inviteSeq && gAdminOk && !gAdminData.empty() && gAdminData.find('\t') == std::string::npos){
		ImGui::TextUnformatted(gAdminData.c_str());
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Copy##invite")))
			ImGui::SetClipboardText(gAdminData.c_str());
		ImGui::TextDisabled(_("Send it to them privately: it makes one account."));
	}
	ImGui::Spacing();
	static char lockPassword[MP_PASSWORD_LEN];
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::InputText(_("Room password##lock"), lockPassword, sizeof(lockPassword), ImGuiInputTextFlags_Password);
	ImGui::SameLine();
	if(ImGui::Button(_("Lock this room")) && lockPassword[0]){
		MultiplayerAdmin(MP_ADMIN_LOCK, lockPassword);
		lockPassword[0] = '\0';
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Unlock")))
		MultiplayerAdmin(MP_ADMIN_LOCK, "");
	static int agents = 0;
	const char *policies[] = { _("builders"), _("viewers"), _("not allowed") };
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	if(ImGui::Combo(_("AI agents here"), &agents, policies, 3)){
		static const char *names[] = { "builder", "viewer", "none" };
		MultiplayerAdmin(MP_ADMIN_AGENTS, names[agents]);
	}
}

static void
peerList(void)
{
	ImGui::SeparatorText(_("People"));
	ImGui::ColorButton("##self", ImGui::ColorConvertU32ToFloat4(peerColour(gSelf)),
		ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
	ImGui::SameLine();
	ImGui::Text(_("%s (you)"), gName);
	for(const auto &it : gPeers){
		const Peer &peer = it.second;
		ImGui::PushID((int)peer.id);
		ImGui::ColorButton("##colour", ImGui::ColorConvertU32ToFloat4(peerColour(peer.id)),
			ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
		ImGui::SameLine();
		ImGui::TextUnformatted(peer.name);
		if(peer.agent){
			ImGui::SameLine();
			ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), "AI");
			ImGui::SetItemTooltip(_("An AI agent (Namaka), not a person."));
			if(peer.owner[0]){
				ImGui::SameLine();
				ImGui::TextDisabled(_("via %s"), peer.owner);
				ImGui::SetItemTooltip(gAccountsMode ? _("Its account's: the server knows it from the agent's token.") :
					_("Whose it says it is: without accounts nothing proves it."));
			}
		}
		if(gAccountsMode){
			ImGui::SameLine();
			ImGui::TextDisabled("%s", roleLabel(peer.role));
		}
		if(peer.ping > 0){
			ImGui::SameLine();
			ImGui::TextColored(pingColour(peer.ping), _("%d ms"), peer.ping);
		}
		if(!peer.selection.empty()){
			ImGui::SameLine();
			ImGui::TextDisabled(_("%d selected"), (int)peer.selection.size());
		}
		if(gAccountsMode && gRole >= MP_ROLE_ADMIN && !peer.agent && ImGui::BeginPopupContextItem("##who", ImGuiPopupFlags_MouseButtonRight)){
			ImGui::TextDisabled("%s", peer.name);
			ImGui::Separator();
			int choices[] = { MP_ROLE_VIEWER, MP_ROLE_BUILDER, MP_ROLE_ADMIN };
			for(int to : choices){
				if(to >= MP_ROLE_ADMIN && gRole < MP_ROLE_OWNER)
					continue;
				char label[96];
				snprintf(label, sizeof(label), _("Make a %s here"), roleLabel(to));
				if(ImGui::MenuItem(label, nil, peer.role == to))
					MultiplayerAdmin(MP_ADMIN_SET_ROLE, peer.name, MpRoleName(to), gRoomName.c_str());
				snprintf(label, sizeof(label), _("Make a %s everywhere"), roleLabel(to));
				if(ImGui::MenuItem(label))
					MultiplayerAdmin(MP_ADMIN_SET_ROLE, peer.name, MpRoleName(to), "");
			}
			ImGui::Separator();
			if(ImGui::MenuItem(_("Remove from the room")))
				MultiplayerAdmin(MP_ADMIN_KICK, std::to_string(peer.id).c_str());
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Go to")))
			goToPeer(peer);
		ImGui::SetItemTooltip(_("Look from where they are looking."));
		ImGui::PopID();
	}
}

static void
goToChatLine(const ChatLine &line)
{
	rw::V3d dir = rw::sub(line.target, line.camera);
	if(rw::length(dir) < 0.001f)
		return;
	TheCamera.m_position = line.camera;
	TheCamera.m_target = line.target;
	TheCamera.m_at = rw::normalize(dir);
}

// The conversation, and a line to add to it.
static void
chatPanel(void)
{
	ImGui::SeparatorText(_("Chat"));
	static size_t seen;
	float height = ImGui::GetTextLineHeightWithSpacing() * 9.0f;
	if(ImGui::BeginChild("##chat", ImVec2(0, height), ImGuiChildFlags_Borders)){
		for(const ChatLine &line : gChat){
			ImGui::PushID((int)line.seq);
			time_t when = (time_t)line.time;
			struct tm local;
#ifdef _WIN32
			localtime_s(&local, &when);
#else
			localtime_r(&when, &local);
#endif
			char clock[8];
			strftime(clock, sizeof(clock), "%H:%M", &local);
			ImGui::TextDisabled("%s", clock);
			ImGui::SameLine();
			if(line.peer == 0){
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextWrapped("%s", line.text.c_str());
				ImGui::PopStyleColor();
			}else{
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(peerColour(line.peer)), "%s", line.name);
				if(line.agent){
					ImGui::SameLine(0, 4);
					ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), "AI");
				}
				ImGui::SameLine(0, 4);
				ImGui::TextWrapped("%s", line.text.c_str());
				if(line.area == currentArea && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					goToChatLine(line);
				if(line.area == currentArea)
					ImGui::SetItemTooltip(_("Double-click: look from where they were when they wrote this."));
			}
			ImGui::PopID();
		}
		if(gChat.size() != seen && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 3)
			ImGui::SetScrollHereY(1.0f);
		seen = gChat.size();
	}
	ImGui::EndChild();
	static char message[MP_MAX_CHAT + 1];
	ImGui::SetNextItemWidth(-ImGui::CalcTextSize(_("Send")).x - ImGui::GetStyle().FramePadding.x * 4);
	bool send = ImGui::InputTextWithHint("##say", _("Say something (@name to mention someone)"), message, sizeof(message),
		ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	send |= ImGui::Button(_("Send"));
	if(send && message[0]){
		MultiplayerSay(message);
		message[0] = '\0';
		ImGui::SetKeyboardFocusHere(-1);
	}
}

// Making our map everyone's: a file, and the admin password when the server asks for one.
static void
loadForEveryone(std::string *error)
{
	if(!(gOffers & MP_OFFERS_REPLACE))
		return;
	static char admin[MP_PASSWORD_LEN];
	if(gOffers & MP_REPLACE_NEEDS_ADMIN){
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		ImGui::InputText(_("Admin password"), admin, sizeof(admin), ImGuiInputTextFlags_Password);
		ImGui::SetItemTooltip(_("The server only lets who knows it load another map for everyone."));
	}
	bool ready = !(gOffers & MP_REPLACE_NEEDS_ADMIN) || admin[0];
	ImGui::BeginDisabled(!ready);
	if(ImGui::Button(_("Load a map for everyone..."))){
		char path[1024] = "";
		if(PickFileDialog(path, sizeof(path), _("Map for everyone"), _("Pawn maps"), "pwn;inc;pawn") && path[0]){
			error->clear();
			MultiplayerLoadForEveryone(path, admin, error);
		}
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(_("Replaces the session's map with one from a file, for everyone in it.\n"
		"Unsaved work in the session is lost; the server keeps a copy of the session as it was."));
}

// The Join form: how to sign in, and where.
static struct JoinForm
{
	int kind = 0;	// 0 the code, 1 an account, 2 an invite
	char account[MP_NAME_LEN] = "", password[MP_PASSWORD_LEN] = "", invite[64] = "";
	char newName[MP_NAME_LEN] = "", newPassword[MP_PASSWORD_LEN] = "";
	char room[33] = "", roomPassword[MP_PASSWORD_LEN] = "";
} gForm;

// Sessions we joined before; favourites first and kept.
static void
serverList(char *address, size_t addressSize, int *port, char *code, size_t codeSize)
{
	if(!gServersLoaded)
		loadServers();
	if(gServers.empty())
		return;
	ImGui::SeparatorText(_("Servers"));
	std::stable_sort(gServers.begin(), gServers.end(), [](const ServerEntry &a, const ServerEntry &b){
		return a.favourite && !b.favourite;
	});
	int remove = -1;
	bool changed = false;
	for(size_t i = 0; i < gServers.size(); i++){
		ServerEntry &e = gServers[i];
		ImGui::PushID((int)i);
		if(ImGui::SmallButton(e.favourite ? ICON_FA_STAR : "+")){
			e.favourite = !e.favourite;
			if(!e.favourite)
				e.password.clear();
			changed = true;
		}
		ImGui::SetItemTooltip(e.favourite ? _("A favourite: kept, at the top. Click to make it a recent one.") :
			_("Keep it as a favourite"));
		ImGui::SameLine();
		std::string label = (e.name.empty() ? e.address : e.name) + "  (" + e.address + ":" + std::to_string(e.port) + ")";
		bool chosen = strcmp(address, e.address.c_str()) == 0 && *port == e.port;
		if(ImGui::Selectable(label.c_str(), chosen, ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight(), 0))){
			snprintf(address, addressSize, "%s", e.address.c_str());
			*port = e.port;
			gForm.kind = e.kind == MP_LOGIN_ACCOUNT ? 1 : 0;
			snprintf(code, codeSize, "%s", e.kind == MP_LOGIN_CODE ? e.password.c_str() : "");
			snprintf(gForm.account, sizeof(gForm.account), "%s", e.account.c_str());
			snprintf(gForm.password, sizeof(gForm.password), "%s", e.kind == MP_LOGIN_ACCOUNT ? e.password.c_str() : "");
			snprintf(gForm.room, sizeof(gForm.room), "%s", e.room.c_str());
			gRememberPassword = !e.password.empty();
		}
		if(!e.password.empty())
			ImGui::SetItemTooltip("%s", _("Its password is kept on this computer."));
		ImGui::SameLine();
		if(ImGui::SmallButton("x"))
			remove = (int)i;
		ImGui::SetItemTooltip(_("Forget it"));
		ImGui::PopID();
	}
	if(remove >= 0){
		gServers.erase(gServers.begin() + remove);
		changed = true;
	}
	if(changed)
		saveServers();
}

void
DrawMultiplayerWindow(bool *open)
{
	if(open == nil || !*open)
		return;
	static char name[MP_NAME_LEN], address[256] = "127.0.0.1", code[MP_PASSWORD_LEN];
	static int hostPort = MP_DEFAULT_PORT, joinPort = MP_DEFAULT_PORT;
	static bool openRouterPort = true;
	static std::string error;
	if(name[0] == '\0'){
		const char *user = getenv("USER");
		if(user == nil) user = getenv("USERNAME");
		MpCopyName(name, user ? user : "Mapper");
	}
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_USER " Multiplayer###multiplayer", open)){
		ImGui::End();
		return;
	}
	float field = ImGui::GetFontSize() * 12.0f;
	bool ready = false;
	switch(GetMultiplayerState()){
	case MP_OFF:
		ImGui::TextWrapped(_("Edit one map together: one of you hosts the open map, or a haumea-server keeps it, "
			"and the others join."));
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Your name"), name, sizeof(name));
		ImGui::SeparatorText(_("Host"));
		ImGui::SetNextItemWidth(field);
		ImGui::InputInt(_("Port##host"), &hostPort);
		hostPort = clamp(hostPort, 1, 65535);
		ImGui::Checkbox(_("Open the port on my router"), &openRouterPort);
		ImGui::SetItemTooltip(_("Asks your router to forward the port to this computer (UPnP),\n"
			"so people join over the internet without setting it up by hand."));
		if(ImGui::Button(_("Host this map"))){
			error.clear();
			MultiplayerHost(hostPort, name, &error, openRouterPort);
		}
		ImGui::SetItemTooltip(_("Others join with this computer's address, the port and the session code.\n"
			"Over the internet the port must be forwarded to this computer, or use a VPN such as Tailscale.\n"
			"For a session that stays up without you, run haumea-server where your SA-MP server runs."));
		serverList(address, sizeof(address), &joinPort, code, sizeof(code));
		ImGui::SeparatorText(_("Join"));
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Address"), address, sizeof(address));
		ImGui::SetNextItemWidth(field);
		ImGui::InputInt(_("Port##join"), &joinPort);
		joinPort = clamp(joinPort, 1, 65535);
		// "##join_kind": the Account and Invite code fields below have the same labels.
		ImGui::RadioButton(_("Session code##join_kind"), &gForm.kind, 0);
		ImGui::SetItemTooltip(_("Someone hosting from Haumea, or a server with one password for everyone."));
		ImGui::SameLine();
		ImGui::RadioButton(_("Account##join_kind"), &gForm.kind, 1);
		ImGui::SetItemTooltip(_("A server with accounts: your name and password there."));
		ImGui::SameLine();
		ImGui::RadioButton(_("Invite code##join_kind"), &gForm.kind, 2);
		ImGui::SetItemTooltip(_("The first time on a server with accounts: the code you were sent makes your account."));
		ready = address[0] != '\0';
		if(gForm.kind == 0){
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Code or password"), code, sizeof(code), ImGuiInputTextFlags_Password);
			ImGui::SetItemTooltip(_("The host's session code, or the password haumea-server was started with."));
			ready &= code[0] != '\0';
		}else if(gForm.kind == 1){
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Account"), gForm.account, sizeof(gForm.account));
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Password"), gForm.password, sizeof(gForm.password), ImGuiInputTextFlags_Password);
			ready &= gForm.account[0] && gForm.password[0];
		}else{
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Invite code"), gForm.invite, sizeof(gForm.invite));
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Choose a name"), gForm.newName, sizeof(gForm.newName));
			ImGui::SetItemTooltip(_("Your account's name on this server: 2-31 letters, digits, _ - or ."));
			ImGui::SetNextItemWidth(field);
			ImGui::InputText(_("Choose a password"), gForm.newPassword, sizeof(gForm.newPassword), ImGuiInputTextFlags_Password);
			ImGui::SetItemTooltip(_("8 characters or more. You sign in with it from now on; the invite is used up."));
			ready &= gForm.invite[0] && gForm.newName[0] && strlen(gForm.newPassword) >= 8;
		}
		if(gForm.kind != 0){
			ImGui::SetNextItemWidth(field);
			ImGui::InputTextWithHint(_("Room"), _("empty: pick from the list"), gForm.room, sizeof(gForm.room));
		}
		ImGui::Checkbox(_("Remember the password for favourites"), &gRememberPassword);
		ImGui::SetItemTooltip(_("Kept in your Haumea settings on this computer, for servers you star."));
		if(PawnMapHasUnsavedChanges() || TextDrawFileHasUnsavedChanges())
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
				_("Joining replaces the open map and textdraw file: save them first to keep your changes."));
		ImGui::BeginDisabled(!ready);
		if(ImGui::Button(_("Join")) && ready){
			error.clear();
			if(gForm.kind == 0)
				MultiplayerJoin(address, joinPort, code, name, &error);
			else{
				MultiplayerJoinRequest join;
				join.address = address;
				join.port = joinPort;
				join.name = name;
				join.kind = gForm.kind == 1 ? MP_LOGIN_ACCOUNT : MP_LOGIN_INVITE;
				join.account = gForm.account;
				join.password = gForm.kind == 1 ? gForm.password : gForm.invite;
				join.room = gForm.room;
				join.newName = gForm.newName;
				join.newPassword = gForm.newPassword;
				MultiplayerJoinWith(join, &error);
			}
		}
		ImGui::EndDisabled();
		break;
	case MP_LOBBY:
		lobbyPanel(&error);
		break;
	case MP_HOSTING:
		ImGui::Text(_("Hosting on port %d"), MpRelayPort());
		ImGui::Text(_("Session code: %s"), gPassword);
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Copy")))
			ImGui::SetClipboardText(gPassword);
		switch(GetUpnpState()){
		case UPNP_WORKING:
			ImGui::TextDisabled(_("Opening the port on your router..."));
			break;
		case UPNP_OPEN: {
			std::string external = UpnpExternalAddress();
			if(external.empty())
				ImGui::TextWrapped(_("Your router forwards the port: over the internet, others join at your "
					"public address."));
			else{
				ImGui::Text(_("Over the internet, others join at %s"), external.c_str());
				ImGui::SameLine();
				if(ImGui::SmallButton(_("Copy##address")))
					ImGui::SetClipboardText(external.c_str());
			}
			break;
		}
		case UPNP_FAILED:
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Your router didn't open the port (%s)."),
				UpnpError().c_str());
			ImGui::TextWrapped(_("On your own network others can still join. Over the internet, forward port %d "
				"to this computer, use a VPN such as Tailscale, or run haumea-server."), MpRelayPort());
			break;
		}
		if(!gIpv6.empty()){
			ImGui::TextWrapped(_("Over IPv6, others can join at %s when your router lets them in."), gIpv6.c_str());
			ImGui::SameLine();
			if(ImGui::SmallButton(_("Copy##ipv6")))
				ImGui::SetClipboardText(gIpv6.c_str());
		}
		peerList();
		chatPanel();
		ImGui::Spacing();
		if(ImGui::Button(_("Stop hosting")))
			MultiplayerLeave();
		ImGui::SetItemTooltip(_("Everyone keeps the map as it is now."));
		break;
	case MP_CONNECTING:
		ImGui::Text(gRelayConnected ? _("Receiving the map...") : _("Connecting to %s..."), gAddress);
		if(ImGui::Button(_("Cancel")))
			MultiplayerLeave();
		break;
	case MP_JOINED:
		if(gAccountsMode){
			ImGui::Text(_("In room %s on %s, as %s"), gRoomName.c_str(), gSessionName, roleLabel(gRole));
			if(gRole == MP_ROLE_VIEWER)
				ImGui::TextColored(UiSemanticColor(UI_COLOR_INFO), _("You're a viewer here: you can look around and chat."));
		}else
			ImGui::Text(_("In %s's session"), gSessionName);
		if(gPingMs >= 0){
			ImGui::SameLine();
			ImGui::TextDisabled(_("%d ms"), gPingMs);
		}
		if(gOffers & MP_OFFERS_MAPS)
			ImGui::TextDisabled(_("The server keeps a copy of the map whenever it's saved."));
		peerList();
		chatPanel();
		ImGui::Spacing();
		loadForEveryone(&error);
		if(gAccountsMode && gRole >= MP_ROLE_ADMIN)
			adminPanel();
		if(ImGui::Button(_("Leave")))
			MultiplayerLeave();
		ImGui::SetItemTooltip(_("You keep the map as it is now; save it to keep a copy."));
		break;
	}
	if(!error.empty())
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), "%s", error.c_str());
	else if(!gStatus.empty() && gState == MP_OFF)
		ImGui::TextDisabled("%s", gStatus.c_str());
	ImGui::End();
}

std::string
MultiplayerStatusJson(void)
{
	static const char *states[] = { "off", "hosting", "connecting", "joined", "lobby" };
	auto text = [](const char *s){
		std::string out = "\"";
		for(; *s; s++){
			if(*s == '"' || *s == '\\') out += '\\';
			if((unsigned char)*s >= 32) out += *s;
		}
		return out + "\"";
	};
	char head[256];
	snprintf(head, sizeof(head), "\"state\":\"%s\",\"port\":%d,\"self\":%u,\"elements\":%d,\"code\":",
		states[GetMultiplayerState()], GetMultiplayerPort(), gSelf, (int)gElementOf.size());
	static const char *upnpStates[] = { "off", "working", "open", "failed" };
	std::string json = head + text(GetMultiplayerCode()) + ",\"session\":" + text(gSessionName) +
		",\"ipv6\":" + text(gHosting ? gIpv6.c_str() : "") +
		",\"upnp\":{\"state\":\"" + upnpStates[GetUpnpState()] + "\",\"external\":" +
		text(UpnpExternalAddress().c_str()) + ",\"error\":" + text(UpnpError().c_str()) + "}" +
		",\"status\":" + text(gStatus.c_str()) +
		",\"accounts\":" + (gAccountsMode ? "true" : "false") +
		",\"fingerprint\":" + text(gFingerprint.c_str()) +
		",\"role\":" + text(MpRoleName(gRole)) +
		",\"room\":" + text(gRoomName.c_str()) +
		",\"ping\":" + std::to_string(gPingMs) +
		",\"admin_reply\":{\"seq\":" + std::to_string(gAdminSeq) + ",\"ok\":" + (gAdminOk ? "true" : "false") +
		",\"text\":" + text(gAdminText.c_str()) + ",\"data\":" + text(gAdminData.c_str()) + "}" +
		",\"lobby\":[" + [&]{
			std::string out;
			for(const LobbyRoom &room : gLobby)
				out += std::string(out.empty() ? "" : ",") + "{\"name\":" + text(room.name.c_str()) + ",\"locked\":" +
					(room.locked ? "true" : "false") + ",\"people\":" + std::to_string(room.count) + ",\"role\":" +
					text(MpRoleName(room.role)) + "}";
			return out;
		}() + "]" +
		",\"peers\":[";
	bool first = true;
	for(const auto &it : gPeers){
		const Peer &peer = it.second;
		char item[400];
		snprintf(item, sizeof(item), "%s{\"id\":%u,\"agent\":%s,\"role\":\"%s\",\"ping\":%d,\"camera\":[%.3f,%.3f,%.3f],\"shown_camera\":[%.3f,%.3f,%.3f],\"name\":",
			first ? "" : ",", peer.id, peer.agent ? "true" : "false", MpRoleName(peer.role), peer.ping, peer.camera.x, peer.camera.y, peer.camera.z,
			peer.shownCamera.x, peer.shownCamera.y, peer.shownCamera.z);
		json += item + text(peer.name) + ",\"owner\":" + text(peer.owner) + ",\"selection\":[";
		for(size_t i = 0; i < peer.selection.size(); i++){
			ObjectInst *inst = instanceOf(peer.selection[i]);
			json += (i ? "," : "") + std::to_string(inst ? inst->m_id : -1);
		}
		json += "]}";
		first = false;
	}
	return json + "]";
}
