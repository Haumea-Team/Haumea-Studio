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
static const double MP_CHANNEL_INTERVAL = 0.5;

/*
 * Session state
 */

namespace {
struct Peer
{
	uint32 id;
	char name[MP_NAME_LEN];
	rw::V3d camera, target;
	int area;
	std::vector<uint32> selection;	// element ids
};
}

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
	gLastPresenceState.clear();
	gLastThorough = 0.0;
	gMissingModels = 0;
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
	return r.ok;
}

static std::string
ownPresence(void)
{
	std::vector<uint32> selected;
	for(CPtrNode *p = selection.first; p && (int)selected.size() < MP_MAX_SELECTION; p = p->next)
		if(uint32 element = elementOf((ObjectInst*)p->item, false))
			selected.push_back(element);
	return presencePayload(gSelf, gName, TheCamera.m_position, TheCamera.m_target, currentArea, selected);
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
	return joinRelay(address, port, password, name, error);
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
MultiplayerHost(int port, const char *name, std::string *error, bool openRouterPort)
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
	// Our own map seeds our own relay, as the first editor into haumea-server's does.
	if(!joinRelay("127.0.0.1", MpRelayPort(), code, name, error)){
		resetSession();
		return false;
	}
	if(openRouterPort)
		UpnpOpenPort(MpRelayPort());
	gIpv6 = publicIpv6();
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
	gNextElement = counted;
	gWorldIndex.clear();
	if(seed){
		// An empty session: ours becomes it.
		seedSession();
		gState = MP_JOINED;
		if(!gHosting)
			Toast(TOAST_SAVE, _("Your map started the session on %s"), gSessionName);
		log("Multiplayer: seeded %s:%d as peer %u\n", gAddress, gPort, gSelf);
		return;
	}
	// The session's map replaces ours, and its textdraw file ours (untitled:
	// saving it never writes over our own file).
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
	if(!r.ok || magic != MP_MAGIC){
		lostRelay(_("That isn't a Haumea session."));
		return;
	}
	if(protocol != MP_PROTOCOL){
		lostRelay(_("The session runs a different Haumea version: update both to the same one."));
		return;
	}
	std::string password = gPassword;
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
				if(!gPeers.count(peer.id) && gState == MP_JOINED)
					ToastFor(TOAST_SAVE, 4.0f, _("%s joined the session"), peer.name);
				gPeers[peer.id] = peer;
			}
			break;
		}
		case MSG_FILE:
			if(gSelf)
				receiveFile(frame.payload);
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
				Toast(TOAST_SAVE, _("Joined %s's session"), gSessionName);
				log("Multiplayer: joined %s:%d as peer %u\n", gAddress, gPort, gSelf);
			}
			break;
		}
	}
	if(!alive){
		lostRelay(gState == MP_JOINED ? _("The session ended.") : _("The session closed the connection."));
		return;
	}
	if(!MpFlush(gRelay) || MpNow() - gRelay.heard > MP_TIMEOUT)
		lostRelay(_("Lost the connection to the session."));
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

void
MultiplayerUpdate(void)
{
	if(gHosting)
		MpRelayUpdate();
	if(gState == MP_OFF)
		return;
	updateClient();
	if(gState != MP_JOINED)
		return;
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
	for(const auto &it : gPeers){
		const Peer &peer = it.second;
		ImU32 colour = peerColour(peer.id);
		rw::RGBA rgba = peerRgba(peer.id);
		if(peer.area == currentArea){
			ImVec2 at;
			rw::V3d dir = rw::sub(peer.target, peer.camera);
			float length = rw::length(dir);
			if(length > 0.001f)
				RenderLine(peer.camera, rw::add(peer.camera, rw::scale(dir, 4.0f / length)), rgba, rgba);
			if(projectPoint(peer.camera, &at)){
				draw->AddCircleFilled(at, 7 * ui, colour);
				draw->AddCircle(at, 7 * ui, IM_COL32(0, 0, 0, 200), 0, 1.5f * ui);
				draw->AddText(ImVec2(at.x + 10 * ui, at.y - ImGui::GetFontSize() * .5f), colour, peer.name);
			}
		}
		for(uint32 element : peer.selection){
			ObjectInst *inst = instanceOf(element);
			if(inst == nil || inst->m_isDeleted || inst->m_area != currentArea)
				continue;
			ObjectDef *obj = GetObjectDef(inst->m_objectId);
			if(obj && obj->m_colModel)
				RenderWireBox(&obj->m_colModel->boundingBox, rgba, &inst->m_matrix);
			ImVec2 at;
			if(element == peer.selection[0] && projectPoint(inst->m_translation, &at))
				draw->AddText(ImVec2(at.x + 6 * ui, at.y + 6 * ui), colour, peer.name);
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
		if(!peer.selection.empty()){
			ImGui::SameLine();
			ImGui::TextDisabled(_("%d selected"), (int)peer.selection.size());
		}
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Go to")))
			goToPeer(peer);
		ImGui::SetItemTooltip(_("Look from where they are looking."));
		ImGui::PopID();
	}
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
		ImGui::SeparatorText(_("Join"));
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Address"), address, sizeof(address));
		ImGui::SetNextItemWidth(field);
		ImGui::InputInt(_("Port##join"), &joinPort);
		joinPort = clamp(joinPort, 1, 65535);
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Code or password"), code, sizeof(code), ImGuiInputTextFlags_Password);
		ImGui::SetItemTooltip(_("The host's session code, or the password haumea-server was started with."));
		if(PawnMapHasUnsavedChanges() || TextDrawFileHasUnsavedChanges())
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
				_("Joining replaces the open map and textdraw file: save them first to keep your changes."));
		ImGui::BeginDisabled(address[0] == '\0' || code[0] == '\0');
		if(ImGui::Button(_("Join")) && address[0] && code[0]){
			error.clear();
			MultiplayerJoin(address, joinPort, code, name, &error);
		}
		ImGui::EndDisabled();
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
		ImGui::Text(_("In %s's session"), gSessionName);
		peerList();
		ImGui::Spacing();
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
	static const char *states[] = { "off", "hosting", "connecting", "joined" };
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
		",\"peers\":[";
	bool first = true;
	for(const auto &it : gPeers){
		const Peer &peer = it.second;
		char item[256];
		snprintf(item, sizeof(item), "%s{\"id\":%u,\"camera\":[%.3f,%.3f,%.3f],\"name\":", first ? "" : ",",
			peer.id, peer.camera.x, peer.camera.y, peer.camera.z);
		json += item + text(peer.name) + ",\"selection\":[";
		for(size_t i = 0; i < peer.selection.size(); i++){
			ObjectInst *inst = instanceOf(peer.selection[i]);
			json += (i ? "," : "") + std::to_string(inst ? inst->m_id : -1);
		}
		json += "]}";
		first = false;
	}
	return json + "]";
}
