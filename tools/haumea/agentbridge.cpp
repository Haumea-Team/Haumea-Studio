// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#define _CRT_RAND_S	// rand_s: Namaka's per-start token
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "haumea.h"
#include "editoricons.h"
#include "trains.h"
#include "agentbridge.h"
#include "multiplayer.h"
#include "sapaths.h"
#include "areas.h"
#include "categories.h"
#include "objfilters.h"
#include "modelmeta.h"
#include "materialtext.h"
#include "mapcheck.h"
#include "maptools.h"
#include "radarart.h"
#include "testserver.h"
#include "encoding.h"
#include "namaka.h"
#include "workspace.h"
#include "keycast.h"
#include "i18n.h"
#ifdef RW_GL3
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static const int AGENT_PROTOCOL_VERSION = 1;
// Framed streams support normal bulk receipts while retaining a hard bound.
static const size_t AGENT_MAX_RESPONSE_BYTES = 4 * 1024 * 1024;

static const char*
agentBuildId(void)
{
	// Hash the executable itself rather than one translation unit's timestamp.
	// Incremental builds can relink changes from objectinst.cpp without compiling
	// this file, so __DATE__/__TIME__ alone can falsely identify two binaries as
	// the same build — exactly the stale-deployment failure this field prevents.
	static char buildId[96];
	if(buildId[0])
		return buildId;
	const char *path = sk::args.argc > 0 ? sk::args.argv[0] : nil;
	FILE *file = path ? fopen(path, "rb") : nil;
	if(file == nil){
		snprintf(buildId, sizeof(buildId), "agentbridge-%s-%s", __DATE__, __TIME__);
		return buildId;
	}
	unsigned long long hash = 1469598103934665603ULL;
	unsigned char bytes[16384];
	size_t count;
	while((count = fread(bytes, 1, sizeof(bytes), file)) > 0)
		for(size_t i = 0; i < count; i++){
			hash ^= bytes[i];
			hash *= 1099511628211ULL;
		}
	fclose(file);
	snprintf(buildId, sizeof(buildId), "haumea-fnv1a64-%016llx", hash);
	return buildId;
}

static bool gAgentBridgeInitialized;
static bool gAgentBridgeEnabled;
static bool gAgentCapturePending;
// capture_screen: the frame with the editor's panels, taken after they are drawn.
static bool gAgentCaptureScreen;
static char gAgentBridgeDirectory[1024];
static char gAgentSocketPath[1024];
static char gAgentSceneLogicalPath[256];
static char gAgentScenePhysicalPath[1024];
static char gAgentCapturePath[1024];
static std::string gAgentPendingRequestId;
static uint32 gAgentSceneRevision;
static uint32 gAgentCameraRevision;

struct AgentCameraPose
{
	rw::V3d position;
	rw::V3d target;
	rw::V3d up;
	float fov;
};
static AgentCameraPose gAgentTrackedCamera;
static bool gAgentTrackedCameraValid;
static AgentCameraPose gAgentCapturePose;
static AgentCameraPose gAgentCaptureRestorePose;
static bool gAgentCaptureRestore;
static uint32 gAgentCaptureCameraRevision;
static std::string gAgentCaptureLabel;

// camera_move flies the user's live camera; camera_back undoes the last moves.
static const float AGENT_CAMERA_FLY_SECONDS = 0.3f;
static const int AGENT_CAMERA_BACK_POSES = 10;
static std::vector<AgentCameraPose> gAgentCameraBack;
static bool gAgentCameraFlyPending;
static AgentCameraPose gAgentCameraFlyFrom, gAgentCameraFlyTo;
static float gAgentCameraFlyTime;
static std::string gAgentCameraFlyRequestId;

#ifdef _WIN32
typedef SOCKET AgentSocket;
static const AgentSocket INVALID_AGENT_SOCKET = INVALID_SOCKET;
#else
typedef int AgentSocket;
static const AgentSocket INVALID_AGENT_SOCKET = -1;
#endif
static AgentSocket gAgentSocket = INVALID_AGENT_SOCKET;
static AgentSocket gAgentReplySocket = INVALID_AGENT_SOCKET;
static std::string gAgentToken;
static bool gAgentTcp;
static bool gAgentUnixBound;
static bool gAgentSocketBusy;	// another process answers on the socket: left to it
static int gAgentTcpPort;

static void closeAgentSocket(AgentSocket &socket)
{
	if(socket == INVALID_AGENT_SOCKET) return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
	socket = INVALID_AGENT_SOCKET;
}

static bool setAgentBlocking(AgentSocket socket, bool blocking)
{
#ifdef _WIN32
	u_long mode = blocking ? 0 : 1;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0 && fcntl(socket, F_SETFL,
		blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0;
#endif
}

static bool receiveAgentBytes(char *data, size_t size)
{
	while(size){
		int count = (int)recv(gAgentReplySocket, data, (int)size, 0);
		if(count <= 0) return false;
		data += count;
		size -= count;
	}
	return true;
}

struct AgentSceneSnapshot
{
	int32 id;
	rw::V3d position;
	rw::Quat rotation;
	bool deleted;
	// Everything else an agent edit can change, restored on rollback.
	bool hasState;
	ObjectPawnProperties properties;
	std::vector<std::pair<int, ObjectMaterialOverride>> materials;
	int area;
	int vehicleColor1, vehicleColor2, vehicleRespawnDelay;
	int vehiclePaintjob;
	char vehiclePlate[VEHICLE_PLATE_LEN];
	uint16 vehicleMods[VEHICLE_MOD_SLOTS];
};
static bool gAgentSessionActive;
static std::string gAgentSessionId;
static std::vector<AgentSceneSnapshot> gAgentSessionSnapshot;
static std::vector<TextDraw> gAgentTextDrawSnapshot;	// rollback restores the textdraws too
static unsigned int gAgentTextDrawSeq;	// their change count then: untouched, rollback leaves them be
// Native map instances temporarily suppressed inside a leased scratch session.
// They are tracked separately so rollback never treats unrelated world objects
// as agent-created scene content.
static std::vector<AgentSceneSnapshot> gAgentScopedWorldSnapshot;

// Several agents in one editor: a request names its agent after an '@' in
// its request id (older editors just echo it back), and each agent has its
// own scratch session, swapped into the globals above while its requests
// run. What an agent's requests change is noted as its, so rolling back or
// undoing leaves what another agent changed since alone.
namespace {
struct AgentSession
{
	bool active = false;
	std::string id;
	std::vector<AgentSceneSnapshot> snapshot, scopedWorld;
	std::vector<TextDraw> textDraws;
	unsigned int textDrawSeq = 0;
};
}
static std::map<std::string, AgentSession> gOtherAgentSessions;	// by agent, all but the loaded one
static std::string gLoadedAgent;	// whose session the globals hold
static std::unordered_map<int32, std::string> gLastAgentEdit;	// instance -> the agent that changed it last
static std::string gLastTextDrawAgent;	// the agent that changed the textdraw file last

bool
AgentBridgeSessionActive(void)
{
	if(gAgentSessionActive)
		return true;
	for(const auto &other : gOtherAgentSessions)
		if(other.second.active)
			return true;
	return false;
}

static void
loadAgentSession(const std::string &agent)
{
	if(agent == gLoadedAgent)
		return;
	AgentSession &stored = gOtherAgentSessions[gLoadedAgent];
	stored.active = gAgentSessionActive;
	stored.id = std::move(gAgentSessionId);
	stored.snapshot = std::move(gAgentSessionSnapshot);
	stored.scopedWorld = std::move(gAgentScopedWorldSnapshot);
	stored.textDraws = std::move(gAgentTextDrawSnapshot);
	stored.textDrawSeq = gAgentTextDrawSeq;
	AgentSession next = std::move(gOtherAgentSessions[agent]);
	gOtherAgentSessions.erase(agent);
	gAgentSessionActive = next.active;
	gAgentSessionId = std::move(next.id);
	gAgentSessionSnapshot = std::move(next.snapshot);
	gAgentScopedWorldSnapshot = std::move(next.scopedWorld);
	gAgentTextDrawSnapshot = std::move(next.textDraws);
	gAgentTextDrawSeq = next.textDrawSeq;
	gLoadedAgent = agent;
}

// Another agent changed it after this one: it stays as they left it.
static bool
changedByAnotherAgent(const ObjectInst *inst)
{
	auto it = gLastAgentEdit.find(inst->m_id);
	return it != gLastAgentEdit.end() && it->second != gLoadedAgent;
}

static bool
otherAgentEditing(void)
{
	for(const auto &other : gOtherAgentSessions)
		if(other.second.active)
			return true;
	return false;
}

static std::string
jsonEscape(const char *text)
{
	std::string escaped;
	if(text == nil)
		return escaped;
	for(const unsigned char *p = (const unsigned char*)text; *p; p++){
		switch(*p){
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if(*p >= 0x20)
				escaped += (char)*p;
			break;
		}
	}
	return escaped;
}

static AgentCameraPose
currentCameraPose(void)
{
	AgentCameraPose pose = { TheCamera.m_position, TheCamera.m_target,
		TheCamera.m_up, TheCamera.m_fov };
	return pose;
}

static bool
cameraPoseDiffers(const AgentCameraPose &a, const AgentCameraPose &b)
{
	return rw::length(rw::sub(a.position, b.position)) > 0.0001f ||
		rw::length(rw::sub(a.target, b.target)) > 0.0001f ||
		rw::length(rw::sub(a.up, b.up)) > 0.0001f || fabsf(a.fov - b.fov) > 0.0001f;
}

static void
applyCameraPose(const AgentCameraPose &pose)
{
	TheCamera.m_position = pose.position;
	TheCamera.m_target = pose.target;
	TheCamera.m_at = rw::normalize(rw::sub(pose.target, pose.position));
	TheCamera.m_up = rw::normalize(pose.up);
	TheCamera.m_localup = TheCamera.m_up;
	TheCamera.m_fov = std::max(1.0f, std::min(pose.fov, 150.0f));
	gAgentTrackedCamera = currentCameraPose();
	gAgentTrackedCameraValid = true;
}

static void
trackLiveCamera(void)
{
	AgentCameraPose pose = currentCameraPose();
	if(!gAgentTrackedCameraValid){
		gAgentTrackedCamera = pose;
		gAgentTrackedCameraValid = true;
		return;
	}
	if(cameraPoseDiffers(pose, gAgentTrackedCamera)){
		gAgentCameraRevision++;
		gAgentTrackedCamera = pose;
	}
}

static std::string
cameraPoseJson(const AgentCameraPose &pose)
{
	rw::V3d forward = rw::normalize(rw::sub(pose.target, pose.position));
	rw::V3d right = rw::normalize(rw::cross(forward, pose.up));
	rw::V3d viewUp = rw::normalize(rw::cross(right, forward));
	char body[960];
	snprintf(body, sizeof(body),
		"{\"position\":[%.5f,%.5f,%.5f],\"target\":[%.5f,%.5f,%.5f],"
		"\"forward\":[%.6f,%.6f,%.6f],\"right\":[%.6f,%.6f,%.6f],"
		"\"up\":[%.6f,%.6f,%.6f],\"up_reference\":[%.6f,%.6f,%.6f],\"fov\":%.3f}",
		pose.position.x, pose.position.y, pose.position.z,
		pose.target.x, pose.target.y, pose.target.z,
		forward.x, forward.y, forward.z, right.x, right.y, right.z,
		viewUp.x, viewUp.y, viewUp.z, pose.up.x, pose.up.y, pose.up.z, pose.fov);
	return body;
}

static std::string
cameraContextBody(void)
{
	rw::V3d forward = rw::normalize(rw::sub(TheCamera.m_target, TheCamera.m_position));
	rw::V3d right = rw::normalize(rw::cross(forward, TheCamera.m_up));
	rw::V3d up = rw::normalize(rw::cross(right, forward));
	char body[1024];
	snprintf(body, sizeof(body),
		"\"camera\":{\"position\":[%.5f,%.5f,%.5f],\"target\":[%.5f,%.5f,%.5f],"
		"\"forward\":[%.6f,%.6f,%.6f],\"right\":[%.6f,%.6f,%.6f],"
		"\"up\":[%.6f,%.6f,%.6f],\"fov\":%.3f,\"aspect_ratio\":%.6f,"
		"\"viewport\":[%d,%d],\"near_plane\":%.4f,\"far_plane\":%.4f,"
		"\"projection\":\"perspective\",\"camera_revision\":%u}",
		TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z,
		TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z,
		forward.x, forward.y, forward.z, right.x, right.y, right.z,
		up.x, up.y, up.z, TheCamera.m_fov, TheCamera.m_aspectRatio,
		sk::globals.width, sk::globals.height, TheCamera.m_rwcam->nearPlane,
		TheCamera.m_rwcam->farPlane, gAgentCameraRevision);
	return body;
}

static ObjectInst*
getAgentInstUnderSegment(const rw::V3d &start, const rw::V3d &end,
	rw::V3d *hitPosition, float *hitDistance, float targetTolerance)
{
	rw::V3d delta = rw::sub(end, start);
	float segmentLength = rw::length(delta);
	if(segmentLength < 0.001f)
		return nil;
	Ray ray = { start, rw::normalize(delta) };
	float bestDistance = std::max(0.0f, segmentLength - targetTolerance);
	ObjectInst *best = nil;
	rw::V3d bestHit = { 0.0f, 0.0f, 0.0f };
	for(CPtrNode *node = instances.first; node; node = node->next){
		ObjectInst *inst = (ObjectInst*)node->item;
		if(inst->m_isDeleted || inst->m_numChildren > 0)
			continue;
		if(!gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
			continue;
		rw::V3d hit;
		if(!IntersectRayColModel(ray, inst, &hit))
			continue;
		float distance = rw::dot(rw::sub(hit, start), ray.dir);
		if(distance < 0.01f || distance >= bestDistance)
			continue;
		bestDistance = distance;
		bestHit = hit;
		best = inst;
	}
	if(best){
		if(hitPosition) *hitPosition = bestHit;
		if(hitDistance) *hitDistance = bestDistance;
	}
	return best;
}

static bool gRequestFailed;	// the request being handled answered with an error

static void
writeResponse(const std::string &requestId, bool ok, const std::string &body)
{
	if(!ok)
		gRequestFailed = true;
	std::string response = std::string("{\"protocol_version\":") +
		std::to_string(AGENT_PROTOCOL_VERSION) + ",\"scene_revision\":" +
		std::to_string(gAgentSceneRevision) + ",\"request_id\":\"" +
		jsonEscape(requestId.c_str()) + "\",\"ok\":" + (ok ? "true" : "false") +
		"," + body + "}\n";
	// The framed stream transport removes the tiny AF_UNIX datagram ceiling, but
	// a hard application limit still prevents accidental unbounded replies.
	if(response.size() > AGENT_MAX_RESPONSE_BYTES){
		response = std::string("{\"protocol_version\":") +
			std::to_string(AGENT_PROTOCOL_VERSION) + ",\"scene_revision\":" +
			std::to_string(gAgentSceneRevision) + ",\"request_id\":\"" +
			jsonEscape(requestId.c_str()) +
			"\",\"ok\":false,\"error\":\"response exceeds the engine limit; use a paginated command\"}\n";
	}
	if(gAgentReplySocket != INVALID_AGENT_SOCKET){
		uint32 framedSize = htonl((uint32)response.size());
		const char *parts[2] = { (const char*)&framedSize, response.data() };
		size_t sizes[2] = { sizeof(framedSize), response.size() };
		for(int part = 0; part < 2; part++){
			size_t sent = 0;
			while(sent < sizes[part]){
				int count = (int)send(gAgentReplySocket, parts[part] + sent,
					(int)(sizes[part] - sent),
#ifdef MSG_NOSIGNAL
					MSG_NOSIGNAL
#else
					0
#endif
				);
				if(count <= 0)
					break;
				sent += (size_t)count;
			}
		}
		closeAgentSocket(gAgentReplySocket);
		return;
	}
	char finalPath[1200], temporaryPath[1200];
	snprintf(finalPath, sizeof(finalPath), "%s/response.json", gAgentBridgeDirectory);
	snprintf(temporaryPath, sizeof(temporaryPath), "%s/response.tmp", gAgentBridgeDirectory);
	FILE *file = fopen(temporaryPath, "wb");
	if(file == nil)
		return;
	fwrite(response.data(), 1, response.size(), file);
	fclose(file);
	remove(finalPath);
	rename(temporaryPath, finalPath);
}

static void
shutdownAgentSocket(void)
{
	closeAgentSocket(gAgentSocket);
	closeAgentSocket(gAgentReplySocket);
#ifndef _WIN32
	if(gAgentUnixBound) unlink(gAgentSocketPath);
#endif
}

static bool listenAgentTcp(long port, const char *token);

static bool initializeAgentTcp(const char *portText)
{
	const char *token = getenv("HAUMEA_ENGINE_TOKEN");
	char *end = nil;
	long port = strtol(portText, &end, 10);
	if(!portText[0] || *end || port < 1 || port > 65535 || !token ||
	   strlen(token) < 32 || strlen(token) > 256 || strpbrk(token, "\r\n")){
		log("Agent TCP requires a valid port and HAUMEA_ENGINE_TOKEN (32-256 characters, no newlines)\n");
		return false;
	}
	return listenAgentTcp(port, token);
}

// Loopback only; port 0 lets the system pick one (gAgentTcpPort says which).
static bool listenAgentTcp(long port, const char *token)
{
#ifdef _WIN32
	WSADATA data;
	if(WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
	gAgentSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(gAgentSocket == INVALID_AGENT_SOCKET) return false;
#ifdef _WIN32
	BOOL exclusive = TRUE;
	if(setsockopt(gAgentSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
	   (const char*)&exclusive, sizeof(exclusive)) != 0){
		shutdownAgentSocket();
		return false;
	}
#endif
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)port);
	if(!setAgentBlocking(gAgentSocket, false) ||
	   bind(gAgentSocket, (sockaddr*)&address, sizeof(address)) != 0 ||
	   listen(gAgentSocket, 16) != 0){
		shutdownAgentSocket();
		return false;
	}
	socklen_t length = sizeof(address);
	if(getsockname(gAgentSocket, (sockaddr*)&address, &length) == 0)
		gAgentTcpPort = ntohs(address.sin_port);
	gAgentToken = token;
	gAgentTcp = true;
	atexit(shutdownAgentSocket);
	return true;
}

#ifndef _WIN32
static bool
initializeAgentSocket(void)
{
	if(strlen(gAgentSocketPath) >= sizeof(sockaddr_un::sun_path)){
		log("Agent socket path is too long (%d bytes; AF_UNIX allows %d): %s\n"
			"Use a shorter --agent-socket, such as $XDG_RUNTIME_DIR/haumea.sock.\n",
			(int)strlen(gAgentSocketPath), (int)sizeof(sockaddr_un::sun_path) - 1, gAgentSocketPath);
		return false;
	}
	gAgentSocket = socket(AF_UNIX, SOCK_STREAM, 0);
	if(gAgentSocket < 0)
		return false;
	int flags = fcntl(gAgentSocket, F_GETFL, 0);
	if(flags < 0 || fcntl(gAgentSocket, F_SETFL, flags | O_NONBLOCK) < 0){
		shutdownAgentSocket();
		return false;
	}
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, gAgentSocketPath, sizeof(address.sun_path) - 1);
	// Another Haumea's socket is never taken over: only one nobody answers on
	// (refused, or no file) is stale and replaced. The probe doesn't block, so
	// a stuck peer can't hold up the start; anything unclear counts as busy.
	int probe = socket(AF_UNIX, SOCK_STREAM, 0);
	if(probe >= 0){
		fcntl(probe, F_SETFL, fcntl(probe, F_GETFL, 0) | O_NONBLOCK);
		bool stale = connect(probe, (sockaddr*)&address, sizeof(address)) < 0 &&
			(errno == ECONNREFUSED || errno == ENOENT);
		close(probe);
		if(!stale){
			gAgentSocketBusy = true;
			shutdownAgentSocket();
			log("Agent socket %s is in use by another process (another Haumea?); not listening\n",
				gAgentSocketPath);
			return false;
		}
	}
	unlink(gAgentSocketPath);
	if(bind(gAgentSocket, (sockaddr*)&address, sizeof(address)) < 0){
		shutdownAgentSocket();
		return false;
	}
	if(listen(gAgentSocket, 16) < 0){
		shutdownAgentSocket();
		return false;
	}
	gAgentUnixBound = true;
	chmod(gAgentSocketPath, S_IRUSR | S_IWUSR);
	atexit(shutdownAgentSocket);
	return true;
}
#endif

static void
writeError(const std::string &requestId, const char *message)
{
	writeResponse(requestId, false,
		std::string("\"error\":\"") + jsonEscape(message) + "\"");
}

#ifdef _WIN32
static std::string
namakaConnectionPath(void)
{
	return std::string(GetConfigDir()) + "/namaka/connection.json";
}

// At exit: only while the file is still ours. Had this Haumea hung, another
// may have found it stale and written its own, which must stay.
static void
removeNamakaConnection(void)
{
	if(!gAgentTcp || !gAgentBridgeEnabled)
		return;
	int port = 0;
	if(FILE *f = fopen(namakaConnectionPath().c_str(), "rb")){
		char buffer[512];
		size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
		fclose(f);
		buffer[n] = '\0';
		const char *at = strstr(buffer, "\"port\":");
		if(at)
			sscanf(at + 7, "%d", &port);
	}
	if(port == gAgentTcpPort)
		DeleteFileA(namakaConnectionPath().c_str());
}

// Whether another Haumea answers on the port and token the file names: an
// authenticated ping with a short timeout. Anything else means it's stale.
static bool
namakaConnectionAnswers(int port, const std::string &token)
{
	SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(probe == INVALID_SOCKET)
		return false;
	DWORD timeout = 1500;
	setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)port);
	bool answered = false;
	if(connect(probe, (sockaddr*)&address, sizeof(address)) == 0){
		std::string request = "HAUMEA_AUTH/1 " + token + "\nHAUMEA_IPC/1\nprobe\nping";
		uint32 size = htonl((uint32)request.size());
		char reply[512];
		if(send(probe, (const char*)&size, 4, 0) == 4 &&
		   send(probe, request.data(), (int)request.size(), 0) == (int)request.size()){
			// A framed reply: its size, then the JSON.
			uint32 replySize = 0;
			if(recv(probe, (char*)&replySize, 4, MSG_WAITALL) == 4){
				replySize = ntohl(replySize);
				int want = (int)std::min<uint32>(replySize, sizeof(reply) - 1);
				int n = want > 0 ? recv(probe, reply, want, MSG_WAITALL) : 0;
				if(n > 0){
					reply[n] = '\0';
					answered = strstr(reply, "\"ok\":true") != nil;
				}
			}
		}
	}
	closesocket(probe);
	return answered;
}

static bool
startNamakaTcp(void)
{
	WSADATA data;
	if(WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return false;
	std::string path = namakaConnectionPath();
	// Another Haumea's file is left alone; a stale one is replaced.
	std::string old;
	if(FILE *f = fopen(path.c_str(), "rb")){
		char buffer[1024];
		size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
		fclose(f);
		old.assign(buffer, n);
	}
	int oldPort = 0;
	char oldToken[300] = "";
	// {"port": N, "token": "..."}, as written below (spaces allowed).
	const char *portAt = strstr(old.c_str(), "\"port\":");
	const char *tokenAt = strstr(old.c_str(), "\"token\":");
	if(portAt && tokenAt && sscanf(portAt + 7, "%d", &oldPort) == 1 &&
	   sscanf(tokenAt + 8, " \"%299[^\"]", oldToken) == 1 && namakaConnectionAnswers(oldPort, oldToken)){
		gAgentSocketBusy = true;
		log("Agent: another Haumea is listening for Namaka (127.0.0.1:%d); not listening\n", oldPort);
		return false;
	}
	std::string token;
	for(int i = 0; i < 48; i++){
		unsigned int value = 0;
		if(rand_s(&value) != 0){
			log("Agent: no random numbers for Namaka's key; not listening\n");
			return false;
		}
		static const char hex[] = "0123456789abcdef";
		token += hex[value & 15];
	}
	if(!listenAgentTcp(0, token.c_str()))
		return false;
	// Written beside it and moved over it, so a reader never sees half a file.
	EnsureParentDirectoriesForPath(path.c_str());
	std::string temp = path + ".tmp";
	FILE *f = fopen(temp.c_str(), "wb");
	bool written = f && fprintf(f, "{\"port\": %d, \"token\": \"%s\"}\n", gAgentTcpPort, token.c_str()) > 0;
	if(f)
		written = fclose(f) == 0 && written;
	if(!written || !MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)){
		DeleteFileA(temp.c_str());
		shutdownAgentSocket();
		log("Agent: could not write %s\n", path.c_str());
		return false;
	}
	atexit(removeNamakaConnection);
	return true;
}
#endif

static void
initializeBridge(void)
{
	gAgentBridgeInitialized = true;
	for(int i = 1; i + 1 < sk::args.argc; i++){
		if(strcmp(sk::args.argv[i], "--agent-bridge") == 0){
			strncpy(gAgentBridgeDirectory, sk::args.argv[i + 1], sizeof(gAgentBridgeDirectory) - 1);
			gAgentBridgeDirectory[sizeof(gAgentBridgeDirectory) - 1] = '\0';
		}else if(strcmp(sk::args.argv[i], "--agent-socket") == 0){
			strncpy(gAgentSocketPath, sk::args.argv[i + 1], sizeof(gAgentSocketPath) - 1);
			gAgentSocketPath[sizeof(gAgentSocketPath) - 1] = '\0';
		}
	}
	const char *tcpPort = getenv("HAUMEA_ENGINE_TCP_PORT");
	if(tcpPort){
		gAgentBridgeEnabled = initializeAgentTcp(tcpPort);
		log(gAgentBridgeEnabled ? "Agent authenticated loopback TCP enabled\n" :
			"Agent TCP initialization failed\n");
		return; // Explicit TCP configuration must never fall back to an unauthenticated bridge.
	}
#ifdef _WIN32
	// Windows has no Unix sockets here: a plain start listens for Namaka on
	// loopback TCP with a fresh token, written where haumea_ipc reads it.
	if(!gAgentSocketPath[0] && !gAgentBridgeDirectory[0] && NamakaShouldListen()){
		gAgentBridgeEnabled = startNamakaTcp();
		if(gAgentBridgeEnabled)
			log("Agent IPC v%d listening for Namaka on 127.0.0.1:%d\n", AGENT_PROTOCOL_VERSION, gAgentTcpPort);
		return;
	}
#endif
#ifndef _WIN32
	// A plain start listens for Namaka when it is installed (and allowed).
	if(!gAgentSocketPath[0] && !gAgentBridgeDirectory[0] && NamakaShouldListen()){
		std::string path = NamakaDefaultSocketPath();
		if(path.size() >= sizeof(sockaddr_un::sun_path))
			log("$HOME is too long for the Namaka socket (%d bytes, max %d): set HAUMEA_NAMAKA_SOCKET "
				"to a shorter path, such as $XDG_RUNTIME_DIR/haumea.sock, and HAUMEA_ENGINE_SOCKET to the same\n",
				(int)path.size(), (int)sizeof(sockaddr_un::sun_path) - 1);
		else
			snprintf(gAgentSocketPath, sizeof(gAgentSocketPath), "%s", path.c_str());
	}
	if(gAgentSocketPath[0]){
		gAgentBridgeEnabled = initializeAgentSocket();
		if(gAgentBridgeEnabled)
			log("Agent IPC v%d listening on %s\n", AGENT_PROTOCOL_VERSION, gAgentSocketPath);
		else
			log("Agent IPC could not bind %s: %s\n", gAgentSocketPath, strerror(errno));
	}
#endif
	if(!gAgentBridgeEnabled && gAgentBridgeDirectory[0]){
		gAgentBridgeEnabled = true;
		log("Legacy agent file bridge enabled at %s\n", gAgentBridgeDirectory);
	}
}

static void
splitRequestContent(const std::string &content, std::vector<std::string> &lines)
{
	size_t start = 0;
	while(start <= content.size()){
		size_t end = content.find('\n', start);
		if(end == std::string::npos)
			end = content.size();
		std::string line = content.substr(start, end - start);
		if(!line.empty() && line[line.size() - 1] == '\r')
			line.resize(line.size() - 1);
		lines.push_back(line);
		if(end == content.size())
			break;
		start = end + 1;
	}
}

static bool
readRequest(std::vector<std::string> &lines)
{
	if(gAgentSocket != INVALID_AGENT_SOCKET){
		gAgentReplySocket = accept(gAgentSocket, nil, nil);
		if(gAgentReplySocket == INVALID_AGENT_SOCKET) return false;
		if(!setAgentBlocking(gAgentReplySocket, true)){
			closeAgentSocket(gAgentReplySocket);
			return false;
		}
#ifdef SO_NOSIGPIPE
		int noSignal = 1;
		setsockopt(gAgentReplySocket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
#ifdef _WIN32
		DWORD timeout = 250, sendTimeout = 2000;
#else
		struct timeval timeout = { 0, 250000 }, sendTimeout = { 2, 0 };
#endif
		setsockopt(gAgentReplySocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
		setsockopt(gAgentReplySocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));
		uint32 framedSize = 0;
		if(!receiveAgentBytes((char*)&framedSize, sizeof(framedSize))){
			closeAgentSocket(gAgentReplySocket);
			return false;
		}
		framedSize = ntohl(framedSize);
		if(framedSize == 0 || framedSize > 1024 * 1024){
			closeAgentSocket(gAgentReplySocket);
			return false;
		}
		std::string content(framedSize, '\0');
		if(!receiveAgentBytes(&content[0], framedSize)){
			closeAgentSocket(gAgentReplySocket);
			return false;
		}
		if(gAgentTcp){
			std::string prefix = "HAUMEA_AUTH/1 " + gAgentToken + "\n";
			unsigned int difference = content.size() < prefix.size();
			for(size_t i = 0; i < prefix.size(); i++)
				difference |= (unsigned char)prefix[i] ^
					(i < content.size() ? (unsigned char)content[i] : 0);
			if(difference){
				writeError("unknown", "authentication failed");
				return false;
			}
			content.erase(0, prefix.size());
		}
		splitRequestContent(content, lines);
		if(lines.empty() || lines[0] != "HAUMEA_IPC/1"){
			std::string requestId = lines.size() > 1 ? lines[1] : "unknown";
			writeError(requestId, "unsupported or missing protocol version");
			lines.clear();
			return false;
		}
		lines.erase(lines.begin());
		if(lines.size() < 2 || lines[0].empty() || lines[1].empty()){
			writeError("unknown", "missing request id or command");
			lines.clear();
			return false;
		}
		return true;
	}
	char requestPath[1200];
	snprintf(requestPath, sizeof(requestPath), "%s/request.txt", gAgentBridgeDirectory);
	FILE *file = fopen(requestPath, "rb");
	if(file == nil)
		return false;

	std::string content;
	char buffer[4096];
	while(!feof(file)){
		size_t size = fread(buffer, 1, sizeof(buffer), file);
		content.append(buffer, size);
		if(content.size() > 4 * 1024 * 1024){
			fclose(file);
			remove(requestPath);
			return false;
		}
	}
	fclose(file);
	remove(requestPath);

	splitRequestContent(content, lines);
	return true;
}

static bool
parseInt(const std::string &text, int *value)
{
	char *end = nil;
	long parsed = strtol(text.c_str(), &end, 10);
	if(end == text.c_str() || *end != '\0')
		return false;
	*value = (int)parsed;
	return true;
}

static bool
parseFloat(const std::string &text, float *value)
{
	char *end = nil;
	float parsed = strtof(text.c_str(), &end);
	if(end == text.c_str() || *end != '\0')
		return false;
	*value = parsed;
	return true;
}

static bool
parseFloatVector3(const std::string &text, rw::V3d *value)
{
	const char *cursor = text.c_str();
	float values[3];
	for(int i = 0; i < 3; i++){
		while(isspace((unsigned char)*cursor) || *cursor == '[' || *cursor == ',') cursor++;
		char *end = nil;
		values[i] = strtof(cursor, &end);
		if(end == cursor)
			return false;
		cursor = end;
	}
	while(isspace((unsigned char)*cursor) || *cursor == ',' || *cursor == ']') cursor++;
	if(*cursor != '\0')
		return false;
	*value = { values[0], values[1], values[2] };
	return true;
}

static bool
parseUint32Field(const std::string &text, uint32 *value)
{
	char *end = nil;
	unsigned long long parsed = strtoull(text.c_str(), &end, 0);
	if(end == text.c_str() || *end != '\0' || parsed > 0xFFFFFFFFULL)
		return false;
	*value = (uint32)parsed;
	return true;
}

static ObjectDef*
resolveObject(const std::string &text, int *objectId)
{
	// The agent speaks Pawn model IDs: custom models are negative there.
	int parsedId;
	if(parseInt(text, &parsedId)){
		parsedId = EditorModelId(parsedId);
		ObjectDef *object = GetObjectDef(parsedId);
		if(object){
			*objectId = parsedId;
			return object;
		}
	}
	return GetObjectDef(text.c_str(), objectId);
}

static std::string
lowercase(const char *text)
{
	std::string lowered = text ? text : "";
	for(size_t i = 0; i < lowered.size(); i++)
		lowered[i] = (char)tolower((unsigned char)lowered[i]);
	return lowered;
}

// Without a scratch IPL scene the agent edits the open Pawn map: every
// scripted object, vehicle and element in it. World objects stay reference
// geometry; deleting one becomes a RemoveBuildingForPlayer in the export.
static bool
agentMapMode(void)
{
	return gAgentSceneLogicalPath[0] == '\0';
}

static bool
isAgentSceneInstance(const ObjectInst *inst)
{
	if(inst == nil)
		return false;
	if(agentMapMode())
		return !IsPawnWorldObject((ObjectInst*)inst);
	return inst->m_file && inst->m_file->name &&
		LogicalPathEquals(inst->m_file->name, gAgentSceneLogicalPath);
}

static bool
getAgentGroundPlacementSurface(rw::V3d position, rw::V3d *ground, rw::V3d *normal = nil, bool forStanding = false)
{
	struct SelectionState
	{
		ObjectInst *inst;
		int selected;
	};
	std::vector<SelectionState> states;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted)
			continue;
		states.push_back({ inst, inst->m_selected });
		// Ground placement should use GTA's terrain and existing world as its
		// support, never a roof or prop produced by an earlier agent command.
		inst->m_selected = true;
	}
	bool found = GetGroundPlacementSurface(position, ground, normal, true, forStanding);
	for(size_t i = 0; i < states.size(); i++)
		states[i].inst->m_selected = states[i].selected;
	return found;
}

struct AgentSupportSample
{
	rw::V3d support;
	rw::V3d ground;
	rw::V3d normal;
	float clearance;
};

struct AgentPlacementAnalysis
{
	std::vector<AgentSupportSample> samples;
	int missingSamples;
	float minClearance;
	float maxClearance;
	float minRequiredZ;
	float maxRequiredZ;
	float maxSlopeDegrees;
	rw::V3d averageNormal;
};

struct AgentSupportContract
{
	const char *profile;
	int sampleGridSize;
	float maxFloatingClearance;
	float maxPenetration;
	float maxSupportRelief;
};

struct AgentSupportEvaluation
{
	AgentSupportContract contract;
	bool analyzed;
	bool floating;
	bool embedded;
	bool unsupportedRelief;
	bool missingSupport;
	bool valid;
};

static AgentSupportContract
agentSupportContract(int objectId)
{
	ObjectDef *object = GetObjectDef(objectId);
	float footprint = 0.0f;
	if(object && object->m_colModel){
		CBox box = object->m_colModel->boundingBox;
		footprint = std::max(box.max.x - box.min.x, box.max.y - box.min.y);
	}
	if(footprint >= 6.0f)
		return { "building", 3, 0.15f, 0.55f, 0.60f };
	return { "prop", 3, 0.15f, 0.25f, 0.40f };
}

static AgentSupportEvaluation
evaluateAgentSupport(int objectId, bool analyzed, const AgentPlacementAnalysis &analysis)
{
	AgentSupportContract contract = agentSupportContract(objectId);
	float relief = analyzed ? analysis.maxRequiredZ - analysis.minRequiredZ : 0.0f;
	AgentSupportEvaluation result = {
		contract,
		analyzed,
		analyzed && analysis.maxClearance > contract.maxFloatingClearance,
		analyzed && analysis.minClearance < -contract.maxPenetration,
		analyzed && relief > contract.maxSupportRelief,
		!analyzed || analysis.missingSamples > 0,
		false,
	};
	result.valid = result.analyzed && !result.floating && !result.embedded &&
		!result.unsupportedRelief && !result.missingSupport;
	return result;
}

static std::string
agentSupportEvaluationJson(const AgentSupportEvaluation &evaluation,
	const AgentPlacementAnalysis *analysis)
{
	float minClearance = analysis ? analysis->minClearance : 0.0f;
	float maxClearance = analysis ? analysis->maxClearance : 0.0f;
	float relief = analysis ? analysis->maxRequiredZ - analysis->minRequiredZ : 0.0f;
	char body[768];
	snprintf(body, sizeof(body),
		"{\"contract\":\"terrain-support-v1\",\"profile\":\"%s\",\"sample_grid\":%d,"
		"\"max_floating_clearance\":%.3f,\"max_penetration\":%.3f,"
		"\"max_support_relief\":%.3f,\"analyzed\":%s,\"valid\":%s,"
		"\"floating\":%s,\"embedded\":%s,\"unsupported_relief\":%s,"
		"\"missing_support\":%s,\"min_clearance\":%.4f,\"max_clearance\":%.4f,"
		"\"support_relief\":%.4f}",
		evaluation.contract.profile, evaluation.contract.sampleGridSize,
		evaluation.contract.maxFloatingClearance, evaluation.contract.maxPenetration,
		evaluation.contract.maxSupportRelief, evaluation.analyzed ? "true" : "false",
		evaluation.valid ? "true" : "false", evaluation.floating ? "true" : "false",
		evaluation.embedded ? "true" : "false",
		evaluation.unsupportedRelief ? "true" : "false",
		evaluation.missingSupport ? "true" : "false", minClearance, maxClearance, relief);
	return body;
}

static bool
analyzeAgentPlacement(int objectId, const rw::V3d &position, const rw::Quat &rotation,
	int gridSize, AgentPlacementAnalysis *analysis)
{
	ObjectDef *object = GetObjectDef(objectId);
	if(object == nil || object->m_colModel == nil)
		return false;
	// Corners, edge midpoints, and center capture the rigid footprint while
	// avoiding pathological detailed COL queries on dense intermediate points.
	gridSize = std::max(2, std::min(gridSize, 3));
	analysis->samples.clear();
	analysis->missingSamples = 0;
	analysis->minClearance = 1.0e30f;
	analysis->maxClearance = -1.0e30f;
	analysis->minRequiredZ = 1.0e30f;
	analysis->maxRequiredZ = -1.0e30f;
	analysis->maxSlopeDegrees = 0.0f;
	analysis->averageNormal = { 0.0f, 0.0f, 0.0f };

	CBox box = object->m_colModel->boundingBox;
	rw::Matrix rotationMatrix;
	rotationMatrix.rotate(conj(rotation), rw::COMBINEREPLACE);
	rotationMatrix.pos = { 0.0f, 0.0f, 0.0f };
	const float radiansToDegrees = 57.2957795131f;
	for(int y = 0; y < gridSize; y++){
		float fy = (float)y / (float)(gridSize - 1);
		for(int x = 0; x < gridSize; x++){
			float fx = (float)x / (float)(gridSize - 1);
			rw::V3d local = {
				box.min.x + (box.max.x - box.min.x) * fx,
				box.min.y + (box.max.y - box.min.y) * fy,
				box.min.z
			};
			rw::V3d rotated;
			rw::V3d::transformPoints(&rotated, &local, 1, &rotationMatrix);
			rw::V3d support = add(position, rotated);
			rw::V3d ground, normal;
			if(!getAgentGroundPlacementSurface(support, &ground, &normal)){
				analysis->missingSamples++;
				continue;
			}
			float clearance = support.z - ground.z;
			float requiredZ = ground.z - rotated.z;
			analysis->samples.push_back({ support, ground, normal, clearance });
			analysis->minClearance = std::min(analysis->minClearance, clearance);
			analysis->maxClearance = std::max(analysis->maxClearance, clearance);
			analysis->minRequiredZ = std::min(analysis->minRequiredZ, requiredZ);
			analysis->maxRequiredZ = std::max(analysis->maxRequiredZ, requiredZ);
			analysis->averageNormal = add(analysis->averageNormal, normal);
			float normalZ = std::max(-1.0f, std::min(1.0f, normal.z));
			analysis->maxSlopeDegrees = std::max(analysis->maxSlopeDegrees,
				acosf(normalZ) * radiansToDegrees);
		}
	}
	if(analysis->samples.empty())
		return false;
	analysis->averageNormal = normalize(analysis->averageNormal);
	return true;
}

static std::string
agentPlacementAnalysisJson(const AgentPlacementAnalysis &analysis)
{
	char summary[768];
	snprintf(summary, sizeof(summary),
		"{\"sample_count\":%d,\"missing_samples\":%d,\"min_clearance\":%.4f,"
		"\"max_clearance\":%.4f,\"support_relief\":%.4f,\"recommended_no_float_z\":%.4f,"
		"\"max_slope_degrees\":%.3f,\"average_normal\":[%.5f,%.5f,%.5f],\"samples\":[",
		(int)analysis.samples.size(), analysis.missingSamples, analysis.minClearance,
		analysis.maxClearance, analysis.maxRequiredZ - analysis.minRequiredZ,
		analysis.minRequiredZ, analysis.maxSlopeDegrees, analysis.averageNormal.x,
		analysis.averageNormal.y, analysis.averageNormal.z);
	std::string json = summary;
	for(size_t i = 0; i < analysis.samples.size(); i++){
		if(i) json += ',';
		const AgentSupportSample &sample = analysis.samples[i];
		char item[384];
		snprintf(item, sizeof(item),
			"{\"xy\":[%.3f,%.3f],\"support_z\":%.4f,\"ground_z\":%.4f,"
			"\"clearance\":%.4f,\"normal\":[%.5f,%.5f,%.5f]}",
			sample.support.x, sample.support.y, sample.support.z, sample.ground.z,
			sample.clearance, sample.normal.x, sample.normal.y, sample.normal.z);
		json += item;
	}
	json += "]}";
	return json;
}

static bool
placeObject(const std::vector<std::string> &fields, size_t offset,
	std::vector<ObjectInst*> &created, ObjectInst **placedHd, std::string *error)
{
	if(fields.size() < offset + 6){
		*error = "place requires model, x, y, z, heading and snap";
		return false;
	}
	int objectId;
	ObjectDef *object = resolveObject(fields[offset], &objectId);
	if(object == nil){
		*error = "unknown model: " + fields[offset];
		return false;
	}
	float x, y, z, heading;
	int snap;
	if(!parseFloat(fields[offset + 1], &x) || !parseFloat(fields[offset + 2], &y) ||
	   !parseFloat(fields[offset + 3], &z) || !parseFloat(fields[offset + 4], &heading) ||
	   !parseInt(fields[offset + 5], &snap)){
		*error = "invalid numeric placement field";
		return false;
	}

	rw::V3d position = { x, y, z };
	if(snap){
		rw::V3d ground;
		if(!getAgentGroundPlacementSurface(position, &ground)){
			*error = "no ground surface at placement";
			return false;
		}
		position.z = ground.z + GetPlacementBaseOffset(objectId);
	}
	rw::Quat rotation = MakeObjectRotationDegrees({ 0.0f, 0.0f, heading });
	SetSpawnObjectId(objectId);
	ObjectInst *spawned[4] = {};
	int count = SpawnPlaceObjectNoUndo(position, &rotation, spawned, 4);
	if(count == 0){
		*error = "Haumea could not create the object";
		return false;
	}
	for(int i = 0; i < count; i++)
		created.push_back(spawned[i]);
	*placedHd = spawned[count - 1];
	return true;
}

static bool
requireAgentSession(const std::string &requestId)
{
	if(gAgentSessionActive)
		return true;
	writeError(requestId, "begin a scratch session before mutating the scene");
	return false;
}


static AgentSceneSnapshot
captureAgentSnapshot(ObjectInst *inst)
{
	AgentSceneSnapshot snapshot;
	snapshot.id = inst->m_id;
	snapshot.position = inst->m_translation;
	snapshot.rotation = inst->m_rotation;
	snapshot.deleted = inst->m_isDeleted;
	snapshot.hasState = true;
	snapshot.properties = *GetObjectPawnProperties(inst);
	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
		const ObjectMaterialOverride *m = GetObjectMaterialOverride((const ObjectInst*)inst, slot);
		if(m && (m->active || m->textEnabled))
			snapshot.materials.push_back({ slot, *m });
	}
	snapshot.area = inst->m_area;
	snapshot.vehicleColor1 = inst->m_vehicleColor1;
	snapshot.vehicleColor2 = inst->m_vehicleColor2;
	snapshot.vehicleRespawnDelay = inst->m_vehicleRespawnDelay;
	snapshot.vehiclePaintjob = inst->m_vehiclePaintjob;
	memcpy(snapshot.vehiclePlate, inst->m_vehiclePlate, sizeof(snapshot.vehiclePlate));
	memcpy(snapshot.vehicleMods, inst->m_vehicleMods, sizeof(snapshot.vehicleMods));
	return snapshot;
}

static void
restoreAgentState(ObjectInst *inst, const AgentSceneSnapshot &snapshot)
{
	if(!snapshot.hasState)
		return;
	SetObjectPawnProperties(inst, snapshot.properties, false);
	if(HasObjectMaterialOverrides(inst) || !snapshot.materials.empty()){
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++)
			ClearObjectMaterialOverride(inst, slot, false);
		for(size_t i = 0; i < snapshot.materials.size(); i++)
			SetObjectMaterialOverride(inst, snapshot.materials[i].first,
				snapshot.materials[i].second, false);
	}
	inst->m_area = snapshot.area;
	if(inst->m_isVehicle && (inst->m_vehicleColor1 != snapshot.vehicleColor1 ||
	   inst->m_vehicleColor2 != snapshot.vehicleColor2)){
		inst->m_vehicleColor1 = snapshot.vehicleColor1;
		inst->m_vehicleColor2 = snapshot.vehicleColor2;
		inst->RefreshVehicleColors();
	}
	inst->m_vehicleRespawnDelay = snapshot.vehicleRespawnDelay;
	memcpy(inst->m_vehiclePlate, snapshot.vehiclePlate, sizeof(inst->m_vehiclePlate));
	if(inst->m_isVehicle && (inst->m_vehiclePaintjob != snapshot.vehiclePaintjob ||
	   memcmp(inst->m_vehicleMods, snapshot.vehicleMods, sizeof(inst->m_vehicleMods)) != 0)){
		inst->m_vehiclePaintjob = snapshot.vehiclePaintjob;
		memcpy(inst->m_vehicleMods, snapshot.vehicleMods, sizeof(inst->m_vehicleMods));
		inst->RefreshVehicleUpgrades();
	}
}

static void
beginAgentSession(const std::string &sessionId)
{
	gAgentSessionSnapshot.clear();
	gAgentScopedWorldSnapshot.clear();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		// Deleted instances (a closed map's, say) stay deleted on rollback anyway.
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted)
			continue;
		gAgentSessionSnapshot.push_back(captureAgentSnapshot(inst));
	}
	gAgentTextDrawSnapshot = GetTextDraws();
	gAgentTextDrawSeq = TextDrawsChangeSeq();
	gAgentSessionId = sessionId;
	gAgentSessionActive = true;
}

static void
clearAgentSession(void)
{
	gAgentSessionSnapshot.clear();
	gAgentScopedWorldSnapshot.clear();
	gAgentSessionId.clear();
	gAgentSessionActive = false;
}

static void
restoreAgentTransform(ObjectInst *inst, const AgentSceneSnapshot &snapshot)
{
	std::vector<UndoTransform> transforms;
	if(CaptureObjectTransformTargets(inst, false, transforms)){
		PreviewObjectTransformTargets(inst, transforms, snapshot.position,
			snapshot.rotation, UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
		CommitObjectTransformTargets(transforms);
	}
}

static void
rollbackAgentSession(void)
{
	std::unordered_map<int32, AgentSceneSnapshot> snapshots;
	for(size_t i = 0; i < gAgentSessionSnapshot.size(); i++)
		snapshots[gAgentSessionSnapshot[i].id] = gAgentSessionSnapshot[i];

	// In a multiplayer session, what someone else changed last stays theirs:
	// only this editor's own edits are rolled back.
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!isAgentSceneInstance(inst) || MultiplayerChangedByOthers(inst) || changedByAnotherAgent(inst))
			continue;
		auto found = snapshots.find(inst->m_id);
		if(found == snapshots.end()){
			if(!inst->m_isDeleted)
				inst->Delete();
			continue;
		}
		const AgentSceneSnapshot &snapshot = found->second;
		if(inst->m_isDeleted && !snapshot.deleted)
			inst->Undelete();
		else if(!inst->m_isDeleted && snapshot.deleted)
			inst->Delete();
		if(!snapshot.deleted){
			restoreAgentTransform(inst, snapshot);
			restoreAgentState(inst, snapshot);
		}
	}
	for(size_t i = 0; i < gAgentScopedWorldSnapshot.size(); i++){
		const AgentSceneSnapshot &snapshot = gAgentScopedWorldSnapshot[i];
		ObjectInst *inst = GetInstanceByID(snapshot.id);
		if(inst == nil || MultiplayerChangedByOthers(inst) || changedByAnotherAgent(inst))
			continue;
		if(agentMapMode() && inst->m_isDeleted != snapshot.deleted){
			// Removed through the Pawn map's normal delete.
			if(snapshot.deleted)
				inst->Delete();
			else
				inst->Undelete();
			continue;
		}
		// Native instances intentionally remain outside the editable IPL document,
		// so normal Delete/Undelete guards must not mark their source archive dirty.
		inst->m_isDeleted = snapshot.deleted;
		if(snapshot.deleted)
			inst->Deselect();
	}
	// Edited or saved during the session: back to the snapshot, which then
	// differs from what is on disk.
	if(TextDrawsChangeSeq() != gAgentTextDrawSeq && (gLastTextDrawAgent.empty() || gLastTextDrawAgent == gLoadedAgent)){
		GetTextDraws() = gAgentTextDrawSnapshot;
		TextDrawsChanged();
	}
	ClearSelection();
	ResetUndoHistory();
	clearAgentSession();
}

static int
countAgentInstances(bool includeLods)
{
	int count = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted)
			continue;
		if(!includeLods && inst->m_numChildren > 0)
			continue;
		count++;
	}
	return count;
}

// Material text and names travel as one IPC line, so the companion escapes
// them: \\, \n, \r, \t and \xHH for bytes outside ASCII (SA-MP draws the
// text as Windows-1252 bytes).
static std::string
decodeAgentText(const std::string &text)
{
	std::string out;
	for(size_t i = 0; i < text.size(); i++){
		char c = text[i];
		if(c != '\\' || i + 1 >= text.size()){
			out += c;
			continue;
		}
		char next = text[++i];
		switch(next){
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		case 'x':
			if(i + 2 < text.size() && isxdigit((unsigned char)text[i+1]) &&
			   isxdigit((unsigned char)text[i+2])){
				out += (char)strtol(text.substr(i+1, 2).c_str(), nil, 16);
				i += 2;
			}else
				out += 'x';
			break;
		default: out += next; break;
		}
	}
	return out;
}

// JSON for strings: the editor's text is UTF-8, which JSON carries as it is;
// control characters, quotes and backslashes are escaped.
static std::string
jsonEscapeBytes(const char *text)
{
	std::string escaped;
	// Bytes that aren't UTF-8 (which the editor's text never should be) go
	// out as \u00XX, so the reply stays valid JSON.
	bool utf8 = text && IsValidUtf8(text);
	for(const unsigned char *p = (const unsigned char*)text; p && *p; p++){
		if(*p < 0x20 || *p == '"' || *p == '\\' || (*p >= 0x80 && !utf8)){
			char code[8];
			snprintf(code, sizeof(code), "\\u%04x", *p);
			escaped += code;
		}else
			escaped += (char)*p;
	}
	return escaped;
}

static bool
parseColorField(const std::string &text, rw::RGBA *color)
{
	char *end = nil;
	long long parsed = strtoll(text.c_str(), &end, 0);
	if(end == text.c_str() || *end != '\0')
		return false;
	uint32 argb = (uint32)parsed;
	*color = { (uint8)(argb >> 16), (uint8)(argb >> 8), (uint8)argb, (uint8)(argb >> 24) };
	return true;
}

static uint32
argbFromColor(const rw::RGBA &color)
{
	return (uint32)color.alpha << 24 | (uint32)color.red << 16 |
	       (uint32)color.green << 8 | color.blue;
}

static std::string
agentMaterialsJson(ObjectInst *inst)
{
	std::string json;
	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
		const ObjectMaterialOverride *m = GetObjectMaterialOverride((const ObjectInst*)inst, slot);
		if(m == nil || !(m->active || m->textEnabled))
			continue;
		char item[512];
		if(m->textEnabled){
			snprintf(item, sizeof(item),
				"{\"slot\":%d,\"kind\":\"text\",\"material_size\":%d,\"font_size\":%d,"
				"\"bold\":%s,\"text_color\":%u,\"background_color\":%u,\"alignment\":%d,",
				slot, m->materialSize, m->fontSize, m->bold ? "true" : "false",
				argbFromColor(m->textColor), argbFromColor(m->backgroundColor), m->alignment);
			json += json.empty() ? "" : ",";
			json += item;
			json += "\"font\":\"" + jsonEscapeBytes(m->font) + "\",\"text\":\"" +
				jsonEscapeBytes(m->text) + "\"}";
		}else{
			snprintf(item, sizeof(item),
				"{\"slot\":%d,\"kind\":\"texture\",\"model_id\":%d,\"color\":%u,",
				slot, m->hasTexture ? PawnModelId(m->sourceModel) : -1, argbFromColor(m->color));
			json += json.empty() ? "" : ",";
			json += item;
			json += "\"txd\":\"" + jsonEscapeBytes(m->txdName) + "\",\"texture\":\"" +
				jsonEscapeBytes(m->textureName) + "\"}";
		}
	}
	return json;
}

static ObjectInst*
agentMaterialTarget(const std::string &requestId, const std::vector<std::string> &lines,
	int *slot)
{
	int instanceId;
	if(lines.size() < 4 || !parseInt(lines[2], &instanceId) || !parseInt(lines[3], slot)){
		writeError(requestId, "material commands require an instance id and a slot");
		return nil;
	}
	ObjectInst *inst = GetInstanceByID(instanceId);
	if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
		writeError(requestId, "instance does not belong to the active agent scene");
		return nil;
	}
	if(inst->m_isVehicle){
		writeError(requestId, "vehicles have no SA-MP material slots");
		return nil;
	}
	if(*slot < -1 || *slot >= MAX_PAWN_MATERIALS){
		writeError(requestId, "slot must be 0-15 (or -1 to clear every slot)");
		return nil;
	}
	return inst;
}

// ---- SA-MP / open.mp elements ----

// Agent names for SampElementKind, plus "vehicle", which is an object in the
// enum but a separate kind to the agent.
static const char *const agentKindNames[NUM_SAMP_ELEMENT_KINDS] = {
	"object", "pickup", "label", "actor", "class", "gangzone", "mapicon", "npc", "pathpoint", "sound",
	"checkpoint", "area"
};
static const int AGENT_KIND_VEHICLE = NUM_SAMP_ELEMENT_KINDS;

static const char *const agentAreaShapes[NUM_SAMP_AREA_SHAPES] = {
	"circle", "cylinder", "sphere", "rectangle", "cuboid", "cube", "polygon"
};

static int
parseAgentKind(const std::string &text)
{
	std::string name = lowercase(text.c_str());
	if(name == "vehicle")
		return AGENT_KIND_VEHICLE;
	if(name == "3dtext" || name == "text_label")
		return SAMP_ELEMENT_LABEL;
	if(name == "gang_zone")
		return SAMP_ELEMENT_GANGZONE;
	if(name == "map_icon")
		return SAMP_ELEMENT_MAPICON;
	if(name == "path_point")
		return SAMP_ELEMENT_PATHPOINT;
	if(name == "cp" || name == "race_checkpoint")
		return SAMP_ELEMENT_CHECKPOINT;
	if(name == "dynamic_area")
		return SAMP_ELEMENT_AREA;
	for(int i = 0; i < NUM_SAMP_ELEMENT_KINDS; i++)
		if(name == agentKindNames[i])
			return i;
	return -1;
}

static int
agentKindOf(const ObjectInst *inst)
{
	return inst->m_isVehicle ? AGENT_KIND_VEHICLE : GetSampElementKind(inst);
}

static const char*
agentKindName(int kind)
{
	return kind == AGENT_KIND_VEHICLE ? "vehicle" :
		kind >= 0 && kind < NUM_SAMP_ELEMENT_KINDS ? agentKindNames[kind] : "object";
}

static std::string
jsonFloat(float value)
{
	char text[32];
	snprintf(text, sizeof(text), "%.4g", value);
	return text;
}

static std::string
jsonVector3(const rw::V3d &value)
{
	return "[" + jsonFloat(value.x) + "," + jsonFloat(value.y) + "," + jsonFloat(value.z) + "]";
}

static std::string
jsonColor(uint32 rgba)
{
	char text[16];
	snprintf(text, sizeof(text), "\"0x%08X\"", rgba);
	return text;
}

// The properties that matter for the instance's kind, keyed like `properties`.
static std::string
agentPropertiesJson(ObjectInst *inst)
{
	const ObjectPawnProperties &p = *GetObjectPawnProperties(inst);
	int kind = agentKindOf(inst);
	std::string json = "{";
	auto add = [&json](const char *key, const std::string &value){
		if(json.size() > 1) json += ',';
		json += std::string("\"") + key + "\":" + value;
	};
	auto str = [](const char *text){ return "\"" + jsonEscapeBytes(text) + "\""; };
	if(p.name[0]) add("name", str(p.name));
	if(p.group[0]) add("group", str(p.group));
	if(p.comment[0]) add("comment", str(p.comment));
	add("world", std::to_string(p.virtualWorld));
	add("interior", std::to_string(inst->m_area));
	if(p.interiorZeroOnly && inst->m_area == 0)
		add("interior_zero_only", "true");
	if(kind != SAMP_ELEMENT_NPC && kind != SAMP_ELEMENT_CLASS && kind != SAMP_ELEMENT_PATHPOINT &&
	   kind != SAMP_ELEMENT_SOUND && kind != SAMP_ELEMENT_CHECKPOINT && kind != SAMP_ELEMENT_AREA){
		add("draw_distance", jsonFloat(p.drawDistance));
		add("stream_distance", jsonFloat(p.streamDistance));
	}
	if(kind != AGENT_KIND_VEHICLE && SampElementTakesArea(kind) && p.areaName[0])
		add("area", str(p.areaName));
	switch(kind){
	case SAMP_ELEMENT_OBJECT:
		if(p.attachParent >= 0 || p.attachParent == PAWN_ATTACH_PLAYER){
			add("attach_to", p.attachParent == PAWN_ATTACH_PLAYER ? str("player") : std::to_string(p.attachParent));
			add("attach_sync_rotation", p.attachSyncRotation ? "true" : "false");
			if(p.attachBone >= 1){
				add("attach_bone", std::to_string(p.attachBone));
				add("attach_slot", std::to_string(p.attachSlot));
				add("attach_offset", jsonVector3(p.attachOffset));
				add("attach_rotation", jsonVector3(p.attachRotation));
				add("attach_scale", jsonVector3(p.attachScale));
				add("attach_color1", jsonColor(p.attachColor1));
				add("attach_color2", jsonColor(p.attachColor2));
				add("attach_skin", std::to_string(p.attachSkin));
			}
		}
		if(p.moveEnabled){
			add("move_offset", jsonVector3(p.moveOffset));
			add("move_speed", jsonFloat(p.moveSpeed));
			if(p.moveRotates)
				add("move_rotation", jsonVector3(p.moveRotation));
		}
		break;
	case AGENT_KIND_VEHICLE:
		add("color1", std::to_string(inst->m_vehicleColor1));
		add("color2", std::to_string(inst->m_vehicleColor2));
		add("respawn_delay", std::to_string(inst->m_vehicleRespawnDelay));
		add("paintjob", std::to_string(inst->m_vehiclePaintjob - 1));
		add("plate", str(inst->m_vehiclePlate));
		{
			std::string mods = "[";
			for(int i = 0; i < VEHICLE_MOD_SLOTS; i++)
				if(inst->m_vehicleMods[i])
					mods += (mods.size() > 1 ? "," : "") + std::to_string(inst->m_vehicleMods[i]);
			add("mods", mods + "]");
		}
		break;
	case SAMP_ELEMENT_PICKUP:
		add("model", std::to_string(p.elementModel));
		add("pickup_type", std::to_string(p.pickupType));
		break;
	case SAMP_ELEMENT_LABEL:
		add("text", str(p.labelText));
		add("color", jsonColor(p.labelColor));
		add("test_los", p.labelTestLOS ? "true" : "false");
		break;
	case SAMP_ELEMENT_ACTOR:
		add("skin", std::to_string(p.elementModel));
		add("health", jsonFloat(p.actorHealth));
		add("invulnerable", p.actorInvulnerable ? "true" : "false");
		break;
	case SAMP_ELEMENT_CLASS:{
		add("skin", std::to_string(p.elementModel));
		add("team", std::to_string(p.classTeam));
		std::string weapons = "[", ammo = "[";
		for(int i = 0; i < PAWN_CLASS_WEAPONS; i++){
			weapons += (i ? "," : "") + std::to_string(p.classWeapons[i]);
			ammo += (i ? "," : "") + std::to_string(p.classAmmo[i]);
		}
		add("weapons", weapons + "]");
		add("ammo", ammo + "]");
		break;
	}
	case SAMP_ELEMENT_GANGZONE:
		add("size_x", jsonFloat(p.zoneSizeX));
		add("size_y", jsonFloat(p.zoneSizeY));
		add("color", jsonColor(p.zoneColor));
		add("zone_mode", std::string("\"") + (p.zoneMode == SAMP_ZONE_PLAYER ? "player" :
			p.zoneMode == SAMP_ZONE_STREAMED ? "streamed" : "global") + "\"");
		break;
	case SAMP_ELEMENT_MAPICON:
		add("icon", std::to_string(p.mapIconType));
		add("color", jsonColor(p.mapIconColor));
		add("style", std::to_string(p.mapIconStyle));
		break;
	case SAMP_ELEMENT_NPC:
		add("npc_name", str(p.npcName));
		add("skin", std::to_string(p.elementModel));
		add("health", jsonFloat(p.actorHealth));
		add("armour", jsonFloat(p.npcArmour));
		add("invulnerable", p.actorInvulnerable ? "true" : "false");
		add("weapon", std::to_string(p.npcWeapon));
		add("ammo", std::to_string(p.npcAmmo));
		add("path", str(p.pathName));
		add("move_type", std::to_string(p.npcMoveType));
		add("move_speed", jsonFloat(p.npcMoveSpeed));
		add("path_reversed", p.npcPathReversed ? "true" : "false");
		add("path_repeat", p.npcPathRepeat == NPC_PATH_LOOP ? "\"loop\"" :
			p.npcPathRepeat == NPC_PATH_BACK_AND_FORTH ? "\"back_and_forth\"" : "\"once\"");
		add("recording", str(p.npcRecording));
		add("recording_at_npc", p.npcRecordingAtNpc ? "true" : "false");
		add("vehicle", std::to_string(p.npcVehicle));
		add("seat", std::to_string(p.npcSeat));
		add("plays_node", p.npcPlaysNode ? "true" : "false");
		add("node", std::to_string(p.npcNode));
		add("node_point", std::to_string(p.npcNodePoint));
		add("node_radius", jsonFloat(p.npcNodeRadius));
		add("node_set_angle", p.npcNodeSetAngle ? "true" : "false");
		break;
	case SAMP_ELEMENT_PATHPOINT:
		add("path", str(p.pathName));
		add("order", std::to_string(p.pathOrder));
		add("stop_range", jsonFloat(p.pathStopRange));
		break;
	case SAMP_ELEMENT_CHECKPOINT:
		add("stream_distance", jsonFloat(p.streamDistance));
		add("checkpoint_kind", p.cpKind == SAMP_CHECKPOINT_RACE ? "\"race\"" : "\"normal\"");
		add("size", jsonFloat(p.cpSize));
		if(p.cpKind == SAMP_CHECKPOINT_RACE){
			add("race_type", std::to_string(p.cpRaceType));
			add("race", str(p.cpRace));
			add("order", std::to_string(p.cpOrder));
			add("race_mode", p.cpRaceMode == SAMP_RACE_CIRCUIT ? "\"circuit\"" : "\"point_to_point\"");
			if(p.cpRaceMode == SAMP_RACE_CIRCUIT)
				add("race_laps", std::to_string(p.cpRaceLaps));
			add("race_playable", p.cpRacePlayable ? "true" : "false");
		}
		break;
	case SAMP_ELEMENT_AREA:{
		int shape = p.areaShape;
		add("area_shape", std::string("\"") + agentAreaShapes[shape] + "\"");
		if(shape == SAMP_AREA_CIRCLE || shape == SAMP_AREA_CYLINDER || shape == SAMP_AREA_SPHERE)
			add("radius", jsonFloat(p.areaRadius));
		if(shape == SAMP_AREA_RECTANGLE || shape == SAMP_AREA_CUBOID || shape == SAMP_AREA_CUBE){
			add("size_x", jsonFloat(p.areaSize.x));
			add("size_y", jsonFloat(p.areaSize.y));
		}
		if(shape == SAMP_AREA_CUBOID || shape == SAMP_AREA_CUBE)
			add("size_z", jsonFloat(p.areaSize.z));
		if(shape == SAMP_AREA_CYLINDER || shape == SAMP_AREA_POLYGON)
			add("height", jsonFloat(p.areaHeight));
		if(shape == SAMP_AREA_POLYGON){
			std::string points = "[";
			for(int k = 0; k < p.areaPointCount; k++)
				points += std::string(k ? "," : "") + "[" + jsonFloat(inst->m_translation.x + p.areaPoints[k][0]) +
					"," + jsonFloat(inst->m_translation.y + p.areaPoints[k][1]) + "]";
			add("points", points + "]");
		}
		add("items", std::to_string(CountItemsInArea(p.name)));
		break;
	}
	case SAMP_ELEMENT_SOUND:
		add("sound_kind", p.soundKind == SAMP_SOUND_STREAM ? "\"stream\"" : "\"game\"");
		add("radius", jsonFloat(p.soundRadius));
		if(p.soundKind == SAMP_SOUND_STREAM){
			add("url", str(p.soundUrl));
			add("use_position", p.soundUsePos ? "true" : "false");
		}else{
			add("sound_id", std::to_string(p.soundId));
			add("stop_on_leave", p.soundStopOnLeave ? "true" : "false");
		}
		break;
	}
	if((kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_NPC) && p.animName[0]){
		add("anim_lib", str(p.animLib));
		add("anim_name", str(p.animName));
		add("anim_delta", jsonFloat(p.animDelta));
		add("anim_loop", p.animLoop ? "true" : "false");
		add("anim_lock_x", p.animLockX ? "true" : "false");
		add("anim_lock_y", p.animLockY ? "true" : "false");
		add("anim_freeze", p.animFreeze ? "true" : "false");
		add("anim_time", std::to_string(p.animTime));
	}
	return json + "}";
}

static bool
parseBoolField(const std::string &text, bool *value)
{
	std::string v = lowercase(text.c_str());
	if(v == "1" || v == "true" || v == "yes" || v == "on"){ *value = true; return true; }
	if(v == "0" || v == "false" || v == "no" || v == "off"){ *value = false; return true; }
	return false;
}

// 0xRRGGBBAA, #RRGGBBAA or a number.
static bool
parseRgbaField(const std::string &text, uint32 *value)
{
	std::string t = text;
	if(!t.empty() && t[0] == '#')
		t = "0x" + t.substr(1);
	char *end = nil;
	long long parsed = strtoll(t.c_str(), &end, 0);
	if(end == t.c_str() || *end != '\0')
		return false;
	*value = (uint32)parsed;
	return true;
}

static bool
parseIntList(const std::string &text, int *out, int count)
{
	size_t start = 0;
	for(int i = 0; i < count; i++){
		size_t end = text.find(',', start);
		std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		while(!item.empty() && (item[0] == ' ' || item[0] == '[')) item.erase(0, 1);
		while(!item.empty() && (item.back() == ' ' || item.back() == ']')) item.pop_back();
		if(item.empty())
			out[i] = 0;
		else if(!parseInt(item, &out[i]))
			return false;
		if(end == std::string::npos){
			for(i++; i < count; i++) out[i] = 0;
			break;
		}
		start = end + 1;
	}
	return true;
}

// "parts that fit: Spoiler 1000, 1001; Exhaust ...", for error messages.
static std::string
agentFittingComponents(int vehicleModel)
{
	std::string bySlot[VEHICLE_MOD_SLOTS];
	for(int c = 1000; c <= 1193; c++){
		if(!IsVehicleComponentValid(vehicleModel, c))
			continue;
		int slot = GetVehicleComponentSlot(c);
		if(slot < 0 || slot >= VEHICLE_MOD_SLOTS)
			continue;
		bySlot[slot] += (bySlot[slot].empty() ? "" : ", ") + std::to_string(c);
	}
	std::string text;
	for(int slot = 0; slot < VEHICLE_MOD_SLOTS; slot++)
		if(!bySlot[slot].empty())
			text += (text.empty() ? "" : "; ") + std::string(GetVehicleComponentSlotName(slot)) + " " + bySlot[slot];
	return text.empty() ? "this vehicle takes no tuning parts" : "parts that fit, one per slot: " + text;
}

// Edits that are not ObjectPawnProperties fields.
struct AgentInstanceEdit
{
	bool setArea;
	int area;
	bool setColors;
	int color1, color2;
	bool setRespawn;
	int respawnDelay;
	bool setAttach;
	int32 attachTo;
	bool setAttachBone;
	bool setAttachSlot;
	bool setAttachOffset;
	bool setAttachRotation;
	bool setAttachScale;
	bool setAttachColor1;
	bool setAttachColor2;
	bool setAttachSkin;
	bool syncRace;	// race_mode/laps/playable: copied to the race's other checkpoints
	bool setPaintjob;
	int paintjob;
	bool setPlate;
	std::string plate;
	bool setMods;
	uint16 mods[VEHICLE_MOD_SLOTS];
};

// Applies one key=value to the properties of an instance of `kind`.
static bool
applyAgentProperty(ObjectInst *inst, int kind, ObjectPawnProperties &p, AgentInstanceEdit &edit,
	const std::string &key, const std::string &rawValue, std::string *error)
{
	std::string value = decodeAgentText(rawValue);
	auto fail = [&](const char *why){ *error = key + ": " + why; return false; };
	auto only = [&](std::initializer_list<int> kinds){
		for(int k : kinds) if(k == kind) return true;
		const char *name = agentKindName(kind);
		*error = key + " does not apply to " + (strchr("aeiou", name[0]) ? "an " : "a ") + name;
		return false;
	};
	auto copy = [&](char *dst, size_t size){
		if(value.size() >= size) return fail("too long");
		memcpy(dst, value.c_str(), value.size() + 1);
		return true;
	};
	int i; float f; bool b; uint32 c;
	bool isPed = kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_NPC;

	if(key == "name"){
		if(!value.empty() && !IsValidPawnObjectName(value.c_str()))
			return fail("not a valid Pawn identifier");
		if(!value.empty() && FindObjectByPawnName(value.c_str(), inst))
			return fail("another element already uses this name");
		return copy(p.name, sizeof(p.name));
	}
	if(key == "group") return copy(p.group, sizeof(p.group));
	if(key == "comment") return copy(p.comment, sizeof(p.comment));
	if(key == "world"){
		if(!parseInt(value, &i)) return fail("expects an integer (-1: every world)");
		p.virtualWorld = i; return true;
	}
	if(key == "interior"){
		if(!parseInt(value, &i) || i < 0 || i > 255) return fail("expects 0-255");
		edit.setArea = true; edit.area = i; return true;
	}
	if(key == "interior_zero_only"){
		if(kind == AGENT_KIND_VEHICLE) return fail("vehicles use interior");
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		p.interiorZeroOnly = b; return true;
	}
	if(key == "draw_distance" || key == "stream_distance"){
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a distance >= 0 (0: default)");
		(key == "draw_distance" ? p.drawDistance : p.streamDistance) = f; return true;
	}
	if(key == "vehicle"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i)) return fail("expects a vehicle's instance id, or -1 for on foot");
		ObjectInst *vehicle = i >= 0 ? GetInstanceByID(i) : nil;
		if(i >= 0 && (vehicle == nil || !vehicle->m_isVehicle || vehicle->m_isDeleted))
			return fail("is not a vehicle in the map");
		p.npcVehicle = i < 0 ? -1 : i; return true;
	}
	if(key == "seat"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i) || i < 0 || i > 3) return fail("expects 0 (driver) to 3");
		p.npcSeat = i; return true;
	}
	if(key == "attach_to"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(lowercase(value.c_str()) == "player")
			i = PAWN_ATTACH_PLAYER;
		else if(!parseInt(value, &i) || i < PAWN_ATTACH_NONE) return fail("expects an instance id, player, or -1 to detach");
		edit.setAttach = true; edit.attachTo = i; return true;
	}
	if(key == "attach_sync_rotation"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		p.attachSyncRotation = b; return true;
	}
	if(key == "attach_bone"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(!parseInt(value, &i) || i < 1 || i > 18) return fail("expects a player bone id from 1 to 18");
		p.attachBone = i; edit.setAttachBone = true; return true;
	}
	if(key == "attach_slot"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(!parseInt(value, &i) || i < 0 || i > 9) return fail("expects a player slot from 0 to 9");
		p.attachSlot = i; edit.setAttachSlot = true; return true;
	}
	if(key == "move_offset" || key == "move_rotation"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(lowercase(value.c_str()) == "none"){
			if(key == "move_offset") p.moveEnabled = false;
			else p.moveRotates = false;
			return true;
		}
		rw::V3d vector;
		if(!parseFloatVector3(value, &vector)) return fail("expects three numbers, e.g. [0.0, 0.0, 3.0], or none");
		if(!p.moveEnabled && p.moveSpeed <= 0.0f) p.moveSpeed = 2.0f;
		p.moveEnabled = true;
		if(key == "move_offset") p.moveOffset = vector;
		else{ p.moveRotation = vector; p.moveRotates = true; }
		return true;
	}
	if(key == "move_speed"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(!parseFloat(value, &f) || f <= 0.0f || f > 1000.0f) return fail("expects metres a second, above 0");
		p.moveEnabled = true; p.moveSpeed = f; return true;
	}
	if(key == "attach_offset" || key == "attach_rotation" || key == "attach_scale"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		rw::V3d vector;
		if(!parseFloatVector3(value, &vector)) return fail("expects three numbers, e.g. [0.0, 0.0, 0.0]");
		if(key == "attach_offset"){
			p.attachOffset = vector; edit.setAttachOffset = true;
		}else if(key == "attach_rotation"){
			p.attachRotation = vector; edit.setAttachRotation = true;
		}else{
			if(vector.x <= 0.0f || vector.y <= 0.0f || vector.z <= 0.0f ||
			   vector.x > 50.0f || vector.y > 50.0f || vector.z > 50.0f)
				return fail("scale values must be above 0 and at most 50");
			p.attachScale = vector; edit.setAttachScale = true;
		}
		return true;
	}
	if(key == "attach_color1" || key == "attach_color2"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		uint32 color;
		if(!parseUint32Field(value, &color)) return fail("expects a 32-bit colour, e.g. 0xFFFFFFFF");
		if(key == "attach_color1"){ p.attachColor1 = color; edit.setAttachColor1 = true; }
		else { p.attachColor2 = color; edit.setAttachColor2 = true; }
		return true;
	}
	if(key == "attach_skin"){
		if(!only({ SAMP_ELEMENT_OBJECT })) return false;
		if(!parseInt(value, &i) || i < 0 || i > 311) return fail("expects a skin id from 0 to 311");
		p.attachSkin = i; edit.setAttachSkin = true; return true;
	}
	if(key == "color1" || key == "color2"){
		if(!only({ AGENT_KIND_VEHICLE })) return false;
		if(!parseInt(value, &i) || i < -1 || i > 255) return fail("expects a vehicle colour 0-255, or -1 for random");
		if(!edit.setColors){ edit.color1 = inst->m_vehicleColor1; edit.color2 = inst->m_vehicleColor2; }
		edit.setColors = true;
		(key == "color1" ? edit.color1 : edit.color2) = i; return true;
	}
	if(key == "respawn_delay"){
		if(!only({ AGENT_KIND_VEHICLE })) return false;
		if(!parseInt(value, &i)) return fail("expects seconds (-1: never)");
		edit.setRespawn = true; edit.respawnDelay = i; return true;
	}
	if(key == "paintjob"){
		if(!only({ AGENT_KIND_VEHICLE })) return false;
		int count = GetVehiclePaintjobCount(inst->m_objectId);
		if(!parseInt(value, &i) || i < -1 || i >= count){
			*error = key + ": this vehicle has " + std::to_string(count) + " paintjob(s); -1 for none";
			return false;
		}
		edit.setPaintjob = true; edit.paintjob = i + 1; return true;
	}
	if(key == "plate"){
		if(!only({ AGENT_KIND_VEHICLE })) return false;
		if(value.size() >= VEHICLE_PLATE_LEN) return fail("at most 32 characters");
		edit.setPlate = true; edit.plate = value; return true;
	}
	if(key == "mods"){
		if(!only({ AGENT_KIND_VEHICLE })) return false;
		int list[VEHICLE_MOD_SLOTS];
		if(!parseIntList(value, list, VEHICLE_MOD_SLOTS))
			return fail("expects component ids 1000-1193, comma-separated ([] for none)");
		memset(edit.mods, 0, sizeof(edit.mods));
		for(int k = 0; k < VEHICLE_MOD_SLOTS && list[k]; k++){
			if(!IsVehicleComponentValid(inst->m_objectId, list[k])){
				*error = key + ": component " + std::to_string(list[k]) + " does not fit this vehicle; " +
					agentFittingComponents(inst->m_objectId);
				return false;
			}
			int slot = GetVehicleComponentSlot(list[k]);
			if(slot < 0 || slot >= VEHICLE_MOD_SLOTS || edit.mods[slot]){
				*error = key + ": two components for the " + GetVehicleComponentSlotName(slot) + " slot";
				return false;
			}
			edit.mods[slot] = (uint16)list[k];
		}
		edit.setMods = true; return true;
	}
	if(key == "model" || key == "skin"){
		if(!only({ SAMP_ELEMENT_PICKUP, SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_CLASS, SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i)) return fail("expects a model id");
		i = EditorModelId(i);
		if(i < 0) return fail("expects a model id");
		if(isPed && i != 0 && !(GetObjectDef(i) && GetObjectDef(i)->m_isPed))
			return fail("is not a skin");
		if(kind == SAMP_ELEMENT_PICKUP && !(GetObjectDef(i) && !GetObjectDef(i)->m_isPed && !GetObjectDef(i)->m_isVehicle))
			return fail("is not an object model");
		p.elementModel = i; return true;
	}
	if(key == "pickup_type"){
		if(!only({ SAMP_ELEMENT_PICKUP })) return false;
		if(!parseInt(value, &i)) return fail("expects an integer");
		p.pickupType = i; return true;
	}
	if(key == "text"){
		if(!only({ SAMP_ELEMENT_LABEL })) return false;
		if(value.empty()) return fail("must not be empty");
		return copy(p.labelText, sizeof(p.labelText));
	}
	if(key == "color"){
		if(!only({ SAMP_ELEMENT_LABEL, SAMP_ELEMENT_GANGZONE, SAMP_ELEMENT_MAPICON })) return false;
		if(!parseRgbaField(value, &c)) return fail("expects 0xRRGGBBAA");
		(kind == SAMP_ELEMENT_LABEL ? p.labelColor : kind == SAMP_ELEMENT_GANGZONE ? p.zoneColor : p.mapIconColor) = c;
		return true;
	}
	if(key == "test_los"){
		if(!only({ SAMP_ELEMENT_LABEL })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		p.labelTestLOS = b; return true;
	}
	if(key == "health"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a number >= 0");
		p.actorHealth = f; return true;
	}
	if(key == "invulnerable"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		p.actorInvulnerable = b; return true;
	}
	if(key == "team"){
		if(!only({ SAMP_ELEMENT_CLASS })) return false;
		if(!parseInt(value, &i)) return fail("expects a team, or -1 for none");
		p.classTeam = i; return true;
	}
	if(key == "weapons" || (key == "ammo" && kind == SAMP_ELEMENT_CLASS)){
		if(!only({ SAMP_ELEMENT_CLASS })) return false;
		if(!parseIntList(value, key == "weapons" ? p.classWeapons : p.classAmmo, PAWN_CLASS_WEAPONS))
			return fail("expects up to three comma-separated integers");
		return true;
	}
	if(key == "size_x" || key == "size_y"){
		if(!only({ SAMP_ELEMENT_GANGZONE, SAMP_ELEMENT_AREA })) return false;
		if(!parseFloat(value, &f) || f <= 0.0f) return fail("expects a size > 0");
		if(kind == SAMP_ELEMENT_AREA)
			(key == "size_x" ? p.areaSize.x : p.areaSize.y) = f;
		else
			(key == "size_x" ? p.zoneSizeX : p.zoneSizeY) = f;
		return true;
	}
	if(key == "size_z"){
		if(!only({ SAMP_ELEMENT_AREA })) return false;
		if(!parseFloat(value, &f) || f <= 0.0f) return fail("expects a height > 0");
		p.areaSize.z = f; return true;
	}
	if(key == "area_shape"){
		if(!only({ SAMP_ELEMENT_AREA })) return false;
		std::string name = lowercase(value.c_str());
		for(int k = 0; k < NUM_SAMP_AREA_SHAPES; k++)
			if(name == agentAreaShapes[k]){
				p.areaShape = k;
				return true;
			}
		return fail("expects circle, cylinder, sphere, rectangle, cuboid, cube or polygon");
	}
	if(key == "height"){
		if(!only({ SAMP_ELEMENT_AREA })) return false;
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a height >= 0 (a polygon's 0: no height limit)");
		p.areaHeight = f; return true;
	}
	if(key == "points"){
		if(!only({ SAMP_ELEMENT_AREA })) return false;
		// x,y pairs in world coordinates, comma-separated; kept relative to the area's position.
		std::string list = value;
		for(char &ch : list)
			if(ch == '[' || ch == ']' || ch == ';') ch = ',';
		float coords[PAWN_AREA_POINTS * 2];
		int n = 0;
		const char *at = list.c_str();
		while(*at){
			while(*at == ',' || *at == ' ') at++;
			if(*at == '\0') break;
			char *end;
			float v = strtof(at, &end);
			if(end == at) return fail("expects x,y pairs of numbers");
			if(n == PAWN_AREA_POINTS * 2) return fail("has more than 32 corners");
			coords[n++] = v;
			at = end;
		}
		if(n % 2 || n < 6) return fail("expects 3 or more x,y pairs");
		p.areaShape = SAMP_AREA_POLYGON;
		p.areaPointCount = n / 2;
		for(int k = 0; k < n / 2; k++){
			p.areaPoints[k][0] = coords[k*2] - inst->m_translation.x;
			p.areaPoints[k][1] = coords[k*2+1] - inst->m_translation.y;
		}
		return true;
	}
	if(key == "area"){
		if(kind == AGENT_KIND_VEHICLE || !SampElementTakesArea(kind))
			return fail("only objects, pickups, labels, actors, map icons and checkpoints can be limited to an area");
		if(!value.empty() && FindAreaByName(value.c_str()) == nil)
			return fail("is not the name of a dynamic area in the map (\"\" for none)");
		return copy(p.areaName, sizeof(p.areaName));
	}
	if(key == "zone_mode"){
		if(!only({ SAMP_ELEMENT_GANGZONE })) return false;
		std::string name = lowercase(value.c_str());
		if(name == "global") p.zoneMode = SAMP_ZONE_GLOBAL;
		else if(name == "player") p.zoneMode = SAMP_ZONE_PLAYER;
		else if(name == "streamed") p.zoneMode = SAMP_ZONE_STREAMED;
		else return fail("expects global, player or streamed");
		return true;
	}
	if(key == "icon" || key == "style"){
		if(!only({ SAMP_ELEMENT_MAPICON })) return false;
		if(!parseInt(value, &i)) return fail("expects an integer");
		(key == "icon" ? p.mapIconType : p.mapIconStyle) = i; return true;
	}
	if(key == "checkpoint_kind"){
		if(!only({ SAMP_ELEMENT_CHECKPOINT })) return false;
		std::string name = lowercase(value.c_str());
		if(name == "normal") p.cpKind = SAMP_CHECKPOINT_NORMAL;
		else if(name == "race") p.cpKind = SAMP_CHECKPOINT_RACE;
		else return fail("expects normal (CreateDynamicCP) or race (CreateDynamicRaceCP)");
		if(p.cpKind == SAMP_CHECKPOINT_RACE && p.cpRace[0] == '\0')
			strcpy(p.cpRace, "race");
		return true;
	}
	if(key == "size"){
		if(!only({ SAMP_ELEMENT_CHECKPOINT })) return false;
		if(!parseFloat(value, &f) || f <= 0.0f) return fail("expects a size > 0");
		p.cpSize = f; return true;
	}
	if(key == "race_type"){
		if(!only({ SAMP_ELEMENT_CHECKPOINT })) return false;
		if(!parseInt(value, &i) || i < 0 || i > SAMP_RACE_CP_AUTO_AIR)
			return fail("expects a race checkpoint type 0-10 (0 normal, 1 finish, 9 automatic, 10 automatic air)");
		p.cpRaceType = i; return true;
	}
	if(key == "race_mode" || key == "race_laps" || key == "race_playable"){
		// The whole race's settings: set here, then copied to its other
		// checkpoints after the edit (agentSyncRaceSettings).
		if(!only({ SAMP_ELEMENT_CHECKPOINT })) return false;
		if(key == "race_mode"){
			std::string mode = lowercase(value.c_str());
			if(mode == "circuit") p.cpRaceMode = SAMP_RACE_CIRCUIT;
			else if(mode == "point_to_point") p.cpRaceMode = SAMP_RACE_POINT_TO_POINT;
			else return fail("expects point_to_point or circuit");
		}else if(key == "race_laps"){
			if(!parseInt(value, &i) || i < 1 || i > SAMP_RACE_MAX_LAPS) return fail("expects 1-999 laps");
			p.cpRaceLaps = i;
		}else{
			if(!parseBoolField(value, &b)) return fail("expects true or false");
			p.cpRacePlayable = b;
		}
		p.cpKind = SAMP_CHECKPOINT_RACE;
		edit.syncRace = true;
		return true;
	}
	if(key == "race"){
		if(!only({ SAMP_ELEMENT_CHECKPOINT })) return false;
		if(!IsValidPawnObjectName(value.c_str())) return fail("must be a valid Pawn name");
		p.cpKind = SAMP_CHECKPOINT_RACE;
		return copy(p.cpRace, sizeof(p.cpRace));
	}
	if(key == "sound_kind"){
		if(!only({ SAMP_ELEMENT_SOUND })) return false;
		std::string name = lowercase(value.c_str());
		if(name == "stream") p.soundKind = SAMP_SOUND_STREAM;
		else if(name == "game") p.soundKind = SAMP_SOUND_GAME;
		else return fail("expects stream (PlayAudioStreamForPlayer) or game (PlayerPlaySound)");
		return true;
	}
	if(key == "url"){
		if(!only({ SAMP_ELEMENT_SOUND })) return false;
		if(value.find('"') != std::string::npos) return fail("a URL without quotes");
		return copy(p.soundUrl, sizeof(p.soundUrl));
	}
	if(key == "sound_id"){
		if(!only({ SAMP_ELEMENT_SOUND })) return false;
		if(!parseInt(value, &i) || i < 0) return fail("expects a PlayerPlaySound sound id >= 0");
		p.soundId = i; return true;
	}
	if(key == "radius"){
		if(!only({ SAMP_ELEMENT_SOUND, SAMP_ELEMENT_AREA })) return false;
		if(!parseFloat(value, &f) || f <= 0.0f) return fail("expects a distance > 0");
		(kind == SAMP_ELEMENT_AREA ? p.areaRadius : p.soundRadius) = f; return true;
	}
	if(key == "use_position" || key == "stop_on_leave"){
		if(!only({ SAMP_ELEMENT_SOUND })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		(key == "use_position" ? p.soundUsePos : p.soundStopOnLeave) = b; return true;
	}
	if(key == "npc_name"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!IsValidNpcName(value.c_str())) return fail("must be 3-24 characters of A-Z a-z 0-9 []()$@._=");
		if(FindNpcByName(value.c_str(), inst)) return fail("another NPC already uses this name");
		return copy(p.npcName, sizeof(p.npcName));
	}
	if(key == "armour"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a number >= 0");
		p.npcArmour = f; return true;
	}
	if(key == "weapon" || key == "ammo"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i) || i < 0) return fail("expects an integer >= 0");
		(key == "weapon" ? p.npcWeapon : p.npcAmmo) = i; return true;
	}
	if(key == "path"){
		if(!only({ SAMP_ELEMENT_NPC, SAMP_ELEMENT_PATHPOINT })) return false;
		if(!value.empty() && !IsValidPawnObjectName(value.c_str()))
			return fail("a path name is a Pawn identifier");
		if(value.empty() && kind == SAMP_ELEMENT_PATHPOINT) return fail("a path point needs a path");
		return copy(p.pathName, sizeof(p.pathName));
	}
	if(key == "move_type"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i) || i < 0 || i > 5) return fail("expects NPC_MOVE_TYPE 0-5");
		p.npcMoveType = i; return true;
	}
	if(key == "move_speed"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseFloat(value, &f)) return fail("expects a speed, or -1 for NPC_MOVE_SPEED_AUTO");
		p.npcMoveSpeed = f; return true;
	}
	if(key == "path_repeat"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		std::string mode = lowercase(value.c_str());
		if(mode == "once" || mode == "0") p.npcPathRepeat = NPC_PATH_ONCE;
		else if(mode == "loop" || mode == "1") p.npcPathRepeat = NPC_PATH_LOOP;
		else if(mode == "back_and_forth" || mode == "2") p.npcPathRepeat = NPC_PATH_BACK_AND_FORTH;
		else return fail("expects once, loop or back_and_forth");
		return true;
	}
	if(key == "path_reversed"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		p.npcPathReversed = b; return true;
	}
	if(key == "recording"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(value.size() > 4 && lowercase(value.substr(value.size() - 4).c_str()) == ".rec")
			value.resize(value.size() - 4);
		if(value.find_first_of("/\\\"") != std::string::npos)
			return fail("a recording name in npcmodes/recordings, without folders");
		return copy(p.npcRecording, sizeof(p.npcRecording));
	}
	if(key == "recording_at_npc" || key == "plays_node" || key == "node_set_angle"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		(key == "recording_at_npc" ? p.npcRecordingAtNpc : key == "plays_node" ? p.npcPlaysNode :
			p.npcNodeSetAngle) = b;
		return true;
	}
	if(key == "node" || key == "node_point"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i) || i < 0 || (key == "node" && i > 63))
			return fail(key == "node" ? "expects a NODES*.DAT area 0-63" : "expects a node point >= 0");
		(key == "node" ? p.npcNode : p.npcNodePoint) = i;
		return true;
	}
	if(key == "node_radius"){
		if(!only({ SAMP_ELEMENT_NPC })) return false;
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a distance >= 0");
		p.npcNodeRadius = f; return true;
	}
	if(key == "anim_lib" || key == "anim_name"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		for(char ch : value)
			if(!isalnum((unsigned char)ch) && ch != '_')
				return fail("an animation library or name, e.g. PED / WALK_DRUNK (empty: none)");
		return copy(key == "anim_lib" ? p.animLib : p.animName,
			key == "anim_lib" ? sizeof(p.animLib) : sizeof(p.animName));
	}
	if(key == "anim_delta"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		if(!parseFloat(value, &f)) return fail("expects a speed, usually 4.1");
		p.animDelta = f; return true;
	}
	if(key == "anim_loop" || key == "anim_lock_x" || key == "anim_lock_y" || key == "anim_freeze"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		if(!parseBoolField(value, &b)) return fail("expects true or false");
		(key == "anim_loop" ? p.animLoop : key == "anim_lock_x" ? p.animLockX :
			key == "anim_lock_y" ? p.animLockY : p.animFreeze) = b;
		return true;
	}
	if(key == "anim_time"){
		if(!only({ SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })) return false;
		if(!parseInt(value, &i) || i < 0) return fail("expects milliseconds (0: until stopped)");
		p.animTime = i; return true;
	}
	if(key == "order"){
		if(!only({ SAMP_ELEMENT_PATHPOINT, SAMP_ELEMENT_CHECKPOINT })) return false;
		if(!parseInt(value, &i) || i < 0) return fail("expects an integer >= 0");
		(kind == SAMP_ELEMENT_CHECKPOINT ? p.cpOrder : p.pathOrder) = i; return true;
	}
	if(key == "stop_range"){
		if(!only({ SAMP_ELEMENT_PATHPOINT })) return false;
		if(!parseFloat(value, &f) || f < 0.0f) return fail("expects a distance >= 0");
		p.pathStopRange = f; return true;
	}
	*error = "unknown property " + key;
	return false;
}

// Applies key=value lines from lines[first...] to inst. Nothing changes on error.
static bool
applyAgentProperties(ObjectInst *inst, const std::vector<std::string> &lines, size_t first,
	std::string *error)
{
	int kind = agentKindOf(inst);
	ObjectPawnProperties p = *GetObjectPawnProperties(inst);
	AgentInstanceEdit edit = {};
	for(size_t i = first; i < lines.size(); i++){
		if(lines[i].empty())
			continue;
		size_t eq = lines[i].find('=');
		if(eq == std::string::npos || eq == 0){
			*error = "property lines are key=value: " + lines[i];
			return false;
		}
		if(!applyAgentProperty(inst, kind, p, edit, lowercase(lines[i].substr(0, eq).c_str()),
		   lines[i].substr(eq + 1), error))
			return false;
	}
	ObjectPawnProperties requested = p;
	// A new animation plays at SA-MP's usual speed unless one was given.
	if((kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_NPC) && p.animName[0] && p.animDelta <= 0.0f)
		p.animDelta = 4.1f;
	ObjectInst *parent = nil;
	if(edit.setAttach && edit.attachTo >= 0){
		parent = GetInstanceByID(edit.attachTo);
		bool validTarget = parent && !parent->m_isDeleted && parent != inst &&
			(parent->m_isVehicle || IsPlayerAttachmentTarget(parent) ||
			 GetSampElementKind(parent) == SAMP_ELEMENT_OBJECT);
		if(!validTarget){
			*error = "attach_to: not an object, player, NPC or vehicle in the map";
			return false;
		}
	}
	int32 attachmentTargetId = edit.setAttach ? edit.attachTo : p.attachParent;
	ObjectInst *currentTarget = attachmentTargetId >= 0 ? GetInstanceByID(attachmentTargetId) : nil;
	bool playerAttachment = attachmentTargetId == PAWN_ATTACH_PLAYER ||
		IsPlayerAttachmentTarget(currentTarget);
	if((edit.setAttachBone || edit.setAttachSlot || edit.setAttachScale || edit.setAttachColor1 ||
	    edit.setAttachColor2 || edit.setAttachSkin) && !playerAttachment){
		*error = "player bone, slot, scale, colour and skin properties need a player or NPC target";
		return false;
	}
	SetObjectPawnProperties(inst, p, true);
	if(edit.syncRace){
		const ObjectPawnProperties *set = GetObjectPawnProperties(inst);
		SetRaceSettings(set->cpRace, set->cpRaceMode, set->cpRaceLaps, set->cpRacePlayable);
	}
	if(edit.setAttach){
		if(edit.attachTo == PAWN_ATTACH_PLAYER){
			if(!AttachObjectToPlayer(inst, requested.attachSkin)){
				*error = "attach_to: the player already has 10 attached objects";
				return false;
			}
			ObjectPawnProperties attached = *GetObjectPawnProperties(inst);
			if(edit.setAttachBone) attached.attachBone = requested.attachBone;
			if(edit.setAttachSlot) attached.attachSlot = requested.attachSlot;
			if(edit.setAttachOffset) attached.attachOffset = requested.attachOffset;
			if(edit.setAttachRotation) attached.attachRotation = requested.attachRotation;
			if(edit.setAttachScale) attached.attachScale = requested.attachScale;
			if(edit.setAttachColor1) attached.attachColor1 = requested.attachColor1;
			if(edit.setAttachColor2) attached.attachColor2 = requested.attachColor2;
			if(edit.setAttachSkin) attached.attachSkin = requested.attachSkin;
			SetObjectPawnProperties(inst, attached, true);
		}else if(parent){
			if(!AttachObjectInstance(inst, parent, p.attachSyncRotation)){
				*error = "attach_to: this object can't attach to that target";
				return false;
			}
			if(playerAttachment){
				ObjectPawnProperties attached = *GetObjectPawnProperties(inst);
				if(edit.setAttachBone) attached.attachBone = requested.attachBone;
				if(edit.setAttachSlot) attached.attachSlot = requested.attachSlot;
				if(edit.setAttachOffset) attached.attachOffset = requested.attachOffset;
				if(edit.setAttachRotation) attached.attachRotation = requested.attachRotation;
				if(edit.setAttachScale) attached.attachScale = requested.attachScale;
				if(edit.setAttachColor1) attached.attachColor1 = requested.attachColor1;
				if(edit.setAttachColor2) attached.attachColor2 = requested.attachColor2;
				if(edit.setAttachSkin) attached.attachSkin = requested.attachSkin;
				SetObjectPawnProperties(inst, attached, true);
			}
		}else
			DetachObjectInstance(inst);
	}
	if(edit.setArea && inst->m_area != edit.area){
		inst->m_area = edit.area;
		StampChangeSeq(inst);
		inst->m_isDirty = true;
	}
	if(edit.setColors){
		inst->m_vehicleColor1 = edit.color1;
		inst->m_vehicleColor2 = edit.color2;
		inst->RefreshVehicleColors();
		StampChangeSeq(inst);
		inst->m_isDirty = true;
	}
	if(edit.setPlate){
		snprintf(inst->m_vehiclePlate, sizeof(inst->m_vehiclePlate), "%s", edit.plate.c_str());
		StampChangeSeq(inst);
		inst->m_isDirty = true;
	}
	if(edit.setPaintjob || edit.setMods){
		if(edit.setPaintjob)
			inst->m_vehiclePaintjob = edit.paintjob;
		if(edit.setMods)
			memcpy(inst->m_vehicleMods, edit.mods, sizeof(inst->m_vehicleMods));
		inst->RefreshVehicleUpgrades();
		StampChangeSeq(inst);
		inst->m_isDirty = true;
	}
	if(edit.setRespawn){
		inst->m_vehicleRespawnDelay = edit.respawnDelay;
		StampChangeSeq(inst);
		inst->m_isDirty = true;
	}
	return true;
}

// A train as the client makes it: the engine and its carriages on the
// track, how far the engine moved to get there, and its vehicle ids.
static std::string
agentTrainJson(ObjectInst *inst)
{
	TrainCar cars[TRAIN_MAX_CARS];
	float off = 0.0f;
	int n = ComputeTrainCars(inst->m_objectId, inst->m_translation, GetTrainInstAngle(inst), cars, &off);
	std::string json = "{\"vehicle_ids\":" + std::to_string(GetTrainVehicleIds(inst->m_objectId)) +
		",\"clockwise\":" + (TrainAngleIsClockwise(GetTrainInstAngle(inst)) ? "true" : "false") +
		",\"on_tracks\":" + (n > 0 ? "true" : "false");
	char buffer[256];
	if(n > 0){
		snprintf(buffer, sizeof(buffer), ",\"off_track\":%.3f", off);
		json += buffer;
	}
	json += ",\"cars\":[";
	for(int i = 0; i < n; i++){
		snprintf(buffer, sizeof(buffer), "%s{\"model\":%d,\"position\":[%.4f,%.4f,%.4f],\"forward\":[%.4f,%.4f,%.4f]}",
			i ? "," : "", cars[i].model, cars[i].position.x, cars[i].position.y, cars[i].position.z,
			cars[i].forward.x, cars[i].forward.y, cars[i].forward.z);
		json += buffer;
	}
	return json + "]}";
}

static std::string
agentInstanceJson(ObjectInst *inst, bool includeAgentFlag)
{
	ObjectDef *object = GetObjectDef(inst->m_objectId);
	rw::V3d degrees = GetInstanceRotationDegrees(inst);
	char item[768];
	snprintf(item, sizeof(item),
		"{\"instance_id\":%d,\"object_key\":\"runtime:%d\",\"model_id\":%d,"
		"\"name\":\"%s\",\"position\":[%.4f,%.4f,%.4f],"
		"\"rotation\":[%.3f,%.3f,%.3f]%s%s}",
		inst->m_id, inst->m_id, PawnModelId(inst->m_objectId),
		jsonEscape(object ? object->m_name : "").c_str(), inst->m_translation.x,
		inst->m_translation.y, inst->m_translation.z, degrees.x, degrees.y, degrees.z,
		includeAgentFlag ? ",\"agent_scene\":" : "",
		includeAgentFlag ? (isAgentSceneInstance(inst) ? "true" : "false") : "");
	std::string json = item;
	json.erase(json.size() - 1);
	json += std::string(",\"kind\":\"") + agentKindName(agentKindOf(inst)) +
		"\",\"properties\":" + agentPropertiesJson(inst);
	if(HasObjectMaterialOverrides(inst))
		json += ",\"materials\":[" + agentMaterialsJson(inst) + "]";
	if(inst->m_isVehicle && IsTrainEngineModel(inst->m_objectId))
		json += ",\"train\":" + agentTrainJson(inst);
	return json + "}";
}

static bool
agentInstanceInZone(ObjectInst *inst, bool useZone, float centerX, float centerY,
	float radiusSquared)
{
	if(!useZone)
		return true;
	float dx = inst->m_translation.x - centerX;
	float dy = inst->m_translation.y - centerY;
	return dx * dx + dy * dy <= radiusSquared;
}

// What the last open or merge brought in, and what it left out.
static std::string
agentImportReportJson(void)
{
	const PawnImportReport &report = GetLastPawnImportReport();
	std::string json = "{\"placed\":" + std::to_string(report.placed) +
		",\"removed_buildings\":" + std::to_string(report.removed) +
		",\"unreadable_calls\":" + std::to_string(report.unreadableCalls) +
		",\"unknown_model_objects\":" + std::to_string(report.unknownModelObjects) +
		",\"unmatched_removals\":" + std::to_string(report.unmatchedRemovals) +
		",\"unresolved_links\":" + std::to_string(report.unresolvedLinks) + ",\"left_out\":[";
	// What another tool's map had that SA-MP can't show.
	for(size_t i = 0; i < report.notes.size(); i++)
		json += std::string(i ? "," : "") + "\"" + jsonEscape(report.notes[i].c_str()) + "\"";
	return json + "]}";
}

// RemoveBuildingForPlayer(playerid, -1, ...) lines of the open map: every world
// building inside each sphere is removed in game.
static std::string
agentAreaRemovalsJson(void)
{
	std::string json = "[";
	for(const PawnAreaRemoval &r : GetPawnAreaRemovals()){
		if(json.size() > 1) json += ",";
		json += "{\"position\":[" + jsonFloat(r.x) + "," + jsonFloat(r.y) + "," + jsonFloat(r.z) +
			"],\"radius\":" + jsonFloat(r.radius) + "}";
	}
	return json + "]";
}

static std::string
agentMapStatusJson(void)
{
	return std::string("\"map\":{\"path\":\"") + jsonEscape(GetPawnMapPath()) +
		"\",\"untitled\":" + (GetPawnMapPath()[0] ? "false" : "true") +
		",\"format\":\"" + (GetPawnMapFormat() == PAWN_CODE_STREAMER ? "streamer" : "native") +
		"\",\"text_encoding\":\"" + (GetPawnTextEncoding() == PAWN_TEXT_UTF8 ? "utf-8" : "windows-1252") +
		"\",\"unsaved_changes\":" + (PawnMapHasUnsavedChanges() ? "true" : "false") +
		",\"agent_scope\":\"" + (agentMapMode() ? "pawn_map" : "scratch_ipl") +
		"\",\"elements\":" + std::to_string(countAgentInstances(false)) +
		",\"area_removals\":" + agentAreaRemovalsJson() + "}";
}

// Back to editing the Pawn map after a scratch IPL scene.
static void
leaveAgentScene(void)
{
	if(agentMapMode())
		return;
	CloseIplMapDocument();
	clearAgentSession();
	gAgentSceneLogicalPath[0] = '\0';
	gAgentScenePhysicalPath[0] = '\0';
	SetCustomPlacementIpl(nil, nil, false);
}

// ---- TextDraws ----

static std::string
agentTextDrawFileJson(void)
{
	return std::string("\"file\":{\"path\":\"") + jsonEscape(GetTextDrawFilePath()) +
		"\",\"untitled\":" + (GetTextDrawFilePath()[0] ? "false" : "true") +
		",\"unsaved_changes\":" + (TextDrawFileHasUnsavedChanges() ? "true" : "false") +
		",\"textdraws\":" + std::to_string(GetTextDraws().size()) +
		",\"prefix\":\"" + jsonEscape(TextDrawFilePrefix(GetTextDrawFilePath()).c_str()) +
		"\",\"read_only_in_map\":" + std::to_string(GetMapTextDraws().size()) + "}";
}

static std::string
agentTextDrawJson(size_t index, const TextDraw &td)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
		"{\"index\":%d,\"name\":\"%s\",\"per_player\":%s,\"x\":%.3f,\"y\":%.3f,\"font\":%d,"
		"\"letter_x\":%.3f,\"letter_y\":%.3f,\"size_x\":%.3f,\"size_y\":%.3f,\"alignment\":%d,"
		"\"colour\":%s,\"use_box\":%s,\"box_colour\":%s,\"background_colour\":%s,"
		"\"shadow\":%d,\"outline\":%d,\"proportional\":%s,\"selectable\":%s,\"locked\":%s,\"hidden\":%s,"
		"\"preview_model\":%d,\"preview_rot\":[%.3f,%.3f,%.3f],\"preview_zoom\":%.3f,"
		"\"preview_colours\":[%d,%d],\"text\":\"",
		(int)index, jsonEscapeBytes(td.name).c_str(), td.perPlayer ? "true" : "false", td.x, td.y, td.font,
		td.letterX, td.letterY, td.sizeX, td.sizeY, td.alignment,
		jsonColor(td.colour).c_str(), td.useBox ? "true" : "false", jsonColor(td.boxColour).c_str(),
		jsonColor(td.backgroundColour).c_str(), td.shadow, td.outline,
		td.proportional ? "true" : "false", td.selectable ? "true" : "false", td.locked ? "true" : "false",
		td.hidden ? "true" : "false",
		td.previewModel, td.previewRotX, td.previewRotY, td.previewRotZ, td.previewZoom,
		td.previewColour1, td.previewColour2);
	return std::string(buf) + jsonEscapeBytes(td.text.c_str()) + "\",\"group\":\"" +
		jsonEscapeBytes(td.group.c_str()) + "\"}";
}

// key=value lines, as "properties" takes them.
static bool
applyTextDrawFields(TextDraw &td, const std::vector<std::string> &lines, size_t first, std::string *error)
{
	for(size_t i = first; i < lines.size(); i++){
		if(lines[i].empty())
			continue;
		size_t eq = lines[i].find('=');
		if(eq == std::string::npos){
			*error = "expected key=value, got " + lines[i];
			return false;
		}
		std::string key = lines[i].substr(0, eq), value = lines[i].substr(eq + 1);
		float f = 0.0f;
		int n = 0;
		bool b = false;
		uint32 c = 0;
		int list[3];
		bool ok = true;
		if(key == "name") snprintf(td.name, sizeof(td.name), "%s", value.c_str());
		else if(key == "text") td.text = decodeAgentText(value);
		else if(key == "per_player") { ok = parseBoolField(value, &b); td.perPlayer = b; }
		else if(key == "x") { ok = parseFloat(value, &f); td.x = f; }
		else if(key == "y") { ok = parseFloat(value, &f); td.y = f; }
		else if(key == "font") { ok = parseInt(value, &n) && n >= 0 && n <= 5; td.font = n; }
		else if(key == "letter_x") { ok = parseFloat(value, &f); td.letterX = f; }
		else if(key == "letter_y") { ok = parseFloat(value, &f); td.letterY = f; }
		else if(key == "size_x") { ok = parseFloat(value, &f); td.sizeX = f; }
		else if(key == "size_y") { ok = parseFloat(value, &f); td.sizeY = f; }
		else if(key == "alignment") { ok = parseInt(value, &n) && n >= 1 && n <= 3; td.alignment = n; }
		else if(key == "colour" || key == "color") { ok = parseRgbaField(value, &c); td.colour = c; }
		else if(key == "box_colour" || key == "box_color") { ok = parseRgbaField(value, &c); td.boxColour = c; }
		else if(key == "background_colour" || key == "background_color") { ok = parseRgbaField(value, &c); td.backgroundColour = c; }
		else if(key == "use_box") { ok = parseBoolField(value, &b); td.useBox = b; }
		else if(key == "shadow") { ok = parseInt(value, &n); td.shadow = n; }
		else if(key == "outline") { ok = parseInt(value, &n); td.outline = n; }
		else if(key == "proportional") { ok = parseBoolField(value, &b); td.proportional = b; }
		else if(key == "selectable") { ok = parseBoolField(value, &b); td.selectable = b; }
		else if(key == "locked") { ok = parseBoolField(value, &b); td.locked = b; }
		else if(key == "hidden") { ok = parseBoolField(value, &b); td.hidden = b; }
		else if(key == "group") td.group = value;
		else if(key == "preview_model") { ok = parseInt(value, &n); td.previewModel = n; }
		else if(key == "preview_zoom") { ok = parseFloat(value, &f); td.previewZoom = f; }
		else if(key == "preview_rot"){
			float r[3];
			size_t a = value.find(','), b2 = a == std::string::npos ? a : value.find(',', a + 1);
			ok = b2 != std::string::npos && parseFloat(value.substr(0, a), &r[0]) &&
				parseFloat(value.substr(a + 1, b2 - a - 1), &r[1]) && parseFloat(value.substr(b2 + 1), &r[2]);
			if(ok){ td.previewRotX = r[0]; td.previewRotY = r[1]; td.previewRotZ = r[2]; }
		}else if(key == "preview_colours" || key == "preview_colors"){
			ok = parseIntList(value, list, 2);
			if(ok){ td.previewColour1 = list[0]; td.previewColour2 = list[1]; }
		}else{
			*error = "unknown textdraw field " + key;
			return false;
		}
		if(!ok){
			*error = "invalid value for " + key + ": " + value;
			return false;
		}
	}
	return true;
}

// maptool's key=value fields: "x,y,z" vectors and "x,y,z;x,y,z" point lists.
static bool
parseAgentVector(const std::string &text, rw::V3d *out)
{
	float v[3];
	if(sscanf(text.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3)
		return false;
	*out = { v[0], v[1], v[2] };
	return true;
}

// maptool <op>, then key=value lines: the transform tools of Tools on the
// objects in ids (which become the selection), one undo step each.
// maptool radar_art: Tools > Radar art from objects. The objects are ids,
// a group, or all; apply=0 only counts the zones.
static void
handleRadarArt(const std::string &requestId, std::map<std::string, std::string> &f)
{
	RadarArtParams params;
	RadarArtDefaults(&params);
	// Every key is checked: a typo or a value that doesn't parse is an
	// error, not a default the agent didn't ask for.
	static const char *keys[] = { "ids", "group", "cell", "shape", "above_water", "fit", "colours", "colour", "num_colours", "alpha",
		"outline", "outline_colour", "height_shading", "height_levels", "zone_mode", "out_group", "replace", "apply" };
	std::string error;
	for(std::map<std::string, std::string>::iterator it = f.begin(); it != f.end() && error.empty(); ++it)
		if(std::find(std::begin(keys), std::end(keys), it->first) == std::end(keys))
			error = "radar_art: unknown key " + it->first;
	auto bad = [&](const char *key, const char *expects){
		if(error.empty())
			error = std::string("radar_art: ") + key + " expects " + expects;
	};
	auto number = [&](const char *key, float *out, const char *expects){
		float v;
		if(f.count(key) && (parseFloat(f[key], &v) ? (*out = v, false) : true))
			bad(key, expects);
	};
	auto integer = [&](const char *key, int *out, int lo, int hi, const char *expects){
		int v;
		if(f.count(key) && (parseInt(f[key], &v) && v >= lo && v <= hi ? (*out = v, false) : true))
			bad(key, expects);
	};
	auto boolean = [&](const char *key, bool *out){
		bool v;
		if(f.count(key) && (parseBoolField(f[key], &v) ? (*out = v, false) : true))
			bad(key, "true or false");
	};
	auto colour = [&](const char *key, uint32 *out){
		uint32 c;
		if(f.count(key) && (parseRgbaField(f[key], &c) ? (*out = c & 0xFFFFFF00, false) : true))
			bad(key, "0xRRGGBBAA (the alpha is ignored; use alpha)");
	};
	number("cell", &params.cellSize, "metres, 0.5 to 1000");
	int fit = 0;
	integer("fit", &fit, 1, 20000, "a zone count, 1 to 20000");
	if(f.count("colours")){
		std::string c = lowercase(f["colours"].c_str());
		if(c == "one") params.colourMode = RADAR_COLOUR_ONE;
		else if(c == "group") params.colourMode = RADAR_COLOUR_GROUP;
		else if(c == "texture") params.colourMode = RADAR_COLOUR_TEXTURE;
		else if(c == "radar") params.colourMode = RADAR_COLOUR_RADAR;
		else bad("colours", "radar, one, group or texture");
	}
	if(f.count("shape")){
		std::string s = lowercase(f["shape"].c_str());
		if(s == "model") params.shape = RADAR_SHAPE_MESH;
		else if(s == "collision") params.shape = RADAR_SHAPE_COLLISION;
		else bad("shape", "model or collision");
	}
	colour("colour", &params.colour);
	colour("outline_colour", &params.outlineColour);
	integer("num_colours", &params.numColours, 1, 64, "1 to 64");
	integer("alpha", &params.alpha, 0, 255, "0 to 255");
	integer("height_levels", &params.heightLevels, 2, 16, "2 to 16");
	boolean("outline", &params.outline);
	boolean("height_shading", &params.heightShading);
	boolean("replace", &params.replace);
	boolean("above_water", &params.aboveWater);
	bool apply = true;
	boolean("apply", &apply);
	if(f.count("zone_mode")){
		std::string mode = lowercase(f["zone_mode"].c_str());
		if(mode == "global") params.zoneMode = SAMP_ZONE_GLOBAL;
		else if(mode == "player") params.zoneMode = SAMP_ZONE_PLAYER;
		else if(mode == "streamed") params.zoneMode = SAMP_ZONE_STREAMED;
		else bad("zone_mode", "global, player or streamed");
	}
	if(f.count("out_group"))
		snprintf(params.outGroup, sizeof(params.outGroup), "%s", decodeAgentText(f["out_group"]).c_str());
	if(f.count("group")){
		params.source = RADAR_SOURCE_GROUP;
		snprintf(params.group, sizeof(params.group), "%s", decodeAgentText(f["group"]).c_str());
	}
	std::vector<ObjectInst*> objs;
	std::string ids = f.count("ids") ? f["ids"] : "";
	for(size_t at = 0; at < ids.size() && error.empty();){
		size_t end = ids.find(',', at);
		if(end == std::string::npos) end = ids.size();
		int id;
		ObjectInst *inst = parseInt(ids.substr(at, end - at), &id) ? GetInstanceByID(id) : nil;
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted)
			error = "ids must be instances of the active agent scene, separated by commas";
		else
			objs.push_back(inst);
		at = end + 1;
	}
	if(!error.empty()){
		writeError(requestId, error.c_str());
		return;
	}
	if(!objs.empty()){
		params.source = RADAR_SOURCE_SELECTION;
		ClearSelection();
		for(size_t i = 0; i < objs.size(); i++)
			objs[i]->Select();
	}
	if(fit > 0 && !RadarArtFitCellSize(params, fit, &params.cellSize, &error)){
		ClearSelection();
		writeError(requestId, error.c_str());
		return;
	}
	RadarArtResult result;
	bool ok = RadarArtBuild(params, &result, &error);
	int made = 0;
	if(ok && apply){
		made = RadarArtApply(params, &error);	// clears the selection
		ok = made >= 0;
	}
	ClearSelection();
	if(!ok){
		writeError(requestId, error.c_str());
		return;
	}
	if(made > 0)
		gAgentSceneRevision++;
	char body[256];
	snprintf(body, sizeof(body), "\"result\":\"radar_art\",\"zones\":%d,\"created\":%d,\"cell\":%s,"
		"\"objects\":%d,\"cells\":%d,\"group\":\"", (int)result.zones.size(), made,
		jsonFloat(params.cellSize).c_str(), result.objects, result.cells);
	writeResponse(requestId, true, body + jsonEscapeBytes(params.outGroup) + "\"");
}

static void
handleMapTool(const std::string &requestId, const std::vector<std::string> &lines)
{
	if(lines.size() < 3){
		writeError(requestId, "maptool requires an operation and key=value lines");
		return;
	}
	std::string op = lowercase(lines[2].c_str());
	std::map<std::string, std::string> f;
	for(size_t i = 3; i < lines.size(); i++){
		size_t eq = lines[i].find('=');
		if(eq != std::string::npos)
			f[lowercase(lines[i].substr(0, eq).c_str())] = lines[i].substr(eq + 1);
	}
	auto num = [&](const char *key, float fallback){
		std::map<std::string, std::string>::iterator it = f.find(key);
		float value;
		return it != f.end() && parseFloat(it->second, &value) ? value : fallback;
	};
	auto vec = [&](const char *key, rw::V3d fallback){
		std::map<std::string, std::string>::iterator it = f.find(key);
		rw::V3d value;
		return it != f.end() && parseAgentVector(it->second, &value) ? value : fallback;
	};
	auto flag = [&](const char *key, bool fallback){
		std::map<std::string, std::string>::iterator it = f.find(key);
		bool value;
		return it != f.end() && parseBoolField(it->second, &value) ? value : fallback;
	};
	auto axisOf = [&](const char *key, int fallback){
		std::map<std::string, std::string>::iterator it = f.find(key);
		if(it == f.end()) return fallback;
		std::string a = lowercase(it->second.c_str());
		return a == "x" || a == "0" ? 0 : a == "y" || a == "1" ? 1 : a == "z" || a == "2" ? 2 : -1;
	};

	if(op == "radar_art"){
		handleRadarArt(requestId, f);
		return;
	}
	if(op != "lay_along"){
		// The objects, in the order given (the first is the reference).
		std::vector<ObjectInst*> objs;
		std::string ids = f.count("ids") ? f["ids"] : "";
		for(size_t at = 0; at < ids.size();){
			size_t end = ids.find(',', at);
			if(end == std::string::npos) end = ids.size();
			int id;
			if(!parseInt(ids.substr(at, end - at), &id)){
				writeError(requestId, "ids must be instance ids separated by commas");
				return;
			}
			ObjectInst *inst = GetInstanceByID(id);
			if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
				writeError(requestId, ("instance " + std::to_string(id) + " does not belong to the active agent scene").c_str());
				return;
			}
			objs.push_back(inst);
			at = end + 1;
		}
		if(objs.empty()){
			writeError(requestId, "maptool needs ids");
			return;
		}
		ClearSelection();
		for(size_t i = 0; i < objs.size(); i++)
			objs[i]->Select();
	}

	std::string error, code;
	int result = -1;
	if(op == "array"){
		MapArrayParams p;
		p.count = (int)num("count", 1.0f);
		p.radial = flag("radial", false);
		p.offset = vec("offset", { 0.0f, 0.0f, 0.0f });
		p.rotate = vec("rotate", { 0.0f, 0.0f, 0.0f });
		p.centre = vec("centre", TheCamera.m_target);
		p.angle = num("angle", 360.0f);
		p.rows = (int)num("rows", 1.0f);
		p.rowOffset = vec("row_offset", { 5.0f, 0.0f, 0.0f });
		result = MapToolsArray(p, &error);
	}else if(op == "duplicate"){
		// Like Ctrl+D with the Tools window's duplicate offset, set here for this one.
		MapCloneOffset saved = MapToolsCloneOffset();
		MapCloneOffset &o = MapToolsCloneOffset();
		rw::V3d move = vec("offset", { 0.0f, 0.0f, 0.0f }), turn = vec("rotate", { 0.0f, 0.0f, 0.0f });
		o = MapCloneOffset();
		o.enabled = true;
		o.local = flag("local", false);
		float values[6] = { move.x, move.y, move.z, turn.x, turn.y, turn.z };
		memcpy(o.fixed, values, sizeof(values));
		result = MapToolsDuplicate(&error);
		o = saved;
	}else if(op == "mirror"){
		int axis = axisOf("axis", 0);
		// around: selection (its centre), origin, or the mirror's coordinate.
		std::string around = f.count("around") ? lowercase(f["around"].c_str()) : "selection";
		float at = 0.0f;
		bool atValue = around == "origin" || parseFloat(around, &at);
		if(axis < 0 || axis > 1)
			error = "axis is x or y";
		else if(!atValue && around != "selection")
			error = "around is selection, origin or a coordinate";
		else
			result = MapToolsMirror(axis, atValue ? &at : nil, flag("copy", false), &error);
	}else if(op == "align"){
		std::string mode = f.count("mode") ? lowercase(f["mode"].c_str()) : "min";
		int m = mode == "min" ? 0 : mode == "centre" || mode == "center" ? 1 : mode == "max" ? 2 : -1;
		int axis = axisOf("axis", 2);
		result = m < 0 || axis < 0 ? (error = "mode is min, centre or max; axis x, y or z", -1) :
			MapToolsAlign(axis, m, &error);
	}else if(op == "distribute"){
		int axis = axisOf("axis", 0);
		result = axis < 0 ? (error = "axis is x, y or z", -1) : MapToolsDistribute(axis, &error);
	}else if(op == "snap_rotations")
		result = MapToolsSnapRotations(num("step", 90.0f), &error);
	else if(op == "drop")
		result = MapToolsDropToGround(&error);
	else if(op == "relative"){
		code = MapToolsRelativeOffsetsPawn(flag("attach", true), &error);
		result = code.empty() ? -1 : 0;
		ClearSelection();
	}else if(op == "lay_along"){
		std::vector<rw::V3d> points;
		std::string list = f.count("points") ? f["points"] : "";
		for(size_t at = 0; at < list.size();){
			size_t end = list.find(';', at);
			if(end == std::string::npos) end = list.size();
			rw::V3d p;
			if(!parseAgentVector(list.substr(at, end - at), &p)){
				writeError(requestId, "points are x,y,z separated by ;");
				return;
			}
			points.push_back(p);
			at = end + 1;
		}
		// Pawn model IDs: custom models are negative.
		result = MapToolsLayAlong(points, EditorModelId((int)num("model", 0.0f)), num("spacing", 0.0f),
			num("heading", 0.0f), flag("along_length", true), flag("follow_slope", false),
			flag("on_ground", true), &error);
	}else{
		writeError(requestId, "unknown maptool operation (array, mirror, align, distribute, "
			"snap_rotations, drop, relative, lay_along, radar_art, duplicate)");
		return;
	}
	if(result < 0){
		writeError(requestId, error.c_str());
		return;
	}
	if(op == "relative"){
		writeResponse(requestId, true, "\"result\":\"relative\",\"pawn\":\"" + jsonEscapeBytes(code.c_str()) + "\"");
		return;
	}
	gAgentSceneRevision++;
	// The objects afterwards (with copies or laid objects); the selection is
	// cleared like after place, so captures don't show them tinted.
	std::string body = "\"result\":\"" + op + "\",\"changed\":" + std::to_string(result) + ",\"ids\":[";
	int count = 0;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted) continue;
		if(count++) body += ',';
		body += std::to_string(inst->m_id);
	}
	ClearSelection();
	writeResponse(requestId, true, body + "]");
}

static void
handleRequest(const std::vector<std::string> &lines)
{
	if(lines.size() < 2 || lines[0].empty() || lines[1].empty())
		return;
	const std::string &requestId = lines[0];
	const std::string &command = lines[1];

	if(command == "ping"){
		writeResponse(requestId, true, "\"result\":\"pong\"");
		return;
	}
	if(command == "capabilities"){
		writeResponse(requestId, true, std::string(
			"\"engine\":\"haumea\",\"build_id\":\"") + jsonEscape(agentBuildId()) +
			"\",\"transport\":\"" + (gAgentTcp ? "tcp-loopback-auth-framed-v1" :
			(gAgentSocket != INVALID_AGENT_SOCKET ? "unix-stream-framed-v1" : "legacy-files-v1")) + "\","
			"\"limits\":{\"max_request_bytes\":1048576,\"max_response_bytes\":4194304,\"max_page_items\":256},"
			"\"observation\":[\"rgb_capture\",\"screen_ray_depth\",\"screen_ray_object_id\"],"
			"\"identity\":{\"instance_id\":\"process_local\",\"object_key\":\"checkpoint_manifest\"},"
			"\"commands\":[\"scene\",\"session_begin\",\"session_status\","
			"\"session_commit\",\"session_rollback\",\"catalog\",\"asset_detail\","
			"\"asset_probe\",\"asset_preview\",\"resolve_placement\",\"raycast_segment\","
			"\"inspect_zone\",\"inspect_zone_page\",\"list_page\",\"validate_zone\","
			"\"camera_context\",\"selection\",\"select\",\"screen_to_world\",\"screen_grid\",\"surface_grid\",\"environment\",\"scene_bounds\","
			"\"analyze_placement\",\"fit_terrain\","
			"\"place\",\"batch\",\"transform\",\"transform3d\",\"suppress_world_models\","
			"\"delete\",\"material\",\"material_text\",\"material_clear\",\"clear\",\"list\",\"validate\",\"camera\",\"capture\",\"capture_screen\","
			"\"capture_pose\",\"save\",\"map_status\",\"map_new\",\"map_open\",\"map_merge\",\"place_element\",\"maptool\",\"test_in_game\","
			"\"properties\",\"undo\",\"redo\",\"map_check\",\"texture_search\",\"textdraws\",\"textdraw_add\",\"textdraw_set\",\"textdraw_delete\","
			"\"textdraw_new\",\"textdraw_open\",\"textdraw_save\",\"textdraw_move_from_map\"],"
			"\"element_kinds\":[\"object\",\"vehicle\",\"pickup\",\"label\",\"actor\",\"class\","
			"\"gangzone\",\"mapicon\",\"npc\",\"pathpoint\",\"sound\",\"checkpoint\"]");
		return;
	}
	if(command == "scene"){
		if(lines.size() < 4 || lines[2].empty() || lines[3].empty()){
			writeError(requestId, "scene requires logical and physical IPL paths");
			return;
		}
		CloseIplMapDocument();
		clearAgentSession();
		strncpy(gAgentSceneLogicalPath, lines[2].c_str(), sizeof(gAgentSceneLogicalPath) - 1);
		gAgentSceneLogicalPath[sizeof(gAgentSceneLogicalPath) - 1] = '\0';
		strncpy(gAgentScenePhysicalPath, lines[3].c_str(), sizeof(gAgentScenePhysicalPath) - 1);
		gAgentScenePhysicalPath[sizeof(gAgentScenePhysicalPath) - 1] = '\0';
		SetIplMapDocument(gAgentSceneLogicalPath, gAgentScenePhysicalPath, true);
		SetCustomPlacementIpl(gAgentSceneLogicalPath, gAgentScenePhysicalPath, false);
		ClearSelection();
		ResetUndoHistory();
		gAgentSceneRevision++;
		writeResponse(requestId, true,
			std::string("\"logical_path\":\"") + jsonEscape(gAgentSceneLogicalPath) +
			"\",\"physical_path\":\"" + jsonEscape(gAgentScenePhysicalPath) + "\"");
		return;
	}
	if(command == "session_begin"){
		if(gAgentSessionActive){
			writeError(requestId, "a scratch session is already active");
			return;
		}
		std::string sessionId = lines.size() > 2 && !lines[2].empty() ? lines[2] : requestId;
		beginAgentSession(sessionId);
		writeResponse(requestId, true, std::string("\"session_id\":\"") +
			jsonEscape(sessionId.c_str()) + "\",\"snapshot_instances\":" +
			std::to_string(gAgentSessionSnapshot.size()));
		return;
	}
	if(command == "session_status"){
		writeResponse(requestId, true, std::string("\"active\":") +
			(gAgentSessionActive ? "true" : "false") + ",\"session_id\":\"" +
			jsonEscape(gAgentSessionId.c_str()) + "\",\"live_instances\":" +
			std::to_string(countAgentInstances(false)) + ",\"logical_path\":\"" +
			jsonEscape(gAgentSceneLogicalPath) + "\",\"physical_path\":\"" +
			jsonEscape(gAgentScenePhysicalPath) + "\"," + agentMapStatusJson());
		return;
	}
	if(command == "session_rollback"){
		if(!gAgentSessionActive){
			writeError(requestId, "no scratch session is active");
			return;
		}
		std::string sessionId = gAgentSessionId;
		rollbackAgentSession();
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"rolled_back_session\":\"") +
			jsonEscape(sessionId.c_str()) + "\",\"live_instances\":" +
			std::to_string(countAgentInstances(false)));
		return;
	}
	if(command == "session_commit"){
		if(!gAgentSessionActive){
			writeError(requestId, "no scratch session is active");
			return;
		}
		std::string sessionId = gAgentSessionId;
		clearAgentSession();
		writeResponse(requestId, true, std::string("\"committed_session\":\"") +
			jsonEscape(sessionId.c_str()) + "\",\"live_instances\":" +
			std::to_string(countAgentInstances(false)));
		return;
	}
	if(command == "catalog"){
		std::string query = lines.size() > 2 ? lowercase(lines[2].c_str()) : "";
		int limit = 50;
		if(lines.size() > 3)
			parseInt(lines[3], &limit);
		limit = std::max(1, std::min(limit, 500));
		std::string objects = "\"objects\":[";
		int count = 0;
		for(int id = 0; id < NUMOBJECTDEFS && count < limit; id++){
			ObjectDef *object = GetObjectDef(id);
			if(object == nil || (!query.empty() && lowercase(object->m_name).find(query) == std::string::npos))
				continue;
			if(count++) objects += ',';
			char item[512];
			snprintf(item, sizeof(item), "{\"id\":%d,\"name\":\"%s\",\"draw_distance\":%.2f}",
				PawnModelId(id), jsonEscape(object->m_name).c_str(), object->GetLargestDrawDist());
			objects += item;
		}
		objects += "]";
		writeResponse(requestId, true, objects);
		return;
	}
	if(command == "asset_detail"){
		if(lines.size() < 3){
			writeError(requestId, "asset_detail requires a model id or name");
			return;
		}
		int objectId;
		ObjectDef *object = resolveObject(lines[2], &objectId);
		if(object == nil){
			writeError(requestId, "unknown model");
			return;
		}
		char body[1024];
		if(object->m_colModel){
			CBox &box = object->m_colModel->boundingBox;
			snprintf(body, sizeof(body),
				"\"asset\":{\"id\":%d,\"name\":\"%s\",\"draw_distance\":%.2f,\"has_collision\":true,\"collision_bounds\":[[%.4f,%.4f,%.4f],[%.4f,%.4f,%.4f]],\"bounds_space\":\"model_local_relative_to_origin\",\"identity_axes\":{\"right\":[1,0,0],\"forward\":[0,1,0],\"up\":[0,0,1]},\"ground_offset\":%.4f,\"lod_id\":%d}",
				PawnModelId(objectId), jsonEscape(object->m_name).c_str(), object->GetLargestDrawDist(),
				box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z,
				GetPlacementBaseOffset(objectId), GetLodForObject(objectId));
		}else{
			snprintf(body, sizeof(body),
				"\"asset\":{\"id\":%d,\"name\":\"%s\",\"draw_distance\":%.2f,\"has_collision\":false,\"bounds_space\":\"model_local_relative_to_origin\",\"identity_axes\":{\"right\":[1,0,0],\"forward\":[0,1,0],\"up\":[0,0,1]},\"ground_offset\":0.0,\"lod_id\":%d}",
				PawnModelId(objectId), jsonEscape(object->m_name).c_str(), object->GetLargestDrawDist(),
				GetLodForObject(objectId));
		}
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "asset_probe"){
		if(lines.size() < 3){
			writeError(requestId, "asset_probe requires a model id or name");
			return;
		}
		int objectId = -1;
		ObjectDef *object = resolveObject(lines[2], &objectId);
		if(object == nil){
			writeResponse(requestId, true,
				std::string("\"asset\":{\"query\":\"") + jsonEscape(lines[2].c_str()) +
				"\",\"defined\":false,\"loaded_now\":false,\"renderable\":false,"
				"\"availability\":\"catalog_only\"}");
			return;
		}
		bool ensureLoaded = false;
		int ensure = 0;
		if(lines.size() > 3 && parseInt(lines[3], &ensure)) ensureLoaded = ensure != 0;
		bool loadedBefore = object->IsLoaded();
		if(ensureLoaded && !loadedBefore){
			RequestObject(objectId);
			LoadAllRequestedObjects();
		}
		bool loadedNow = object->IsLoaded();
		std::string availability = loadedNow ? "renderable" :
			(ensureLoaded ? "load_failed" : "defined_unloaded");
		char prefix[768];
		snprintf(prefix, sizeof(prefix),
			"\"asset\":{\"id\":%d,\"name\":\"%s\",\"defined\":true,"
			"\"loaded_before\":%s,\"loaded_now\":%s,\"renderable\":%s,"
			"\"availability\":\"%s\",\"has_collision\":%s,"
			"\"bounds_space\":\"model_local_relative_to_origin\","
			"\"identity_axes\":{\"right\":[1,0,0],\"forward\":[0,1,0],\"up\":[0,0,1]},"
			"\"ground_offset\":%.4f",
			PawnModelId(objectId), jsonEscape(object->m_name).c_str(), loadedBefore ? "true" : "false",
			loadedNow ? "true" : "false", loadedNow ? "true" : "false",
			availability.c_str(), object->m_colModel ? "true" : "false",
			GetPlacementBaseOffset(objectId));
		std::string body = prefix;
		if(object->m_colModel){
			CBox &box = object->m_colModel->boundingBox;
			char bounds[320];
			snprintf(bounds, sizeof(bounds),
				",\"collision_bounds\":[[%.4f,%.4f,%.4f],[%.4f,%.4f,%.4f]]",
				box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
			body += bounds;
		}
		body += "}";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "resolve_placement"){
		if(lines.size() < 8){
			writeError(requestId, "resolve_placement requires model, x, y, z, heading and snap");
			return;
		}
		int objectId, snap;
		float x, y, z, heading;
		ObjectDef *object = resolveObject(lines[2], &objectId);
		if(object == nil){
			writeError(requestId, ("unknown model: " + lines[2]).c_str());
			return;
		}
		if(!parseFloat(lines[3], &x) || !parseFloat(lines[4], &y) ||
		   !parseFloat(lines[5], &z) || !parseFloat(lines[6], &heading) ||
		   !parseInt(lines[7], &snap)){
			writeError(requestId, "invalid resolve_placement field");
			return;
		}
		if(snap){
			rw::V3d ground;
			if(!getAgentGroundPlacementSurface({ x, y, z }, &ground)){
				writeError(requestId, "no ground surface at placement");
				return;
			}
			z = ground.z + GetPlacementBaseOffset(objectId);
		}
		rw::Quat rotation = MakeObjectRotationDegrees({ 0.0f, 0.0f, heading });
		AgentPlacementAnalysis analysis;
		AgentSupportContract contract = agentSupportContract(objectId);
		bool analyzed = analyzeAgentPlacement(objectId, { x, y, z }, rotation,
			contract.sampleGridSize, &analysis);
		AgentSupportEvaluation evaluation = evaluateAgentSupport(objectId, analyzed, analysis);
		char prefix[384];
		snprintf(prefix, sizeof(prefix),
			"\"placement\":{\"model_id\":%d,\"position\":[%.5f,%.5f,%.5f],"
			"\"heading\":%.3f,\"snap_resolved\":%s}", PawnModelId(objectId), x, y, z,
			heading, snap ? "true" : "false");
		writeResponse(requestId, true, std::string(prefix) +
			",\"support_evaluation\":" + agentSupportEvaluationJson(
				evaluation, analyzed ? &analysis : nil));
		return;
	}
	if(command == "asset_preview"){
		if(lines.size() < 4){
			writeError(requestId, "asset_preview requires model and PNG path");
			return;
		}
		int objectId, size = 512;
		float angle = 0.785398163f;
		ObjectDef *object = resolveObject(lines[2], &objectId);
		if(object == nil){
			writeError(requestId, ("unknown model: " + lines[2]).c_str());
			return;
		}
		if(lines.size() > 4 && !parseFloat(lines[4], &angle)){
			writeError(requestId, "invalid asset preview angle");
			return;
		}
		if(lines.size() > 5 && !parseInt(lines[5], &size)){
			writeError(requestId, "invalid asset preview size");
			return;
		}
		char previewError[256] = { 0 };
		if(!CaptureObjectPreviewPng(objectId, lines[3].c_str(), size, angle,
		   previewError, sizeof(previewError))){
			writeError(requestId, previewError[0] ? previewError :
				"asset preview could not load or render the model");
			return;
		}
		writeResponse(requestId, true, std::string("\"path\":\"") +
			jsonEscape(lines[3].c_str()) + "\",\"model_id\":" + std::to_string(objectId) +
			",\"angle_radians\":" + std::to_string(angle));
		return;
	}
	if(command == "analyze_placement"){
		int instanceId, gridSize = 3;
		if(lines.size() < 3 || !parseInt(lines[2], &instanceId)){
			writeError(requestId, "analyze_placement requires an instance id");
			return;
		}
		if(lines.size() > 3 && !parseInt(lines[3], &gridSize)){
			writeError(requestId, "invalid placement grid size");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		AgentPlacementAnalysis analysis;
		if(!analyzeAgentPlacement(inst->m_objectId, inst->m_translation,
		   inst->m_rotation, gridSize, &analysis)){
			writeError(requestId, "placement analysis requires collision and terrain support");
			return;
		}
		AgentSupportEvaluation evaluation = evaluateAgentSupport(
			inst->m_objectId, true, analysis);
		writeResponse(requestId, true, std::string("\"instance_id\":") +
			std::to_string(instanceId) + ",\"analysis\":" + agentPlacementAnalysisJson(analysis) +
			",\"support_evaluation\":" + agentSupportEvaluationJson(evaluation, &analysis));
		return;
	}
	if(command == "fit_terrain"){
		if(!requireAgentSession(requestId)) return;
		int instanceId;
		if(lines.size() < 4 || !parseInt(lines[2], &instanceId)){
			writeError(requestId, "fit_terrain requires instance id and building or prop profile");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		std::string profile = lowercase(lines[3].c_str());
		if(profile != "building" && profile != "prop"){
			writeError(requestId, "fit profile must be building or prop");
			return;
		}
		AgentSupportContract contract = agentSupportContract(inst->m_objectId);
		float radius = 30.0f, step = 3.0f, maxRelief = contract.maxSupportRelief;
		if(lines.size() > 4 && !parseFloat(lines[4], &radius)){
			writeError(requestId, "invalid fit radius");
			return;
		}
		if(lines.size() > 5 && !parseFloat(lines[5], &step)){
			writeError(requestId, "invalid fit step");
			return;
		}
		if(lines.size() > 6 && !parseFloat(lines[6], &maxRelief)){
			writeError(requestId, "invalid maximum support relief");
			return;
		}
		radius = std::max(0.0f, std::min(radius, 100.0f));
		step = std::max(1.0f, std::min(step, 10.0f));
		// Callers may demand a flatter result, but may not loosen the validator's
		// shared support contract and manufacture an applied-but-invalid fit.
		maxRelief = std::max(0.05f, std::min(maxRelief, contract.maxSupportRelief));

		rw::V3d originalPosition = inst->m_translation;
		rw::V3d degrees = GetObjectRotationDegrees(inst->m_rotation);
		rw::Quat fittedRotation = MakeObjectRotationDegrees({ 0.0f, 0.0f, degrees.z });
		rw::V3d fittedPosition = originalPosition;
		AgentPlacementAnalysis bestAnalysis;
		bool found = false;
		if(profile == "building"){
			float bestScore = 1.0e30f;
			int extent = (int)ceilf(radius / step);
			for(int iy = -extent; iy <= extent; iy++){
				for(int ix = -extent; ix <= extent; ix++){
					float dx = ix * step, dy = iy * step;
					float distance = sqrtf(dx * dx + dy * dy);
					if(distance > radius + 0.001f) continue;
					rw::V3d candidate = { originalPosition.x + dx,
						originalPosition.y + dy, 0.0f };
					AgentPlacementAnalysis analysis;
					if(!analyzeAgentPlacement(inst->m_objectId, candidate,
					   fittedRotation, contract.sampleGridSize, &analysis) || analysis.missingSamples > 0)
						continue;
					float relief = analysis.maxRequiredZ - analysis.minRequiredZ;
					float score = relief + distance * 0.002f;
					if(score < bestScore){
						bestScore = score;
						bestAnalysis = analysis;
						fittedPosition = candidate;
						found = true;
					}
				}
			}
		}else if(profile == "prop"){
			AgentPlacementAnalysis uprightAnalysis;
			rw::V3d probe = { originalPosition.x, originalPosition.y, 0.0f };
			if(analyzeAgentPlacement(inst->m_objectId, probe, fittedRotation,
			   contract.sampleGridSize, &uprightAnalysis)){
				fittedRotation = BuildGroundAlignedRotationFromRotation(
					fittedRotation, uprightAnalysis.averageNormal);
				found = analyzeAgentPlacement(inst->m_objectId, probe,
					fittedRotation, contract.sampleGridSize, &bestAnalysis) &&
					bestAnalysis.missingSamples == 0;
				fittedPosition = probe;
			}
		}

		float relief = found ? bestAnalysis.maxRequiredZ - bestAnalysis.minRequiredZ : 1.0e30f;
		if(!found || relief > maxRelief){
			std::string body = "\"applied\":false,\"profile\":\"" + jsonEscape(profile.c_str()) +
				"\",\"reason\":\"no sufficiently supported placement found\"";
			if(found)
				body += ",\"best_candidate\":[" + std::to_string(fittedPosition.x) + "," +
					std::to_string(fittedPosition.y) + "],\"analysis\":" +
					agentPlacementAnalysisJson(bestAnalysis);
			writeResponse(requestId, true, body);
			return;
		}
		// Pick the middle of the exact Z interval accepted by the shared contract.
		// This leaves numerical margin on both the floating and penetration bounds.
		float minimumValidZ = bestAnalysis.maxRequiredZ - contract.maxPenetration;
		float maximumValidZ = bestAnalysis.minRequiredZ + contract.maxFloatingClearance;
		if(minimumValidZ > maximumValidZ){
			AgentSupportEvaluation evaluation = evaluateAgentSupport(
				inst->m_objectId, true, bestAnalysis);
			writeResponse(requestId, true,
				"\"applied\":false,\"profile\":\"" + jsonEscape(profile.c_str()) +
				"\",\"reason\":\"support relief has no validator-safe Z interval\","
				"\"analysis\":" + agentPlacementAnalysisJson(bestAnalysis) +
				",\"support_evaluation\":" + agentSupportEvaluationJson(evaluation, &bestAnalysis));
			return;
		}
		fittedPosition.z = (minimumValidZ + maximumValidZ) * 0.5f;
		AgentPlacementAnalysis finalAnalysis;
		bool finalAnalyzed = analyzeAgentPlacement(inst->m_objectId, fittedPosition,
			fittedRotation, contract.sampleGridSize, &finalAnalysis);
		AgentSupportEvaluation finalEvaluation = evaluateAgentSupport(
			inst->m_objectId, finalAnalyzed, finalAnalysis);
		if(!finalEvaluation.valid){
			writeResponse(requestId, true,
				"\"applied\":false,\"profile\":\"" + jsonEscape(profile.c_str()) +
				"\",\"reason\":\"best candidate does not satisfy terrain-support-v1\","
				"\"analysis\":" + (finalAnalyzed ? agentPlacementAnalysisJson(finalAnalysis) : "null") +
				",\"support_evaluation\":" + agentSupportEvaluationJson(
					finalEvaluation, finalAnalyzed ? &finalAnalysis : nil));
			return;
		}
		rw::V3d fittedDegrees = GetObjectRotationDegrees(fittedRotation);
		float relocationDistance = length(sub(fittedPosition, originalPosition));
		float rotationDelta = fabsf(fittedDegrees.x - degrees.x) +
			fabsf(fittedDegrees.y - degrees.y) + fabsf(fittedDegrees.z - degrees.z);
		if(relocationDistance < 0.0001f && rotationDelta < 0.001f){
			writeResponse(requestId, true,
				"\"applied\":false,\"profile\":\"" + jsonEscape(profile.c_str()) +
				"\",\"reason\":\"object already satisfies terrain-support-v1\","
				"\"analysis\":" + agentPlacementAnalysisJson(finalAnalysis) +
				",\"support_evaluation\":" + agentSupportEvaluationJson(finalEvaluation, &finalAnalysis));
			return;
		}
		std::vector<UndoTransform> transforms;
		if(!CaptureObjectTransformTargets(inst, false, transforms)){
			writeError(requestId, "could not capture terrain-fit transform");
			return;
		}
		PreviewObjectTransformTargets(inst, transforms, fittedPosition,
			fittedRotation, UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
		CommitObjectTransformTargets(transforms);
		gAgentSceneRevision++;
		char prefix[512];
		snprintf(prefix, sizeof(prefix),
			"\"applied\":true,\"profile\":\"%s\",\"position\":[%.4f,%.4f,%.4f],"
			"\"rotation\":[%.3f,%.3f,%.3f],\"relocation_distance\":%.4f,"
			"\"rotation_delta_degrees\":%.4f,\"analysis\":",
			jsonEscape(profile.c_str()).c_str(), fittedPosition.x, fittedPosition.y,
			fittedPosition.z, fittedDegrees.x, fittedDegrees.y, fittedDegrees.z,
			relocationDistance, rotationDelta);
		writeResponse(requestId, true, std::string(prefix) + agentPlacementAnalysisJson(finalAnalysis) +
			",\"support_evaluation\":" + agentSupportEvaluationJson(finalEvaluation, &finalAnalysis));
		return;
	}
	if(command == "place"){
		if(!requireAgentSession(requestId)) return;
		std::vector<ObjectInst*> created;
		ObjectInst *hd = nil;
		std::string error;
		if(!placeObject(lines, 2, created, &hd, &error)){
			writeError(requestId, error.c_str());
			return;
		}
		UndoRecordPaste(created.data(), (int)created.size());
		// Agent reviews need an unbiased viewport; editor selection tint would
		// otherwise make every newly placed asset appear bright red in captures.
		ClearSelection();
		gAgentSceneRevision++;
		char body[256];
		snprintf(body, sizeof(body), "\"instance_id\":%d,\"model_id\":%d,\"z\":%.4f,\"created_count\":%d",
			hd->m_id, PawnModelId(hd->m_objectId), hd->m_translation.z, (int)created.size());
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "batch"){
		if(!requireAgentSession(requestId)) return;
		std::vector<ObjectInst*> created;
		std::string placed = "\"placements\":[";
		int placementCount = 0;
		for(size_t i = 2; i < lines.size(); i++){
			if(lines[i].empty()) continue;
			std::vector<std::string> fields;
			size_t start = 0;
			while(start <= lines[i].size()){
				size_t end = lines[i].find('\t', start);
				if(end == std::string::npos) end = lines[i].size();
				fields.push_back(lines[i].substr(start, end - start));
				if(end == lines[i].size()) break;
				start = end + 1;
			}
			ObjectInst *hd = nil;
			std::string error;
			if(!placeObject(fields, 0, created, &hd, &error)){
				for(size_t j = 0; j < created.size(); j++) created[j]->Delete();
				writeError(requestId, ("batch row " + std::to_string(i - 1) + ": " + error).c_str());
				return;
			}
			if(placementCount++) placed += ',';
			char item[160];
			snprintf(item, sizeof(item), "{\"instance_id\":%d,\"model_id\":%d,\"z\":%.4f}",
				hd->m_id, PawnModelId(hd->m_objectId), hd->m_translation.z);
			placed += item;
		}
		placed += "]";
		if(created.empty()){
			writeError(requestId, "batch contains no placements");
			return;
		}
		UndoRecordPaste(created.data(), (int)created.size());
		ClearSelection();
		gAgentSceneRevision++;
		writeResponse(requestId, true, placed);
		return;
	}
	if(command == "place_element"){
		// kind, model (-1: the kind's default), x, y, z, heading, snap, then key=value lines
		if(!requireAgentSession(requestId)) return;
		if(lines.size() < 9){
			writeError(requestId, "place_element requires kind, model, x, y, z, heading and snap");
			return;
		}
		int kind = parseAgentKind(lines[2]);
		int model;
		float x, y, z, heading;
		int snap;
		if(kind < 0){
			writeError(requestId, ("unknown element kind " + lines[2] + "; one of object, vehicle, "
				"pickup, label, actor, class, gangzone, mapicon, npc, pathpoint, sound, checkpoint, area").c_str());
			return;
		}
		if(!parseFloat(lines[4], &x) || !parseFloat(lines[5], &y) || !parseFloat(lines[6], &z) ||
		   !parseFloat(lines[7], &heading) || !parseInt(lines[8], &snap)){
			writeError(requestId, "invalid numeric placement field");
			return;
		}
		if(lines[3].empty() || !parseInt(lines[3], &model)){
			if(!resolveObject(lines[3], &model)){
				if(!lines[3].empty()){
					writeError(requestId, ("unknown model: " + lines[3]).c_str());
					return;
				}
				model = -1;
			}
		}
		if(model != -1)
			model = EditorModelId(model);
		ObjectDef *def = model >= 0 ? GetObjectDef(model) : nil;
		bool isPedKind = kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_NPC;
		int elementKind = kind == AGENT_KIND_VEHICLE ? SAMP_ELEMENT_OBJECT : kind;
		int elementModel = model;
		if(kind == SAMP_ELEMENT_OBJECT || kind == AGENT_KIND_VEHICLE){
			bool vehicle = def && def->m_isVehicle;
			if(def == nil || def->m_isPed || vehicle != (kind == AGENT_KIND_VEHICLE)){
				writeError(requestId, kind == AGENT_KIND_VEHICLE ? "a vehicle needs a vehicle model (400-611)" :
					"an object needs an object model (vehicles and skins are their own kinds)");
				return;
			}
		}else if(isPedKind){
			if(model < 0) elementModel = 0;
			else if(model != 0 && !(def && def->m_isPed)){
				writeError(requestId, "actors, classes and NPCs take a skin id as model");
				return;
			}
		}else if(kind == SAMP_ELEMENT_PICKUP){
			if(model < 0) elementModel = 1239;
			else if(def == nil || def->m_isPed || def->m_isVehicle){
				writeError(requestId, "a pickup needs an object model");
				return;
			}
		}else if(model >= 0){
			writeError(requestId, "labels, gang zones, map icons and path points take no model (-1)");
			return;
		}
		int displayModel = kind == SAMP_ELEMENT_OBJECT || kind == AGENT_KIND_VEHICLE ? model :
			GetSampElementDisplayModel(elementKind, elementModel);
		if(GetObjectDef(displayModel) == nil){
			writeError(requestId, "Haumea has no model to draw this element with");
			return;
		}
		// A path point's path comes from its properties; NPC names stay unique.
		char pathName[PAWN_OBJECT_NAME_LEN] = "", raceName[PAWN_OBJECT_NAME_LEN] = "";
		for(size_t i = 9; i < lines.size(); i++){
			if(lines[i].compare(0, 5, "path=") == 0)
				snprintf(pathName, sizeof(pathName), "%s", decodeAgentText(lines[i].substr(5)).c_str());
			// A race checkpoint goes on the end of its race, like a path point.
			if(lines[i].compare(0, 5, "race=") == 0)
				snprintf(raceName, sizeof(raceName), "%s", decodeAgentText(lines[i].substr(5)).c_str());
		}
		if(kind == SAMP_ELEMENT_PATHPOINT && pathName[0] && !IsValidPawnObjectName(pathName)){
			writeError(requestId, "path: a path name is a Pawn identifier");
			return;
		}

		rw::V3d position = { x, y, z };
		if(snap){
			rw::V3d ground;
			// Cars, peds and path points stand on the ground, not on lamp posts.
			if(!getAgentGroundPlacementSurface(position, &ground, nil, ModelStandsOnGround(displayModel, elementKind))){
				writeError(requestId, "no ground surface at placement");
				return;
			}
			position.z = ground.z + (isPedKind ? 1.0f : GetElementPlacementBaseOffset(elementKind, displayModel));
		}
		int oldSpawnId = GetSpawnObjectId(), oldSpawnKind = GetSpawnElementKind();
		std::string oldPath = GetSpawnPathName(), oldRace = GetSpawnRaceName();
		SetSpawnObjectId(displayModel);
		SetSpawnElementKind(elementKind);
		if(pathName[0])
			SetSpawnPathName(pathName);
		SetSpawnRaceName(kind == SAMP_ELEMENT_CHECKPOINT && IsValidPawnObjectName(raceName) ? raceName : "");
		rw::Quat rotation = MakeObjectRotationDegrees({ 0.0f, 0.0f, heading });
		ObjectInst *spawned[4] = {};
		int count = SpawnPlaceObjectNoUndo(position, &rotation, spawned, 4);
		SetSpawnObjectId(oldSpawnId);
		SetSpawnElementKind(oldSpawnKind);
		SetSpawnPathName(oldPath.c_str());
		SetSpawnRaceName(oldRace.c_str());
		if(count == 0){
			writeError(requestId, "Haumea could not create the element");
			return;
		}
		ObjectInst *hd = spawned[count - 1];
		std::vector<ObjectInst*> created(spawned, spawned + count);
		if(kind != SAMP_ELEMENT_OBJECT && kind != AGENT_KIND_VEHICLE &&
		   GetObjectPawnProperties(hd)->elementModel != elementModel){
			ObjectPawnProperties p = *GetObjectPawnProperties(hd);
			p.elementModel = elementModel;
			SetObjectPawnProperties(hd, p, false);
		}
		std::string error;
		if(!applyAgentProperties(hd, lines, 9, &error)){
			for(int i = 0; i < count; i++) spawned[i]->Delete();
			writeError(requestId, error.c_str());
			return;
		}
		UndoRecordPaste(created.data(), (int)created.size());
		ClearSelection();
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"instance_id\":") + std::to_string(hd->m_id) +
			",\"created_count\":" + std::to_string(count) + ",\"object\":" + agentInstanceJson(hd, false));
		return;
	}
	if(command == "prefab_list"){
		std::vector<PrefabListing> prefabs;
		ListPrefabs(prefabs);
		std::string json = "\"result\":\"prefabs\",\"prefabs\":[";
		for(size_t i = 0; i < prefabs.size(); i++){
			const PrefabListing &p = prefabs[i];
			json += std::string(i ? "," : "") + "{\"name\":\"" + jsonEscapeBytes(p.name.c_str()) +
				"\",\"folder\":\"" + jsonEscapeBytes(p.folder.c_str()) + "\",\"path\":\"" +
				jsonEscapeBytes(p.path.c_str()) + "\",\"instances\":" + std::to_string(p.info.instances) +
				",\"elements\":" + std::to_string(p.info.elements) + ",\"vehicles\":" +
				std::to_string(p.info.vehicles) + ",\"tags\":\"" + jsonEscapeBytes(p.info.tags.c_str()) + "\"}";
		}
		writeResponse(requestId, true, json + "]");
		return;
	}
	if(command == "prefab_place"){
		// prefab_place <name, folder/name or path> x y z heading snap [group]
		if(!requireAgentSession(requestId)) return;
		float x, y, z, heading;
		int snap;
		if(lines.size() < 8 || !parseFloat(lines[3], &x) || !parseFloat(lines[4], &y) || !parseFloat(lines[5], &z) ||
		   !parseFloat(lines[6], &heading) || !parseInt(lines[7], &snap)){
			writeError(requestId, "prefab_place requires a prefab, x, y, z, heading and snap");
			return;
		}
		std::string want = decodeAgentText(lines[2]), path;
		if(doesFileExist(want.c_str()))
			path = want;
		else{
			std::vector<PrefabListing> prefabs;
			ListPrefabs(prefabs);
			// The name, or any end of "folder/name": "Road works", "Street/Road works",
			// "Starter/Street/Road works".
			std::string lower = lowercase(want.c_str());
			for(const PrefabListing &p : prefabs){
				std::string full = lowercase((p.folder + "/" + p.name).c_str());
				if(full == lower || (full.size() > lower.size() &&
				   full.compare(full.size() - lower.size(), lower.size(), lower) == 0 &&
				   full[full.size() - lower.size() - 1] == '/')){
					path = p.path;
					break;
				}
			}
		}
		if(path.empty()){
			writeError(requestId, ("no prefab called " + want + "; prefab_list shows them").c_str());
			return;
		}
		rw::V3d position = { x, y, z };
		if(snap){
			rw::V3d ground;
			if(!getAgentGroundPlacementSurface(position, &ground, nil, true)){
				writeError(requestId, "no ground surface at placement");
				return;
			}
			position.z = ground.z;
		}
		std::string group = lines.size() > 8 ? decodeAgentText(lines[8]) : "";
		int count = ImportPrefabAtEx(path.c_str(), position, heading, group.empty() ? nil : group.c_str());
		if(count <= 0){
			writeError(requestId, "Haumea could not place the prefab");
			return;
		}
		std::string ids;
		for(CPtrNode *p = selection.first; p; p = p->next)
			ids += (ids.empty() ? "" : ",") + std::to_string(((ObjectInst*)p->item)->m_id);
		ClearSelection();
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"created_count\":") + std::to_string(count) +
			",\"instance_ids\":[" + ids + "],\"path\":\"" + jsonEscapeBytes(path.c_str()) + "\"");
		return;
	}
	if(command == "prefab_save"){
		// prefab_save <name> <folder> <tags> <origin lowest|centre|first> <mine|workspace> <id> [<id> ...]
		if(lines.size() < 8){
			writeError(requestId, "prefab_save requires name, folder, tags, origin, where and instance ids");
			return;
		}
		std::string name = decodeAgentText(lines[2]), folder = decodeAgentText(lines[3]);
		std::string tags = decodeAgentText(lines[4]), originName = lowercase(lines[5].c_str());
		if(name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || folder.find("..") != std::string::npos){
			writeError(requestId, "prefab_save needs a plain name (and a folder without ..)");
			return;
		}
		int origin = originName == "first" ? PREFAB_ORIGIN_FIRST :
			originName == "centre" || originName == "center" ? PREFAB_ORIGIN_CENTRE : PREFAB_ORIGIN_LOWEST;
		std::string dir = PrefabSaveDir(lowercase(lines[6].c_str()) == "workspace", folder.c_str());
		if(dir.empty()){
			writeError(requestId, "no workspace is set to save in");
			return;
		}
		ClearSelection();
		for(size_t i = 7; i < lines.size(); i++){
			int id;
			ObjectInst *inst = parseInt(lines[i], &id) ? GetInstanceByID(id) : nil;
			if(inst && !inst->m_isDeleted)
				inst->Select();
		}
		std::string path = dir + "/" + name + ".haumea";
		int count = selection.first ? ExportPrefabEx(path.c_str(), origin, tags.c_str()) : 0;
		ClearSelection();
		if(count <= 0){
			writeError(requestId, "nothing to save: no live instance ids");
			return;
		}
		writeResponse(requestId, true, std::string("\"saved_count\":") + std::to_string(count) +
			",\"path\":\"" + jsonEscapeBytes(path.c_str()) + "\"");
		return;
	}
	if(command == "model_filter" || command == "model_meta"){
		// model_filter <filters> [limit] [name]: models passing the Object
		// Browser's filters (objfilters.h for the keys), in their sort order.
		// model_meta <id,id,...>: the same facts for these models.
		std::vector<int> ids;
		int total = 0, limit = 200;
		if(command == "model_filter"){
			std::string error;
			std::string name = lines.size() > 4 ? decodeAgentText(lines[4]) : "";
			total = FilterModels(lines.size() > 2 ? decodeAgentText(lines[2]).c_str() : "", name.c_str(), ids, &error);
			if(total < 0){
				writeError(requestId, error.c_str());
				return;
			}
			if(lines.size() > 3)
				parseInt(lines[3], &limit);
			limit = clamp(limit, 1, 5000);
		}else{
			std::string list = lines.size() > 2 ? lines[2] : "";
			for(size_t at = 0; at < list.size();){
				size_t comma = list.find(',', at);
				int id = EditorModelId(atoi(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
				if(GetModelMeta(id))
					ids.push_back(id);
				at = comma == std::string::npos ? list.size() : comma + 1;
			}
			total = (int)ids.size();
			limit = 5000;
		}
		std::string json = "\"result\":\"models\",\"total\":" + std::to_string(total) + ",\"models\":[";
		for(int i = 0; i < (int)ids.size() && i < limit; i++){
			int id = ids[i];
			const ModelMeta *m = GetModelMeta(id);
			ObjectDef *obj = GetObjectDef(id);
			if(m == nil || obj == nil)
				continue;
			int category = GetObjectCategory(id);
			std::string flags, cities, zones;
			struct { bool on; const char *name; } fl[] = { { m->dynamic, "dynamic" }, { m->explosive, "explosive" }, { m->breakable, "breakable" },
				{ m->timed, "timed" }, { m->animated, "animated" }, { m->upgrade, "upgrade" },
				{ m->building, "building" }, { m->interior, "interior" }, { m->alpha, "alpha" },
				{ m->twoSided, "two_sided" }, { m->isLod, "lod" }, { m->hasLod, "has_lod" } };
			for(auto &f : fl)
				if(f.on) flags += std::string(flags.empty() ? "" : ",") + "\"" + f.name + "\"";
			for(int c = 0; c < NUM_MODEL_CITIES; c++)
				if(m->cities & 1u << c)
					cities += std::string(cities.empty() ? "" : ",") + "\"" + GetModelCityName(c) + "\"";
			for(int z : m->zones)
				zones += std::string(zones.empty() ? "" : ",") + "\"" + jsonEscapeBytes(GetMetaZoneName(z)) + "\"";
			json += std::string(i ? "," : "") + "{\"model\":" + std::to_string(PawnModelId(id)) +
				",\"name\":\"" + jsonEscapeBytes(obj->m_name) + "\",\"category\":\"" +
				jsonEscapeBytes(category >= 0 && category < NUM_OBJ_CATEGORIES ? objCategories[category].name : "") +
				"\",\"source\":\"" + GetModelSourceName(m->source) + "\",\"ide\":\"" + jsonEscapeBytes(m->ideFile) +
				"\",\"txd\":\"" + jsonEscapeBytes(m->txdName) + "\",\"size\":[" + jsonFloat(m->width) + "," +
				jsonFloat(m->depth) + "," + jsonFloat(m->height) + "],\"collision\":" + (m->hasCollision ? "true" : "false") +
				",\"draw_distance\":" + jsonFloat(m->drawDistance) + ",\"flags\":[" + flags + "]" +
				(m->timed ? ",\"hours\":[" + std::to_string(m->timeOn) + "," + std::to_string(m->timeOff) + "]" : "") +
				",\"placements\":" + std::to_string(m->placements) + ",\"cities\":[" + cities + "],\"zones\":[" + zones +
				"],\"polygons\":" + std::to_string(m->polygons) + ",\"materials\":" + std::to_string(m->materials) + "}";
		}
		writeResponse(requestId, true, json + "]");
		return;
	}
	if(command == "texture_search"){
		// texture_search <query> [limit]: textures by name or TXD, with the
		// model whose TXD it is (the pair SA-MP resolves).
		int limit = 50;
		if(lines.size() > 3)
			parseInt(lines[3], &limit);
		limit = clamp(limit, 1, 256);
		std::vector<TextureSearchResult> results;
		SearchTextures(lines.size() > 2 ? decodeAgentText(lines[2]).c_str() : "", limit, results);
		std::string json = "\"result\":\"textures\",\"textures\":[";
		for(size_t i = 0; i < results.size(); i++){
			const TextureSearchResult &r = results[i];
			char head[64];
			snprintf(head, sizeof(head), "%s{\"model\":%d,\"txd\":\"", i ? "," : "", r.model);
			json += head + jsonEscapeBytes(r.txd.c_str()) + "\",\"texture\":\"" + jsonEscapeBytes(r.texture.c_str()) +
				"\",\"samp\":" + (r.samp ? "true" : "false") + ",\"custom\":" + (r.custom ? "true" : "false") +
				",\"tstudio_index\":" + std::to_string(r.tstudioIndex) +
				(r.tiles ? std::string(",\"tiles\":\"") + (r.tiles == 3 ? "both" : r.tiles == 1 ? "across" : "down") + "\"" :
					std::string()) + "}";
		}
		writeResponse(requestId, true, json + "]");
		return;
	}
	if(command == "textdraws"){
		std::vector<TextDraw> &tds = GetTextDraws();
		std::string json = "\"result\":\"textdraws\"," + agentTextDrawFileJson() +
			",\"canvas\":[640,448],\"textdraws\":[";
		for(size_t i = 0; i < tds.size(); i++){
			if(i) json += ",";
			json += agentTextDrawJson(i, tds[i]);
		}
		writeResponse(requestId, true, json + "]");
		return;
	}
	if(command == "textdraw_new" || command == "textdraw_open" || command == "textdraw_save" ||
	   command == "textdraw_move_from_map"){
		// The textdraw file, apart from the map: textdraw_new [discard];
		// textdraw_open <path> [discard]; textdraw_save [path];
		// textdraw_move_from_map [path] (an old map's read-only textdraws).
		// Saving keeps the session; the others replace what rollback would restore.
		if(gAgentSessionActive && command != "textdraw_save"){
			writeError(requestId, "commit or roll back the edit session first");
			return;
		}
		std::string path = lines.size() > 2 ? lines[2] : std::string();
		std::string error;
		if(command == "textdraw_new" || command == "textdraw_open"){
			bool open = command == "textdraw_open";
			if(open && path.empty()){
				writeError(requestId, "textdraw_open requires a .pwn or .inc path");
				return;
			}
			size_t discardField = open ? 3 : 2;
			bool discard = false;
			if(lines.size() > discardField)
				parseBoolField(lines[discardField], &discard);
			if(TextDrawFileHasUnsavedChanges() && !discard){
				writeError(requestId, "the textdraw file has unsaved changes: save it first, or pass discard");
				return;
			}
			if(open && !OpenTextDrawFile(path.c_str(), &error)){
				writeError(requestId, error.c_str());
				return;
			}
			if(!open)
				NewTextDrawFile();
			if(open)
				AddRecentTextDrawFile(path.c_str());
		}else if(command == "textdraw_save"){
			if(!SaveTextDrawFile(path.c_str(), &error)){
				writeError(requestId, (error + (path.empty() && !GetTextDrawFilePath()[0] ? ": give a path" : "")).c_str());
				return;
			}
			AddRecentTextDrawFile(GetTextDrawFilePath());
		}else{
			if(path.empty() && !GetTextDrawFilePath()[0])
				path = SuggestedTextDrawFilePath(GetPawnMapPath());
			if(MoveMapTextDrawsToFile(path.c_str(), &error) < 0){
				writeError(requestId, error.c_str());
				return;
			}
		}
		gAgentSceneRevision++;
		writeResponse(requestId, true, "\"result\":\"" + command + "\"," + agentTextDrawFileJson());
		return;
	}
	if(command == "textdraw_add" || command == "textdraw_set" || command == "textdraw_delete"){
		if(!requireAgentSession(requestId)) return;
		std::vector<TextDraw> &tds = GetTextDraws();
		int index = -1;
		size_t first = 2;
		if(command != "textdraw_add"){
			if(lines.size() < 3 || !parseInt(lines[2], &index) || index < 0 || index >= (int)tds.size()){
				writeError(requestId, "textdraw index out of range");
				return;
			}
			first = 3;
		}
		if(command == "textdraw_delete"){
			tds.erase(tds.begin() + index);
			TextDrawsChanged();
			gAgentSceneRevision++;
			writeResponse(requestId, true, "\"result\":\"deleted\"");
			return;
		}
		TextDraw td;
		if(index >= 0)
			td = tds[index];
		else{
			InitTextDraw(&td);
			td.x = 320.0f;
			td.y = 200.0f;
			td.text = "Text";
		}
		std::string error;
		if(!applyTextDrawFields(td, lines, first, &error)){
			writeError(requestId, error.c_str());
			return;
		}
		if(index < 0){
			tds.push_back(td);
			index = (int)tds.size() - 1;
		}else
			tds[index] = td;
		TextDrawsChanged();
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"result\":\"applied\",\"textdraw\":") +
			agentTextDrawJson(index, tds[index]));
		return;
	}
	if(command == "test_in_game"){
		// test_in_game run|status|stop|restart [log lines]: the File > Test in game
		// server. run exports the map as it is now; compiling and starting take a
		// few seconds, so poll status.
		std::string action = lines.size() > 2 ? lowercase(lines[2].c_str()) : "status";
		int logLines = 40;
		if(lines.size() > 3 && (!parseInt(lines[3], &logLines) || logLines < 0 || logLines > 2000)){
			writeError(requestId, "log lines must be 0-2000");
			return;
		}
		if(action == "run")
			TestServerRunMap(GetPawnMapFormat());
		else if(action == "stop")
			TestServerStop();
		else if(action == "restart")
			TestServerRestart();
		else if(action != "status"){
			writeError(requestId, "test_in_game takes run, status, stop or restart");
			return;
		}
		TestServerReport report;
		TestServerGetReport(&report, logLines);
		auto list = [](const std::vector<std::string> &items){
			std::string json = "[";
			for(size_t i = 0; i < items.size(); i++)
				json += (i ? ",\"" : "\"") + jsonEscapeBytes(items[i].c_str()) + "\"";
			return json + "]";
		};
		writeResponse(requestId, true, "\"state\":\"" + report.state + "\",\"status\":\"" +
			jsonEscapeBytes(report.status.c_str()) + "\",\"source\":\"" + jsonEscapeBytes(report.source.c_str()) +
			"\",\"port\":" + std::to_string(report.port) + ",\"compiler\":" + list(report.compiler) +
			",\"log\":" + list(report.log));
		return;
	}
	if(command == "maptool"){
		if(!requireAgentSession(requestId)) return;
		handleMapTool(requestId, lines);
		return;
	}
	if(command == "properties"){
		// id, then key=value lines
		if(!requireAgentSession(requestId)) return;
		int instanceId;
		if(lines.size() < 4 || !parseInt(lines[2], &instanceId)){
			writeError(requestId, "properties requires an instance id and key=value lines");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		std::string error;
		if(!applyAgentProperties(inst, lines, 3, &error)){
			writeError(requestId, error.c_str());
			return;
		}
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"result\":\"applied\",\"object\":") +
			agentInstanceJson(inst, false));
		return;
	}
	if(command == "transform3d"){
		if(!requireAgentSession(requestId)) return;
		if(lines.size() < 9){
			writeError(requestId, "transform3d requires id, position xyz and rotation xyz");
			return;
		}
		int instanceId;
		float x, y, z, pitch, roll, yaw;
		if(!parseInt(lines[2], &instanceId) || !parseFloat(lines[3], &x) ||
		   !parseFloat(lines[4], &y) || !parseFloat(lines[5], &z) ||
		   !parseFloat(lines[6], &pitch) || !parseFloat(lines[7], &roll) ||
		   !parseFloat(lines[8], &yaw)){
			writeError(requestId, "invalid transform3d field");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		std::vector<UndoTransform> transforms;
		if(!CaptureObjectTransformTargets(inst, false, transforms)){
			writeError(requestId, "could not capture transform target");
			return;
		}
		PreviewObjectTransformTargets(inst, transforms, { x, y, z },
			MakeObjectRotationDegrees({ pitch, roll, yaw }),
			UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
		CommitObjectTransformTargets(transforms);
		gAgentSceneRevision++;
		writeResponse(requestId, true, "\"result\":\"transformed3d\"");
		return;
	}
	if(command == "transform"){
		if(!requireAgentSession(requestId)) return;
		if(lines.size() < 8){
			writeError(requestId, "transform requires id, x, y, z, heading and snap");
			return;
		}
		int instanceId, snap;
		float x, y, z, heading;
		if(!parseInt(lines[2], &instanceId) || !parseFloat(lines[3], &x) ||
		   !parseFloat(lines[4], &y) || !parseFloat(lines[5], &z) ||
		   !parseFloat(lines[6], &heading) || !parseInt(lines[7], &snap)){
			writeError(requestId, "invalid transform field");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		if(!isAgentSceneInstance(inst) || inst->m_isDeleted){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		rw::V3d position = { x, y, z };
		if(snap){
			rw::V3d ground;
			if(!getAgentGroundPlacementSurface(position, &ground, nil, InstStandsOnGround(inst))){
				writeError(requestId, "no ground surface at transform position");
				return;
			}
			ObjectDef *def = GetObjectDef(inst->m_objectId);
			position.z = ground.z + (def && def->m_isPed ? 1.0f :
				GetElementPlacementBaseOffset(GetSampElementKind(inst), inst->m_objectId));
		}
		std::vector<UndoTransform> transforms;
		if(!CaptureObjectTransformTargets(inst, false, transforms)){
			writeError(requestId, "could not capture transform target");
			return;
		}
		PreviewObjectTransformTargets(inst, transforms, position,
			MakeObjectRotationDegrees({ 0.0f, 0.0f, heading }), UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
		CommitObjectTransformTargets(transforms);
		gAgentSceneRevision++;
		writeResponse(requestId, true, "\"result\":\"transformed\"");
		return;
	}
	if(command == "material" || command == "material_text" || command == "material_clear"){
		if(!requireAgentSession(requestId)) return;
		int slot;
		ObjectInst *inst = agentMaterialTarget(requestId, lines, &slot);
		if(inst == nil)
			return;
		if(command == "material_clear"){
			for(int i = 0; i < MAX_PAWN_MATERIALS; i++)
				if(slot < 0 || i == slot)
					ClearObjectMaterialOverride(inst, i);
			gAgentSceneRevision++;
			writeResponse(requestId, true, "\"result\":\"cleared\"");
			return;
		}
		if(slot < 0){
			writeError(requestId, "slot must be 0-15");
			return;
		}
		ObjectMaterialOverride material = *GetObjectMaterialOverride(inst, slot);
		if(command == "material"){
			// id, slot, model, txd, texture, color (ARGB; 0 keeps the model's)
			int model;
			if(lines.size() < 8 || !parseInt(lines[4], &model) ||
			   !parseColorField(lines[7], &material.color)){
				writeError(requestId, "material requires id, slot, model, txd, texture and color");
				return;
			}
			model = EditorModelId(model);
			std::string txd = decodeAgentText(lines[5]), texture = decodeAgentText(lines[6]);
			material.active = true;
			material.textEnabled = false;
			material.hasTexture = model >= 0;
			material.sourceModel = model;
			strncpy(material.txdName, txd.c_str(), sizeof(material.txdName)-1);
			material.txdName[sizeof(material.txdName)-1] = '\0';
			strncpy(material.textureName, texture.c_str(), sizeof(material.textureName)-1);
			material.textureName[sizeof(material.textureName)-1] = '\0';
		}else{
			// id, slot, size, font, font size, bold, text ARGB, background ARGB,
			// alignment, escaped text
			int size, fontSize, bold, alignment;
			if(lines.size() < 12 || !parseInt(lines[4], &size) || !parseInt(lines[6], &fontSize) ||
			   !parseInt(lines[7], &bold) || !parseColorField(lines[8], &material.textColor) ||
			   !parseColorField(lines[9], &material.backgroundColor) ||
			   !parseInt(lines[10], &alignment)){
				writeError(requestId, "material_text requires id, slot, size, font, font_size, "
					"bold, text_color, background_color, alignment and text");
				return;
			}
			int width, height;
			if(!GetMaterialTextDimensions(size, &width, &height)){
				writeError(requestId, "material size must be an OBJECT_MATERIAL_SIZE code (10-140)");
				return;
			}
			std::string font = decodeAgentText(lines[5]), text = decodeAgentText(lines[11]);
			if(text.empty() || text.size() >= sizeof(material.text) || font.size() >= 32){
				writeError(requestId, "text must be 1-255 bytes and the font name under 32");
				return;
			}
			material.textEnabled = true;
			material.materialSize = size;
			material.fontSize = fontSize;
			material.bold = bold != 0;
			material.alignment = alignment;
			strncpy(material.font, font.c_str(), sizeof(material.font)-1);
			material.font[sizeof(material.font)-1] = '\0';
			strncpy(material.text, text.c_str(), sizeof(material.text)-1);
			material.text[sizeof(material.text)-1] = '\0';
		}
		SetObjectMaterialOverride(inst, slot, material);
		gAgentSceneRevision++;
		// A texture SA-MP won't find is applied anyway, with a warning, as in
		// the Materials panel.
		std::string warning;
		if(material.active && !material.textEnabled){
			char why[512];
			if(CheckMaterialTexture(material, why, sizeof(why)) >= MATERIAL_TEXTURE_WARNING)
				warning = std::string(",\"warning\":\"") + jsonEscape(why) + "\"";
		}
		writeResponse(requestId, true, std::string("\"result\":\"applied\",\"object\":") +
			agentInstanceJson(inst, false) + warning);
		return;
	}
	if(command == "delete"){
		if(!requireAgentSession(requestId)) return;
		int instanceId;
		if(lines.size() < 3 || !parseInt(lines[2], &instanceId)){
			writeError(requestId, "delete requires an instance id");
			return;
		}
		ObjectInst *inst = GetInstanceByID(instanceId);
		bool world = agentMapMode() && inst && !inst->m_isDeleted && IsPawnWorldObject(inst);
		if(!world && (!isAgentSceneInstance(inst) || inst->m_isDeleted)){
			writeError(requestId, "instance does not belong to the active agent scene");
			return;
		}
		if(world){
			// A world building: exported as RemoveBuildingForPlayer.
			bool captured = false;
			for(size_t i = 0; i < gAgentScopedWorldSnapshot.size(); i++)
				captured |= gAgentScopedWorldSnapshot[i].id == inst->m_id;
			if(!captured)
				gAgentScopedWorldSnapshot.push_back(captureAgentSnapshot(inst));
		}
		ObjectInst *deleted[1] = { inst };
		UndoRecordDelete(deleted, 1);
		inst->Delete();
		gAgentSceneRevision++;
		writeResponse(requestId, true, "\"result\":\"deleted\"");
		return;
	}
	if(command == "suppress_world_models"){
		if(!requireAgentSession(requestId)) return;
		if(lines.size() < 6){
			writeError(requestId, "suppress_world_models requires x, y, radius and model ids");
			return;
		}
		float centerX, centerY, radius;
		if(!parseFloat(lines[2], &centerX) || !parseFloat(lines[3], &centerY) ||
		   !parseFloat(lines[4], &radius) || radius <= 0.0f || radius > 500.0f){
			writeError(requestId, "invalid suppression zone");
			return;
		}
		std::unordered_set<int> modelIds;
		for(size_t i = 5; i < lines.size(); i++){
			int modelId;
			if(!parseInt(lines[i], &modelId) || (modelId = EditorModelId(modelId)) < 0 || modelId >= NUMOBJECTDEFS){
				writeError(requestId, "invalid suppression model id");
				return;
			}
			modelIds.insert(modelId);
		}
		if(modelIds.size() > 64){
			writeError(requestId, "suppression model whitelist is limited to 64 ids");
			return;
		}

		float radiusSquared = radius * radius;
		std::vector<ObjectInst*> targets;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted || isAgentSceneInstance(inst) ||
			   modelIds.find(inst->m_objectId) == modelIds.end())
				continue;
			float dx = inst->m_translation.x - centerX;
			float dy = inst->m_translation.y - centerY;
			if(dx * dx + dy * dy <= radiusSquared)
				targets.push_back(inst);
		}
		if(targets.size() > 256){
			writeError(requestId, "suppression zone matched more than 256 instances");
			return;
		}

		std::unordered_set<int32> captured;
		for(size_t i = 0; i < gAgentScopedWorldSnapshot.size(); i++)
			captured.insert(gAgentScopedWorldSnapshot[i].id);
		std::string suppressed = "\"suppressed\":[";
		for(size_t i = 0; i < targets.size(); i++){
			ObjectInst *inst = targets[i];
			if(captured.insert(inst->m_id).second)
				gAgentScopedWorldSnapshot.push_back({ inst->m_id, inst->m_translation,
					inst->m_rotation, inst->m_isDeleted });
			if(i) suppressed += ',';
			char item[256];
			snprintf(item, sizeof(item),
				"{\"instance_id\":%d,\"model_id\":%d,\"position\":[%.3f,%.3f,%.3f]}",
				inst->m_id, PawnModelId(inst->m_objectId), inst->m_translation.x,
				inst->m_translation.y, inst->m_translation.z);
			suppressed += item;
			// This is a runtime-only visibility override. ObjectInst::Delete refuses
			// native instances outside the active document and would also imply a
			// persistent IPL edit, neither of which is wanted for a scratch proposal.
			inst->m_isDeleted = true;
			inst->Deselect();
		}
		suppressed += "],\"suppressed_count\":" + std::to_string(targets.size());
		gAgentSceneRevision++;
		writeResponse(requestId, true, suppressed);
		return;
	}
	if(command == "clear"){
		if(!requireAgentSession(requestId)) return;
		std::vector<ObjectInst*> deleted;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(isAgentSceneInstance(inst) && !inst->m_isDeleted)
				deleted.push_back(inst);
		}
		gAgentSceneRevision++;
		if(!deleted.empty()){
			UndoRecordDelete(deleted.data(), (int)deleted.size());
			for(size_t i = 0; i < deleted.size(); i++) deleted[i]->Delete();
		}
		writeResponse(requestId, true,
			std::string("\"deleted_count\":") + std::to_string(deleted.size()));
		return;
	}
	if(command == "select"){
		// select <id> [<id> ...]: replaces the editor selection, e.g. to show the
		// user which objects the agent means. No ids clears it.
		ClearSelection();
		int count = 0;
		for(size_t i = 2; i < lines.size(); i++){
			int instanceId;
			if(!parseInt(lines[i], &instanceId))
				continue;
			ObjectInst *inst = GetInstanceByID(instanceId);
			if(inst == nil || inst->m_isDeleted)
				continue;
			inst->Select();
			if(inst->m_selected)
				count++;
		}
		writeResponse(requestId, true, std::string("\"selected_count\":") + std::to_string(count));
		return;
	}
	if(command == "selection"){
		std::string body = "\"items\":["; int count = 0;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted || !inst->m_selected) continue;
			if(count == 256){ writeError(requestId, "selection exceeds 256 objects; narrow the selection"); return; }
			if(count++) body += ',';
			body += agentInstanceJson(inst, true);
		}
		writeResponse(requestId, true, body + "]"); return;
	}
	if(command == "environment"){
		if(lines.size() != 2 && lines.size() != 7){
			writeError(requestId, "environment accepts no fields or hour minute weather_a weather_b blend");
			return;
		}
		if(lines.size() == 7){
			int hour, minute, a, b; float blend;
			if(!parseInt(lines[2], &hour) || !parseInt(lines[3], &minute) ||
			   !parseInt(lines[4], &a) || !parseInt(lines[5], &b) || !parseFloat(lines[6], &blend) ||
			   hour < 0 || hour > 23 || minute < 0 || minute > 59 || a < 0 || b < 0 ||
			   a >= params.numWeathers || b >= params.numWeathers || blend < 0 || blend > 1){
				writeError(requestId, "invalid environment settings"); return;
			}
			currentHour = hour; currentMinute = minute;
			Weather::oldWeather = a; Weather::newWeather = b; Weather::interpolation = blend;
			gAgentSceneRevision++;
		}
		char head[256];
		snprintf(head, sizeof(head), "\"hour\":%d,\"minute\":%d,\"weather_a\":%d,\"weather_b\":%d,\"blend\":%.4f,\"weathers\":[",
			currentHour, currentMinute, Weather::oldWeather, Weather::newWeather, Weather::interpolation);
		std::string body = head;
		for(int i = 0; i < params.numWeathers; i++){
			if(i) body += ',';
			body += "{\"id\":" + std::to_string(i) + ",\"name\":\"" + jsonEscape(params.weatherInfo[i].name) + "\"}";
		}
		writeResponse(requestId, true, body + "]"); return;
	}
	if(command == "surface_grid"){
		float x, y, z, radius; int grid;
		if(lines.size() != 7 || !parseFloat(lines[2], &x) || !parseFloat(lines[3], &y) ||
		   !parseFloat(lines[4], &z) || !parseFloat(lines[5], &radius) || !parseInt(lines[6], &grid) ||
		   grid < 2 || grid > 25 || radius <= 0 || radius > 250){
			writeError(requestId, "surface_grid requires xyz radius(0,250] grid[2,25]"); return;
		}
		std::string body = "\"samples\":[";
		for(int row = 0; row < grid; row++) for(int col = 0; col < grid; col++){
			if(row || col) body += ',';
			rw::V3d probe = { x - radius + 2 * radius * col / (grid-1), y - radius + 2 * radius * row / (grid-1), z + 2 };
			rw::V3d hit, normal;
			char item[384];
			if(GetGroundPlacementSurface(probe, &hit, &normal, true))
				snprintf(item, sizeof(item), "{\"hit\":true,\"point\":[%.5f,%.5f,%.5f],\"normal\":[%.5f,%.5f,%.5f]}",
					hit.x, hit.y, hit.z, normal.x, normal.y, normal.z);
			else snprintf(item, sizeof(item), "{\"hit\":false,\"probe\":[%.5f,%.5f,%.5f]}", probe.x, probe.y, probe.z);
			body += item;
		}
		writeResponse(requestId, true, body + "],\"grid\":" + std::to_string(grid)); return;
	}
	if(command == "screen_grid"){
		int columns, rows, revision;
		if(lines.size() != 5 || !parseInt(lines[2], &columns) || !parseInt(lines[3], &rows) ||
		   !parseInt(lines[4], &revision) || columns < 2 || columns > 64 || rows < 2 || rows > 48 ||
		   revision != (int)gAgentCameraRevision){
			writeError(requestId, "screen_grid requires columns[2,64] rows[2,48] current camera revision"); return;
		}
		rw::V3d forward = rw::normalize(rw::sub(TheCamera.m_target, TheCamera.m_position));
		rw::V3d right = rw::scale(rw::normalize(rw::cross(forward, TheCamera.m_up)), TheCamera.m_rwcam->viewWindow.x);
		rw::V3d up = rw::scale(rw::normalize(rw::cross(right, forward)), TheCamera.m_rwcam->viewWindow.y);
		std::string body = "\"samples\":[";
		for(int row = 0; row < rows; row++) for(int col = 0; col < columns; col++){
			if(row || col) body += ',';
			float u = (col + 0.5f) / columns, v = (row + 0.5f) / rows;
			Ray ray; ray.start = TheCamera.m_position;
			ray.dir = rw::normalize(rw::add(forward, rw::add(rw::scale(right, 2*u-1), rw::scale(up, 1-2*v))));
			rw::V3d hit; float depth = 0;
			ObjectInst *inst = GetVisibleInstUnderRay(ray, &hit, &depth);
			char item[512];
			if(inst) snprintf(item, sizeof(item),
				"{\"hit\":true,\"pixel\":[%.1f,%.1f],\"point\":[%.5f,%.5f,%.5f],\"depth\":%.5f,\"instance_id\":%d,\"model_id\":%d,\"agent_scene\":%s}",
				u*sk::globals.width, v*sk::globals.height, hit.x, hit.y, hit.z, depth, inst->m_id, PawnModelId(inst->m_objectId),
				isAgentSceneInstance(inst) ? "true" : "false");
			else snprintf(item, sizeof(item), "{\"hit\":false,\"pixel\":[%.1f,%.1f]}", u*sk::globals.width, v*sk::globals.height);
			body += item;
		}
		writeResponse(requestId, true, body + "]"); return;
	}
	if(command == "camera_context"){
		writeResponse(requestId, true, cameraContextBody());
		return;
	}
	if(command == "raycast_segment"){
		if(lines.size() < 8){
			writeError(requestId, "raycast_segment requires start xyz and target xyz");
			return;
		}
		float values[6], targetTolerance = 1.0f;
		for(int i = 0; i < 6; i++){
			if(!parseFloat(lines[i + 2], &values[i])){
				writeError(requestId, "invalid raycast_segment field");
				return;
			}
		}
		if(lines.size() > 8 && (!parseFloat(lines[8], &targetTolerance) || targetTolerance < 0.0f)){
			writeError(requestId, "invalid raycast_segment target tolerance");
			return;
		}
		rw::V3d start = { values[0], values[1], values[2] };
		rw::V3d target = { values[3], values[4], values[5] };
		float distance = rw::length(rw::sub(target, start));
		if(distance < 0.001f){
			writeError(requestId, "raycast_segment endpoints must differ");
			return;
		}
		rw::V3d hit;
		float hitDistance = 0.0f;
		ObjectInst *inst = getAgentInstUnderSegment(
			start, target, &hit, &hitDistance, targetTolerance);
		if(inst){
			ObjectDef *object = GetObjectDef(inst->m_objectId);
			char body[768];
			snprintf(body, sizeof(body),
				"\"ray\":{\"visible\":false,\"distance\":%.5f,\"target_tolerance\":%.3f,"
				"\"hit\":{\"point\":[%.5f,%.5f,%.5f],\"distance\":%.5f,"
				"\"fraction\":%.6f,\"instance_id\":%d,\"model_id\":%d,\"name\":\"%s\","
				"\"agent_scene\":%s}}",
				distance, targetTolerance, hit.x, hit.y, hit.z, hitDistance,
				hitDistance / distance, inst->m_id, PawnModelId(inst->m_objectId),
				jsonEscape(object ? object->m_name : "").c_str(),
				isAgentSceneInstance(inst) ? "true" : "false");
			writeResponse(requestId, true, body);
		}else{
			char body[256];
			snprintf(body, sizeof(body),
				"\"ray\":{\"visible\":true,\"distance\":%.5f,\"target_tolerance\":%.3f,\"hit\":null}",
				distance, targetTolerance);
			writeResponse(requestId, true, body);
		}
		return;
	}
	if(command == "screen_to_world"){
		if(lines.size() < 6){
			writeError(requestId, "screen_to_world requires pixel x/y and viewport width/height");
			return;
		}
		float pixelX, pixelY;
		int width, height;
		if(!parseFloat(lines[2], &pixelX) || !parseFloat(lines[3], &pixelY) ||
		   !parseInt(lines[4], &width) || !parseInt(lines[5], &height) ||
		   width <= 0 || height <= 0 || width != sk::globals.width ||
		   height != sk::globals.height || pixelX < 0.0f || pixelY < 0.0f ||
		   pixelX >= width || pixelY >= height){
			writeError(requestId, "invalid or stale screen_to_world viewport or pixel");
			return;
		}
		float ndcX = (pixelX + 0.5f) / width * 2.0f - 1.0f;
		float ndcY = (pixelY + 0.5f) / height * 2.0f - 1.0f;
		rw::V3d forward = rw::normalize(rw::sub(TheCamera.m_target, TheCamera.m_position));
		rw::V3d right = rw::scale(rw::normalize(rw::cross(forward, TheCamera.m_up)),
			TheCamera.m_rwcam->viewWindow.x);
		rw::V3d up = rw::scale(rw::normalize(rw::cross(right, forward)),
			TheCamera.m_rwcam->viewWindow.y);
		Ray ray;
		ray.start = TheCamera.m_position;
		ray.dir = rw::normalize(rw::add(forward,
			rw::add(rw::scale(right, ndcX), rw::scale(up, -ndcY))));
		rw::V3d hit;
		float hitT = 0.0f;
		ObjectInst *inst = GetVisibleInstUnderRay(ray, &hit, &hitT);
		if(inst){
			char body[640];
			snprintf(body, sizeof(body),
				"\"hit\":true,\"source\":\"geometry\",\"point\":[%.5f,%.5f,%.5f],"
				"\"depth\":%.5f,\"instance_id\":%d,\"object_key\":\"runtime:%d\","
				"\"model_id\":%d,\"agent_scene\":%s,\"camera_revision\":%u",
				hit.x, hit.y, hit.z, hitT, inst->m_id, inst->m_id,
				inst->m_objectId, isAgentSceneInstance(inst) ? "true" : "false",
				gAgentCameraRevision);
			writeResponse(requestId, true, body);
			return;
		}
		float t = fabsf(ray.dir.z) < 0.001f ? 50.0f :
			(TheCamera.m_target.z - ray.start.z) / ray.dir.z;
		t = std::max(1.0f, std::min(t, 5000.0f));
		rw::V3d projected = rw::add(ray.start, rw::scale(ray.dir, t));
		rw::V3d normal = { 0.0f, 0.0f, 1.0f };
		if(!GetGroundPlacementSurface(projected, &hit, &normal, true)){
			writeResponse(requestId, true, "\"hit\":false");
			return;
		}
		char body[512];
		snprintf(body, sizeof(body),
			"\"hit\":true,\"source\":\"ground_fallback\",\"point\":[%.5f,%.5f,%.5f],"
			"\"normal\":[%.6f,%.6f,%.6f],\"depth\":%.5f,\"camera_revision\":%u",
			hit.x, hit.y, hit.z, normal.x, normal.y, normal.z,
			rw::length(rw::sub(hit, ray.start)), gAgentCameraRevision);
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "list_page"){
		int offset = 0, limit = 8;
		if(lines.size() > 2) parseInt(lines[2], &offset);
		if(lines.size() > 3) parseInt(lines[3], &limit);
		offset = std::max(0, offset);
		limit = std::max(1, std::min(limit, 256));
		bool useZone = lines.size() > 6;
		float centerX = 0.0f, centerY = 0.0f, radius = 0.0f;
		if(useZone && (!parseFloat(lines[4], &centerX) || !parseFloat(lines[5], &centerY) ||
		   !parseFloat(lines[6], &radius) || radius <= 0.0f)){
			writeError(requestId, "invalid list_page zone");
			return;
		}
		std::vector<ObjectInst*> matches;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(!isAgentSceneInstance(inst) || inst->m_isDeleted || inst->m_numChildren > 0 ||
			   !agentInstanceInZone(inst, useZone, centerX, centerY, radius * radius))
				continue;
			matches.push_back(inst);
		}
		std::sort(matches.begin(), matches.end(), [](ObjectInst *a, ObjectInst *b) {
			return a->m_id < b->m_id;
		});
		std::string body = "\"items\":[";
		int end = std::min((int)matches.size(), offset + limit);
		for(int i = offset; i < end; i++){
			if(i > offset) body += ',';
			body += agentInstanceJson(matches[i], false);
		}
		body += "],\"page\":{\"offset\":" + std::to_string(offset) +
			",\"limit\":" + std::to_string(limit) + ",\"returned\":" +
			std::to_string(std::max(0, end - offset)) + ",\"total\":" +
			std::to_string(matches.size()) + ",\"next_offset\":";
		body += end < (int)matches.size() ? std::to_string(end) : "null";
		body += "}";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "inspect_zone_page"){
		if(lines.size() < 7){
			writeError(requestId, "inspect_zone_page requires x, y, radius, offset and limit");
			return;
		}
		float centerX, centerY, radius;
		int offset, limit;
		if(!parseFloat(lines[2], &centerX) || !parseFloat(lines[3], &centerY) ||
		   !parseFloat(lines[4], &radius) || !parseInt(lines[5], &offset) ||
		   !parseInt(lines[6], &limit) || radius <= 0.0f){
			writeError(requestId, "invalid inspect_zone_page field");
			return;
		}
		offset = std::max(0, offset);
		limit = std::max(1, std::min(limit, 256));
		std::vector<ObjectInst*> matches;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted || inst->m_numChildren > 0 ||
			   !agentInstanceInZone(inst, true, centerX, centerY, radius * radius))
				continue;
			matches.push_back(inst);
		}
		std::sort(matches.begin(), matches.end(), [](ObjectInst *a, ObjectInst *b) {
			return a->m_id < b->m_id;
		});
		std::string body = "\"items\":[";
		int end = std::min((int)matches.size(), offset + limit);
		for(int i = offset; i < end; i++){
			if(i > offset) body += ',';
			body += agentInstanceJson(matches[i], true);
		}
		body += "],\"page\":{\"offset\":" + std::to_string(offset) +
			",\"limit\":" + std::to_string(limit) + ",\"returned\":" +
			std::to_string(std::max(0, end - offset)) + ",\"total\":" +
			std::to_string(matches.size()) + ",\"next_offset\":";
		body += end < (int)matches.size() ? std::to_string(end) : "null";
		body += "}";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "list"){
		std::string body = "\"instances\":[";
		int count = 0;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(!isAgentSceneInstance(inst) || inst->m_isDeleted)
				continue;
			ObjectDef *object = GetObjectDef(inst->m_objectId);
			if(count++) body += ',';
			(void)object;
			body += agentInstanceJson(inst, false);
		}
		body += "]";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "scene_bounds"){
		bool found = false;
		float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
		float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(!isAgentSceneInstance(inst) || inst->m_isDeleted || inst->m_numChildren > 0)
				continue;
			if(!found){
				minX = maxX = inst->m_translation.x;
				minY = maxY = inst->m_translation.y;
				minZ = maxZ = inst->m_translation.z;
				found = true;
			}else{
				minX = std::min(minX, inst->m_translation.x);
				minY = std::min(minY, inst->m_translation.y);
				minZ = std::min(minZ, inst->m_translation.z);
				maxX = std::max(maxX, inst->m_translation.x);
				maxY = std::max(maxY, inst->m_translation.y);
				maxZ = std::max(maxZ, inst->m_translation.z);
			}
		}
		if(!found){
			writeError(requestId, "the active agent scene is empty");
			return;
		}
		char body[512];
		snprintf(body, sizeof(body),
			"\"bounds\":{\"min\":[%.4f,%.4f,%.4f],\"max\":[%.4f,%.4f,%.4f],\"center\":[%.4f,%.4f,%.4f]},\"instance_count\":%d",
			minX, minY, minZ, maxX, maxY, maxZ,
			(minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f,
			countAgentInstances(false));
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "inspect_zone"){
		if(lines.size() < 5){
			writeError(requestId, "inspect_zone requires center x, y and radius");
			return;
		}
		float centerX, centerY, radius;
		if(!parseFloat(lines[2], &centerX) || !parseFloat(lines[3], &centerY) ||
		   !parseFloat(lines[4], &radius) || radius <= 0.0f){
			writeError(requestId, "invalid inspect_zone field");
			return;
		}
		int limit = 500;
		if(lines.size() > 5) parseInt(lines[5], &limit);
		limit = std::max(1, std::min(limit, 2000));
		float radiusSquared = radius * radius;
		std::string body = "\"objects\":[";
		int count = 0;
		for(CPtrNode *p = instances.first; p && count < limit; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted || inst->m_numChildren > 0)
				continue;
			float dx = inst->m_translation.x - centerX;
			float dy = inst->m_translation.y - centerY;
			if(dx * dx + dy * dy > radiusSquared)
				continue;
			ObjectDef *object = GetObjectDef(inst->m_objectId);
			if(count++) body += ',';
			char item[640];
			snprintf(item, sizeof(item),
				"{\"instance_id\":%d,\"model_id\":%d,\"name\":\"%s\",\"position\":[%.3f,%.3f,%.3f],\"agent_scene\":%s}",
				inst->m_id, inst->m_objectId, jsonEscape(object ? object->m_name : "").c_str(),
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				isAgentSceneInstance(inst) ? "true" : "false");
			body += item;
		}
		body += "],\"truncated\":";
		body += count >= limit ? "true" : "false";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "validate_zone"){
		if(lines.size() < 7){
			writeError(requestId, "validate_zone requires x, y, radius, offset and limit");
			return;
		}
		float centerX, centerY, radius;
		int offset, limit;
		if(!parseFloat(lines[2], &centerX) || !parseFloat(lines[3], &centerY) ||
		   !parseFloat(lines[4], &radius) || !parseInt(lines[5], &offset) ||
		   !parseInt(lines[6], &limit) || radius <= 0.0f){
			writeError(requestId, "invalid validate_zone field");
			return;
		}
		offset = std::max(0, offset);
		limit = std::max(1, std::min(limit, 256));
		std::vector<ObjectInst*> matches;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			// Only objects sit on terrain; peds stand above it and elements float.
			if(!isAgentSceneInstance(inst) || inst->m_isDeleted || inst->m_numChildren > 0 ||
			   agentKindOf(inst) != SAMP_ELEMENT_OBJECT ||
			   !agentInstanceInZone(inst, true, centerX, centerY, radius * radius))
				continue;
			matches.push_back(inst);
		}
		std::sort(matches.begin(), matches.end(), [](ObjectInst *a, ObjectInst *b) {
			return a->m_id < b->m_id;
		});
		std::string body = "\"items\":[";
		int end = std::min((int)matches.size(), offset + limit);
		int issueCount = 0;
		for(int i = offset; i < end; i++){
			ObjectInst *inst = matches[i];
			AgentPlacementAnalysis analysis;
			AgentSupportContract contract = agentSupportContract(inst->m_objectId);
			bool analyzed = analyzeAgentPlacement(inst->m_objectId, inst->m_translation,
				inst->m_rotation, contract.sampleGridSize, &analysis);
			AgentSupportEvaluation evaluation = evaluateAgentSupport(
				inst->m_objectId, analyzed, analysis);
			float relief = analyzed ? analysis.maxRequiredZ - analysis.minRequiredZ : 0.0f;
			if(!evaluation.valid) issueCount++;
			if(i > offset) body += ',';
			char item[384];
			snprintf(item, sizeof(item),
				"{\"instance_id\":%d,\"valid\":%s,\"floating\":%s,\"embedded\":%s,"
				"\"unsupported_slope\":%s,\"min_clearance\":%.3f,\"max_clearance\":%.3f,"
				"\"support_relief\":%.3f}", inst->m_id, evaluation.valid ? "true" : "false",
				evaluation.floating ? "true" : "false",
				evaluation.embedded ? "true" : "false",
				evaluation.unsupportedRelief ? "true" : "false", analyzed ? analysis.minClearance : 0.0f,
				analyzed ? analysis.maxClearance : 0.0f, relief);
			body += item;
		}
		body += "],\"issue_count_in_page\":" + std::to_string(issueCount) +
			",\"page\":{\"offset\":" + std::to_string(offset) +
			",\"limit\":" + std::to_string(limit) + ",\"returned\":" +
			std::to_string(std::max(0, end - offset)) + ",\"total\":" +
			std::to_string(matches.size()) + ",\"next_offset\":";
		body += end < (int)matches.size() ? std::to_string(end) : "null";
		body += "}";
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "map_check"){
		// The editor's Map Check: what breaks or looks wrong in game, for the map's save format.
		MapCheckResult check;
		RunMapCheck(GetPawnMapFormat(), &check);
		static const char *severities[] = { "error", "warning", "note" };
		std::string items;
		for(size_t i = 0; i < check.items.size(); i++){
			const MapCheckItem &item = check.items[i];
			std::string ids;
			for(size_t j = 0; j < item.instances.size(); j++)
				ids += (j ? "," : "") + std::to_string(item.instances[j]);
			items += std::string(i ? "," : "") + "{\"severity\":\"" + severities[item.severity] +
				"\",\"category\":\"" + jsonEscape(item.category) + "\",\"text\":\"" +
				jsonEscape(item.text.c_str()) + "\",\"instance_ids\":[" + ids + "]" +
				(item.textDraw >= 0 ? ",\"textdraw\":" + std::to_string(item.textDraw) : std::string()) + "}";
		}
		writeResponse(requestId, true, std::string("\"format\":\"") +
			(check.pawnFormat == PAWN_CODE_STREAMER ? "streamer" : "native") +
			"\",\"errors\":" + std::to_string(check.numErrors) +
			",\"warnings\":" + std::to_string(check.numWarnings) + ",\"items\":[" + items + "]");
		return;
	}
	if(command == "validate"){
		std::vector<ObjectInst*> sceneInstances;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(isAgentSceneInstance(inst) && !inst->m_isDeleted && inst->m_numChildren == 0 &&
			   agentKindOf(inst) == SAMP_ELEMENT_OBJECT)
				sceneInstances.push_back(inst);
		}
		std::string supportIssues = "\"support_issues\":[";
		int numSupportIssues = 0;
		for(size_t i = 0; i < sceneInstances.size(); i++){
			ObjectInst *inst = sceneInstances[i];
			AgentPlacementAnalysis analysis;
			AgentSupportContract contract = agentSupportContract(inst->m_objectId);
			bool analyzed = analyzeAgentPlacement(inst->m_objectId, inst->m_translation,
				inst->m_rotation, contract.sampleGridSize, &analysis);
			AgentSupportEvaluation evaluation = evaluateAgentSupport(
				inst->m_objectId, analyzed, analysis);
			if(evaluation.valid)
				continue;
			if(numSupportIssues++) supportIssues += ',';
			char issue[640];
			snprintf(issue, sizeof(issue),
				"{\"instance_id\":%d,\"model_id\":%d,\"profile\":\"%s\","
				"\"floating\":%s,\"embedded\":%s,\"unsupported_slope\":%s,"
				"\"min_clearance\":%.4f,\"max_clearance\":%.4f,\"support_relief\":%.4f,"
				"\"missing_samples\":%d,\"support_contract\":\"terrain-support-v1\"}",
				inst->m_id, inst->m_objectId, contract.profile,
				evaluation.floating ? "true" : "false",
				evaluation.embedded ? "true" : "false",
				evaluation.unsupportedRelief ? "true" : "false",
				analyzed ? analysis.minClearance : 0.0f,
				analyzed ? analysis.maxClearance : 0.0f,
				analyzed ? analysis.maxRequiredZ - analysis.minRequiredZ : 0.0f,
				analyzed ? analysis.missingSamples : contract.sampleGridSize * contract.sampleGridSize);
			supportIssues += issue;
		}
		supportIssues += "]";

		std::string overlaps = "\"building_overlaps\":[";
		int numOverlaps = 0;
		for(size_t i = 0; i < sceneInstances.size(); i++){
			CRect a = sceneInstances[i]->GetBoundRect();
			float aw = a.right - a.left, ah = a.top - a.bottom;
			if(aw < 3.0f || ah < 3.0f) continue;
			for(size_t j = i + 1; j < sceneInstances.size(); j++){
				CRect b = sceneInstances[j]->GetBoundRect();
				float bw = b.right - b.left, bh = b.top - b.bottom;
				if(bw < 3.0f || bh < 3.0f) continue;
				float overlapX = std::min(a.right, b.right) - std::max(a.left, b.left);
				float overlapY = std::min(a.top, b.top) - std::max(a.bottom, b.bottom);
				if(overlapX <= 1.0f || overlapY <= 1.0f || overlapX * overlapY <= 4.0f)
					continue;
				if(numOverlaps++) overlaps += ',';
				char issue[192];
				snprintf(issue, sizeof(issue),
					"{\"a\":%d,\"b\":%d,\"overlap_area\":%.3f}",
					sceneInstances[i]->m_id, sceneInstances[j]->m_id, overlapX * overlapY);
				overlaps += issue;
			}
		}
		overlaps += "]";

		std::string duplicates = "\"exact_duplicates\":[";
		std::string propOverlaps = "\"prop_overlaps\":[";
		int numDuplicates = 0, numPropOverlaps = 0;
		for(size_t i = 0; i < sceneInstances.size(); i++){
			ObjectInst *aInst = sceneInstances[i];
			ObjectDef *aObj = GetObjectDef(aInst->m_objectId);
			if(aObj == nil || aObj->m_colModel == nil) continue;
			CRect a = aInst->GetBoundRect();
			float aMinZ = aInst->m_translation.z + aObj->m_colModel->boundingBox.min.z;
			float aMaxZ = aInst->m_translation.z + aObj->m_colModel->boundingBox.max.z;
			for(size_t j = i + 1; j < sceneInstances.size(); j++){
				ObjectInst *bInst = sceneInstances[j];
				ObjectDef *bObj = GetObjectDef(bInst->m_objectId);
				if(bObj == nil || bObj->m_colModel == nil) continue;
				float centerDistance = rw::length(rw::sub(aInst->m_translation, bInst->m_translation));
				bool duplicate = aInst->m_objectId == bInst->m_objectId && centerDistance < 0.01f &&
					GetQuaternionSimilarity(aInst->m_rotation, bInst->m_rotation) > 0.9999f;
				if(duplicate){
					if(numDuplicates++) duplicates += ',';
					duplicates += "{\"a\":" + std::to_string(aInst->m_id) +
						",\"b\":" + std::to_string(bInst->m_id) +
						",\"model_id\":" + std::to_string(aInst->m_objectId) + "}";
					continue;
				}
				CRect b = bInst->GetBoundRect();
				float overlapX = std::min(a.right, b.right) - std::max(a.left, b.left);
				float overlapY = std::min(a.top, b.top) - std::max(a.bottom, b.bottom);
				float bMinZ = bInst->m_translation.z + bObj->m_colModel->boundingBox.min.z;
				float bMaxZ = bInst->m_translation.z + bObj->m_colModel->boundingBox.max.z;
				float overlapZ = std::min(aMaxZ, bMaxZ) - std::max(aMinZ, bMinZ);
				float aw = std::max(0.001f, a.right - a.left);
				float ah = std::max(0.001f, a.top - a.bottom);
				float bw = std::max(0.001f, b.right - b.left);
				float bh = std::max(0.001f, b.top - b.bottom);
				bool bothBuildings = aw >= 3.0f && ah >= 3.0f && bw >= 3.0f && bh >= 3.0f;
				float footprintRatio = overlapX > 0.0f && overlapY > 0.0f ?
					(overlapX * overlapY) / std::min(aw * ah, bw * bh) : 0.0f;
				if(bothBuildings || overlapZ <= 0.05f || footprintRatio < 0.35f)
					continue;
				if(numPropOverlaps++) propOverlaps += ',';
				char issue[256];
				snprintf(issue, sizeof(issue),
					"{\"a\":%d,\"b\":%d,\"footprint_overlap_ratio\":%.3f,\"vertical_overlap\":%.3f}",
					aInst->m_id, bInst->m_id, footprintRatio, overlapZ);
				propOverlaps += issue;
			}
		}
		duplicates += "]";
		propOverlaps += "]";
		writeResponse(requestId, true, std::string("\"valid\":") +
			(numSupportIssues == 0 && numOverlaps == 0 && numDuplicates == 0 &&
			 numPropOverlaps == 0 ? "true" : "false") +
			",\"instance_count\":" + std::to_string(sceneInstances.size()) + "," +
			supportIssues + "," + overlaps + "," + duplicates + "," + propOverlaps);
		return;
	}
	if(command == "camera_move" || command == "camera_back"){
		// Never while the user is steering: they would fight over the camera.
		const char *busy = gGizmoUsing ? "dragging the gizmo" : gCameraLooking ? "looking around (right mouse button)" :
			gPlaceMode || gPrefabPlaceMode ? "placing" : gBrushMode ? "painting with the brush" : nil;
		if(busy){
			std::string message = std::string("busy: the user is ") + busy + ", try again in a moment";
			writeError(requestId, message.c_str());
			return;
		}
		AgentCameraPose from = currentCameraPose();
		AgentCameraPose to = from;
		if(command == "camera_back"){
			if(gAgentCameraBack.empty()){
				writeError(requestId, "no earlier camera to go back to: the agent hasn't moved it");
				return;
			}
			to = gAgentCameraBack.back();
			gAgentCameraBack.pop_back();
		}else{
			// camera_move pose PX PY PZ TX TY TZ
			// camera_move frame CX CY CZ RADIUS DISTANCE PITCH
			// camera_move instances DISTANCE PITCH ID...
			// camera_move selection DISTANCE PITCH
			// DISTANCE 0 fits the sphere in the view; PITCH is degrees down.
			std::string mode = lines.size() > 2 ? lines[2] : "";
			std::vector<float> v;
			size_t firstId = mode == "instances" ? 5 : lines.size();
			for(size_t i = 3; i < lines.size() && i < firstId; i++){
				float f;
				if(!parseFloat(lines[i], &f)){
					writeError(requestId, "invalid camera_move field");
					return;
				}
				v.push_back(f);
			}
			rw::V3d center = { 0.0f, 0.0f, 0.0f };
			float radius = 0.0f, distance = 0.0f, pitch = 30.0f;
			if(mode == "pose" && v.size() == 6){
				to.position = { v[0], v[1], v[2] };
				to.target = { v[3], v[4], v[5] };
			}else if(mode == "frame" && v.size() == 6){
				center = { v[0], v[1], v[2] };
				radius = v[3]; distance = v[4]; pitch = v[5];
			}else if((mode == "instances" || mode == "selection") && v.size() == 2){
				distance = v[0]; pitch = v[1];
				std::vector<int> ids;
				for(size_t i = firstId; i < lines.size(); i++){
					int id;
					if(!parseInt(lines[i], &id)){
						writeError(requestId, "invalid instance id");
						return;
					}
					ids.push_back(id);
				}
				rw::V3d lo = center, hi = center;
				int found = 0;
				for(CPtrNode *p = instances.first; p; p = p->next){
					ObjectInst *inst = (ObjectInst*)p->item;
					if(inst->m_isDeleted)
						continue;
					if(mode == "selection" ? !inst->m_selected :
					   std::find(ids.begin(), ids.end(), (int)inst->m_id) == ids.end())
						continue;
					rw::V3d c = inst->m_translation;
					float r = 1.0f;
					ObjectDef *obj = GetObjectDef(inst->m_objectId);
					if(obj && obj->m_colModel){
						rw::V3d::transformPoints(&c, &obj->m_colModel->boundingSphere.center, 1, &inst->m_matrix);
						r = obj->m_colModel->boundingSphere.radius;
					}
					rw::V3d a = { c.x - r, c.y - r, c.z - r }, b = { c.x + r, c.y + r, c.z + r };
					if(found++ == 0){
						lo = a; hi = b;
					}else{
						lo = { std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z) };
						hi = { std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z) };
					}
				}
				if(found == 0){
					writeError(requestId, mode == "selection" ? "nothing is selected" : "no such instance in the map");
					return;
				}
				center = rw::scale(rw::add(lo, hi), 0.5f);
				radius = rw::length(rw::sub(hi, lo)) * 0.5f;
			}else{
				writeError(requestId, "camera_move takes pose, frame, instances or selection");
				return;
			}
			if(mode != "pose"){
				if(!(radius >= 0.0f) || !(distance >= 0.0f) || !(pitch >= -89.0f && pitch <= 89.0f)){
					writeError(requestId, "invalid radius, distance or pitch");
					return;
				}
				if(distance <= 0.0f){
					float half = std::max(5.0f, std::min(TheCamera.m_fov, 120.0f)) * 0.5f * (float)M_PI / 180.0f;
					distance = std::max(8.0f, std::max(radius, 1.0f) * 1.3f / sinf(half));
				}
				// Keep the user's heading; tilt PITCH degrees down at the point.
				rw::V3d heading = rw::sub(from.target, from.position);
				heading.z = 0.0f;
				if(rw::length(heading) < 0.001f)
					heading = { 0.0f, 1.0f, 0.0f };
				heading = rw::normalize(heading);
				float pr = pitch * (float)M_PI / 180.0f;
				rw::V3d dir = { heading.x * cosf(pr), heading.y * cosf(pr), -sinf(pr) };
				to.target = center;
				to.position = rw::sub(center, rw::scale(dir, distance));
			}
			if(rw::length(rw::sub(to.target, to.position)) < 0.001f){
				writeError(requestId, "camera position and target must differ");
				return;
			}
			gAgentCameraBack.push_back(from);
			if((int)gAgentCameraBack.size() > AGENT_CAMERA_BACK_POSES)
				gAgentCameraBack.erase(gAgentCameraBack.begin());
		}
		rw::V3d direction = rw::normalize(rw::sub(to.target, to.position));
		to.up = fabsf(direction.z) > 0.995f ? rw::V3d{ 0.0f, 1.0f, 0.0f } : rw::V3d{ 0.0f, 0.0f, 1.0f };
		gAgentCameraFlyFrom = from;
		gAgentCameraFlyTo = to;
		gAgentCameraFlyTime = 0.0f;
		gAgentCameraFlyRequestId = requestId;
		gAgentCameraFlyPending = true;
		return;
	}
	if(command == "camera"){
		if(lines.size() < 9){
			writeError(requestId, "camera requires position xyz, target xyz and fov");
			return;
		}
		float values[7];
		for(int i = 0; i < 7; i++)
			if(!parseFloat(lines[i + 2], &values[i])){
				writeError(requestId, "invalid camera field");
				return;
			}
		rw::V3d position = { values[0], values[1], values[2] };
		rw::V3d target = { values[3], values[4], values[5] };
		rw::V3d direction = rw::sub(target, position);
		if(rw::length(direction) < 0.001f){
			writeError(requestId, "camera position and target must differ");
			return;
		}
		AgentCameraPose pose;
		pose.position = position;
		pose.target = target;
		// Process() applies zero-input orbit updates every frame. Reset both up
		// vectors so a previously invalid camera cannot poison the requested view.
		pose.up = fabsf(rw::normalize(direction).z) > 0.995f ?
			rw::V3d{ 0.0f, 1.0f, 0.0f } : rw::V3d{ 0.0f, 0.0f, 1.0f };
		pose.fov = values[6];
		applyCameraPose(pose);
		gAgentCameraRevision++;
		writeResponse(requestId, true, std::string("\"result\":\"camera_set\",\"camera_revision\":") +
			std::to_string(gAgentCameraRevision));
		return;
	}
	if(command == "capture" || command == "capture_screen"){
		if(lines.size() < 3 || lines[2].empty()){
			writeError(requestId, "capture requires a PNG path");
			return;
		}
		strncpy(gAgentCapturePath, lines[2].c_str(), sizeof(gAgentCapturePath) - 1);
		gAgentCapturePath[sizeof(gAgentCapturePath) - 1] = '\0';
		gAgentCaptureLabel = lines.size() > 3 ? lines[3] : "current";
		gAgentCapturePose = currentCameraPose();
		gAgentCaptureCameraRevision = gAgentCameraRevision;
		gAgentCaptureRestore = false;
		gAgentCaptureScreen = command == "capture_screen";
		gAgentPendingRequestId = requestId;
		gAgentCapturePending = true;
		return;
	}
	if(command == "capture_pose"){
		if(lines.size() < 14 || lines[2].empty()){
			writeError(requestId, "capture_pose requires path, label, position, target, up and fov");
			return;
		}
		AgentCameraPose requested;
		if(!parseFloat(lines[4], &requested.position.x) ||
		   !parseFloat(lines[5], &requested.position.y) ||
		   !parseFloat(lines[6], &requested.position.z) ||
		   !parseFloat(lines[7], &requested.target.x) ||
		   !parseFloat(lines[8], &requested.target.y) ||
		   !parseFloat(lines[9], &requested.target.z) ||
		   !parseFloat(lines[10], &requested.up.x) ||
		   !parseFloat(lines[11], &requested.up.y) ||
		   !parseFloat(lines[12], &requested.up.z) ||
		   !parseFloat(lines[13], &requested.fov) ||
		   rw::length(rw::sub(requested.target, requested.position)) < 0.001f ||
		   rw::length(requested.up) < 0.001f ||
		   rw::length(rw::cross(rw::normalize(rw::sub(requested.target, requested.position)),
		                        rw::normalize(requested.up))) < 0.001f){
			writeError(requestId, "invalid capture_pose field");
			return;
		}
		if(lines.size() > 14){
			int expected;
			if(!parseInt(lines[14], &expected) || (uint32)expected != gAgentCameraRevision){
				writeResponse(requestId, false, std::string("\"error\":\"camera revision changed\","
					"\"camera_revision\":") + std::to_string(gAgentCameraRevision));
				return;
			}
		}
		strncpy(gAgentCapturePath, lines[2].c_str(), sizeof(gAgentCapturePath) - 1);
		gAgentCapturePath[sizeof(gAgentCapturePath) - 1] = '\0';
		gAgentCaptureLabel = lines[3];
		gAgentCaptureRestorePose = currentCameraPose();
		gAgentCaptureRestore = true;
		applyCameraPose(requested);
		gAgentCameraRevision++;
		gAgentCapturePose = currentCameraPose();
		gAgentCaptureCameraRevision = gAgentCameraRevision;
		gAgentPendingRequestId = requestId;
		gAgentCapturePending = true;
		return;
	}
	if(command == "map_status"){
		writeResponse(requestId, true, agentMapStatusJson());
		return;
	}
	// Multiplayer editing: host [port [name [upnp]]], join address port code
	// [name], leave, status.
	if(command == "multiplayer_host" || command == "multiplayer_join"){
		bool host = command == "multiplayer_host";
		int port = MP_DEFAULT_PORT;
		size_t portField = host ? 2 : 3;
		if(lines.size() > portField && !lines[portField].empty() && !parseInt(lines[portField], &port)){
			writeError(requestId, "invalid port");
			return;
		}
		if(!host && (lines.size() < 5 || lines[2].empty() || lines[4].empty())){
			writeError(requestId, "multiplayer_join requires address, port and session code");
			return;
		}
		size_t nameField = host ? 3 : 5;
		const char *name = lines.size() > nameField ? lines[nameField].c_str() : "Namaka";
		std::string error;
		bool upnp = !(host && lines.size() > 4 && lines[4] == "0");
		bool ok = host ? MultiplayerHost(port, name, &error, upnp) :
			MultiplayerJoin(lines[2].c_str(), port, lines[4].c_str(), name, &error);
		if(!ok){
			writeError(requestId, error.c_str());
			return;
		}
		writeResponse(requestId, true, MultiplayerStatusJson());
		return;
	}
	// The Nodes tool (open.mp NPC nodes): nodes_add x y z [vehicle], and
	// nodes_status: a fingerprint of every area, and the edited ones.
	if(command == "nodes_add"){
		rw::V3d pos;
		int vehicle = 1;
		if(lines.size() < 5 || !parseFloat(lines[2], &pos.x) || !parseFloat(lines[3], &pos.y) ||
		   !parseFloat(lines[4], &pos.z) || (lines.size() > 5 && !parseInt(lines[5], &vehicle))){
			writeError(requestId, "nodes_add requires x y z [vehicle 0/1]");
			return;
		}
		SAPaths::EnsureLoaded();
		const char *reason = nil;
		SAPaths::Node *node = SAPaths::NodesLoaded() ? SAPaths::AddNode(pos, vehicle != 0, nil, &reason) : nil;
		int area = -1, point = -1;
		bool isVehicle = false;
		rw::V3d at;
		if(node == nil || !SAPaths::GetNodeInfo(node, &area, &point, &isVehicle, &at)){
			writeError(requestId, reason ? reason : "the node files aren't loaded");
			return;
		}
		char body[128];
		snprintf(body, sizeof(body), "\"area\":%d,\"point\":%d,\"vehicle\":%s", area, point,
			isVehicle ? "true" : "false");
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "nodes_status"){
		SAPaths::EnsureLoaded();
		char body[160];
		snprintf(body, sizeof(body), "\"loaded\":%s,\"fingerprint\":\"%016llx\",\"edited_areas\":%d",
			SAPaths::NodesLoaded() ? "true" : "false", (unsigned long long)SAPaths::NodesFingerprint(),
			SAPaths::GetDirtyAreaCount());
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "multiplayer_leave"){
		MultiplayerLeave();
		writeResponse(requestId, true, MultiplayerStatusJson());
		return;
	}
	if(command == "multiplayer_status"){
		writeResponse(requestId, true, MultiplayerStatusJson());
		return;
	}
	if(command == "map_new" || command == "map_open"){
		// map_new [discard]; map_open <path> [discard]
		if(gAgentSessionActive){
			writeError(requestId, "commit or roll back the edit session first");
			return;
		}
		bool open = command == "map_open";
		if(open && (lines.size() < 3 || lines[2].empty())){
			writeError(requestId, "map_open requires a .pwn, .inc, Texture Studio .db, MTA:SA .map or .ipl path");
			return;
		}
		size_t discardField = open ? 3 : 2;
		bool discard = false;
		if(lines.size() > discardField)
			parseBoolField(lines[discardField], &discard);
		if(PawnMapHasUnsavedChanges() && !discard){
			writeError(requestId, "the open map has unsaved changes: save it first, or pass discard");
			return;
		}
		leaveAgentScene();
		if(open && !OpenPawnMapFile(lines[2].c_str())){
			writeError(requestId, ("could not open " + lines[2]).c_str());
			return;
		}
		if(!open)
			NewPawnMapDocument();
		gAgentSceneRevision++;
		std::string body = agentMapStatusJson();
		if(open){
			body += ",\"import\":" + agentImportReportJson();
		}
		writeResponse(requestId, true, body);
		return;
	}
	if(command == "map_merge"){
		// map_merge <path>: adds a map's contents to the open map, like File >
		// Merge, inside the edit session (rollback takes them out again).
		if(!requireAgentSession(requestId)) return;
		if(lines.size() < 3 || lines[2].empty()){
			writeError(requestId, "map_merge requires a .pwn, .inc, Texture Studio .db, MTA:SA .map or .ipl path");
			return;
		}
		// World buildings the map removes come back on rollback too.
		std::unordered_set<int32> deletedBefore;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted && IsPawnWorldObject(inst))
				deletedBefore.insert(inst->m_id);
		}
		int skipped = 0;
		int merged = MergeMapFile(lines[2].c_str(), &skipped);
		if(merged < 0){
			writeError(requestId, ("could not read " + lines[2]).c_str());
			return;
		}
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted && IsPawnWorldObject(inst) && !deletedBefore.count(inst->m_id)){
				AgentSceneSnapshot snapshot = captureAgentSnapshot(inst);
				snapshot.deleted = false;
				gAgentScopedWorldSnapshot.push_back(snapshot);
			}
		}
		ClearSelection();
		gAgentSceneRevision++;
		writeResponse(requestId, true, agentMapStatusJson() + ",\"merged\":" + std::to_string(merged) +
			",\"skipped\":" + std::to_string(skipped) + ",\"import\":" + agentImportReportJson());
		return;
	}
	if(command == "save"){
		// save [path] [native|streamer] [windows-1252|utf-8]: saves the open map
		// like Ctrl+S; a path works like Save As and becomes the map's path.
		if(gAgentSessionActive){
			writeError(requestId, "commit or roll back the edit session before saving");
			return;
		}
		int format = -1;
		if(lines.size() > 3 && !lines[3].empty()){
			if(lines[3] == "streamer")
				format = PAWN_CODE_STREAMER;
			else if(lines[3] == "native")
				format = PAWN_CODE_NATIVE;
			else{
				writeError(requestId, "save format must be native or streamer");
				return;
			}
		}
		PawnTextEncoding oldEncoding = GetPawnTextEncoding(), encoding = oldEncoding;
		if(lines.size() > 4 && !lines[4].empty()){
			std::string e = lowercase(lines[4].c_str());
			if(e == "utf-8" || e == "utf8")
				encoding = PAWN_TEXT_UTF8;
			else if(e == "windows-1252" || e == "cp1252")
				encoding = PAWN_TEXT_CP1252;
			else{
				writeError(requestId, "text encoding must be windows-1252 or utf-8");
				return;
			}
		}
		const char *path = lines.size() > 2 && !lines[2].empty() ? lines[2].c_str() : nil;
		if(path == nil && GetPawnMapPath()[0] == '\0'){
			writeError(requestId, "the map is untitled: give a path to save it to");
			return;
		}
		SetPawnTextEncoding(encoding);
		if(!SavePawnMapTo(path, format)){
			SetPawnTextEncoding(oldEncoding);
			writeError(requestId, "Pawn export failed");
			return;
		}
		writeResponse(requestId, true, agentMapStatusJson());
		return;
	}
	if(command == "undo" || command == "redo"){
		// undo [steps]: the editor's own undo history, so the user's edits count too.
		if(!requireAgentSession(requestId)) return;
		if(otherAgentEditing()){
			// One history for everyone: its last step may be another agent's.
			writeError(requestId, "another agent is editing this map too: finish_edit_session(commit=False) "
				"takes back only your edits");
			return;
		}
		int steps = 1;
		if(lines.size() > 2 && (!parseInt(lines[2], &steps) || steps < 1 || steps > 100)){
			writeError(requestId, "steps must be 1-100");
			return;
		}
		for(int i = 0; i < steps; i++)
			command == "undo" ? Undo() : Redo();
		ClearSelection();
		gAgentSceneRevision++;
		writeResponse(requestId, true, std::string("\"result\":\"") + command + "\",\"steps\":" +
			std::to_string(steps) + ",\"live_instances\":" + std::to_string(countAgentInstances(false)));
		return;
	}
	writeError(requestId, "unknown command");
}

// One frame of a camera_move fly-to, answered when it lands. The user taking
// the camera (RMB look or the gizmo) ends it where it is.
static void
updateCameraFly(void)
{
	gAgentCameraFlyTime += std::max(0.0f, std::min(timeStep, 0.1f));
	bool interrupted = gCameraLooking || gGizmoUsing;
	float t = std::min(1.0f, gAgentCameraFlyTime / AGENT_CAMERA_FLY_SECONDS);
	if(!interrupted){
		float s = t * t * (3.0f - 2.0f * t);
		AgentCameraPose pose = gAgentCameraFlyTo;
		pose.position = rw::add(gAgentCameraFlyFrom.position,
			rw::scale(rw::sub(gAgentCameraFlyTo.position, gAgentCameraFlyFrom.position), s));
		pose.target = rw::add(gAgentCameraFlyFrom.target,
			rw::scale(rw::sub(gAgentCameraFlyTo.target, gAgentCameraFlyFrom.target), s));
		if(rw::length(rw::sub(pose.target, pose.position)) < 0.001f)
			pose.target = gAgentCameraFlyTo.target;
		rw::V3d d = rw::normalize(rw::sub(pose.target, pose.position));
		pose.up = fabsf(d.z) > 0.995f ? rw::V3d{ 0.0f, 1.0f, 0.0f } : rw::V3d{ 0.0f, 0.0f, 1.0f };
		applyCameraPose(pose);
		if(t < 1.0f)
			return;
	}
	gAgentCameraRevision++;
	gAgentCameraFlyPending = false;
	writeResponse(gAgentCameraFlyRequestId, true, cameraContextBody() +
		",\"camera_revision\":" + std::to_string(gAgentCameraRevision) +
		",\"interrupted\":" + (interrupted ? "true" : "false") +
		",\"back_poses\":" + std::to_string(gAgentCameraBack.size()));
	gAgentCameraFlyRequestId.clear();
}

// ---- What Namaka is doing, for the keycast overlay ----
// One short line per command that changes something; a burst of the same
// kind adds up in one line (keycast replaces a line with the same key).
// Reads show as "looking at the scene", at most every 2 s. Only the overlay
// is translated; the protocol stays English.

static double
agentcastNow(void)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void
agentcastNote(const std::vector<std::string> &lines)
{
	static std::string lastKey;
	static int total;
	static double lastTime = -100.0, lastLook = -100.0;
	const std::string &c = lines[1];
	// The key, and the line: a format with %d for a count, or plain text.
	const char *key = nil, *format = nil;
	int count = 1;
	if(c == "place" || c == "batch"){
		key = "place";
		format = _("placing objects: %d");
		if(c == "batch"){
			count = 0;
			for(size_t i = 2; i < lines.size(); i++)
				count += !lines[i].empty();
		}
	}else if(c == "place_element"){
		key = "place_element";
		format = _("placing elements: %d");
	}else if(c == "prefab_place"){
		key = "prefab";
		format = _("placing a prefab");
	}else if(c == "transform" || c == "transform3d"){
		key = "move";
		format = _("moving objects: %d");
	}else if(c == "delete"){
		key = "delete";
		format = _("deleting elements: %d");
	}else if(c == "clear"){
		key = "delete";
		format = _("clearing its objects");
	}else if(c == "properties"){
		key = "properties";
		format = _("changing elements: %d");
	}else if(c == "material" || c == "material_text" || c == "material_clear"){
		key = "material";
		format = _("changing materials");
	}else if(c == "camera" || c == "camera_move" || c == "camera_back"){
		key = "camera";
		format = _("moving the camera");
	}else if(c == "map_new"){
		key = "map";
		format = _("starting a new map");
	}else if(c == "map_open"){
		key = "map";
		format = _("opening a map");
	}else if(c == "map_merge"){
		key = "map";
		format = _("merging a map into this one");
	}else if(c == "save" || c == "session_commit"){
		key = "save";
		format = _("saving the map");
	}else if(c == "session_rollback"){
		key = "undo";
		format = _("putting the map back as it was");
	}else if(c == "undo"){
		key = "undo";
		format = _("undoing");
	}else if(c == "redo"){
		key = "undo";
		format = _("redoing");
	}else if(c == "maptool" || c == "fit_terrain"){
		key = "arrange";
		format = _("arranging objects");
	}else if(c == "select"){
		key = "select";
		format = _("selecting objects");
	}else if(c == "suppress_world_models"){
		key = "world";
		format = _("hiding world buildings");
	}else if(c == "prefab_save"){
		key = "prefab";
		format = _("saving a prefab");
	}else if(c.compare(0, 9, "textdraw_") == 0){
		key = "textdraw";
		format = _("editing textdraws");
	}else if(c == "test_in_game"){
		key = "test";
		format = _("testing the map in game");
	}else if(c == "capture" || c == "capture_screen" || c == "capture_pose"){
		key = "capture";
		format = _("taking a screenshot");
	}else if(c == "map_check" || c == "validate" || c == "validate_zone"){
		key = "check";
		format = _("checking the map");
	}else if(c == "ping" || c == "capabilities" || c == "session_begin" || c == "session_status"){
		return;	// the connection itself, not something to watch
	}else{
		// Everything else only reads.
		double now = agentcastNow();
		if(now - lastLook < 2.0)
			return;
		lastLook = now;
		KeycastAgentAction(_("looking at the scene"), "look");
		return;
	}
	double now = agentcastNow();
	if(lastKey == key && now - lastTime < 1.5)
		total += count;
	else
		total = count;
	lastKey = key;
	lastTime = now;
	char text[160];
	snprintf(text, sizeof(text), format, total);
	KeycastAgentAction(text, key);
}

void
AgentBridgeUpdate(void)
{
	if(!gAgentBridgeInitialized)
		initializeBridge();
	trackLiveCamera();
	if(gAgentCameraFlyPending)
		updateCameraFly();
	if(!gAgentBridgeEnabled || gAgentCapturePending || gAgentCameraFlyPending)
		return;
	std::vector<std::string> lines;
	if(readRequest(lines)){
		gRequestFailed = false;
		size_t at = lines.size() >= 2 ? lines[0].find('@') : std::string::npos;
		std::string agent = at == std::string::npos ? "" : lines[0].substr(at + 1);
		loadAgentSession(agent);
		uint32 before = GetLatestChangeSeq();
		unsigned int textDrawsBefore = TextDrawsChangeSeq();
		handleRequest(lines);
		if(GetLatestChangeSeq() != before)
			for(CPtrNode *p = instances.first; p; p = p->next){
				ObjectInst *inst = (ObjectInst*)p->item;
				if(inst->m_changeSeq > before)
					gLastAgentEdit[inst->m_id] = agent;
			}
		if(TextDrawsChangeSeq() != textDrawsBefore)
			gLastTextDrawAgent = agent;
		if(!gRequestFailed && lines.size() >= 2)
			agentcastNote(lines);
	}
}

void
AgentBridgeCaptureAfterUiRender(void)
{
	if(!gAgentCaptureScreen)
		return;
	gAgentCaptureScreen = false;
	AgentBridgeCaptureAfterWorldRender();
}

void
AgentBridgeCaptureAfterWorldRender(void)
{
	if(!gAgentCapturePending || Scene.camera == nil || Scene.camera->frameBuffer == nil)
		return;
	if(gAgentCaptureScreen)
		return;	// wait for the panels
#ifdef RW_GL3
	// librw reads the framebuffer into width*3-byte rows; with the default pack
	// alignment of 4, a width that is not a multiple of 4 overruns the buffer.
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
#endif
	rw::Image *image = Scene.camera->frameBuffer->toImage();
	bool captured = image != nil;
	if(image != nil){
		rw::writePNG(image, gAgentCapturePath);
		image->destroy();
	}
	bool restored = false;
	if(gAgentCaptureRestore){
		// Input is processed before the bridge each frame, and capture completes in
		// the same frame. A different revision here nevertheless fails closed if a
		// future renderer makes capture asynchronous.
		if(gAgentCameraRevision == gAgentCaptureCameraRevision){
			applyCameraPose(gAgentCaptureRestorePose);
			gAgentCameraRevision++;
			restored = true;
		}
	}
	std::string body = std::string("\"path\":\"") + jsonEscape(gAgentCapturePath) +
		"\",\"label\":\"" + jsonEscape(gAgentCaptureLabel.c_str()) +
		"\",\"actual_pose\":" + cameraPoseJson(gAgentCapturePose) +
		",\"capture_camera_revision\":" + std::to_string(gAgentCaptureCameraRevision) +
		",\"camera_revision\":" + std::to_string(gAgentCameraRevision) +
		",\"restored\":" + (restored ? "true" : "false");
	if(captured)
		writeResponse(gAgentPendingRequestId, true, body);
	else
		writeResponse(gAgentPendingRequestId, false,
			std::string("\"error\":\"could not read the camera framebuffer\",") + body);
	gAgentCapturePending = false;
	gAgentCaptureRestore = false;
	gAgentCaptureLabel.clear();
	gAgentPendingRequestId.clear();
}

const char*
AgentBridgeSocketPath(void)
{
	return gAgentBridgeEnabled && !gAgentTcp ? gAgentSocketPath : "";
}

bool
AgentBridgeSocketBusy(void)
{
	return gAgentSocketBusy;
}

bool
AgentBridgeListening(void)
{
	return gAgentBridgeEnabled && (gAgentTcp || gAgentUnixBound);
}
