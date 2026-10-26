// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Multiplayer editing: one editor hosts its Pawn map over TCP and others join
// it. Every change to a map element (objects, vehicles, the SA-MP elements,
// removed or changed GTA buildings) shows up on all of them, and each one sees
// where the others are looking and what they have selected.
enum MultiplayerState {
	MP_OFF,
	MP_HOSTING,
	MP_CONNECTING,	// joining: connected or connecting, the map not received yet
	MP_JOINED,
};
static const int MP_DEFAULT_PORT = 7790;

int GetMultiplayerState(void);
// port 0 lets the system pick one (GetMultiplayerPort says which).
bool MultiplayerHost(int port, const char *name, std::string *error);
// Replaces the open map with the host's once connected.
bool MultiplayerJoin(const char *address, int port, const char *code, const char *name, std::string *error);
void MultiplayerLeave(void);
int GetMultiplayerPort(void);
const char *GetMultiplayerCode(void);	// what joining needs, while hosting
int GetMultiplayerPeerCount(void);	// the others in the session
void MultiplayerUpdate(void);	// every frame: the network and the map's changes
void MultiplayerOverlay(bool enabled);	// the others' cameras and selections
void DrawMultiplayerWindow(bool *open);
std::string MultiplayerStatusJson(void);	// for the agent bridge
