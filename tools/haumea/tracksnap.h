// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// SA-MP's tubes (with their poles, brackets, lights and covers) and stunt
// roads, with their lights: a piece moved near another one joins it.
bool IsTrackPiece(int model);
// Where a piece of objectId, at pos with SA-MP rotation rot (degrees), joins
// a track piece of the map within a few metres, or false. moving: the piece
// itself when it is in the map (it isn't a target), or nil. With a ray (a
// normalized direction), a place near the mouse ray counts as near too.
bool FindTrackSnap(int objectId, const ObjectInst *moving, rw::V3d pos, rw::V3d rot, rw::V3d *outPos, rw::V3d *outRot,
	const rw::V3d *rayOrigin = nullptr, const rw::V3d *rayDir = nullptr);
