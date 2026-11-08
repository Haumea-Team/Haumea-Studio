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
#include "mpaccounts.h"

#include <ctype.h>
#include <deque>
#include <errno.h>
#include <map>
#include <memory>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <direct.h>
#include <io.h>
#endif
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
// A room: one map and its session. A relay without rooms (an editor
// hosting) has one, with no name.
struct Room
{
	std::string name;
	std::string statePath;	// "" keeps it in memory only
	bool seeded = false;	// has a map; before that the first editor in sends its own
	uint32_t seeder = 0;	// the peer sending it
	int32_t game = -1;
	uint32_t sizes[3] = {};	// the editors' property, material and vehicle structs
	std::vector<Entry> entries;
	std::unordered_map<uint32_t, size_t> index;
	std::unordered_map<std::string, uint32_t> world;	// a GTA building's one id
	std::string channels[MP_CHANNEL_COUNT];
	bool hasChannel[MP_CHANNEL_COUNT] = {};
	std::unordered_map<uint32_t, uint32_t> counters;	// a peer id's highest element count
	std::map<std::string, std::string> files;	// by key (MpFileKey)
	size_t fileBytes = 0;
	bool dirty = false;	// changed since the last save
	double lastSave = 0.0;
	// statePath + ".log": every change since the last save, written as it comes,
	// so a relay killed outright loses nothing; a save starts it again.
	FILE *journal = nullptr;
	size_t journalBytes = 0;
	bool journalUnflushed = false;
	std::deque<std::string> chat;	// the last messages, as the editors get them
	// Keeping the map as Pawn: changed since an editor last sent it.
	bool mapChanged = false;
	double lastMapFile = 0.0, saveAsked = 0.0;
	uint32_t saveAskedPeer = 0;
};
struct Client : MpConnection
{
	bool waiting = false;	// accepted while the session is being seeded
	bool agent = false;	// an AI agent's editor, as its hello said: the others see a tag
	std::string presence;	// its last, as the others get it
	int32_t game = -1;
	uint32_t sizes[3] = {};
	std::deque<double> chatTimes;	// its recent messages, to keep a flood out
	Room *room = nullptr;	// where it is (nullptr: not in one yet)
	// Signing in (protocol 5).
	bool loggedIn = false;	// its MSG_LOGIN came: the challenge went
	int kind = MP_LOGIN_CODE;
	std::string loginName;	// the account, invite or token id it named
	uint8_t passwordKey[MP_KEY_LEN] = {};
	bool authenticated = false;	// its hello opened
	std::string account;	// with accounts: whose it is (an agent: its owner's)
	int role = MP_ROLE_BUILDER;	// in its room
	bool inLobby = false;
	double noticed = 0.0;	// when it was last told its edits weren't kept
};
struct AgentToken
{
	std::string owner;
	uint8_t salt[MP_SALT_LEN], key[MP_KEY_LEN];
	double expires;
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
static const uint32_t STATE_VERSION = 2;	// 2: and the chat; 1 still loads

static MpRelayOptions gOptions;
static std::map<std::string, std::unique_ptr<Room>> gRooms;
static MpAccounts gAccounts;	// with stateDir: accounts, invites, room settings, identity
static std::map<std::string, AgentToken> gAgentTokens;	// by id; kept in memory only, a day at most
// The room what runs now works on: the client's whose frame it is, or each in turn.
static Room *gRoom;

// A relay without rooms has this one, with no name.
static Room *
defaultRoom(void)
{
	std::unique_ptr<Room> &room = gRooms[""];
	if(!room)
		room.reset(new Room());
	return room.get();
}
static bool gRunning;
static MpSocket gListen = MP_NO_SOCKET;
static int gPort;
static std::vector<Client> gClients;
static std::map<std::string, Strikes> gStrikes;
static uint8_t gSalt[MP_SALT_LEN], gPasswordKey[MP_KEY_LEN];

// The session.
// statePath + ".log": every change since the last save, written as it comes,
// so a relay killed outright loses nothing; a save starts it again.
enum { JOURNAL_ELEMENT = 1, JOURNAL_CHANNEL, JOURNAL_FILE, JOURNAL_CHAT };
// The chat: the last messages, as the editors get them.
// Keeping the map as Pawn (mapsDir, mapOut): changed since an editor last sent it.
static const double MP_SAVE_ANSWER_TIME = 60.0;
static const double MP_SAVE_INTERVAL = 60.0;
static const size_t MP_MAX_JOURNAL = 64u << 20;

static void
relayLine(const char *prefix, const char *format, va_list args)
{
	if(gOptions.log == nullptr)
		return;
	char line[640];
	int n = snprintf(line, sizeof(line), "%s", prefix);
	vsnprintf(line + n, sizeof(line) - n, format, args);
	gOptions.log(line);
}

static void
relayLog(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	relayLine("[Info] ", format, args);
	va_end(args);
}

static void
relayWarn(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	relayLine("[Warning] ", format, args);
	va_end(args);
}

// Who comes and goes (logging.log_connection_messages, as open.mp has it).
static void
connectionLog(const char *format, ...)
{
	if(!gOptions.logConnections)
		return;
	va_list args;
	va_start(args, format);
	relayLine("[Info] ", format, args);
	va_end(args);
}

static void
clearSession(void)
{
	gRoom->seeded = false;
	gRoom->seeder = 0;
	gRoom->game = -1;
	memset(gRoom->sizes, 0, sizeof(gRoom->sizes));
	gRoom->entries.clear();
	gRoom->index.clear();
	gRoom->world.clear();
	for(int i = 0; i < MP_CHANNEL_COUNT; i++){
		gRoom->channels[i].clear();
		gRoom->hasChannel[i] = false;
	}
	gRoom->counters.clear();
	gRoom->files.clear();
	gRoom->fileBytes = 0;
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
	uint32_t &highest = gRoom->counters[element >> MP_PEER_SHIFT];
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
		auto known = gRoom->world.find(key);
		if(known == gRoom->world.end())
			gRoom->world[key] = h.element;
		else if(known->second != h.element){
			h.element = known->second;
			memcpy(&blob[offsetof(MpElementHead, element)], &h.element, sizeof(h.element));
		}
	}
	auto it = gRoom->index.find(h.element);
	if(it == gRoom->index.end()){
		// A new placed element is its creator's: its peer id is in it.
		if(!world && peer != 0 && (h.element >> MP_PEER_SHIFT) != peer)
			return false;
		if(gRoom->entries.size() >= MP_MAX_ELEMENTS)
			return false;
		gRoom->index[h.element] = gRoom->entries.size();
		gRoom->entries.push_back(Entry());
		it = gRoom->index.find(h.element);
	}
	Entry &e = gRoom->entries[it->second];
	e.id = h.element;
	e.world = world;
	e.deleted = (h.flags & EL_DELETED) != 0;
	e.blob = blob;
	countElement(h.element);
	gRoom->dirty = true;
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
	return gRoom->statePath + ".log";
}

static void
openJournal(bool fresh)
{
	if(gRoom->journal)
		fclose(gRoom->journal);
	gRoom->journal = fopen(journalPath().c_str(), fresh ? "wb" : "ab");
	gRoom->journalBytes = 0;
	if(gRoom->journal == nullptr)
		relayLog("could not write %s: changes are kept only every %d seconds", journalPath().c_str(),
			(int)MP_SAVE_INTERVAL);
}

static void
journal(uint8_t kind, const std::string &record)
{
	if(gRoom->journal == nullptr || !gRoom->seeded)
		return;
	std::string out(1, (char)kind);
	putString(out, record);
	if(fwrite(out.data(), 1, out.size(), gRoom->journal) != out.size())
		relayLog("could not write %s", journalPath().c_str());
	gRoom->journalBytes += out.size();
	gRoom->journalUnflushed = true;
}

static bool
saveRoom(std::string *error)
{
	if(gRoom->statePath.empty() || !gRoom->seeded)
		return true;
	std::string out;
	MpPut(out, STATE_MAGIC);
	MpPut(out, STATE_VERSION);
	MpPut(out, gRoom->game);
	for(uint32_t size : gRoom->sizes)
		MpPut(out, size);
	MpPut(out, (uint32_t)gRoom->counters.size());
	for(const auto &c : gRoom->counters){
		MpPut(out, c.first);
		MpPut(out, c.second);
	}
	MpPut(out, (uint32_t)gRoom->entries.size());
	for(const Entry &e : gRoom->entries)
		putString(out, e.blob);
	for(int i = 1; i < MP_CHANNEL_COUNT; i++){
		out += (char)gRoom->hasChannel[i];
		putString(out, gRoom->channels[i]);
	}
	MpPut(out, (uint32_t)gRoom->files.size());
	for(const auto &file : gRoom->files){
		putString(out, file.first);
		putString(out, file.second);
	}
	MpPut(out, (uint32_t)gRoom->chat.size());
	for(const std::string &message : gRoom->chat)
		putString(out, message);
	std::string temporary = gRoom->statePath + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	bool ok = f && fwrite(out.data(), 1, out.size(), f) == out.size();
	if(f && fclose(f) != 0)
		ok = false;
#ifdef _WIN32
	if(ok)
		remove(gRoom->statePath.c_str());
#endif
	if(!ok || rename(temporary.c_str(), gRoom->statePath.c_str()) != 0){
		remove(temporary.c_str());
		if(error)
			*error = "could not write " + gRoom->statePath;
		return false;
	}
	gRoom->dirty = false;
	gRoom->lastSave = MpNow();
	openJournal(true);	// all in the state file now
	return true;
}

// No file is an empty session; a file that can't be read stops the relay
// rather than being written over.
static bool
loadState(std::string *error)
{
	FILE *f = fopen(gRoom->statePath.c_str(), "rb");
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
	if(!r.ok || magic != STATE_MAGIC || version < 1 || version > STATE_VERSION){
		*error = gRoom->statePath + " is not a session this version of haumea-server can read";
		return false;
	}
	r.get(&gRoom->game);
	for(uint32_t &size : gRoom->sizes)
		r.get(&size);
	r.get(&count);
	for(uint32_t i = 0; r.ok && i < count; i++){
		uint32_t peer = 0, highest = 0;
		r.get(&peer);
		r.get(&highest);
		gRoom->counters[peer] = highest;
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
		gRoom->hasChannel[i] = has != 0;
		getString(r, &gRoom->channels[i]);
	}
	r.get(&count);
	for(uint32_t i = 0; r.ok && i < count; i++){
		std::string key, bytes;
		if(getString(r, &key) && getString(r, &bytes)){
			gRoom->fileBytes += bytes.size();
			gRoom->files[key] = std::move(bytes);
		}
	}
	if(version >= 2){
		r.get(&count);
		for(uint32_t i = 0; r.ok && i < count; i++){
			std::string message;
			if(getString(r, &message))
				gRoom->chat.push_back(message);
		}
	}
	if(!r.ok){
		clearSession();
		*error = gRoom->statePath + " is damaged";
		return false;
	}
	gRoom->seeded = true;
	gRoom->dirty = false;
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
				gRoom->channels[(uint8_t)record[0]] = record.substr(1);
				gRoom->hasChannel[(uint8_t)record[0]] = true;
			}else if(kind == JOURNAL_FILE){
				std::string key, bytes;
				if(MpReadFilePayload(record, &key, &bytes) && !gRoom->files.count(key)){
					gRoom->fileBytes += bytes.size();
					gRoom->files[key] = std::move(bytes);
				}
			}else if(kind == JOURNAL_CHAT){
				gRoom->chat.push_back(record);
				while(gRoom->chat.size() > (size_t)MP_CHAT_HISTORY)
					gRoom->chat.pop_front();
			}
		}
	}
	relayLog("loaded %d elements from %s (%d later changes)", (int)gRoom->entries.size(), gRoom->statePath.c_str(),
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
	return c.peer != 0 && !c.waiting && !c.closing && c.room == gRoom;
}

static void
broadcast(const std::string &frame, uint32_t except)
{
	for(Client &c : gClients)
		if(inSession(c) && c.peer != except)
			MpQueue(c, frame);
}

static void reject(Client &c, const char *reason);

// Its Haumea speaks another multiplayer protocol: say which side is behind,
// and warn this server's owner when it's this server.
static void
wrongVersion(Client &c, uint32_t theirs)
{
	if(theirs > MP_PROTOCOL){
		relayWarn("%s has a newer Haumea (multiplayer protocol %u; this speaks %u): update haumea-server", c.address,
			theirs, MP_PROTOCOL);
		reject(c, "this server is older than your Haumea: its owner needs to update it");
	}else
		reject(c, "your Haumea is older than this server: update Haumea");
}

static void
reject(Client &c, const char *reason)
{
	if(c.unopened)
		c.sealed = false;	// it can't open ours either
	sendTo(c, MSG_REJECT, reason);
	c.closing = true;
	connectionLog("refused %s%s%s: %s", c.address, c.account.empty() ? "" : " as ", c.account.c_str(), reason);
}

// What the relay says about a peer after its presence: whether an agent's,
// its role there, whose agent, its ping (as the editor measured it).
static std::string
presenceTail(const Client &c, uint16_t ping = 0)
{
	std::string out;
	out += (char)(c.agent ? 1 : 0);
	out += (char)c.role;
	char owner[MP_NAME_LEN] = {};
	if(c.agent && !c.account.empty())
		MpCopyName(owner, c.account.c_str());
	out.append(owner, MP_NAME_LEN);
	MpPut(out, ping);
	return out;
}

static int roomPeople(const Room *room);

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
	payload += presenceTail(c);
	return payload;
}

// A presence as the editor sent it, with what the relay knows put in: its
// peer, its name and whether it's an agent (the last byte, never the editor's say).
static std::string
stampPresence(const Client &c, const std::string &sent)
{
	std::string out = sent;
	memcpy(&out[0], &c.peer, 4);
	memcpy(&out[4], c.name, MP_NAME_LEN);
	// The selection count tells where the flag goes.
	size_t countAt = 4 + MP_NAME_LEN + sizeof(float) * 6 + 4;
	uint32_t count = 0;
	if(out.size() < countAt + 4)
		return std::string();
	memcpy(&count, &out[countAt], 4);
	size_t end = countAt + 4 + (size_t)std::min<uint32_t>(count, MP_MAX_SELECTION) * 4;
	if(count > MP_MAX_SELECTION || out.size() < end)
		return std::string();
	// The editor's ping, after its selection.
	uint16_t ping = 0;
	if(out.size() >= end + 2)
		memcpy(&ping, &out[end], 2);
	out.resize(end);
	out += presenceTail(c, ping);
	return out;
}

// A chat message as everyone gets it: who (0 is the session itself), their
// name, whether an agent said it, when, from where, and what.
static std::string
chatPayload(uint32_t peer, const char *name, bool agent, const float *where, int32_t area, const std::string &text)
{
	std::string out;
	MpPut(out, peer);
	char padded[MP_NAME_LEN];
	MpCopyName(padded, name);
	out.append(padded, MP_NAME_LEN);
	out += (char)(agent ? 1 : 0);
	MpPut(out, (int64_t)time(nullptr));
	float zero[6] = {};
	out.append((const char*)(where ? where : zero), sizeof(zero));
	MpPut(out, area);
	putString(out, text);
	return out;
}

static void
addChat(const std::string &payload)
{
	gRoom->chat.push_back(payload);
	while(gRoom->chat.size() > (size_t)MP_CHAT_HISTORY)
		gRoom->chat.pop_front();
	gRoom->dirty = true;
	journal(JOURNAL_CHAT, payload);
	broadcast(MpMakeFrame(MSG_CHAT, payload), 0);
}

// The session telling everyone something, in the chat.
static void
sessionSays(const char *format, ...)
{
	char text[400];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	addChat(chatPayload(0, gOptions.name.c_str(), false, nullptr, -1, text));
}

static void
welcome(Client &c, bool seed)
{
	std::string payload;
	MpPut(payload, c.peer);
	MpPut(payload, gRoom->counters[c.peer]);
	payload += (char)seed;
	char name[MP_NAME_LEN];
	MpCopyName(name, gOptions.name.c_str());
	payload.append(name, MP_NAME_LEN);
	uint32_t offers = 0;
	if(!gOptions.mapsDir.empty() || !gOptions.mapOut.empty())
		offers |= MP_OFFERS_MAPS;
	if(gOptions.allowReplace && (!gAccounts.isOpen() || c.role >= MP_ROLE_ADMIN))
		offers |= MP_OFFERS_REPLACE;
	if(!gAccounts.isOpen() && !gOptions.adminPassword.empty())
		offers |= MP_REPLACE_NEEDS_ADMIN;
	MpPut(payload, offers);
	payload += (char)c.role;
	putString(payload, gRoom->name);
	offers = 0;
	sendTo(c, MSG_WELCOME, payload);
}

// The session as it is, then who is here.
static void
sendSession(Client &c)
{
	c.waiting = false;
	welcome(c, false);
	// What the elements need first: the files custom models are made of, the format.
	for(const auto &file : gRoom->files)
		sendTo(c, MSG_FILE, MpFilePayload(file.first, file.second));
	for(int i = 1; i < MP_CHANNEL_AFTER_ELEMENTS; i++)
		if(gRoom->hasChannel[i])
			sendTo(c, MSG_CHANNEL, MpChannelPayload(0, (uint8_t)i, gRoom->channels[i]));
	for(const Entry &e : gRoom->entries)
		if(e.world || !e.deleted)
			sendTo(c, MSG_ELEMENT, MpFromPeer(0, e.blob));
	for(int i = MP_CHANNEL_AFTER_ELEMENTS; i < MP_CHANNEL_COUNT; i++)
		if(gRoom->hasChannel[i])
			sendTo(c, MSG_CHANNEL, MpChannelPayload(0, (uint8_t)i, gRoom->channels[i]));
	for(const Client &other : gClients)
		if(inSession(other) && other.peer != c.peer)
			sendTo(c, MSG_PRESENCE, other.presence.empty() ? presenceStub(other) : other.presence);
	for(const std::string &message : gRoom->chat)
		sendTo(c, MSG_CHAT, message);
	sendTo(c, MSG_SNAPSHOT_DONE, "");
	broadcast(MpMakeFrame(MSG_PRESENCE, presenceStub(c)), c.peer);
	connectionLog("%s joined %s%s%sfrom %s as peer %u (%d there)", c.name, gRoom->name.empty() ? "" : "room ",
		gRoom->name.c_str(), gRoom->name.empty() ? "" : " ", c.address, c.peer, roomPeople(gRoom));
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

static int
roomPeople(const Room *room)
{
	int count = 0;
	for(const Client &c : gClients)
		count += c.peer != 0 && !c.closing && c.room == room;
	return count;
}

static bool
roomNameValid(const std::string &name)
{
	if(name.empty() || name.size() > 32)
		return false;
	for(unsigned char ch : name)
		if(!isalnum(ch) && ch != '-' && ch != '_')
			return false;
	return true;
}

// The rooms kept in the folder: <name>.hmps.
static std::vector<std::string>
listRooms(const std::string &folder)
{
	std::vector<std::string> names;
#ifdef _WIN32
	_finddata_t found;
	intptr_t handle = _findfirst((folder + "/*.hmps").c_str(), &found);
	if(handle != -1){
		do names.push_back(std::string(found.name, strlen(found.name) - 5));
		while(_findnext(handle, &found) == 0);
		_findclose(handle);
	}
#else
	if(DIR *dir = opendir(folder.c_str())){
		while(dirent *entry = readdir(dir)){
			std::string name = entry->d_name;
			if(name.size() > 5 && name.compare(name.size() - 5, 5, ".hmps") == 0)
				names.push_back(name.substr(0, name.size() - 5));
		}
		closedir(dir);
	}
#endif
	std::vector<std::string> valid;
	for(const std::string &name : names)
		if(roomNameValid(name))
			valid.push_back(name);
	return valid;
}

// A room by name, made (empty) when create allows.
static Room *
roomNamed(const std::string &name, bool create)
{
	auto it = gRooms.find(name);
	if(it != gRooms.end())
		return it->second.get();
	if(!create)
		return nullptr;
	std::unique_ptr<Room> &room = gRooms[name];
	room.reset(new Room());
	room->name = name;
	room->statePath = gAccounts.roomsFolder() + "/" + name + ".hmps";
	room->lastMapFile = MpNow();
	return room.get();
}

// An account's role in a room: its own there, else its own everywhere.
static int
roleIn(const std::string &account, const std::string &room)
{
	const MpAccount *a = gAccounts.find(account);
	if(a == nullptr)
		return MP_ROLE_NONE;
	MpRoomSettings settings = gAccounts.room(room);
	auto own = settings.roles.find(a->name);
	return own != settings.roles.end() ? own->second : a->role;
}

// The rooms, as one in the lobby sees them.
static void
sendRooms(Client &c)
{
	std::string payload;
	MpPut(payload, (uint32_t)gRooms.size());
	for(const auto &it : gRooms){
		const Room *room = it.second.get();
		MpRoomSettings settings = gAccounts.room(room->name);
		putString(payload, room->name);
		payload += (char)settings.locked;
		MpPut(payload, (uint32_t)roomPeople(room));
		int role = roleIn(c.account, room->name);
		if(c.agent)
			role = std::min(role, settings.agentRole);
		payload += (char)role;
		std::string names;
		for(const Client &other : gClients)
			if(other.room == room && other.peer && !other.closing && names.size() < 200)
				names += (names.empty() ? "" : ", ") + std::string(other.name);
		putString(payload, names);
	}
	const MpAccount *account = gAccounts.find(c.account);
	payload += (char)(account ? account->role : MP_ROLE_NONE);	// may it make rooms
	sendTo(c, MSG_ROOMS, payload);
}

// Into a room: its lock, the role there, then the session as before.
static bool
enterRoom(Client &c, const std::string &name, const std::string &roomPassword, std::string *why)
{
	if(gAccounts.isOpen()){
		if(!roomNameValid(name)){
			*why = "a room's name is 1-32 letters, digits, - or _";
			return false;
		}
		const MpAccount *account = gAccounts.find(c.account);
		int base = account ? account->role : MP_ROLE_NONE;
		MpRoomSettings settings = gAccounts.room(name);
		int role = roleIn(c.account, name);
		if(c.agent)
			role = std::min(role, settings.agentRole);
		if(role == MP_ROLE_NONE){
			*why = c.agent ? "agents aren't allowed in that room" : "you have no role in that room";
			return false;
		}
		if(!gRooms.count(name) && base < MP_ROLE_ADMIN){
			*why = "there's no room called " + name + " (an admin can make it)";
			return false;
		}
		if(settings.locked && base < MP_ROLE_ADMIN){
			uint8_t key[MP_KEY_LEN];
			MpPasswordKey(roomPassword, settings.lockSalt, key);
			bool right = crypto_verify32(key, settings.lockKey) == 0;
			crypto_wipe(key, sizeof(key));
			if(!right){
				*why = "that room is locked: wrong room password";
				return false;
			}
		}
		Room *room = roomNamed(name, false);
		if(room && room->game >= 0 && (c.game != room->game || memcmp(c.sizes, room->sizes, sizeof(c.sizes)) != 0)){
			*why = c.game != room->game ? "that room edits a different game" : "a different Haumea version: update to the same one";
			return false;
		}
		if(room == nullptr)
			relayLog("%s made room %s", c.name, name.c_str());
		gRoom = roomNamed(name, true);
		c.role = role;
	}
	if(gRoom->game >= 0 && c.game != gRoom->game){
		*why = "the session edits a different game";
		return false;
	}
	if(gRoom->game >= 0 && memcmp(c.sizes, gRoom->sizes, sizeof(c.sizes)) != 0){
		*why = "a different Haumea version: update to the same one";
		return false;
	}
	if(MpRelayPeerCount() >= gOptions.maxClients || (c.peer = freePeerId()) == 0){
		*why = "the server is full";
		return false;
	}
	c.inLobby = false;
	c.room = gRoom;
	if(gRoom->seeded){
		sendSession(c);
		return true;
	}
	if(gRoom->seeder == 0){
		// An empty session: this editor's map becomes it.
		gRoom->seeder = c.peer;
		gRoom->game = c.game;
		memcpy(gRoom->sizes, c.sizes, sizeof(c.sizes));
		welcome(c, true);
		connectionLog("%s joined %s%s%sfrom %s as peer %u and is sending the map", c.name,
			gRoom->name.empty() ? "" : "room ", gRoom->name.c_str(), gRoom->name.empty() ? "" : " ", c.address, c.peer);
		return true;
	}
	c.waiting = true;
	return true;
}

// Its hello opened, so it knew the password: who it is, then where it goes.
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
	char agent = 0;
	r.get(&agent);
	std::string room, roomPassword, newName, newPassword, owner;
	getString(r, &room);
	getString(r, &roomPassword);
	getString(r, &newName);
	getString(r, &newPassword);
	getString(r, &owner);
	if(!r.ok || magic != MP_MAGIC){
		reject(c, "not a Haumea session");
		return;
	}
	if(protocol != MP_PROTOCOL){
		wrongVersion(c, protocol);
		return;
	}
	name[MP_NAME_LEN - 1] = '\0';
	gStrikes.erase(c.address);	// its hello opened: the password is right
	c.authenticated = true;
	c.agent = agent != 0;
	c.game = game;
	memcpy(c.sizes, sizes, sizeof(sizes));
	if(!gAccounts.isOpen()){
		MpCopyName(c.name, name);
		c.role = MP_ROLE_BUILDER;
		// Whose agent, as it says: without accounts nothing can prove it.
		if(c.agent)
			c.account = owner.substr(0, MP_NAME_LEN - 1);
		std::string why;
		if(!enterRoom(c, "", "", &why))
			reject(c, why.c_str());
		return;
	}
	switch(c.kind){
	case MP_LOGIN_ACCOUNT: {
		const MpAccount *account = gAccounts.find(c.loginName);
		if(account == nullptr){
			reject(c, "wrong password");
			return;
		}
		c.account = account->name;
		c.role = account->role;
		MpCopyName(c.name, account->name.c_str());
		c.agent = false;
		break;
	}
	case MP_LOGIN_INVITE: {
		const MpInvite *invite = gAccounts.findInvite(c.loginName);
		if(invite == nullptr){
			reject(c, "that invite was used up or has expired");
			return;
		}
		std::string why;
		int role = invite->role;
		std::string by = invite->by;
		if(!gAccounts.create(newName, newPassword, role, &why)){
			reject(c, why.c_str());
			return;
		}
		gAccounts.useInvite(c.loginName);
		relayLog("%s made an account with %s's invite, as %s", newName.c_str(), by.empty() ? "an" : by.c_str(),
			MpRoleName(role));
		c.account = gAccounts.find(newName)->name;
		c.role = role;
		MpCopyName(c.name, c.account.c_str());
		c.agent = false;
		break;
	}
	case MP_LOGIN_AGENT: {
		auto token = gAgentTokens.find(c.loginName);
		const MpAccount *owner = token == gAgentTokens.end() ? nullptr : gAccounts.find(token->second.owner);
		if(owner == nullptr){
			reject(c, "that agent token has expired");
			return;
		}
		c.account = owner->name;
		c.role = owner->role;
		c.agent = true;
		MpCopyName(c.name, name);
		break;
	}
	default:
		reject(c, "this server signs in with accounts");
		return;
	}
	if(room.empty()){
		// The lobby: it picks a room from the list.
		c.inLobby = true;
		connectionLog("%s%s signed in from %s", c.name, c.agent ? " (AI)" : "", c.address);
		sendRooms(c);
		return;
	}
	std::string why;
	if(!enterRoom(c, room, roomPassword, &why))
		reject(c, why.c_str());
}

// What it sent first, in the clear: how it signs in. The challenge back has the
// salt of that account (one made up when there is none: no telling which exist),
// and the server's identity signing it, so an editor that knows the server can
// tell when something else answers instead.
static void
login(Client &c, const MpFrame &frame)
{
	MpReader r(frame.payload);
	uint32_t magic = 0, protocol = 0;
	char kind = 0;
	std::string name;
	uint8_t nonce[MP_NONCE_LEN] = {};
	r.get(&magic);
	r.get(&protocol);
	r.get(&kind);
	getString(r, &name);
	r.bytes(nonce, sizeof(nonce));
	if(!r.ok || magic != MP_MAGIC){
		reject(c, "not a Haumea session");
		return;
	}
	if(protocol != MP_PROTOCOL){
		wrongVersion(c, protocol);
		return;
	}
	c.loggedIn = true;
	c.kind = kind;
	c.loginName = name.substr(0, 64);
	gAccounts.reloadIfChanged();	// an invite made a moment ago works at once
	uint8_t salt[MP_SALT_LEN];
	if(!gAccounts.isOpen()){
		if(kind != MP_LOGIN_CODE){
			reject(c, "this session has a code, not accounts");
			return;
		}
		memcpy(salt, gSalt, sizeof(salt));
		memcpy(c.passwordKey, gPasswordKey, sizeof(c.passwordKey));
	}else{
		bool found = false;
		if(kind == MP_LOGIN_ACCOUNT){
			if(const MpAccount *a = gAccounts.find(name)){
				memcpy(salt, a->salt, sizeof(salt));
				memcpy(c.passwordKey, a->key, sizeof(c.passwordKey));
				found = true;
			}
		}else if(kind == MP_LOGIN_INVITE){
			if(const MpInvite *v = gAccounts.findInvite(name)){
				memcpy(salt, v->salt, sizeof(salt));
				memcpy(c.passwordKey, v->key, sizeof(c.passwordKey));
				found = true;
			}
		}else if(kind == MP_LOGIN_AGENT){
			auto token = gAgentTokens.find(name);
			if(token != gAgentTokens.end() && token->second.expires > MpNow()){
				memcpy(salt, token->second.salt, sizeof(salt));
				memcpy(c.passwordKey, token->second.key, sizeof(c.passwordKey));
				found = true;
			}
		}else{
			reject(c, "this server signs in with accounts: give your name and password, or an invite code");
			return;
		}
		if(!found){
			// It fails as a wrong password would.
			gAccounts.madeUpSalt(name, salt);
			MpRandom(c.passwordKey, sizeof(c.passwordKey));
		}
	}
	std::string challenge;
	MpPut(challenge, MP_MAGIC);
	MpPut(challenge, MP_PROTOCOL);
	challenge.append((const char*)salt, sizeof(salt));
	challenge.append((const char*)c.publicKey, sizeof(c.publicKey));
	challenge += (char)(!gAccounts.isOpen() && gOptions.passwordIgnoresCase);
	char mode = gAccounts.isOpen() ? 1 : 0;
	challenge += mode;
	uint8_t identity[32] = {}, signature[64] = {};
	if(gAccounts.isOpen()){
		memcpy(identity, gAccounts.identityPublic(), sizeof(identity));
		std::string signedPart(MP_IDENTITY_CONTEXT);
		signedPart.append((const char*)nonce, sizeof(nonce));
		signedPart.append((const char*)salt, sizeof(salt));
		signedPart.append((const char*)c.publicKey, sizeof(c.publicKey));
		signedPart += mode;
		gAccounts.sign(signedPart, signature);
	}
	challenge.append((const char*)identity, sizeof(identity));
	challenge.append((const char*)signature, sizeof(signature));
	sendTo(c, MSG_CHALLENGE, challenge);
}

static void
seeded(void)
{
	gRoom->seeded = true;
	gRoom->seeder = 0;
	gRoom->dirty = true;
	relayLog("the session has %d elements", (int)gRoom->entries.size());
	std::string error;
	if(!gRoom->statePath.empty() && !saveRoom(&error))
		relayLog("%s", error.c_str());
	for(Client &c : gClients)
		if(c.waiting && c.peer && c.room == gRoom)
			sendSession(c);
}

static void
dropClient(size_t index)
{
	Client &c = gClients[index];
	if(c.room)
		gRoom = c.room;
	uint32_t peer = c.peer;
	bool joined = inSession(c) || (c.closing && peer);
	MpCloseSocket(c.socket);
	if(peer)
		connectionLog("%s (peer %u) left%s%s", c.name, peer, c.room && !c.room->name.empty() ? " room " : "",
			c.room ? c.room->name.c_str() : "");
	gClients.erase(gClients.begin() + index);
	if(peer && joined){
		std::string payload;
		MpPut(payload, peer);
		broadcast(MpMakeFrame(MSG_PEER_LEFT, payload), peer);
	}
	if(peer && peer == gRoom->seeder && !gRoom->seeded){
		// Gone before the whole map arrived: the next one waiting sends theirs.
		clearSession();
		for(Client &other : gClients)
			if(other.waiting && other.peer && other.room == gRoom){
				other.waiting = false;
				gRoom->seeder = other.peer;
				welcome(other, true);
				break;
			}
	}
}

/*
 * Chat
 */

static void
receiveChat(Client &c, const std::string &payload)
{
	MpReader r(payload);
	float where[6];
	int32_t area = -1;
	std::string text;
	r.bytes(where, sizeof(where));
	r.get(&area);
	if(!getString(r, &text) || c.waiting)
		return;
	// One line of printable text.
	std::string clean;
	for(unsigned char ch : text)
		if(ch >= 32 || ch == '\t')
			clean += (char)ch;
	size_t first = clean.find_first_not_of(" \t");
	if(first == std::string::npos)
		return;
	clean = clean.substr(first, MP_MAX_CHAT);
	for(float v : where)
		if(!(v > -1e6f && v < 1e6f))
			return;
	// Twenty in ten seconds at most.
	double now = MpNow();
	while(!c.chatTimes.empty() && now - c.chatTimes.front() > 10.0)
		c.chatTimes.pop_front();
	if(c.chatTimes.size() >= 20)
		return;
	c.chatTimes.push_back(now);
	if(gOptions.logChat)
		relayLog("[chat]%s%s%s %s%s: %s", gRoom->name.empty() ? "" : " [", gRoom->name.c_str(), gRoom->name.empty() ? "" : "]",
			c.name, c.agent ? " (AI)" : "", clean.c_str());
	addChat(chatPayload(c.peer, c.name, c.agent, where, area, clean));
}

/*
 * The map as Pawn: kept where the server's owner wants it
 */

static bool
makeFolder(const std::string &path)
{
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
	return mkdir(path.c_str(), 0750) == 0 || errno == EEXIST;
#endif
}

// Written to a file beside it, then put in its place: a crash never leaves half a map.
static bool
writeWhole(const std::string &path, const std::string &bytes)
{
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

static std::string
stamp(void)
{
	time_t now = time(nullptr);
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	char text[32];
	strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
	return text;
}

// The latest map in mapsDir, a dated copy in mapsDir/history (the last 50 of
// each name kept), and the same map in mapOut.
static bool
keepMap(const std::string &name, const std::string &pawn, std::string *where)
{
	std::string base;
	for(char ch : name)
		if(isalnum((unsigned char)ch) || ch == '-' || ch == '_')
			base += ch;
	if(base.empty())
		base = "session";
	bool ok = true;
	where->clear();
	if(!gOptions.mapsDir.empty()){
		// With rooms, each room's maps in a folder of its own.
		std::string folder = gOptions.mapsDir;
		makeFolder(folder);
		if(!gRoom->name.empty()){
			folder += "/" + gRoom->name;
			makeFolder(folder);
		}
		std::string history = folder + "/history";
		makeFolder(history);
		ok &= writeWhole(folder + "/" + base + ".pwn", pawn);
		ok &= writeWhole(history + "/" + base + "-" + stamp() + ".pwn", pawn);
		*where = folder + "/" + base + ".pwn";
	}
	if(!gOptions.mapOut.empty() && (gRoom->name.empty() || gRoom->name == gOptions.mapOutRoom)){
		ok &= writeWhole(gOptions.mapOut, pawn);
		*where = gOptions.mapOut;
	}
	return ok;
}

static void
receiveMapFile(Client &c, const std::string &payload)
{
	if(gOptions.mapsDir.empty() && gOptions.mapOut.empty())
		return;
	MpReader r(payload);
	char reason = 0;
	std::string name, pawn;
	r.get(&reason);
	if(!getString(r, &name) || !getString(r, &pawn) || pawn.empty() || pawn.size() > MP_MAX_MAP_FILE)
		return;
	std::string where;
	bool ok = keepMap(name, pawn, &where);
	gRoom->lastMapFile = MpNow();
	gRoom->mapChanged = false;
	gRoom->saveAsked = 0.0;
	gRoom->saveAskedPeer = 0;
	if(!ok){
		relayLog("could not keep the map %s sent", c.name);
		return;
	}
	relayLog("%s %s the map: %s (%d bytes)", c.name, reason ? "autosaved" : "saved", where.c_str(), (int)pawn.size());
	if(!reason)
		sessionSays("%s saved the map; the server kept a copy", c.name);
}

// Every autosaveSeconds, when the map changed: one editor sends it.
static void
autosaveMap(void)
{
	if(gOptions.autosaveSeconds <= 0.0 || (gOptions.mapsDir.empty() && gOptions.mapOut.empty()) || !gRoom->seeded ||
	   !gRoom->mapChanged)
		return;
	double now = MpNow();
	if(gRoom->saveAsked > 0.0 && now - gRoom->saveAsked < MP_SAVE_ANSWER_TIME)
		return;	// waiting for the answer
	if(gRoom->saveAsked == 0.0 && now - gRoom->lastMapFile < gOptions.autosaveSeconds)
		return;
	// A person's editor before an agent's; the one asked last only when nobody else is here.
	Client *pick = nullptr;
	for(Client &c : gClients){
		if(!inSession(c))
			continue;
		bool better = pick == nullptr || (pick->agent && !c.agent) ||
			(pick->peer == gRoom->saveAskedPeer && c.peer != gRoom->saveAskedPeer);
		if(better)
			pick = &c;
	}
	if(pick == nullptr)
		return;
	gRoom->saveAsked = now;
	gRoom->saveAskedPeer = pick->peer;
	sendTo(*pick, MSG_SAVE_REQUEST, "");
}

/*
 * Replacing the session's map
 */

static void
replaceSession(Client &c, const std::string &payload)
{
	MpReader r(payload);
	std::string admin;
	getString(r, &admin);
	if(!gOptions.allowReplace){
		sendTo(c, MSG_REJECT, "this server doesn't let a map replace the session's");
		c.closing = true;
		return;
	}
	if(gAccounts.isOpen() && c.role < MP_ROLE_ADMIN){
		sendTo(c, MSG_NOTICE, "only an admin can load another map for everyone");
		return;
	}
	if(!gAccounts.isOpen() && !gOptions.adminPassword.empty() && admin != gOptions.adminPassword){
		relayLog("%s gave a wrong admin password to replace the map", c.name);
		sendTo(c, MSG_REJECT, "wrong admin password: the session's map stays");
		c.closing = true;
		return;
	}
	// The map as it was stays: a dated copy of the session beside its state.
	std::string saved;
	if(!gRoom->statePath.empty()){
		std::string error;
		saveRoom(&error);
		FILE *in = fopen(gRoom->statePath.c_str(), "rb");
		if(in){
			std::string bytes;
			char buffer[65536];
			size_t n;
			while((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
				bytes.append(buffer, n);
			fclose(in);
			saved = gRoom->statePath + "." + stamp() + ".bak";
			if(!writeWhole(saved, bytes))
				saved.clear();
		}
	}
	relayLog("%s replaces the session's map%s%s", c.name, saved.empty() ? "" : "; the old one is in ", saved.c_str());
	clearSession();
	gRoom->game = c.game;
	memcpy(gRoom->sizes, c.sizes, sizeof(gRoom->sizes));
	gRoom->seeder = c.peer;
	gRoom->mapChanged = true;
	for(Client &other : gClients)
		if(inSession(other) && other.peer != c.peer){
			other.waiting = true;
			sendTo(other, MSG_RESET, std::string(c.name, strnlen(c.name, MP_NAME_LEN)));
		}
	welcome(c, true);
	sessionSays("%s loaded another map for everyone", c.name);
}

/*
 * Roles
 */

// A viewer's edit isn't kept: what the session has goes back to it, so its
// map snaps back to everyone's.
static void
bounce(Client &c, const MpFrame &frame)
{
	if(frame.type == MSG_ELEMENT && frame.payload.size() > 4){
		std::string blob = frame.payload.substr(4);
		MpElementHead h;
		std::string file;
		if(readHead(blob, &h, &file)){
			auto it = gRoom->index.find(h.element);
			if(it != gRoom->index.end())
				sendTo(c, MSG_ELEMENT, MpFromPeer(0, gRoom->entries[it->second].blob));
			else{
				// One it made: gone again.
				h.flags |= EL_DELETED;
				memcpy(&blob[offsetof(MpElementHead, flags)], &h.flags, sizeof(h.flags));
				sendTo(c, MSG_ELEMENT, MpFromPeer(0, blob));
			}
		}
	}else if(frame.type == MSG_CHANNEL && frame.payload.size() >= 5){
		uint8_t channel = (uint8_t)frame.payload[4];
		if(channel > 0 && channel < MP_CHANNEL_COUNT)
			sendTo(c, MSG_CHANNEL, MpChannelPayload(0, channel, gRoom->channels[channel]));
	}
	double now = MpNow();
	if(now - c.noticed > 10.0){
		c.noticed = now;
		sendTo(c, MSG_NOTICE, "You're a viewer in this room: your edits aren't kept.");
	}
}

static void
adminReply(Client &c, bool ok, const std::string &text, const std::string &data = std::string())
{
	std::string payload(1, (char)ok);
	putString(payload, text);
	putString(payload, data);
	sendTo(c, MSG_ADMIN_REPLY, payload);
}

// Who may give a role to whom: an admin up to builder (and not to an admin
// or the owner), the owner anything.
static bool
mayChange(int actor, int target, int to)
{
	if(actor >= MP_ROLE_OWNER)
		return true;
	return actor >= MP_ROLE_ADMIN && target < MP_ROLE_ADMIN && to < MP_ROLE_ADMIN;
}

static void
admin(Client &c, const std::string &payload)
{
	if(!gAccounts.isOpen()){
		adminReply(c, false, "this session has no accounts");
		return;
	}
	MpReader r(payload);
	char op = 0;
	r.get(&op);
	std::string a, b, d;
	getString(r, &a);
	getString(r, &b);
	getString(r, &d);
	const MpAccount *me = gAccounts.find(c.account);
	int mine = me ? me->role : MP_ROLE_NONE;
	int here = c.role;
	std::string room = gRoom ? gRoom->name : std::string();
	if(op == MP_ADMIN_AGENT_TOKEN){
		if(c.agent){
			adminReply(c, false, "an agent can't hand out agent tokens");
			return;
		}
		static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
		uint8_t random[30];
		MpRandom(random, sizeof(random));
		std::string id = "a", secret;
		for(int i = 0; i < 8; i++)
			id += letters[random[i] % (sizeof(letters) - 1)];
		for(int i = 8; i < 30; i++)
			secret += letters[random[i] % (sizeof(letters) - 1)];
		AgentToken token;
		token.owner = c.account;
		token.expires = MpNow() + 86400.0;
		MpRandom(token.salt, sizeof(token.salt));
		MpPasswordKey(secret, token.salt, token.key);
		gAgentTokens[id] = token;
		relayLog("%s got an agent token", c.name);
		adminReply(c, true, "an agent token for a day", id + "-" + secret);
		return;
	}
	if(op == MP_ADMIN_ROOMS){
		sendRooms(c);
		return;
	}
	if(here < MP_ROLE_ADMIN && mine < MP_ROLE_ADMIN){
		adminReply(c, false, "only an admin can do that");
		return;
	}
	int actor = std::max(here, mine);
	switch(op){
	case MP_ADMIN_INVITE: {
		int role = MpParseRole(a);
		int uses = atoi(b.c_str()), days = atoi(d.c_str());
		if(role == MP_ROLE_NONE || (role >= MP_ROLE_ADMIN && actor < MP_ROLE_OWNER)){
			adminReply(c, false, "you can invite viewers and builders" + std::string(actor >= MP_ROLE_OWNER ? ", admins and owners" : ""));
			return;
		}
		std::string code = gAccounts.invite(role, uses > 0 ? uses : 1, days > 0 ? days : 7, c.account);
		relayLog("%s (%s) made an invite for a %s", c.name, MpRoleName(actor), MpRoleName(role));
		adminReply(c, true, std::string("an invite for a ") + MpRoleName(role), code);
		return;
	}
	case MP_ADMIN_SET_ROLE: {
		const MpAccount *target = gAccounts.find(a);
		int to = b.empty() ? MP_ROLE_NONE : MpParseRole(b);
		if(target == nullptr || (!b.empty() && to == MP_ROLE_NONE)){
			adminReply(c, false, "no such account or role");
			return;
		}
		std::string where = d;
		int targetRole = where.empty() ? target->role : roleIn(target->name, where);
		if(!mayChange(actor, std::max(targetRole, target->role), to) || target->name == c.account){
			adminReply(c, false, "you can't change that one's role");
			return;
		}
		if(where.empty()){
			if(to == MP_ROLE_NONE){
				adminReply(c, false, "give a role");
				return;
			}
			gAccounts.setRole(target->name, to);
		}else{
			MpRoomSettings settings = gAccounts.room(where);
			if(to == MP_ROLE_NONE)
				settings.roles.erase(target->name);
			else
				settings.roles[target->name] = to;
			gAccounts.saveRoom(where, settings);
		}
		// Who is in now gets it now.
		for(Client &other : gClients)
			if(other.account == target->name && other.room && !other.closing){
				int now = roleIn(other.account, other.room->name);
				if(other.agent)
					now = std::min(now, gAccounts.room(other.room->name).agentRole);
				other.role = now;
				Room *saved = gRoom;
				gRoom = other.room;
				if(other.presence.size())
					broadcast(MpMakeFrame(MSG_PRESENCE, stampPresence(other, other.presence)), 0);
				gRoom = saved;
				sendTo(other, MSG_NOTICE, std::string("You're now a ") + MpRoleName(now) + " here.");
			}
		relayLog("%s (%s) made %s a %s%s%s", c.name, MpRoleName(actor), target->name.c_str(),
			to == MP_ROLE_NONE ? "(back to their own role)" : MpRoleName(to), where.empty() ? "" : " in room ", where.c_str());
		adminReply(c, true, "done");
		return;
	}
	case MP_ADMIN_KICK: {
		uint32_t peer = (uint32_t)atoi(a.c_str());
		for(Client &other : gClients)
			if(other.peer == peer && other.room == gRoom && !other.closing){
				int theirs = std::max(other.role, roleIn(other.account, room));
				if(theirs >= actor && actor < MP_ROLE_OWNER){
					adminReply(c, false, "you can't remove someone with your role or above");
					return;
				}
				relayLog("%s (%s) removed %s from room %s", c.name, MpRoleName(actor), other.name, room.c_str());
				reject(other, (std::string("removed by ") + c.name).c_str());
				adminReply(c, true, "removed");
				return;
			}
		adminReply(c, false, "nobody like that here");
		return;
	}
	case MP_ADMIN_LOCK: {
		MpRoomSettings settings = gAccounts.room(room);
		settings.locked = !a.empty();
		if(settings.locked){
			MpRandom(settings.lockSalt, sizeof(settings.lockSalt));
			MpPasswordKey(a, settings.lockSalt, settings.lockKey);
		}
		gAccounts.saveRoom(room, settings);
		relayLog("%s (%s) %s room %s", c.name, MpRoleName(actor), settings.locked ? "locked" : "unlocked", room.c_str());
		adminReply(c, true, settings.locked ? "locked" : "unlocked");
		return;
	}
	case MP_ADMIN_AGENTS: {
		MpRoomSettings settings = gAccounts.room(room);
		int to = a == "none" ? MP_ROLE_NONE : MpParseRole(a);
		if(a != "none" && (to == MP_ROLE_NONE || to > MP_ROLE_BUILDER)){
			adminReply(c, false, "agents may be builders, viewers or none");
			return;
		}
		settings.agentRole = to;
		gAccounts.saveRoom(room, settings);
		relayLog("%s (%s) let agents in room %s be %s", c.name, MpRoleName(actor), room.c_str(), a.c_str());
		adminReply(c, true, "done");
		return;
	}
	case MP_ADMIN_DELETE_ROOM: {
		Room *target = roomNamed(a, false);
		if(target == nullptr){
			adminReply(c, false, "no such room");
			return;
		}
		if(roomPeople(target) > 0){
			adminReply(c, false, "people are in it");
			return;
		}
		// Its files go aside, not away.
		std::string aside = target->statePath + "." + stamp() + ".deleted";
		rename(target->statePath.c_str(), aside.c_str());
		::remove((target->statePath + ".log").c_str());
		gAccounts.forgetRoom(a);
		if(target->journal)
			fclose(target->journal);
		gRooms.erase(a);
		if(gRoom == target)
			gRoom = nullptr;
		relayLog("%s (%s) deleted room %s (kept as %s)", c.name, MpRoleName(actor), a.c_str(), aside.c_str());
		adminReply(c, true, "deleted");
		return;
	}
	case MP_ADMIN_USERS: {
		std::string lines;
		for(const MpAccount &account : gAccounts.all())
			lines += account.name + "\t" + MpRoleName(account.role) + "\n";
		adminReply(c, true, "accounts", lines);
		return;
	}
	}
	adminReply(c, false, "unknown action");
}

// In the lobby: going into a room, or anything an account does before it.
static void
lobbyFrame(Client &c, const MpFrame &frame)
{
	if(frame.type == MSG_ENTER){
		MpReader r(frame.payload);
		std::string room, password;
		getString(r, &room);
		getString(r, &password);
		std::string why;
		if(!r.ok || !enterRoom(c, room, password, &why)){
			sendTo(c, MSG_NOTICE, why.empty() ? "couldn't go into that room" : why);
			sendRooms(c);
		}
		return;
	}
	if(frame.type == MSG_ADMIN){
		gRoom = nullptr;
		admin(c, frame.payload);
		return;
	}
	if(frame.type == MSG_PING)
		sendTo(c, MSG_PONG, frame.payload.substr(0, 16));
}

static void
receiveFrame(Client &c, const MpFrame &frame)
{
	bool edits = frame.type == MSG_ELEMENT || frame.type == MSG_CHANNEL || frame.type == MSG_FILE ||
		frame.type == MSG_MAP_FILE;
	if(edits && c.role < MP_ROLE_BUILDER){
		bounce(c, frame);
		return;
	}
	if(frame.type == MSG_ADMIN){
		admin(c, frame.payload);
		return;
	}
	if(frame.type == MSG_PING){
		sendTo(c, MSG_PONG, frame.payload.substr(0, 16));
		return;
	}
	switch(frame.type){
	case MSG_ELEMENT: {
		if(frame.payload.size() <= 4 || c.waiting)
			return;
		std::string blob = frame.payload.substr(4);
		if(storeElement(c.peer, blob)){
			gRoom->mapChanged = true;
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
		gRoom->channels[channel] = frame.payload.substr(5);
		gRoom->hasChannel[channel] = true;
		gRoom->dirty = true;
		gRoom->mapChanged = true;
		journal(JOURNAL_CHANNEL, frame.payload.substr(4));
		broadcast(MpMakeFrame(MSG_CHANNEL, MpChannelPayload(c.peer, channel, gRoom->channels[channel])), c.peer);
		return;
	}
	case MSG_PRESENCE: {
		if(frame.payload.size() < 4 + MP_NAME_LEN)
			return;
		// A peer speaks only for itself.
		std::string stamped = stampPresence(c, frame.payload);
		if(stamped.empty())
			return;
		c.presence = stamped;
		if(!c.waiting)
			broadcast(MpMakeFrame(MSG_PRESENCE, c.presence), c.peer);
		return;
	}
	case MSG_FILE: {
		std::string key, bytes;
		if(c.waiting || !MpReadFilePayload(frame.payload, &key, &bytes) || gRoom->files.count(key) ||
		   gRoom->fileBytes + bytes.size() > MP_MAX_FILE_BYTES)
			return;
		gRoom->fileBytes += bytes.size();
		gRoom->files[key] = bytes;
		gRoom->dirty = true;
		journal(JOURNAL_FILE, frame.payload);
		broadcast(MpMakeFrame(MSG_FILE, frame.payload), c.peer);
		return;
	}
	case MSG_SEED_DONE:
		if(c.peer == gRoom->seeder && !gRoom->seeded)
			seeded();
		return;
	case MSG_CHAT:
		receiveChat(c, frame.payload);
		return;
	case MSG_MAP_FILE:
		if(!c.waiting)
			receiveMapFile(c, frame.payload);
		return;
	case MSG_REPLACE:
		if(!c.waiting && gRoom->seeded)
			replaceSession(c, frame.payload);
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
	gRooms.clear();
	gAgentTokens.clear();
	gAccounts = MpAccounts();
	if(!options.stateDir.empty()){
		// Rooms and accounts: every room kept in the folder comes back.
		if(!gAccounts.open(options.stateDir, error))
			return false;
		for(const std::string &name : listRooms(gAccounts.roomsFolder())){
			gRoom = roomNamed(name, true);
			if(!loadState(error))
				return false;
			if(gRoom->seeded)
				openJournal(false);
		}
		relayLog("%d room(s), %d account(s); this server's fingerprint is %s", (int)gRooms.size(),
			(int)gAccounts.all().size(), MpFingerprint(gAccounts.identityPublic()).c_str());
		if(gAccounts.all().empty())
			relayWarn("no accounts yet: make an owner's invite with haumea-server invite --role owner");
	}else{
		gRoom = defaultRoom();
		gRoom->statePath = options.statePath;
		gRoom->lastMapFile = MpNow();
	}
	gStrikes.clear();
	std::string password = options.password;
	if(options.passwordIgnoresCase)
		for(char &ch : password)
			ch = (char)toupper((unsigned char)ch);
	MpRandom(gSalt, sizeof(gSalt));
	MpPasswordKey(password, gSalt, gPasswordKey);
	if(options.stateDir.empty()){
		if(!gRoom->statePath.empty() && !loadState(error))
			return false;
		if(gRoom->seeded)
			openJournal(false);
	}
	if(!MpStartNetwork() || !openListener(options.bindAddress ? options.bindAddress : "", options.port, error))
		return false;
	gRunning = true;
	for(auto &it : gRooms)
		it.second->lastSave = MpNow();
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
		else
			MpNewKeyPair(c);	// the challenge goes once it says how it signs in
		gClients.push_back(c);
	}
	for(size_t i = 0; i < gClients.size(); i++){
		std::vector<MpFrame> frames;
		bool alive = MpReceive(gClients[i], frames);
		for(const MpFrame &frame : frames){
			// hello() and receiveFrame() may add to gClients' frames but not to gClients.
			Client &c = gClients[i];
			// Not in a room yet: with rooms, none; without, the one there is.
			gRoom = c.room ? c.room : gAccounts.isOpen() ? nullptr : defaultRoom();
			if(!alive || c.closing)
				break;
			if(!c.sealed){
				// How it signs in, then its key: everything after is sealed.
				if(!c.loggedIn && frame.type == MSG_LOGIN)
					login(c, frame);
				else if(c.loggedIn && frame.type == MSG_KEY && frame.payload.size() == MP_KEY_LEN)
					MpSeal(c, true, c.publicKey, (const uint8_t*)frame.payload.data(), c.passwordKey);
				else
					alive = false;
			}else if(!c.authenticated){
				if(frame.type == MSG_HELLO)
					hello(c, frame);
				else
					alive = false;
			}else if(c.inLobby)
				lobbyFrame(c, frame);
			else if(c.peer != 0)
				receiveFrame(c, frame);
		}
		Client &c = gClients[i];
		if(c.unopened && !c.authenticated && !c.closing){
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
	static double lastCheck;
	if(gAccounts.isOpen() && t - lastCheck > 2.0){
		lastCheck = t;
		gAccounts.reloadIfChanged();
		for(auto it = gAgentTokens.begin(); it != gAgentTokens.end();)
			it = it->second.expires < t ? gAgentTokens.erase(it) : std::next(it);
	}
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
	for(auto &it : gRooms){
		gRoom = it.second.get();
		if(gRoom->journalUnflushed){
			fflush(gRoom->journal);
			gRoom->journalUnflushed = false;
		}
		autosaveMap();
		if(gRoom->dirty && !gRoom->statePath.empty() && (t - gRoom->lastSave > MP_SAVE_INTERVAL || gRoom->journalBytes > MP_MAX_JOURNAL)){
			std::string error;
			if(!saveRoom(&error)){
				relayLog("%s", error.c_str());
				gRoom->lastSave = t;	// not every frame
			}
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
	for(auto &it : gRooms){
		gRoom = it.second.get();
		std::string error;
		if(gRoom->dirty && !saveRoom(&error))
			relayLog("%s", error.c_str());
		if(gRoom->journal){
			fclose(gRoom->journal);
			gRoom->journal = nullptr;
		}
	}
	for(Client &c : gClients)
		MpCloseSocket(c.socket);
	gClients.clear();
	MpCloseSocket(gListen);
	gRunning = false;
	gRooms.clear();
	gRoom = nullptr;
	crypto_wipe(gPasswordKey, sizeof(gPasswordKey));
	relayLog("stopped");
}

bool MpRelayRunning(void) { return gRunning; }
int MpRelayPort(void) { return gRunning ? gPort : 0; }
int MpRelayElementCount(void) { return gRooms.count("") ? (int)gRooms[""]->entries.size() : 0; }

bool
MpRelaySave(std::string *error)
{
	bool ok = true;
	for(auto &it : gRooms){
		gRoom = it.second.get();
		ok &= saveRoom(error);
	}
	return ok;
}

int
MpRelayPeerCount(void)
{
	int count = 0;
	for(const Client &c : gClients)
		count += c.peer != 0 && !c.closing;
	return count;
}
