// SPDX-License-Identifier: GPL-2.0-or-later
// Multiplayer editing's wire format, shared by the editor (multiplayer.cpp)
// and the relay (mprelay.cpp) that the editor runs to host and haumea-server
// runs on its own. Nothing here knows the editor: the relay keeps elements as
// the bytes the editors send, reading only their ElementHead.
//
// A session: every editor connects to the relay. The first one into an empty
// session seeds it with its map; everyone after gets the relay's copy. The
// relay puts every change in one order and sends it to all, the sender too.
//
// The connection is encrypted (Monocypher): the relay sends a fresh X25519
// key and the salt of its password key (Argon2id), the editor answers with
// its own fresh key, and both make two XChaCha20-Poly1305 keys from the
// shared secret keyed with the password key. Everything after is sealed, so
// someone watching sees neither the map nor anything to try passwords on;
// a wrong password shows as the editor's first sealed frame not opening.
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);	// RtlGenRandom
#ifdef _MSC_VER
#pragma comment(lib, "advapi32")
#endif
#else
#include <sys/random.h>	// getentropy
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#endif
#include "minilzo/minilzo.h"
#include "monocypher/monocypher.h"

#include <algorithm>
#include <chrono>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

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
	MSG_HELLO = 1,	// editor, the first sealed frame: who it is
	MSG_WELCOME,	// relay: the editor's peer id; the session follows, or the editor seeds it
	MSG_REJECT,	// relay: why not, then it closes
	MSG_ELEMENT,	// an element's state, after the peer that changed it
	MSG_PRESENCE,	// a peer's name, camera and selection
	MSG_PEER_LEFT,
	MSG_SNAPSHOT_DONE,	// relay: the whole session was sent
	MSG_CHANNEL,	// one of the map's lists (MP_CHANNEL_*), whole, after the peer that changed it
	MSG_SEED_DONE,	// editor: its whole map was sent into the empty session
	MSG_FILE,	// a file a channel names (a custom model's DFF or TXD): its key, then its bytes
	MSG_CHALLENGE,	// relay, plain: magic, protocol, password salt, its X25519 key, whether the password ignores case
	MSG_KEY,	// editor, plain: its X25519 key; sealed from here on
};
// The map's own lists, each sent whole when it changes.
// Joining editors get the ones before MP_CHANNEL_AFTER_ELEMENTS before the
// elements, which need them.
enum {
	MP_CHANNEL_META = 1,	// the Pawn format and text encoding
	MP_CHANNEL_CUSTOM_MODELS,	// 0.3.DL models: their files, then artconfig.txt lines
	MP_CHANNEL_AFTER_ELEMENTS,
	MP_CHANNEL_TEXTDRAWS = MP_CHANNEL_AFTER_ELEMENTS,	// the map's textdraws, as editor XML
	MP_CHANNEL_REMOVALS,	// RemoveBuildingForPlayer(-1, ...) spheres
	MP_CHANNEL_RADAR_ART,	// radar art layers
	MP_CHANNEL_TEXTDRAW_FILE,	// the TextDraw editor's file, as editor XML
	MP_CHANNEL_PINS,	// alignment pins and guides, as editor XML
	MP_CHANNEL_LAYOUT_PATHS,	// 3D paths, as editor XML with element ids for their objects
	MP_CHANNEL_NODES,	// the Nodes tool's 64 areas, one channel each: NODES<area>.DAT
	MP_CHANNEL_COUNT = MP_CHANNEL_NODES + 64
};
static const uint8_t MSG_COMPRESSED = 0x80;
static const uint32_t MP_MAGIC = 0x504d4d48;	// "HMMP"
static const uint32_t MP_PROTOCOL = 3;
static const size_t MP_MAX_FRAME = 48 << 20;	// a big DFF
static const size_t MP_MAX_UNPACKED = 64 << 20;
static const size_t MP_MAX_BACKLOG = 256 << 20;	// unsent bytes before a peer is dropped
static const int MP_MAX_CLIENTS = 32;
static const int MP_MAX_SELECTION = 256;
static const double MP_KEEPALIVE = 1.0;
static const double MP_TIMEOUT = 30.0;
static const double MP_CONNECT_TIMEOUT = 10.0;
static const int MP_DEFAULT_PORT_NUMBER = 7790;
enum { MP_NAME_LEN = 32, MP_PASSWORD_LEN = 64 };
// Element ids: the creating peer's id in the top bits, its own count below.
static const int MP_PEER_SHIFT = 22;
static const uint32_t MP_ELEMENT_MASK = (1u << MP_PEER_SHIFT) - 1;
static const uint32_t MP_MAX_PEER_ID = (1u << (32 - MP_PEER_SHIFT)) - 1;

// An element's state starts with this; the rest only the editors read.
struct MpElementHead
{
	double degrees[3];	// the map file's own rotation numbers
	uint32_t element, lod, attachParent, npcVehicle;	// element ids, 0 for none
	int32_t model, area;
	int32_t iplIndex, imageIndex, binInstIndex;	// a GTA building: where the game has it
	uint32_t flags, materials, fileLength;	// fileLength: the building's IPL name follows
	float position[3], rotation[4], sourceRotation[4];
};
enum {
	EL_WORLD = 1,
	EL_DELETED = 2,
	EL_VEHICLE = 4,
	EL_SOURCE_DEGREES = 8,
	EL_ATTACH_PLAYER = 16,
};

static inline double
MpNow(void)
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/*
 * Sockets
 */

static inline void
MpCloseSocket(MpSocket &socket)
{
	if(socket == MP_NO_SOCKET) return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
	socket = MP_NO_SOCKET;
}

static inline bool
MpSetNonBlocking(MpSocket socket)
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

static inline bool
MpWouldBlock(void)
{
#ifdef _WIN32
	int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EINPROGRESS;
#endif
}

static inline bool
MpStartNetwork(void)
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

static inline void
MpTuneSocket(MpSocket socket)
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

template <typename T> static inline void
MpPut(std::string &out, const T &value)
{
	out.append((const char*)&value, sizeof(T));
}

struct MpReader
{
	const char *at;
	size_t left;
	bool ok;
	MpReader(const std::string &data) : at(data.data()), left(data.size()), ok(true) {}
	bool bytes(void *out, size_t size){
		if(!ok || left < size)
			return ok = false;
		memcpy(out, at, size);
		at += size;
		left -= size;
		return true;
	}
	template <typename T> bool get(T *value){ return bytes(value, sizeof(T)); }
	std::string rest(void){ std::string s(at, left); at += left; left = 0; return s; }
};

static inline bool
MpLzoReady(void)
{
	static int ready = -1;
	if(ready < 0)
		ready = lzo_init() == LZO_E_OK;
	return ready == 1;
}

// A record: a type byte, the payload. Large payloads are LZO compressed (the
// properties are mostly empty text): then the payload is the unpacked size
// and the packed bytes. MpQueue sends it in an envelope.
static inline std::string
MpMakeFrame(uint8_t type, const std::string &payload)
{
	std::string body;
	if(payload.size() >= 256 && MpLzoReady()){
		static std::vector<unsigned char> work(LZO1X_1_MEM_COMPRESS);
		std::vector<unsigned char> packed(payload.size() + payload.size() / 16 + 64 + 3);
		lzo_uint size = 0;
		if(lzo1x_1_compress((const unsigned char*)payload.data(), payload.size(), packed.data(), &size,
		   work.data()) == LZO_E_OK && size + 4 < payload.size()){
			type |= MSG_COMPRESSED;
			MpPut(body, (uint32_t)payload.size());
			body.append((const char*)packed.data(), size);
		}
	}
	if(!(type & MSG_COMPRESSED))
		body = payload;
	return (char)type + body;
}

// A payload after the peer it came from (MSG_ELEMENT, MSG_CHANNEL).
static inline std::string
MpFromPeer(uint32_t origin, const std::string &payload)
{
	std::string out;
	MpPut(out, origin);
	return out + payload;
}

static inline std::string
MpChannelPayload(uint32_t origin, uint8_t channel, const std::string &data)
{
	std::string out;
	MpPut(out, origin);
	out += (char)channel;
	return out + data;
}

// A file's key: what its bytes hash to (FNV-1a, 64 bits) and its size.
static inline std::string
MpFileKey(const char *data, size_t size)
{
	uint64_t hash = 14695981039346656037ull;
	for(size_t i = 0; i < size; i++){
		hash ^= (unsigned char)data[i];
		hash *= 1099511628211ull;
	}
	char key[48];
	snprintf(key, sizeof(key), "%016llx-%llu", (unsigned long long)hash, (unsigned long long)size);
	return key;
}

static inline std::string
MpFilePayload(const std::string &key, const std::string &bytes)
{
	std::string out;
	out += (char)key.size();
	return out + key + bytes;
}

// The key must be the bytes': a file is never stored under someone else's.
static inline bool
MpReadFilePayload(const std::string &payload, std::string *key, std::string *bytes)
{
	if(payload.empty() || payload.size() < 1 + (size_t)(unsigned char)payload[0])
		return false;
	size_t length = (unsigned char)payload[0];
	*key = payload.substr(1, length);
	*bytes = payload.substr(1 + length);
	return !key->empty() && *key == MpFileKey(bytes->data(), bytes->size());
}

/*
 * Encryption
 */

static inline void
MpRandom(uint8_t *out, size_t size)
{
#ifdef _WIN32
	SystemFunction036(out, (ULONG)size);
#else
	for(size_t at = 0; at < size; at += 256)
		if(getentropy(out + at, std::min(size - at, (size_t)256)) != 0)
			abort();	// no randomness: no keys worth having
#endif
}

enum { MP_SALT_LEN = 16, MP_KEY_LEN = 32 };

// What a password is to the key schedule; slow on purpose (Argon2id, 16 MB).
static inline void
MpPasswordKey(const std::string &password, const uint8_t salt[MP_SALT_LEN], uint8_t key[MP_KEY_LEN])
{
	crypto_argon2_config config = { CRYPTO_ARGON2_ID, 16384, 3, 1 };
	crypto_argon2_inputs inputs = { (const uint8_t*)password.data(), salt, (uint32_t)password.size(), MP_SALT_LEN };
	std::vector<uint8_t> work((size_t)config.nb_blocks * 1024);
	crypto_argon2(key, MP_KEY_LEN, work.data(), config, inputs, crypto_argon2_no_extras);
	crypto_wipe(work.data(), work.size());
}

struct MpConnection
{
	MpSocket socket = MP_NO_SOCKET;
	std::string in, out;
	uint32_t peer = 0;	// 0 until its hello was accepted
	char name[MP_NAME_LEN] = {};
	char address[64] = {};
	double heard = 0.0;
	bool closing = false;	// close once the rest was sent (a rejection)
	// Our X25519 key for this connection, then the two sealing keys.
	uint8_t secret[MP_KEY_LEN] = {}, publicKey[MP_KEY_LEN] = {};
	bool sealed = false;
	bool unopened = false;	// a sealed frame didn't open: a wrong password, or tampering
	uint8_t sendKey[MP_KEY_LEN] = {}, receiveKey[MP_KEY_LEN] = {};
	uint64_t sent = 0, received = 0;	// the nonces
};

static inline void
MpNewKeyPair(MpConnection &c)
{
	MpRandom(c.secret, sizeof(c.secret));
	crypto_x25519_public_key(c.publicKey, c.secret);
}

// Both ends: the relay's and the editor's public keys, the password key.
static inline void
MpSeal(MpConnection &c, bool relay, const uint8_t relayKey[MP_KEY_LEN], const uint8_t editorKey[MP_KEY_LEN],
	const uint8_t passwordKey[MP_KEY_LEN])
{
	uint8_t shared[MP_KEY_LEN], message[3 * MP_KEY_LEN], keys[2 * MP_KEY_LEN];
	crypto_x25519(shared, c.secret, relay ? editorKey : relayKey);
	memcpy(message, shared, MP_KEY_LEN);
	memcpy(message + MP_KEY_LEN, relayKey, MP_KEY_LEN);
	memcpy(message + 2 * MP_KEY_LEN, editorKey, MP_KEY_LEN);
	crypto_blake2b_keyed(keys, sizeof(keys), passwordKey, MP_KEY_LEN, message, sizeof(message));
	memcpy(c.sendKey, keys + (relay ? MP_KEY_LEN : 0), MP_KEY_LEN);
	memcpy(c.receiveKey, keys + (relay ? 0 : MP_KEY_LEN), MP_KEY_LEN);
	crypto_wipe(shared, sizeof(shared));
	crypto_wipe(message, sizeof(message));
	crypto_wipe(keys, sizeof(keys));
	crypto_wipe(c.secret, sizeof(c.secret));
	c.sealed = true;
}

static inline void
MpNonce(uint64_t count, uint8_t nonce[24])
{
	memset(nonce, 0, 24);
	memcpy(nonce, &count, sizeof(count));
}

/*
 * Envelopes: a u32 size, then 0 and a record, or 1, a MAC and the sealed
 * record. Only the handshake and a refusal go unsealed.
 */

static inline void
MpQueue(MpConnection &c, const std::string &record)
{
	std::string envelope;
	if(!c.sealed){
		MpPut(envelope, (uint32_t)(record.size() + 1));
		envelope += '\0';
		envelope += record;
	}else{
		MpPut(envelope, (uint32_t)(record.size() + 17));
		envelope += '\1';
		size_t at = envelope.size();
		envelope.resize(at + 16 + record.size());
		uint8_t nonce[24];
		MpNonce(c.sent++, nonce);
		crypto_aead_lock((uint8_t*)&envelope[at + 16], (uint8_t*)&envelope[at], c.sendKey, nonce, nullptr, 0,
			(const uint8_t*)record.data(), record.size());
	}
	c.out += envelope;
}

struct MpFrame
{
	uint8_t type;
	std::string payload;
};

static inline bool
MpFlush(MpConnection &c)
{
	size_t sent = 0;
	while(sent < c.out.size()){
		int size = (int)std::min(c.out.size() - sent, (size_t)1 << 20);
		int n = (int)send(c.socket, c.out.data() + sent, size, MSG_NOSIGNAL);
		if(n > 0){
			sent += n;
			continue;
		}
		if(n < 0 && MpWouldBlock())
			break;
		return false;
	}
	c.out.erase(0, sent);
	return c.out.size() <= MP_MAX_BACKLOG;
}

static inline bool
MpUnpack(const char *data, size_t size, MpFrame *frame)
{
	if(size < 1)
		return false;
	uint8_t type = (uint8_t)data[0];
	data++, size--;
	frame->type = type & ~MSG_COMPRESSED;
	if(!(type & MSG_COMPRESSED)){
		frame->payload.assign(data, size);
		return true;
	}
	uint32_t unpacked;
	if(size < 4 || !MpLzoReady())
		return false;
	memcpy(&unpacked, data, 4);
	if(unpacked > MP_MAX_UNPACKED)
		return false;
	frame->payload.resize(unpacked);
	lzo_uint out = unpacked;
	return lzo1x_decompress_safe((const unsigned char*)data + 4, size - 4,
		(unsigned char*)&frame->payload[0], &out, nullptr) == LZO_E_OK && out == unpacked;
}

// Reads what has arrived; false when the connection is gone or misbehaves
// (c.unopened: a sealed frame that didn't open). Unsealed frames after the
// handshake can only be refusals.
static inline bool
MpReceive(MpConnection &c, std::vector<MpFrame> &frames)
{
	char buffer[65536];
	bool open = true;
	for(size_t total = 0; total < (32u << 20);){
		int n = (int)recv(c.socket, buffer, sizeof(buffer), 0);
		if(n > 0){
			c.in.append(buffer, n);
			c.heard = MpNow();
			total += n;
			continue;
		}
		// Closed: what came before still counts (a rejection's reason).
		open = n < 0 && MpWouldBlock();
		break;
	}
	size_t at = 0;
	while(c.in.size() - at >= 4){
		uint32_t size;
		memcpy(&size, c.in.data() + at, 4);
		if(size < 2 || size > MP_MAX_FRAME + 17)
			return false;
		if(c.in.size() - at - 4 < size)
			break;
		const char *body = c.in.data() + at + 5;
		size_t length = size - 1;
		std::string record;
		MpFrame frame;
		if(c.in[at + 4] == 0){
			if(!MpUnpack(body, length, &frame) || (c.sealed && frame.type != MSG_REJECT))
				return false;
		}else{
			if(!c.sealed || length < 17)
				return false;
			record.resize(length - 16);
			uint8_t nonce[24];
			MpNonce(c.received++, nonce);
			if(crypto_aead_unlock((uint8_t*)&record[0], (const uint8_t*)body, c.receiveKey, nonce, nullptr, 0,
			   (const uint8_t*)body + 16, record.size()) != 0){
				c.unopened = true;
				return false;
			}
			if(!MpUnpack(record.data(), record.size(), &frame))
				return false;
		}
		bool keying = frame.type == MSG_CHALLENGE || frame.type == MSG_KEY;
		frames.push_back(std::move(frame));
		at += 4 + size;
		if(keying)
			break;	// what follows is sealed with what this sets up: the next call reads it
	}
	c.in.erase(0, at);
	return open;
}

static inline void
MpCopyName(char *dst, const char *src)
{
	snprintf(dst, MP_NAME_LEN, "%s", src && src[0] ? src : "?");
	for(char *p = dst; *p; p++)
		if((unsigned char)*p < 32) *p = ' ';
}
