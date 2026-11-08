// SPDX-License-Identifier: GPL-2.0-or-later
// The multiplayer relay (mpprotocol.h): keeps one map session and puts every
// change in one order. The editor runs one to host (File > Multiplayer), and
// haumea-server runs one on its own, so a session lives on a machine that
// stays up and every editor, Namaka's included, joins it the same way.
// Needs nothing of the editor: no game, no window.
#pragma once
#include <string>

struct MpRelayOptions
{
	int port = 7790;	// 0: the system picks one (MpRelayPort says which)
	const char *bindAddress = "";	// "" for every interface, IPv6 and IPv4
	std::string password;	// what joining needs
	bool passwordIgnoresCase = false;	// the editor's six-letter codes
	std::string name = "Haumea server";	// shown to the editors as whose session it is
	std::string statePath;	// "" keeps the session in memory only
	int maxClients = 32;
	// Keeping the map as Pawn: every save lands in mapsDir (the latest, and a
	// dated copy) and in mapOut (one file, e.g. the game server's), and every
	// autosaveSeconds an editor in the session is asked for one when it changed.
	std::string mapsDir, mapOut;
	double autosaveSeconds = 0.0;
	// Replacing the session's map: allowed unless replace is off; with an
	// admin password, only to who gives it.
	bool allowReplace = true;
	std::string adminPassword;
	void (*log)(const char *line) = nullptr;	// "[Info] ...", "[Warning] ..."
	// A server with accounts and rooms (haumea-server): its state folder. Empty:
	// one room, one code or password, as an editor hosting has it.
	std::string stateDir;
	std::string mapOutRoom = "main";	// with rooms, the one mapOut is for
	bool logChat = true, logConnections = true;
};

bool MpRelayStart(const MpRelayOptions &options, std::string *error);
void MpRelayUpdate(void);	// non-blocking: accepts, reads, relays, writes
void MpRelayWait(double seconds);	// sleeps until a socket is ready (haumea-server)
void MpRelayStop(void);	// saves the session first when it has a state file
bool MpRelayRunning(void);
int MpRelayPort(void);
int MpRelayPeerCount(void);
int MpRelayElementCount(void);
bool MpRelaySave(std::string *error);	// to statePath, now
