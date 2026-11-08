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
};
static const int MP_DEFAULT_PORT = 7790;

int GetMultiplayerState(void);
// port 0 lets the system pick one (GetMultiplayerPort says which);
// openRouterPort asks the router to forward it (UPnP, upnp.h).
bool MultiplayerHost(int port, const char *name, std::string *error, bool openRouterPort = true);
// Replaces the open map with the session's once connected, or sends it into
// an empty session (a new haumea-server's). password: the host's code or
// haumea-server's password.
bool MultiplayerJoin(const char *address, int port, const char *password, const char *name, std::string *error);
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
