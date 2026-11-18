// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Build replay: the map's own elements appear one at a time, in the order
// they were loaded (the .pwn's order, then anything added since), while the
// camera circles the map. It only changes what the 3D view draws: the map,
// its saving and its editing see every element as usual.

struct ObjectInst;

void BuildReplayStart(void);
void BuildReplayStop(void);	// shows everything and puts the camera back
bool BuildReplayActive(void);

// Once per frame after the camera's own input: the playhead and the orbit.
void BuildReplayUpdate(void);
// For the renderer: an element the replay hasn't reached yet.
bool BuildReplayHides(const ObjectInst *inst);
// The playback controls, inside the 3D view's overlay pill.
void BuildReplayControls(void);
