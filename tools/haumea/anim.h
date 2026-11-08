// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Skeletal animation for ped skins (actors, player classes, NPCs) in the
// viewport, using GTA's IFP libraries: PED (anim/ped.ifp) and the libraries
// in anim/anim.img. Library and animation names are the ones SA-MP's
// ApplyAnimation / ApplyActorAnimation / NPC_ApplyAnimation take.

#include <stddef.h>

struct ObjectInst;
namespace rw { struct Clump; struct Geometry; }

int AnimNumLibraries(void);
const char *AnimLibraryName(int lib);
int AnimFindLibrary(const char *name);	// -1 when unknown; case-insensitive
// Loads the library on first use.
int AnimNumInLibrary(int lib);
const char *AnimName(int lib, int index);
int AnimFindInLibrary(int lib, const char *name);
float AnimDuration(int lib, int index);	// seconds
// Animations a ped skin (actor, NPC, player) can't use: some move only an
// object's parts, so a skin stands still; others twist the skin out of shape.
enum { ANIM_PED_OK, ANIM_PED_OBJECT_ONLY, ANIM_PED_DEFORMS };
int AnimPedProblem(const char *lib, const char *name);
// The client's animation ID (GetAnimationName, on-foot sync data and .rec
// files) as library and name; false for 0 and unknown IDs.
bool AnimFromSampId(int id, char *lib, size_t libSize, char *name, size_t nameSize);

// Plays an animation on a skin instance, with ApplyActorAnimation's options:
// delta (kept for the script; the editor plays at normal speed), loop, lockX
// and lockY (let root motion move the skin instead of keeping it in place)
// and freeze (hold the last frame when it ends). Not looping animations
// replay after a short pause so the pose stays visible while editing.
// False when the instance is not a skin or the animation is unknown.
// timeMs is ApplyAnimation's time: after that long the animation stops (or
// holds its pose with freeze); 0 plays it for ever (or once when not looping).
bool SetInstanceAnimation(ObjectInst *inst, const char *lib, const char *name,
	float delta, bool loop, bool lockX, bool lockY, bool freeze, int timeMs = 0);
void ClearInstanceAnimation(ObjectInst *inst);
bool InstanceHasAnimation(const ObjectInst *inst);

// What an instance plays, to put it back later (NPC movement preview).
struct AnimPlayback
{
	char lib[32];
	char name[32];
	float delta;
	bool loop, lockX, lockY, freeze;
	int timeMs;
};
bool GetInstanceAnimation(const ObjectInst *inst, AnimPlayback *out);	// false: none

// Once per frame, before the scene is rendered: advances and poses every
// animated instance (time is paused while gPlayAnimations is off).
void AnimUpdate(float timeStep);

// objectdef.cpp hands each skin's geometry here before it stands it upright,
// so animation can skin from the original bind pose.
void AnimNoteSkinBindPose(rw::Clump *clump);
