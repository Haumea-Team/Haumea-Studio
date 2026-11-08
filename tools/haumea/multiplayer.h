// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Multiplayer editing: editors join a session kept by a relay (mprelay.h),
// which one of them runs to host its Pawn map or haumea-server runs on its
// own. Every change to a map element (objects, vehicles, the SA-MP elements,
// removed or changed GTA buildings) and to the map's textdraws, area removals
// and radar art shows up on all of them, and each one sees where the others
// are looking and what they have selected.
enum MultiplayerState {
	MP_OFF,
	MP_HOSTING,
	MP_CONNECTING,	// joining: connected or connecting, the map not received yet
	MP_JOINED,
	MP_LOBBY,	// signed in to a server with rooms: picking one
};
// How to sign in, for MultiplayerJoinWith.
struct MultiplayerJoinRequest
{
	std::string address;
	int port = 7790;
	int kind = 0;	// MP_LOGIN_*: 0 a code or the one password, 1 an account, 2 an invite, 3 an agent token
	std::string account;	// the account's name
	std::string password;	// the code, the account's password, the invite code or the agent token
	std::string name;	// shown to the others (an account's own name wins)
	std::string room, roomPassword;	// "" for the lobby
	std::string newName, newPassword;	// an invite: the account it makes
	std::string owner;	// an agent's: whose it is (a server with accounts knows it from the token)
};
static const int MP_DEFAULT_PORT = 7790;

int GetMultiplayerState(void);
// port 0 lets the system pick one (GetMultiplayerPort says which);
// openRouterPort asks the router to forward it (UPnP, upnp.h).
// thisComputerOnly: only on loopback, for Namaka's background workers.
bool MultiplayerHost(int port, const char *name, std::string *error, bool openRouterPort = true,
	bool thisComputerOnly = false);
// Replaces the open map with the session's once connected, or sends it into
// an empty session (a new haumea-server's). password: the host's code or
// haumea-server's password.
bool MultiplayerJoin(const char *address, int port, const char *password, const char *name, std::string *error);
bool MultiplayerJoinWith(const MultiplayerJoinRequest &join, std::string *error);
bool MultiplayerEnterRoom(const char *room, const char *roomPassword);	// from the lobby
// An admin action (MP_ADMIN_*); the reply comes in MultiplayerStatusJson's admin_reply.
bool MultiplayerAdmin(int op, const char *a = "", const char *b = "", const char *c = "");
// A token for this account's agents: "" while it's being asked for.
std::string MultiplayerAgentToken(void);
// How our own agents join where we are, for the agent bridge (local only):
// address, port, room, and the code when the server has no accounts.
std::string MultiplayerJoinInfoJson(void);
void MultiplayerLeave(void);
int GetMultiplayerPort(void);
const char *GetMultiplayerCode(void);	// what joining needs, while hosting
int GetMultiplayerPeerCount(void);	// the others in the session
void MultiplayerUpdate(void);	// every frame: the network and the map's changes
void MultiplayerOverlay(bool enabled);	// the others' cameras and selections
void DrawMultiplayerWindow(bool *open);
std::string MultiplayerStatusJson(void);	// for the agent bridge
// Last changed by someone else in the session: an agent rolling back its
// own edits leaves it as they made it.
bool MultiplayerChangedByOthers(const struct ObjectInst *inst);
// Where the others see us, instead of our camera: a background worker has no
// one steering its camera, so its marker goes to where it works.
void MultiplayerSetPresencePose(const rw::V3d &camera, const rw::V3d &target);
// Chat: what we say to the session; the messages after one (the agent bridge).
bool MultiplayerSay(const char *text);
std::string MultiplayerChatJson(unsigned int since);
// The open map was saved to path: a relay that keeps maps gets it too.
void MultiplayerMapSaved(const char *path);
// Opens the map at path and makes it the session's, for everyone.
bool MultiplayerLoadForEveryone(const char *path, const char *adminPassword, std::string *error);
bool MultiplayerReplacingMap(void);	// opening a map now doesn't leave the session
