// SPDX-License-Identifier: GPL-2.0-or-later
// Opening the multiplayer port on the router (UPnP IGD), so people join a
// session hosted from the editor over the internet without forwarding it by
// hand. Everything happens on a thread of its own; the window shows the state.
#pragma once
#include <string>

enum UpnpState {
	UPNP_OFF,
	UPNP_WORKING,	// finding the router, asking it
	UPNP_OPEN,	// forwarded: others join at UpnpExternalAddress()
	UPNP_FAILED,	// no router that does UPnP, or it said no (UpnpError())
};

void UpnpOpenPort(int port);	// TCP port, to this computer; renewed while open
void UpnpClosePort(void);	// takes the forward away again (waits a moment for the router)
int GetUpnpState(void);
std::string UpnpExternalAddress(void);	// the router's internet address, when it says
std::string UpnpError(void);
