// SPDX-License-Identifier: GPL-2.0-or-later
// Multiplayer editing (multiplayer.h).
//
// The host keeps the session: clients send it their changes, and it applies
// and relays every change, its own included, to all clients in one order.
// A change is an element's whole state (ElementHead plus the editor's own
// property, vehicle and material structs), found by comparing each element
// with what was last sent or received, so every way of editing is covered,
// undo included, and a new property field travels without changes here.
//
// Two people changing one element at once: a client ignores the others'
// changes to an element while the host hasn't echoed its own yet, since the
// host puts its own after them, so everyone ends with the same state.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "haumea.h"
#include "multiplayer.h"
#include "radarart.h"
#include "documentxml.h"
#include "encoding.h"
#include "icons.h"
#include "i18n.h"
#include "minilzo/minilzo.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <ctype.h>
#include <errno.h>
#include <cmath>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
typedef SOCKET MpSocket;
static const MpSocket MP_NO_SOCKET = INVALID_SOCKET;
#else
typedef int MpSocket;
static const MpSocket MP_NO_SOCKET = -1;
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
	MSG_HELLO = 1,	// client: who it is and the session code
	MSG_WELCOME,	// host: the client's peer id; the map follows
	MSG_REJECT,	// host: why not, then it closes
	MSG_ELEMENT,	// an element's state, with the peer that changed it
	MSG_PRESENCE,	// a peer's name, camera and selection
	MSG_PEER_LEFT,
	MSG_SNAPSHOT_DONE,	// host: the whole map was sent
	MSG_TEXTDRAWS,	// the map's own (read-only) textdraws, as editor XML
};
static const uint8 MSG_COMPRESSED = 0x80;
static const uint32 MP_MAGIC = 0x504d4d48;	// "HMMP"
static const uint32 MP_PROTOCOL = 1;
static const size_t MP_MAX_FRAME = 1 << 20;
static const size_t MP_MAX_UNPACKED = 8 << 20;
static const size_t MP_MAX_BACKLOG = 128 << 20;	// unsent bytes before a peer is dropped
static const int MP_MAX_CLIENTS = 16;
static const int MP_MAX_WRONG_CODES = 20;	// then joining is off until hosting again
static const int MP_MAX_SELECTION = 256;
static const double MP_SEND_INTERVAL = 0.05;
static const double MP_PRESENCE_INTERVAL = 0.1;
static const double MP_KEEPALIVE = 1.0;
static const double MP_TIMEOUT = 30.0;
static const double MP_CONNECT_TIMEOUT = 10.0;
enum { MP_NAME_LEN = 32, MP_CODE_LEN = 16 };

static double
now(void)
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/*
 * Sockets
 */

static void
closeSocket(MpSocket &socket)
{
	if(socket == MP_NO_SOCKET) return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
	socket = MP_NO_SOCKET;
}

static bool
setNonBlocking(MpSocket socket)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0 &&
		fcntl(socket, F_SETFD, FD_CLOEXEC) == 0;
#endif
}

static bool
wouldBlock(void)
{
#ifdef _WIN32
	int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS;
#endif
}

static bool
startNetwork(void)
{
#ifdef _WIN32
	static bool started;
	WSADATA data;
	if(!started && WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return false;
	started = true;
#endif
	return true;
}

static void
tuneSocket(MpSocket socket)
{
	int on = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
#ifdef SO_NOSIGPIPE
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

/*
 * Frames: a u32 size, a type byte, the payload. Large payloads are LZO
 * compressed (the properties are mostly empty text): then the payload is
 * the unpacked size and the packed bytes.
 */

template <typename T> static void
put(std::string &out, const T &value)
{
	out.append((const char*)&value, sizeof(T));
}

namespace {	// file-local: other files have their own Frame and Reader
struct Reader
{
	const char *at;
	size_t left;
	bool ok;
	Reader(const std::string &data) : at(data.data()), left(data.size()), ok(true) {}
	bool bytes(void *out, size_t size){
		if(!ok || left < size)
			return ok = false;
		memcpy(out, at, size);
		at += size;
		left -= size;
		return true;
	}
	template <typename T> bool get(T *value){ return bytes(value, sizeof(T)); }
};
}

static bool
lzoReady(void)
{
	static int ready = -1;
	if(ready < 0)
		ready = lzo_init() == LZO_E_OK;
	return ready == 1;
}

static std::string
makeFrame(uint8 type, const std::string &payload)
{
	std::string body;
	if(payload.size() >= 256 && lzoReady()){
		static std::vector<unsigned char> work(LZO1X_1_MEM_COMPRESS);
		std::vector<unsigned char> packed(payload.size() + payload.size() / 16 + 64 + 3);
		lzo_uint size = 0;
		if(lzo1x_1_compress((const unsigned char*)payload.data(), payload.size(), packed.data(), &size,
		   work.data()) == LZO_E_OK && size + 4 < payload.size()){
			type |= MSG_COMPRESSED;
			put(body, (uint32)payload.size());
			body.append((const char*)packed.data(), size);
		}
	}
	if(!(type & MSG_COMPRESSED))
		body = payload;
	std::string frame;
	put(frame, (uint32)(body.size() + 1));
	frame += (char)type;
	frame += body;
	return frame;
}

namespace {
struct Frame
{
	uint8 type;
	std::string payload;
};
}

namespace {
struct Connection
{
	MpSocket socket = MP_NO_SOCKET;
	std::string in, out;
	uint32 peer = 0;	// 0 until its hello was accepted
	char name[MP_NAME_LEN] = {};
	double heard = 0.0;
	bool closing = false;	// close once the rest was sent (a rejection)
};
}

static bool
flush(Connection &c)
{
	size_t sent = 0;
	while(sent < c.out.size()){
		int size = (int)std::min(c.out.size() - sent, (size_t)1 << 20);
		int n = (int)send(c.socket, c.out.data() + sent, size, MSG_NOSIGNAL);
		if(n > 0){
			sent += n;
			continue;
		}
		if(n < 0 && wouldBlock())
			break;
		return false;
	}
	c.out.erase(0, sent);
	return c.out.size() <= MP_MAX_BACKLOG;
}

static bool
unpack(uint8 type, const char *data, size_t size, Frame *frame)
{
	frame->type = type & ~MSG_COMPRESSED;
	if(!(type & MSG_COMPRESSED)){
		frame->payload.assign(data, size);
		return true;
	}
	uint32 unpacked;
	if(size < 4 || !lzoReady())
		return false;
	memcpy(&unpacked, data, 4);
	if(unpacked > MP_MAX_UNPACKED)
		return false;
	frame->payload.resize(unpacked);
	lzo_uint out = unpacked;
	return lzo1x_decompress_safe((const unsigned char*)data + 4, size - 4,
		(unsigned char*)&frame->payload[0], &out, nil) == LZO_E_OK && out == unpacked;
}

// Reads what has arrived; false when the connection is gone or misbehaves.
static bool
receive(Connection &c, std::vector<Frame> &frames)
{
	char buffer[65536];
	bool open = true;
	for(size_t total = 0; total < (32u << 20);){
		int n = (int)recv(c.socket, buffer, sizeof(buffer), 0);
		if(n > 0){
			c.in.append(buffer, n);
			c.heard = now();
			total += n;
			continue;
		}
		// Closed: what came before still counts (a rejection's reason).
		open = n < 0 && wouldBlock();
		break;
	}
	size_t at = 0;
	while(c.in.size() - at >= 4){
		uint32 size;
		memcpy(&size, c.in.data() + at, 4);
		if(size == 0 || size > MP_MAX_FRAME)
			return false;
		if(c.in.size() - at - 4 < size)
			break;
		Frame frame;
		if(!unpack((uint8)c.in[at + 4], c.in.data() + at + 5, size - 1, &frame))
			return false;
		frames.push_back(std::move(frame));
		at += 4 + size;
	}
	c.in.erase(0, at);
	return open;
}

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

static int gState = MP_OFF;
static MpSocket gListen = MP_NO_SOCKET;
static std::vector<Connection> gClients;	// hosting
static Connection gHost;	// joining
static bool gHostConnected;	// the TCP connection is up (joining)
static double gConnectStarted;
static uint32 gSelf;	// our peer id; the host is 1
static uint32 gNextPeer;
static uint32 gNextElement;
static char gName[MP_NAME_LEN];
static char gCode[MP_CODE_LEN];
static char gHostName[MP_NAME_LEN];
static char gAddress[256];
static int gPort;
static std::string gStatus;	// the last thing that happened, for the window
static std::map<uint32, Peer> gPeers;	// the others
static double gLastSend, gLastPresence, gLastPresenceChange, gLastThorough;
static std::string gLastPresenceState;
static int gMissingModels;	// elements whose model this game doesn't have
static int gWrongCodes;

// Elements by the editor's instance id (m_id, never reused) and by their
// session-wide element id (the creating peer's id in the top bits).
static std::unordered_map<int32, uint32> gElementOf;
static std::unordered_map<uint32, int32> gInstanceOf;
static std::unordered_map<int32, std::string> gBaseline;	// what everyone last had
static std::unordered_map<int32, int> gPending;	// our changes not echoed yet
static std::unordered_map<int32, uint32> gSeenChange;	// m_changeSeq when last compared
static double gLastSweep;
static std::unordered_map<uint32, std::string> gUnresolved;	// waiting for a parent, vehicle or LOD
static std::unordered_map<std::string, ObjectInst*> gWorldIndex;
static std::string gTextDrawBaseline;
static double gLastTextDraws;

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
copyName(char *dst, const char *src)
{
	snprintf(dst, MP_NAME_LEN, "%s", src && src[0] ? src : "?");
	for(char *p = dst; *p; p++)
		if((unsigned char)*p < 32) *p = ' ';
}

static void
resetSession(void)
{
	for(Connection &c : gClients)
		closeSocket(c.socket);
	gClients.clear();
	closeSocket(gHost.socket);
	gHost = Connection();
	closeSocket(gListen);
	gHostConnected = false;
	gState = MP_OFF;
	gSelf = 0;
	gNextPeer = 1;
	gNextElement = 0;
	gCode[0] = '\0';
	gHostName[0] = '\0';
	gPeers.clear();
	gElementOf.clear();
	gInstanceOf.clear();
	gBaseline.clear();
	gPending.clear();
	gSeenChange.clear();
	gUnresolved.clear();
	gWorldIndex.clear();
	gTextDrawBaseline.clear();
	gLastPresenceState.clear();
	gLastThorough = 0.0;
	gMissingModels = 0;
	gWrongCodes = 0;
}

/*
 * Elements
 */

namespace {
struct ElementHead
{
	double degrees[3];	// the map file's own rotation numbers
	uint32 element, lod, attachParent, npcVehicle;	// element ids, 0 for none
	int32 model, area;
	int32 iplIndex, imageIndex, binInstIndex;	// a GTA building: where the game has it
	uint32 flags, materials, fileLength;
	float position[3], rotation[4], sourceRotation[4];
};
}
enum {
	EL_WORLD = 1,
	EL_DELETED = 2,
	EL_VEHICLE = 4,
	EL_SOURCE_DEGREES = 8,
	EL_ATTACH_PLAYER = 16,
};

static ObjectInst*
instanceOf(uint32 element)
{
	auto it = gInstanceOf.find(element);
	return it == gInstanceOf.end() ? nil : GetInstanceByID(it->second);
}

// Two peers may give one GTA building an id each before they hear of the
// other's: the lower one is kept, by everyone.
static uint32
bindElement(ObjectInst *inst, uint32 element)
{
	gInstanceOf[element] = inst->m_id;
	auto it = gElementOf.find(inst->m_id);
	if(it == gElementOf.end() || element < it->second)
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
	gNextElement = (gNextElement + 1) & 0x3fffff;
	return bindElement(inst, gSelf << 22 | gNextElement);
}

static std::string
worldKey(const char *file, int iplIndex, int imageIndex, int binInstIndex)
{
	char numbers[48];
	snprintf(numbers, sizeof(numbers), "|%d|%d|%d", iplIndex, imageIndex, binInstIndex);
	return std::string(file) + numbers;
}

static ObjectInst*
findWorldInstance(const std::string &file, const ElementHead &h)
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
	ElementHead h;
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
			put(materials, *m);
		}
	}
	std::string out;
	put(out, h);
	if(h.fileLength)
		out.append(inst->m_file->name, h.fileLength);
	put(out, props);
	if(inst->m_isVehicle)
		put(out, GetVehicleState(inst));
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
	Reader r(blob);
	ElementHead h;
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
	if(inst == nil && !world && !deleted){
		inst = SpawnDocumentInstance(EditorModelId(h.model), position, rotation);
		if(inst == nil && gMissingModels++ == 0)
			Toast(TOAST_SPAWN, _("This game doesn't have model %d, which the session uses"), h.model);
	}
	if(inst == nil)
		return true;	// a building this game doesn't have, or deleted before we saw it
	bindElement(inst, h.element);

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
	ElementHead h;
	Reader r(blob);
	return r.get(&h) ? h.element : 0;
}

static void
receiveElement(uint32 origin, const std::string &blob)
{
	uint32 element = blobElement(blob);
	ObjectInst *inst = instanceOf(element);
	if(gState != MP_HOSTING && inst){
		auto pending = gPending.find(inst->m_id);
		if(origin == gSelf){
			if(pending != gPending.end() && --pending->second <= 0)
				gPending.erase(pending);
			return;
		}
		// Ours comes after it: the host relays in one order.
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

// The map's textdraws change only when a map is opened or they are moved to
// a textdraw file, so they go as one list.
static std::string
mapTextDrawsXml(void)
{
	return EditorXML::Serialize(BuildTextDrawXML(GetMapTextDraws(), {}, PAWN_TEXT_CP1252));
}

static void
receiveTextDraws(const std::string &xml)
{
	EditorXML::Node document;
	std::vector<TextDraw> textdraws;
	std::vector<TextDrawGuide> guides;
	std::string error;
	if(!EditorXML::Parse(xml, &document, &error) ||
	   !ReadTextDrawXML(document, true, textdraws, guides, &error)){
		log("Multiplayer: unreadable textdraws: %s\n", error.c_str());
		return;
	}
	GetMapTextDraws() = std::move(textdraws);
	BumpChangeSeq();
	gTextDrawBaseline = mapTextDrawsXml();
}

/*
 * Sending
 */

static void
broadcast(const std::string &frame, uint32 except)
{
	for(Connection &c : gClients)
		if(c.peer && c.peer != except && !c.closing)
			c.out += frame;
}

static std::string
elementPayload(uint32 origin, const std::string &blob)
{
	std::string payload;
	put(payload, origin);
	return payload + blob;
}

static void
publishElement(ObjectInst *inst, const std::string &blob)
{
	if(gState == MP_HOSTING)
		broadcast(makeFrame(MSG_ELEMENT, elementPayload(gSelf, blob)), 0);
	else if(gState == MP_JOINED){
		gHost.out += makeFrame(MSG_ELEMENT, elementPayload(gSelf, blob));
		gPending[inst->m_id]++;
	}
}

// Sends what changed since the last time; silent only takes note of it.
static void
sendChanges(bool silent)
{
	bool thorough = now() - gLastThorough > 5.0;
	if(thorough)
		gLastThorough = now();
	if(now() - gLastTextDraws > 0.5 || silent){
		gLastTextDraws = now();
		std::string xml = mapTextDrawsXml();
		if(xml != gTextDrawBaseline){
			gTextDrawBaseline = xml;
			std::string frame = silent ? "" : makeFrame(MSG_TEXTDRAWS, elementPayload(gSelf, xml));
			if(gState == MP_HOSTING)
				broadcast(frame, 0);
			else if(gState == MP_JOINED)
				gHost.out += frame;
		}
	}
	// Encoding every element every time is too slow for big maps: only what
	// was stamped as changed or is selected (being dragged), and all of them
	// now and then for edits that don't stamp.
	bool sweep = silent || now() - gLastSweep > 1.0;
	if(sweep)
		gLastSweep = now();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		bool known = gElementOf.count(inst->m_id) != 0;
		if(!known && !isMapElement(inst, thorough))
			continue;
		auto seen = gSeenChange.find(inst->m_id);
		if(known && !sweep && !inst->m_selected && seen != gSeenChange.end() && seen->second == inst->m_changeSeq)
			continue;
		gSeenChange[inst->m_id] = inst->m_changeSeq;
		std::string state = encodeElement(inst, elementOf(inst, true));
		std::string &baseline = gBaseline[inst->m_id];
		if(state == baseline)
			continue;
		baseline = state;
		if(!silent)
			publishElement(inst, state);
	}
}

static std::string
presencePayload(uint32 peer, const char *name, const rw::V3d &camera, const rw::V3d &target, int area,
	const std::vector<uint32> &selection)
{
	std::string payload;
	put(payload, peer);
	payload.append(name, MP_NAME_LEN);
	put(payload, camera);
	put(payload, target);
	put(payload, (int32)area);
	put(payload, (uint32)selection.size());
	for(uint32 element : selection)
		put(payload, element);
	return payload;
}

static bool
readPresence(const std::string &payload, Peer *peer)
{
	Reader r(payload);
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
	copyName(peer->name, name);
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
	double t = now();
	if(t - gLastPresence < MP_PRESENCE_INTERVAL)
		return;
	std::string payload = ownPresence();
	if(payload == gLastPresenceState && t - gLastPresenceChange < MP_KEEPALIVE)
		return;
	gLastPresence = gLastPresenceChange = t;
	gLastPresenceState = payload;
	if(gState == MP_HOSTING)
		broadcast(makeFrame(MSG_PRESENCE, payload), 0);
	else if(gState == MP_JOINED)
		gHost.out += makeFrame(MSG_PRESENCE, payload);
}

/*
 * Hosting
 */

static void
reject(Connection &c, const char *reason)
{
	c.out += makeFrame(MSG_REJECT, reason);
	c.closing = true;
}

static void
welcome(Connection &c, const Frame &hello)
{
	Reader r(hello.payload);
	uint32 magic = 0, protocol = 0, propsSize = 0, materialSize = 0, vehicleSize = 0;
	int32 game = -1;
	char code[MP_CODE_LEN], name[MP_NAME_LEN];
	r.get(&magic); r.get(&protocol); r.get(&propsSize); r.get(&materialSize); r.get(&vehicleSize);
	r.get(&game);
	r.bytes(code, sizeof(code));
	r.bytes(name, sizeof(name));
	if(!r.ok || magic != MP_MAGIC){
		reject(c, "not a Haumea session");
		return;
	}
	code[MP_CODE_LEN - 1] = '\0';
	name[MP_NAME_LEN - 1] = '\0';
	if(protocol != MP_PROTOCOL || propsSize != sizeof(ObjectPawnProperties) ||
	   materialSize != sizeof(ObjectMaterialOverride) || vehicleSize != sizeof(VehicleState)){
		reject(c, "a different Haumea version: update both to the same one");
		return;
	}
	if(game != (int32)params.map){
		reject(c, "the host edits a different game");
		return;
	}
	bool same = strlen(code) == strlen(gCode);
	for(int i = 0; same && code[i]; i++)
		same = toupper((unsigned char)code[i]) == gCode[i];
	if(gWrongCodes >= MP_MAX_WRONG_CODES){
		reject(c, "too many wrong codes: the host has to host again");
		return;
	}
	if(!same){
		if(++gWrongCodes == MP_MAX_WRONG_CODES)
			Toast(TOAST_SAVE, _("Too many wrong session codes: no one else can join until you host again"));
		reject(c, "wrong session code");
		return;
	}
	int joined = 0;
	for(const Connection &other : gClients)
		joined += other.peer != 0;
	if(joined >= MP_MAX_CLIENTS){
		reject(c, "the session is full");
		return;
	}
	c.peer = ++gNextPeer;
	copyName(c.name, name);
	std::string payload;
	put(payload, c.peer);
	payload.append(gName, MP_NAME_LEN);
	put(payload, (int32)GetPawnMapFormat());
	c.out += makeFrame(MSG_WELCOME, payload);
	// The map as everyone has it, then who is here.
	gLastTextDraws = 0.0;
	sendChanges(false);
	// In the order they were made, which the joined map is made in too:
	// Pawn is written in the instance list's order.
	std::vector<int32> ids;
	for(const auto &known : gBaseline)
		ids.push_back(known.first);
	std::sort(ids.begin(), ids.end());
	for(int32 id : ids){
		ObjectInst *inst = GetInstanceByID(id);
		const std::string &state = gBaseline[id];
		if(inst && !state.empty() && !(inst->m_isDeleted && IsUserMapObject(inst)))
			c.out += makeFrame(MSG_ELEMENT, elementPayload(gSelf, state));
	}
	c.out += makeFrame(MSG_TEXTDRAWS, elementPayload(gSelf, gTextDrawBaseline));
	c.out += makeFrame(MSG_PRESENCE, ownPresence());
	for(const auto &peer : gPeers)
		c.out += makeFrame(MSG_PRESENCE, presencePayload(peer.second.id, peer.second.name,
			peer.second.camera, peer.second.target, peer.second.area, peer.second.selection));
	c.out += makeFrame(MSG_SNAPSHOT_DONE, "");
	// Listed for the others before its first presence arrives; no area: not
	// drawn until then.
	Peer peer = {};
	peer.id = c.peer;
	memcpy(peer.name, c.name, MP_NAME_LEN);
	peer.area = -1;
	gPeers[c.peer] = peer;
	broadcast(makeFrame(MSG_PRESENCE, presencePayload(peer.id, peer.name, peer.camera, peer.target, -1, {})),
		c.peer);
	ToastFor(TOAST_SAVE, 4.0f, _("%s joined the session"), c.name);
}

static void
dropClient(size_t index, bool announce)
{
	Connection &c = gClients[index];
	if(c.peer){
		gPeers.erase(c.peer);
		std::string payload;
		put(payload, c.peer);
		broadcast(makeFrame(MSG_PEER_LEFT, payload), c.peer);
		if(announce)
			ToastFor(TOAST_SAVE, 4.0f, _("%s left the session"), c.name);
	}
	closeSocket(c.socket);
	gClients.erase(gClients.begin() + index);
}

static void
updateHost(void)
{
	for(;;){
		MpSocket socket = accept(gListen, nil, nil);
		if(socket == MP_NO_SOCKET)
			break;
		if(gClients.size() >= 2 * MP_MAX_CLIENTS || !setNonBlocking(socket)){
			closeSocket(socket);
			continue;
		}
		tuneSocket(socket);
		Connection c;
		c.socket = socket;
		c.heard = now();
		gClients.push_back(c);
	}
	for(size_t i = 0; i < gClients.size();){
		Connection &c = gClients[i];
		std::vector<Frame> frames;
		bool alive = receive(c, frames);
		for(const Frame &frame : frames){
			if(!alive || c.closing)
				break;
			if(c.peer == 0){
				if(frame.type == MSG_HELLO)
					welcome(c, frame);
				else
					alive = false;
				continue;
			}
			if(frame.type == MSG_ELEMENT && frame.payload.size() > 4){
				std::string blob = frame.payload.substr(4);
				receiveElement(c.peer, blob);
				broadcast(makeFrame(MSG_ELEMENT, elementPayload(c.peer, blob)), 0);
			}else if(frame.type == MSG_TEXTDRAWS && frame.payload.size() > 4){
				receiveTextDraws(frame.payload.substr(4));
				broadcast(makeFrame(MSG_TEXTDRAWS, frame.payload), c.peer);
			}else if(frame.type == MSG_PRESENCE){
				Peer peer;
				if(readPresence(frame.payload, &peer)){
					peer.id = c.peer;	// a peer speaks only for itself
					memcpy(peer.name, c.name, MP_NAME_LEN);
					gPeers[c.peer] = peer;
					broadcast(makeFrame(MSG_PRESENCE, presencePayload(peer.id, peer.name, peer.camera,
						peer.target, peer.area, peer.selection)), c.peer);
				}
			}
		}
		double silence = now() - c.heard;
		if(!alive || !flush(c) || silence > MP_TIMEOUT || (c.peer == 0 && silence > MP_CONNECT_TIMEOUT) ||
		   (c.closing && c.out.empty()))
			dropClient(i, alive || c.peer);
		else
			i++;
	}
}

bool
MultiplayerHost(int port, const char *name, std::string *error)
{
	if(gState != MP_OFF)
		MultiplayerLeave();
	resetSession();
	if(!isSA() || IsIplMapDocumentOpen()){
		*error = _("Multiplayer edits Pawn maps of San Andreas.");
		return false;
	}
	if(!startNetwork() || (gListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == MP_NO_SOCKET){
		*error = _("Could not open a network socket.");
		return false;
	}
	int on = 1;
#ifdef _WIN32
	setsockopt(gListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
#else
	setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((unsigned short)port);
	if(!setNonBlocking(gListen) || bind(gListen, (sockaddr*)&address, sizeof(address)) != 0 ||
	   listen(gListen, 16) != 0){
		closeSocket(gListen);
		char text[128];
		snprintf(text, sizeof(text), _("Port %d is in use or not allowed."), port);
		*error = text;
		return false;
	}
	socklen_t length = sizeof(address);
	gPort = getsockname(gListen, (sockaddr*)&address, &length) == 0 ? ntohs(address.sin_port) : port;
	static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	std::random_device random;
	for(int i = 0; i < 6; i++)
		gCode[i] = letters[random() % (sizeof(letters) - 1)];
	gCode[6] = '\0';
	copyName(gName, name);
	gSelf = 1;
	gState = MP_HOSTING;
	sendChanges(true);
	gStatus.clear();
	log("Multiplayer: hosting on port %d\n", gPort);
	return true;
}

/*
 * Joining
 */

bool
MultiplayerJoin(const char *address, int port, const char *code, const char *name, std::string *error)
{
	if(gState != MP_OFF)
		MultiplayerLeave();
	resetSession();
	if(!isSA() || IsIplMapDocumentOpen()){
		*error = _("Multiplayer edits Pawn maps of San Andreas.");
		return false;
	}
	char service[16];
	snprintf(service, sizeof(service), "%d", port);
	addrinfo hints = {}, *found = nil;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if(!startNetwork() || getaddrinfo(address, service, &hints, &found) != 0 || found == nil){
		*error = _("Could not find that address.");
		return false;
	}
	gHost.socket = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
	bool started = gHost.socket != MP_NO_SOCKET && setNonBlocking(gHost.socket) &&
		(connect(gHost.socket, found->ai_addr, (int)found->ai_addrlen) == 0 || wouldBlock());
	freeaddrinfo(found);
	if(!started){
		closeSocket(gHost.socket);
		*error = _("Could not connect.");
		return false;
	}
	tuneSocket(gHost.socket);
	snprintf(gAddress, sizeof(gAddress), "%s", address);
	snprintf(gCode, sizeof(gCode), "%s", code ? code : "");
	copyName(gName, name);
	gPort = port;
	gState = MP_CONNECTING;
	gConnectStarted = gHost.heard = now();
	std::string hello;
	put(hello, MP_MAGIC);
	put(hello, MP_PROTOCOL);
	put(hello, (uint32)sizeof(ObjectPawnProperties));
	put(hello, (uint32)sizeof(ObjectMaterialOverride));
	put(hello, (uint32)sizeof(VehicleState));
	put(hello, (int32)params.map);
	hello.append(gCode, MP_CODE_LEN);
	hello.append(gName, MP_NAME_LEN);
	gHost.out += makeFrame(MSG_HELLO, hello);
	gStatus.clear();
	return true;
}

static void
lostHost(const char *why)
{
	gStatus = why;
	ToastFor(TOAST_SAVE, 6.0f, "%s", why);
	log("Multiplayer: %s\n", why);
	resetSession();
}

static bool
socketConnected(MpSocket socket)
{
	fd_set writable, failed;
	FD_ZERO(&writable);
	FD_ZERO(&failed);
	FD_SET(socket, &writable);
	FD_SET(socket, &failed);
	timeval zero = {0, 0};
	if(select((int)socket + 1, nil, &writable, &failed, &zero) <= 0)
		return false;
	int error = 0;
	socklen_t length = sizeof(error);
	getsockopt(socket, SOL_SOCKET, SO_ERROR, (char*)&error, &length);
	if(error != 0 || FD_ISSET(socket, &failed)){
		lostHost(_("Could not connect to the host."));
		return false;
	}
	return true;
}

static void
updateClient(void)
{
	if(!gHostConnected){
		if(now() - gConnectStarted > MP_CONNECT_TIMEOUT){
			lostHost(_("The host didn't answer."));
			return;
		}
		if(!socketConnected(gHost.socket))
			return;
		gHostConnected = true;
		gHost.heard = now();
	}
	std::vector<Frame> frames;
	bool alive = receive(gHost, frames);
	for(const Frame &frame : frames){
		switch(frame.type){
		case MSG_REJECT:
			lostHost((std::string(_("The host refused: ")) + frame.payload.substr(0, 200)).c_str());
			return;
		case MSG_WELCOME: {
			Reader r(frame.payload);
			char name[MP_NAME_LEN];
			int32 format = -1;
			if(r.get(&gSelf) && r.bytes(name, sizeof(name)) && r.get(&format)){
				name[MP_NAME_LEN - 1] = '\0';
				copyName(gHostName, name);
				// The host's map replaces ours.
				NewPawnMapDocument();
				ResetUndoHistory();
				SetPawnMapFormat(format);
				gWorldIndex.clear();
			}
			break;
		}
		case MSG_TEXTDRAWS:
			if(gSelf && frame.payload.size() > 4){
				uint32 origin;
				memcpy(&origin, frame.payload.data(), 4);
				if(origin != gSelf)
					receiveTextDraws(frame.payload.substr(4));
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
		case MSG_PEER_LEFT: {
			uint32 peer;
			Reader r(frame.payload);
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
				gState = MP_JOINED;
				Toast(TOAST_SAVE, _("Joined %s's session"), gHostName);
				log("Multiplayer: joined %s:%d as peer %u\n", gAddress, gPort, gSelf);
			}
			break;
		}
	}
	if(!alive){
		lostHost(gState == MP_JOINED ? _("The host ended the session.") : _("The host closed the connection."));
		return;
	}
	if(!flush(gHost) || now() - gHost.heard > MP_TIMEOUT)
		lostHost(_("Lost the connection to the host."));
}

/*
 * The rest of the editor
 */

void
MultiplayerLeave(void)
{
	if(gState == MP_HOSTING)
		for(Connection &c : gClients)
			flush(c);
	if(gState != MP_OFF)
		log("Multiplayer: left the session\n");
	resetSession();
}

void
MultiplayerUpdate(void)
{
	if(gState == MP_OFF)
		return;
	if(gState == MP_HOSTING)
		updateHost();
	else
		updateClient();
	if(gState != MP_HOSTING && gState != MP_JOINED)
		return;
	if(!gUnresolved.empty())
		retryUnresolved();
	if(now() - gLastSend >= MP_SEND_INTERVAL){
		gLastSend = now();
		sendChanges(false);
	}
	sendPresence();
	if(gState == MP_HOSTING){
		for(size_t i = 0; i < gClients.size();)
			if(!flush(gClients[i]))
				dropClient(i, true);
			else
				i++;
	}else if(!flush(gHost))
		lostHost(_("Lost the connection to the host."));
}

int GetMultiplayerState(void) { return gState; }
int GetMultiplayerPort(void) { return gPort; }
const char *GetMultiplayerCode(void) { return gState == MP_HOSTING ? gCode : ""; }
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
	if(!enabled || (gState != MP_HOSTING && gState != MP_JOINED) || gPeers.empty())
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
	static char name[MP_NAME_LEN], address[256] = "127.0.0.1", code[MP_CODE_LEN];
	static int hostPort = MP_DEFAULT_PORT, joinPort = MP_DEFAULT_PORT;
	static std::string error;
	if(name[0] == '\0'){
		const char *user = getenv("USER");
		if(user == nil) user = getenv("USERNAME");
		copyName(name, user ? user : "Mapper");
	}
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_USER " Multiplayer###multiplayer", open)){
		ImGui::End();
		return;
	}
	float field = ImGui::GetFontSize() * 12.0f;
	switch(gState){
	case MP_OFF:
		ImGui::TextWrapped(_("Edit one map together: one of you hosts the open map, the others join it."));
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Your name"), name, sizeof(name));
		ImGui::SeparatorText(_("Host"));
		ImGui::SetNextItemWidth(field);
		ImGui::InputInt(_("Port##host"), &hostPort);
		hostPort = clamp(hostPort, 1, 65535);
		if(ImGui::Button(_("Host this map"))){
			error.clear();
			MultiplayerHost(hostPort, name, &error);
		}
		ImGui::SetItemTooltip(_("Others join with this computer's address, the port and the session code.\n"
			"Over the internet the port must be forwarded to this computer, or use a VPN such as Tailscale."));
		ImGui::SeparatorText(_("Join"));
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Address"), address, sizeof(address));
		ImGui::SetNextItemWidth(field);
		ImGui::InputInt(_("Port##join"), &joinPort);
		joinPort = clamp(joinPort, 1, 65535);
		ImGui::SetNextItemWidth(field);
		ImGui::InputText(_("Session code"), code, sizeof(code), ImGuiInputTextFlags_CharsUppercase);
		if(PawnMapHasUnsavedChanges())
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING),
				_("Joining replaces the open map: save it first to keep your changes."));
		ImGui::BeginDisabled(address[0] == '\0' || code[0] == '\0');
		if(ImGui::Button(_("Join")) && address[0] && code[0]){
			error.clear();
			MultiplayerJoin(address, joinPort, code, name, &error);
		}
		ImGui::EndDisabled();
		break;
	case MP_HOSTING:
		ImGui::Text(_("Hosting on port %d"), gPort);
		ImGui::Text(_("Session code: %s"), gCode);
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Copy")))
			ImGui::SetClipboardText(gCode);
		peerList();
		ImGui::Spacing();
		if(ImGui::Button(_("Stop hosting")))
			MultiplayerLeave();
		ImGui::SetItemTooltip(_("Everyone keeps the map as it is now."));
		break;
	case MP_CONNECTING:
		ImGui::Text(gHostConnected ? _("Receiving the map...") : _("Connecting to %s..."), gAddress);
		if(ImGui::Button(_("Cancel")))
			MultiplayerLeave();
		break;
	case MP_JOINED:
		ImGui::Text(_("In %s's session"), gHostName);
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
		states[gState], gPort, gSelf, (int)gElementOf.size());
	std::string json = head + text(GetMultiplayerCode()) + ",\"status\":" + text(gStatus.c_str()) +
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
