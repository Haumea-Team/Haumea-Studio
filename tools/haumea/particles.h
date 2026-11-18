// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Include after haumea.h (rw::V3d).
struct ObjectInst;
namespace ParticlePreview
{
extern bool enabled;
struct Stats {
	int systems = 0, live = 0, drawn = 0, missing = 0, definitions = 0;
	bool limited = false;
};
void Render(float dt);
bool HasParticleEffect(const ObjectInst *inst);
void Restart(const ObjectInst *inst);
const Stats &GetStats();
// Current emitter, live particle or drawn sprite count for one Pawn model ID.
int CountForModel(int model, int kind);
// A model's particle effects in a preview: runs effects' particle systems
// for a moment at the model's origin (where they reach: centre and radius),
// then draws them for a camera at eye looking at target, after the model.
bool PreviewModelParticles(int objectId, const std::vector<Effect> &effects, rw::V3d *centre, float *radius);
void RenderModelParticles(int objectId, rw::V3d eye, rw::V3d target, rw::V3d up);
} // namespace ParticlePreview
