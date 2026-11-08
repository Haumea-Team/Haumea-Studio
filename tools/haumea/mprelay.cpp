// SPDX-License-Identifier: GPL-2.0-or-later
// The multiplayer relay (mprelay.h, mpprotocol.h).
//
// The session is the elements in the order they first arrived (the order
// the editors make them in, which is the order Pawn is written in), the
// map's lists (MP_CHANNEL_*) and who is here. An element is the bytes an
// editor sent; only its head is read, to keep one id per GTA building and to
// leave deleted placed elements out of what joining editors get.
#include "mpprotocol.h"
#include "mprelay.h"

#include <ctype.h>
#include <map>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <unordered_map>

namespace {
struct Entry
{
	uint32_t id;
	bool deleted, world;
	std::string blob;
};
struct Client : MpConnection
{
	bool waiting = false;	// accepted while the session is being seeded
	std::string presence;	// its last, as the others get it
};
struct Strikes
{
	int count = 0;
	double until = 0.0;
};
}

static const int MP_MAX_STRIKES = 5;	// wrong passwords from one address, then
static const double MP_STRIKE_PAUSE = 600.0;	// it waits this long
static const size_t MP_MAX_ELEMENTS = 4u << 20;
static const size_t MP_MAX_FILE_BYTES = (size_t)1 << 30;	// the session's custom model files
static const uint32_t STATE_MAGIC = 0x53504d48;	// "HMPS"
static const uint32_t STATE_VERSION = 1;

static MpRelayOptions gOptions;
static bool gRunning;
static MpSocket gListen = MP_NO_SOCKET;
static int gPort;
static std::vector<Client> gClients;
static std::map<std::string, Strikes> gStrikes;
static uint8_t gSalt[MP_SALT_LEN], gPasswordKey[MP_KEY_LEN];

// The session.
static bool gSeeded;	// has a map; before that the first editor in sends its own
static uint32_t gSeeder;	// the peer sending it
static int32_t gGame = -1;
static uint32_t gSizes[3];	// the editors' property, material and vehicle structs
static std::vector<Entry> gEntries;
static std::unordered_map<uint32_t, size_t> gIndex;
static std::unordered_map<std::string, uint32_t> gWorld;	// a GTA building's one id
static std::string gChannels[MP_CHANNEL_COUNT];
static bool gHasChannel[MP_CHANNEL_COUNT];
static std::unordered_map<uint32_t, uint32_t> gCounters;	// a peer id's highest element count
static std::map<std::string, std::string> gFiles;	// by key (MpFileKey)
static size_t gFileBytes;
static bool gDirty;	// changed since the last save
static double gLastSave;
// statePath + ".log": every change since the last save, written as it comes,
// so a relay killed outright loses nothing; a save starts it again.
static FILE *gJournal;
static size_t gJournalBytes;
static bool gJournalUnflushed;
enum { JOURNAL_ELEMENT = 1, JOURNAL_CHANNEL, JOURNAL_FILE };
static const double MP_SAVE_INTERVAL = 60.0;
static const size_t MP_MAX_JOURNAL = 64u << 20;

static void
relayLog(const char *format, ...)
{
	if(gOptions.log == nullptr)
		return;
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	gOptions.log(line);
}

static void
clearSession(void)
{
	gSeeded = false;
	gSeeder = 0;
	gGame = -1;
	memset(gSizes, 0, sizeof(gSizes));
	gEntries.clear();
	gIndex.clear();
	gWorld.clear();
	for(int i = 0; i < MP_CHANNEL_COUNT; i++){
		gChannels[i].clear();
		gHasChannel[i] = false;
	}
	gCounters.clear();
	gFiles.clear();
	gFileBytes = 0;
}

/*
 * Elements
 */

static bool
readHead(const std::string &blob, MpElementHead *h, std::string *file)
{
	MpReader r(blob);
	if(!r.get(h) || h->element == 0 || h->fileLength > 1024)
		return false;
	file->resize(h->fileLength);
	return h->fileLength == 0 || r.bytes(&(*file)[0], h->fileLength);
}

static std::string
worldKey(const std::string &file, const MpElementHead &h)
{
	char numbers[48];
	snprintf(numbers, sizeof(numbers), "|%d|%d|%d", h.iplIndex, h.imageIndex, h.binInstIndex);
	return file + numbers;
}

static void
countElement(uint32_t element)
{
	uint32_t &highest = gCounters[element >> MP_PEER_SHIFT];
	highest = std::max(highest, element & MP_ELEMENT_MASK);
}

// Keeps the element; false to drop it. A GTA building two editors gave an
// id each keeps the first one, written into the blob.
static bool
storeElement(uint32_t peer, std::string &blob)
{
	MpElementHead h;
	std::string file;
	if(!readHead(blob, &h, &file))
		return false;
	bool world = (h.flags & EL_WORLD) != 0;
	if(world){
		std::string key = worldKey(file, h);
		auto known = gWorld.find(key);
		if(known == gWorld.end())
			gWorld[key] = h.element;
		else if(known->second != h.element){
			h.element = known->second;
			memcpy(&blob[offsetof(MpElementHead, element)], &h.element, sizeof(h.element));
		}
	}
	auto it = gIndex.find(h.element);
	if(it == gIndex.end()){
		// A new placed element is its creator's: its peer id is in it.
		if(!world && peer != 0 && (h.element >> MP_PEER_SHIFT) != peer)
			return false;
		if(gEntries.size() >= MP_MAX_ELEMENTS)
			return false;
		gIndex[h.element] = gEntries.size();
		gEntries.push_back(Entry());
		it = gIndex.find(h.element);
	}
	Entry &e = gEntries[it->second];
	e.id = h.element;
	e.world = world;
	e.deleted = (h.flags & EL_DELETED) != 0;
	e.blob = blob;
	countElement(h.element);
	gDirty = true;
	return true;
}

/*
 * The state file: the session as it was, so it outlives the relay.
 */

static void
putString(std::string &out, const std::string &s)
{
	MpPut(out, (uint32_t)s.size());
	out += s;
}

static bool
getString(MpReader &r, std::string *s)
{
	uint32_t size = 0;
	if(!r.get(&size) || size > r.left)
		return r.ok = false;
	s->assign(r.at, size);
	r.at += size;
	r.left -= size;
	return true;
}

static std::string
journalPath(void)
{
	return gOptions.statePath + ".log";
}

static void
openJournal(bool fresh)
{
	if(gJournal)
		fclose(gJournal);
	gJournal = fopen(journalPath().c_str(), fresh ? "wb" : "ab");
	gJournalBytes = 0;
	if(gJournal == nullptr)
		relayLog("could not write %s: changes are kept only every %d seconds", journalPath().c_str(),
			(int)MP_SAVE_INTERVAL);
}

static void
journal(uint8_t kind, const std::string &record)
{
	if(gJournal == nullptr || !gSeeded)
		return;
	std::string out(1, (char)kind);
	putString(out, record);
	if(fwrite(out.data(), 1, out.size(), gJournal) != out.size())
		relayLog("could not write %s", journalPath().c_str());
	gJournalBytes += out.size();
	gJournalUnflushed = true;
}

bool
MpRelaySave(std::string *error)
{
	if(gOptions.statePath.empty() || !gSeeded)
		return true;
	std::string out;
	MpPut(out, STATE_MAGIC);
	MpPut(out, STATE_VERSION);
	MpPut(out, gGame);
	for(uint32_t size : gSizes)
		MpPut(out, size);
	MpPut(out, (uint32_t)gCounters.size());
	for(const auto &c : gCounters){
		MpPut(out, c.first);
		MpPut(out, c.second);
	}
	MpPut(out, (uint32_t)gEntries.size());
	for(const Entry &e : gEntries)
		putString(out, e.blob);
	for(int i = 1; i < MP_CHANNEL_COUNT; i++){
		out += (char)gHasChannel[i];
		putString(out, gChannels[i]);
	}
	MpPut(out, (uint32_t)gFiles.size());
	for(const auto &file : gFiles){
		putString(out, file.first);
		putString(out, file.second);
	}
	std::string temporary = gOptions.statePath + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	bool ok = f && fwrite(out.data(), 1, out.size(), f) == out.size();
	if(f && fclose(f) != 0)
		ok = false;
#ifdef _WIN32
	if(ok)
		remove(gOptions.statePath.c_str());
#endif
	if(!ok || rename(temporary.c_str(), gOptions.statePath.c_str()) != 0){
		remove(temporary.c_str());
		if(error)
			*error = "could not write " + gOptions.statePath;
		return false;
	}
	gDirty = false;
	gLastSave = MpNow();
	openJournal(true);	// all in the state file now
	return true;
}

// No file is an empty session; a file that can't be read stops the relay
// rather than being written over.
static bool
loadState(std::string *error)
{
	FILE *f = fopen(gOptions.statePath.c_str(), "rb");
	if(f == nullptr)
		return true;
	std::string data;
	char buffer[65536];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
		data.append(buffer, n);
	fclose(f);
	MpReader r(data);
	uint32_t magic = 0, version = 0, count = 0;
	r.get(&magic);
	r.get(&version);
	if(!r.ok || magic != STATE_MAGIC || version != STATE_VERSION){
		*error = gOptions.statePath + " is not a session this version of haumea-server can read";
		return false;
	}
	r.get(&gGame);
	for(uint32_t &size : gSizes)
		r.get(&size);
	r.get(&count);
	for(uint32_t i = 0; r.ok && i < count; i++){
		uint32_t peer = 0, highest = 0;
		r.get(&peer);
		r.get(&highest);
		gCounters[peer] = highest;
	}
	r.get(&count);
	for(uint32_t i = 0; r.ok && i < count; i++){
		std::string blob;
		if(getString(r, &blob) && !storeElement(0, blob))
			r.ok = false;
	}
	for(int i = 1; r.ok && i < MP_CHANNEL_COUNT; i++){
		char has = 0;
		r.get(&has);
		gHasChannel[i] = has != 0;
		getString(r, &gChannels[i]);
	}
	r.get(&count);
	for(uint32_t i = 0; r.ok && i < count; i++){
		std::string key, bytes;
		if(getString(r, &key) && getString(r, &bytes)){
			gFileBytes += bytes.size();
			gFiles[key] = std::move(bytes);
		}
	}
	if(!r.ok){
		clearSession();
		*error = gOptions.statePath + " is damaged";
		return false;
	}
	gSeeded = true;
	gDirty = false;
	// What changed after that save; a record cut short by a crash is left out.
	FILE *log = fopen(journalPath().c_str(), "rb");
	int replayed = 0;
	if(log){
		std::string records;
		while((n = fread(buffer, 1, sizeof(buffer), log)) > 0)
			records.append(buffer, n);
		fclose(log);
		MpReader j(records);
		char kind = 0;
		std::string record;
		while(j.get(&kind) && getString(j, &record)){
			replayed++;
			if(kind == JOURNAL_ELEMENT)
				storeElement(0, record);
			else if(kind == JOURNAL_CHANNEL && !record.empty() && (uint8_t)record[0] < MP_CHANNEL_COUNT){
				gChannels[(uint8_t)record[0]] = record.substr(1);
				gHasChannel[(uint8_t)record[0]] = true;
			}else if(kind == JOURNAL_FILE){
				std::string key, bytes;
				if(MpReadFilePayload(record, &key, &bytes) && !gFiles.count(key)){
					gFileBytes += bytes.size();
					gFiles[key] = std::move(bytes);
				}
			}
		}
	}
	relayLog("loaded %d elements from %s (%d later changes)", (int)gEntries.size(), gOptions.statePath.c_str(),
		replayed);
	return true;
}

/*
 * Peers
 */

static void
sendTo(Client &c, uint8_t type, const std::string &payload)
{
	MpQueue(c, MpMakeFrame(type, payload));
}

static bool
inSession(const Client &c)
{
	return c.peer != 0 && !c.waiting && !c.closing;
}

static void
broadcast(const std::string &frame, uint32_t except)
{
	for(Client &c : gClients)
		if(inSession(c) && c.peer != except)
			MpQueue(c, frame);
}

static void
reject(Client &c, const char *reason)
{
	if(c.unopened)
		c.sealed = false;	// it can't open ours either
	sendTo(c, MSG_REJECT, reason);
	c.closing = true;
	relayLog("refused %s: %s", c.address, reason);
}

static std::string
presenceStub(const Client &c)
{
	// As the editors write it: peer, name, camera, target, area, selection.
	std::string payload;
	MpPut(payload, c.peer);
	payload.append(c.name, MP_NAME_LEN);
	float zero[6] = {};
	payload.append((const char*)zero, sizeof(zero));
	MpPut(payload, (int32_t)-1);	// no area: not drawn until its own arrives
	MpPut(payload, (uint32_t)0);
	return payload;
}

static void
welcome(Client &c, bool seed)
{
	std::string payload;
	MpPut(payload, c.peer);
	MpPut(payload, gCounters[c.peer]);
	payload += (char)seed;
	char name[MP_NAME_LEN];
	MpCopyName(name, gOptions.name.c_str());
	payload.append(name, MP_NAME_LEN);
	sendTo(c, MSG_WELCOME, payload);
}

// The session as it is, then who is here.
static void
sendSession(Client &c)
{
	c.waiting = false;
	welcome(c, false);
	// What the elements need first: the files custom models are made of, the format.
	for(const auto &file : gFiles)
		sendTo(c, MSG_FILE, MpFilePayload(file.first, file.second));
	for(int i = 1; i < MP_CHANNEL_AFTER_ELEMENTS; i++)
		if(gHasChannel[i])
			sendTo(c, MSG_CHANNEL, MpChannelPayload(0, (uint8_t)i, gChannels[i]));
	for(const Entry &e : gEntries)
		if(e.world || !e.deleted)
			sendTo(c, MSG_ELEMENT, MpFromPeer(0, e.blob));
	for(int i = MP_CHANNEL_AFTER_ELEMENTS; i < MP_CHANNEL_COUNT; i++)
		if(gHasChannel[i])
			sendTo(c, MSG_CHANNEL, MpChannelPayload(0, (uint8_t)i, gChannels[i]));
	for(const Client &other : gClients)
		if(inSession(other) && other.peer != c.peer)
			sendTo(c, MSG_PRESENCE, other.presence.empty() ? presenceStub(other) : other.presence);
	sendTo(c, MSG_SNAPSHOT_DONE, "");
	broadcast(MpMakeFrame(MSG_PRESENCE, presenceStub(c)), c.peer);
	relayLog("%s joined from %s as peer %u (%d in the session)", c.name, c.address, c.peer, MpRelayPeerCount());
}

static uint32_t
freePeerId(void)
{
	for(uint32_t id = 1; id <= MP_MAX_PEER_ID; id++){
		bool used = false;
		for(const Client &c : gClients)
			used |= c.peer == id;
		if(!used)
			return id;
	}
	return 0;
}

static void
hello(Client &c, const MpFrame &frame)
{
	MpReader r(frame.payload);
	uint32_t magic = 0, protocol = 0, sizes[3] = {};
	int32_t game = -1;
	char name[MP_NAME_LEN];
	r.get(&magic);
	r.get(&protocol);
	for(uint32_t &size : sizes)
		r.get(&size);
	r.get(&game);
	r.bytes(name, sizeof(name));
	if(!r.ok || magic != MP_MAGIC){
		reject(c, "not a Haumea session");
		return;
	}
	if(protocol != MP_PROTOCOL){
		reject(c, "a different Haumea version: update to the same one");
		return;
	}
	name[MP_NAME_LEN - 1] = '\0';
	gStrikes.erase(c.address);	// its hello opened: the password is right
	if(gGame >= 0 && game != gGame){
		reject(c, "the session edits a different game");
		return;
	}
	if(gGame >= 0 && memcmp(sizes, gSizes, sizeof(sizes)) != 0){
		reject(c, "a different Haumea version: update to the same one");
		return;
	}
	if(MpRelayPeerCount() >= gOptions.maxClients){
		reject(c, "the session is full");
		return;
	}
	c.peer = freePeerId();
	if(c.peer == 0){
		reject(c, "the session is full");
		return;
	}
	MpCopyName(c.name, name);
	if(gSeeded){
		sendSession(c);
		return;
	}
	if(gSeeder == 0){
		// An empty session: this editor's map becomes it.
		gSeeder = c.peer;
		gGame = game;
		memcpy(gSizes, sizes, sizeof(sizes));
		welcome(c, true);
		relayLog("%s joined from %s as peer %u and is sending the map", c.name, c.address, c.peer);
		return;
	}
	c.waiting = true;
}

static void
seeded(void)
{
	gSeeded = true;
	gSeeder = 0;
	gDirty = true;
	relayLog("the session has %d elements", (int)gEntries.size());
	std::string error;
	if(!gOptions.statePath.empty() && !MpRelaySave(&error))
		relayLog("%s", error.c_str());
	for(Client &c : gClients)
		if(c.waiting && c.peer)
			sendSession(c);
}

static void
dropClient(size_t index)
{
	Client &c = gClients[index];
	uint32_t peer = c.peer;
	bool joined = inSession(c) || (c.closing && peer);
	MpCloseSocket(c.socket);
	if(peer)
		relayLog("%s (peer %u) left", c.name, peer);
	gClients.erase(gClients.begin() + index);
	if(peer && joined){
		std::string payload;
		MpPut(payload, peer);
		broadcast(MpMakeFrame(MSG_PEER_LEFT, payload), peer);
	}
	if(peer && peer == gSeeder && !gSeeded){
		// Gone before the whole map arrived: the next one waiting sends theirs.
		clearSession();
		for(Client &other : gClients)
			if(other.waiting && other.peer){
				other.waiting = false;
				gSeeder = other.peer;
				welcome(other, true);
				break;
			}
	}
}

static void
receiveFrame(Client &c, const MpFrame &frame)
{
	switch(frame.type){
	case MSG_ELEMENT: {
		if(frame.payload.size() <= 4 || c.waiting)
			return;
		std::string blob = frame.payload.substr(4);
		if(storeElement(c.peer, blob)){
			journal(JOURNAL_ELEMENT, blob);
			broadcast(MpMakeFrame(MSG_ELEMENT, MpFromPeer(c.peer, blob)), 0);
		}
		return;
	}
	case MSG_CHANNEL: {
		if(frame.payload.size() < 5 || c.waiting)
			return;
		uint8_t channel = (uint8_t)frame.payload[4];
		if(channel == 0 || channel >= MP_CHANNEL_COUNT)
			return;
		gChannels[channel] = frame.payload.substr(5);
		gHasChannel[channel] = true;
		gDirty = true;
		journal(JOURNAL_CHANNEL, frame.payload.substr(4));
		broadcast(MpMakeFrame(MSG_CHANNEL, MpChannelPayload(c.peer, channel, gChannels[channel])), c.peer);
		return;
	}
	case MSG_PRESENCE: {
		if(frame.payload.size() < 4 + MP_NAME_LEN)
			return;
		// A peer speaks only for itself.
		c.presence = frame.payload;
		memcpy(&c.presence[0], &c.peer, 4);
		memcpy(&c.presence[4], c.name, MP_NAME_LEN);
		if(!c.waiting)
			broadcast(MpMakeFrame(MSG_PRESENCE, c.presence), c.peer);
		return;
	}
	case MSG_FILE: {
		std::string key, bytes;
		if(c.waiting || !MpReadFilePayload(frame.payload, &key, &bytes) || gFiles.count(key) ||
		   gFileBytes + bytes.size() > MP_MAX_FILE_BYTES)
			return;
		gFileBytes += bytes.size();
		gFiles[key] = bytes;
		gDirty = true;
		journal(JOURNAL_FILE, frame.payload);
		broadcast(MpMakeFrame(MSG_FILE, frame.payload), c.peer);
		return;
	}
	case MSG_SEED_DONE:
		if(c.peer == gSeeder && !gSeeded)
			seeded();
		return;
	}
}

/*
 * The relay
 */

// IPv6 and IPv4 on one socket where the system has IPv6 (an address behind
// carrier-grade NAT often has a public IPv6 one), else IPv4.
static bool
openListener(const char *bindAddress, int port, std::string *error)
{
	sockaddr_storage address = {};
	socklen_t size = 0;
	sockaddr_in6 *v6 = (sockaddr_in6*)&address;
	sockaddr_in *v4 = (sockaddr_in*)&address;
	bool both = bindAddress[0] == '\0';
	for(int attempt = 0; attempt < 2; attempt++){
		bool ipv6 = both ? attempt == 0 : strchr(bindAddress, ':') != nullptr;
		memset(&address, 0, sizeof(address));
		if(ipv6){
			v6->sin6_family = AF_INET6;
			v6->sin6_port = htons((unsigned short)port);
			v6->sin6_addr = in6addr_any;
			if(!both && inet_pton(AF_INET6, bindAddress, &v6->sin6_addr) != 1){
				*error = std::string("not an IP address: ") + bindAddress;
				return false;
			}
			size = sizeof(sockaddr_in6);
		}else{
			v4->sin_family = AF_INET;
			v4->sin_port = htons((unsigned short)port);
			v4->sin_addr.s_addr = htonl(INADDR_ANY);
			if(!both && inet_pton(AF_INET, bindAddress, &v4->sin_addr) != 1){
				*error = std::string("not an IP address: ") + bindAddress;
				return false;
			}
			size = sizeof(sockaddr_in);
		}
		gListen = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
		if(gListen == MP_NO_SOCKET){
			if(both && ipv6)
				continue;	// no IPv6 here
			*error = "could not open a network socket";
			return false;
		}
		int on = 1, off = 0;
#ifdef _WIN32
		setsockopt(gListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
#else
		setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#endif
		if(ipv6)
			setsockopt(gListen, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)(both ? &off : &on), sizeof(int));
		if(MpSetNonBlocking(gListen) && bind(gListen, (sockaddr*)&address, size) == 0 && listen(gListen, 16) == 0)
			break;
		MpCloseSocket(gListen);
		if(!(both && ipv6)){
			*error = "port " + std::to_string(port) + " is in use or not allowed";
			return false;
		}
	}
	socklen_t length = sizeof(address);
	gPort = port;
	if(getsockname(gListen, (sockaddr*)&address, &length) == 0)
		gPort = ntohs(address.ss_family == AF_INET6 ? v6->sin6_port : v4->sin_port);
	return true;
}

// An address as people write it; an IPv4 one on an IPv6 socket as IPv4.
static void
addressText(const sockaddr_storage &from, char *out, size_t size)
{
	out[0] = '\0';
	if(from.ss_family == AF_INET)
		inet_ntop(AF_INET, &((const sockaddr_in&)from).sin_addr, out, (socklen_t)size);
	else if(from.ss_family == AF_INET6){
		const in6_addr &a = ((const sockaddr_in6&)from).sin6_addr;
		if(IN6_IS_ADDR_V4MAPPED(&a))
			inet_ntop(AF_INET, (const char*)&a + 12, out, (socklen_t)size);
		else
			inet_ntop(AF_INET6, &a, out, (socklen_t)size);
	}
}

bool
MpRelayStart(const MpRelayOptions &options, std::string *error)
{
	MpRelayStop();
	gOptions = options;
	clearSession();
	gStrikes.clear();
	std::string password = options.password;
	if(options.passwordIgnoresCase)
		for(char &ch : password)
			ch = (char)toupper((unsigned char)ch);
	MpRandom(gSalt, sizeof(gSalt));
	MpPasswordKey(password, gSalt, gPasswordKey);
	if(!gOptions.statePath.empty() && !loadState(error))
		return false;
	if(gSeeded)
		openJournal(false);
	if(!MpStartNetwork() || !openListener(options.bindAddress ? options.bindAddress : "", options.port, error))
		return false;
	gRunning = true;
	gLastSave = MpNow();
	relayLog("listening on port %d", gPort);
	return true;
}

void
MpRelayUpdate(void)
{
	if(!gRunning)
		return;
	for(;;){
		sockaddr_storage from = {};
		socklen_t length = sizeof(from);
		MpSocket socket = accept(gListen, (sockaddr*)&from, &length);
		if(socket == MP_NO_SOCKET)
			break;
		if((int)gClients.size() >= 2 * gOptions.maxClients || !MpSetNonBlocking(socket)){
			MpCloseSocket(socket);
			continue;
		}
		MpTuneSocket(socket);
		Client c;
		c.socket = socket;
		c.heard = MpNow();
		addressText(from, c.address, sizeof(c.address));
		if(gStrikes[c.address].until > MpNow())
			reject(c, "too many wrong passwords: try again later");
		else{
			MpNewKeyPair(c);
			std::string challenge;
			MpPut(challenge, MP_MAGIC);
			MpPut(challenge, MP_PROTOCOL);
			challenge.append((const char*)gSalt, sizeof(gSalt));
			challenge.append((const char*)c.publicKey, sizeof(c.publicKey));
			challenge += (char)gOptions.passwordIgnoresCase;
			sendTo(c, MSG_CHALLENGE, challenge);
		}
		gClients.push_back(c);
	}
	for(size_t i = 0; i < gClients.size(); i++){
		std::vector<MpFrame> frames;
		bool alive = MpReceive(gClients[i], frames);
		for(const MpFrame &frame : frames){
			// hello() and receiveFrame() may add to gClients' frames but not to gClients.
			Client &c = gClients[i];
			if(!alive || c.closing)
				break;
			if(!c.sealed){
				// The editor's key: everything after is sealed.
				if(frame.type == MSG_KEY && frame.payload.size() == MP_KEY_LEN)
					MpSeal(c, true, c.publicKey, (const uint8_t*)frame.payload.data(), gPasswordKey);
				else
					alive = false;
			}else if(c.peer == 0){
				if(frame.type == MSG_HELLO)
					hello(c, frame);
				else
					alive = false;
			}else
				receiveFrame(c, frame);
		}
		Client &c = gClients[i];
		if(c.unopened && c.peer == 0 && !c.closing){
			// Its hello didn't open: made with another password.
			Strikes &strikes = gStrikes[c.address];
			if(++strikes.count >= MP_MAX_STRIKES){
				strikes.count = 0;
				strikes.until = MpNow() + MP_STRIKE_PAUSE;
			}
			reject(c, "wrong password");
		}else if(!alive)
			c.closing = true, c.heard = 0.0;
	}
	double t = MpNow();
	for(size_t i = 0; i < gClients.size();){
		Client &c = gClients[i];
		double silence = t - c.heard;
		bool keep = MpFlush(c) && silence <= MP_TIMEOUT && !(c.peer == 0 && silence > MP_CONNECT_TIMEOUT) &&
			!(c.closing && c.out.empty());
		if(keep)
			i++;
		else
			dropClient(i);
	}
	if(gJournalUnflushed){
		fflush(gJournal);
		gJournalUnflushed = false;
	}
	if(gDirty && !gOptions.statePath.empty() && (t - gLastSave > MP_SAVE_INTERVAL || gJournalBytes > MP_MAX_JOURNAL)){
		std::string error;
		if(!MpRelaySave(&error)){
			relayLog("%s", error.c_str());
			gLastSave = t;	// not every frame
		}
	}
}

void
MpRelayWait(double seconds)
{
	if(!gRunning)
		return;
	fd_set readable, writable;
	FD_ZERO(&readable);
	FD_ZERO(&writable);
	FD_SET(gListen, &readable);
	MpSocket highest = gListen;
	for(const Client &c : gClients){
		FD_SET(c.socket, &readable);
		if(!c.out.empty())
			FD_SET(c.socket, &writable);
		highest = std::max(highest, c.socket);
	}
	timeval timeout;
	timeout.tv_sec = (long)seconds;
	timeout.tv_usec = (long)((seconds - (long)seconds) * 1e6);
	select((int)highest + 1, &readable, &writable, nullptr, &timeout);
}

void
MpRelayStop(void)
{
	if(!gRunning)
		return;
	for(Client &c : gClients)
		MpFlush(c);
	std::string error;
	if(gDirty && !MpRelaySave(&error))
		relayLog("%s", error.c_str());
	if(gJournal){
		fclose(gJournal);
		gJournal = nullptr;
	}
	for(Client &c : gClients)
		MpCloseSocket(c.socket);
	gClients.clear();
	MpCloseSocket(gListen);
	gRunning = false;
	clearSession();
	crypto_wipe(gPasswordKey, sizeof(gPasswordKey));
	relayLog("stopped");
}

bool MpRelayRunning(void) { return gRunning; }
int MpRelayPort(void) { return gRunning ? gPort : 0; }
int MpRelayElementCount(void) { return (int)gEntries.size(); }

int
MpRelayPeerCount(void)
{
	int count = 0;
	for(const Client &c : gClients)
		count += c.peer != 0 && !c.closing;
	return count;
}
