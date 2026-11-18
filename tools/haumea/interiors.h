// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The Interiors panel: the game's interiors (from its IPL entry/exit
// markers) with ID, name and spawn point. Clicking one moves the camera there
// and shows that interior; right-click copies SetPlayerInterior/SetPlayerPos.
void InteriorsWindow(bool *open);

#include <string>
#include <vector>
// (rw::V3d: include haumea.h first.)
// The same list for Namaka: the interiors whose id, name or GXT key contain
// filter (all for ""), as indexes into the list.
struct GameInteriorInfo {
	int index, id;
	std::string key, name;
	rw::V3d pos;
	float angle;
};
std::vector<GameInteriorInfo> FindGameInteriors(const char *filter);
// Like a click in the panel: the camera behind its spawn point, the interior shown.
bool GoToGameInterior(int index);
// What right-click copies: SetPlayerInterior, SetPlayerPos, SetPlayerFacingAngle.
std::string GameInteriorPawn(int index);
