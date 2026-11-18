// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "icons.h"
#include "buildreplay.h"

#include <math.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

// Each element in the replay keeps its place in m_replayRank (1 for the
// first, 0 for one that isn't in it), so the renderer's test is a compare
// and nothing here holds a pointer an undo or a new map could free.

namespace {

// At 1x a replay lasts about this long, however big the map.
const float BASE_SECONDS = 20.0f;
const float MIN_RATE = 2.0f;	// elements a second at 1x, for small maps
const float ORBIT_SECONDS = 45.0f;	// one turn around the map
const float ORBIT_ELEVATION = 0.55f;	// radians above the horizon

const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
const char *speedLabels[] = { "0.25x", "0.5x", "1x", "2x", "4x", "8x" };
const int NUM_SPEEDS = sizeof(speeds)/sizeof(speeds[0]);

bool active, playing, orbiting;
int count;
float playhead;	// elements shown; the fraction is the next one's progress
int speedIndex = 2;

rw::V3d centre;
float radius;
rw::V3d orbitPosition;	// where the orbit last put the camera

struct SavedCamera
{
	rw::V3d position, target, up, localup;
} saved;

float
rate(void)
{
	float r = count / BASE_SECONDS;
	if(r < MIN_RATE) r = MIN_RATE;
	return r * speeds[speedIndex];
}

// The camera on the orbit at its current heading: centred on the map, far
// enough back for all of it to fit the view.
void
placeOrbitCamera(void)
{
	float fov = TheCamera.m_fov * 3.14159265f / 180.0f;
	float dist = radius / sinf(fov * 0.5f) * 1.1f;
	if(dist < 25.0f) dist = 25.0f;
	rw::V3d dir = rw::sub(TheCamera.m_position, TheCamera.m_target);
	float heading = atan2f(dir.y, dir.x);
	float c = cosf(ORBIT_ELEVATION);
	rw::V3d offset = { cosf(heading)*c*dist, sinf(heading)*c*dist, sinf(ORBIT_ELEVATION)*dist };
	TheCamera.m_target = centre;
	TheCamera.m_position = rw::add(centre, offset);
	TheCamera.orbit(0.0f, 0.0f);	// sets the up vectors for the new view
	orbitPosition = TheCamera.m_position;
}

}

void
BuildReplayStart(void)
{
	if(active)
		BuildReplayStop();

	std::vector<ObjectInst*> order;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		inst->m_replayRank = 0;
		if(!inst->m_isDeleted && IsUserMapObject(inst))
			order.push_back(inst);
	}
	if(order.empty())
		return;
	// Ids are handed out as elements are made: the file's order on load,
	// then everything placed, pasted or cloned since.
	std::sort(order.begin(), order.end(),
		[](const ObjectInst *a, const ObjectInst *b){ return a->m_id < b->m_id; });

	// Framed on where most of the map is: a map often has a stray element
	// across the state (a spawn, a teleport's far end), and a box around
	// everything would leave the build a speck.
	std::vector<float> xs, ys, zs;
	for(size_t i = 0; i < order.size(); i++){
		ObjectInst *inst = order[i];
		inst->m_replayRank = (int)i + 1;
		xs.push_back(inst->m_translation.x);
		ys.push_back(inst->m_translation.y);
		zs.push_back(inst->m_translation.z);
	}
	auto median = [](std::vector<float> &v){
		std::nth_element(v.begin(), v.begin() + v.size()/2, v.end());
		return v[v.size()/2];
	};
	centre = { median(xs), median(ys), median(zs) };
	std::vector<float> dists;
	for(ObjectInst *inst : order)
		dists.push_back(rw::length(rw::sub(inst->m_translation, centre)));
	std::sort(dists.begin(), dists.end());
	radius = dists[(size_t)((dists.size() - 1) * 0.9f)] + 10.0f;	// positions are centres

	saved.position = TheCamera.m_position;
	saved.target = TheCamera.m_target;
	saved.up = TheCamera.m_up;
	saved.localup = TheCamera.m_localup;

	count = (int)order.size();
	playhead = 0.0f;
	active = playing = orbiting = true;
	placeOrbitCamera();
}

void
BuildReplayStop(void)
{
	if(!active)
		return;
	for(CPtrNode *p = instances.first; p; p = p->next)
		((ObjectInst*)p->item)->m_replayRank = 0;
	active = playing = orbiting = false;
	TheCamera.m_position = saved.position;
	TheCamera.m_target = saved.target;
	TheCamera.m_up = saved.up;
	TheCamera.m_localup = saved.localup;
}

bool
BuildReplayActive(void)
{
	return active;
}

bool
BuildReplayHides(const ObjectInst *inst)
{
	return active && inst->m_replayRank > (int)playhead;
}

void
BuildReplayUpdate(void)
{
	if(!active)
		return;
	float dt = ImGui::GetIO().DeltaTime;
	if(playing){
		playhead += rate() * dt;
		if(playhead >= count){
			playhead = (float)count;
			playing = false;
		}
	}
	if(orbiting){
		// Flying or looking around hands the camera back to the user.
		if(rw::length(rw::sub(TheCamera.m_position, orbitPosition)) > 0.01f){
			orbiting = false;
			return;
		}
		TheCamera.orbit(2.0f*3.14159265f / ORBIT_SECONDS * dt, 0.0f);
		orbitPosition = TheCamera.m_position;
	}
}

void
BuildReplayControls(void)
{
	if(!active)
		return;
	float button = ImGui::GetFrameHeight();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled(ICON_FA_VIDEO);
	ImGui::SameLine();

	bool finished = !playing && (int)playhead >= count;
	if(ImGui::Button(playing ? ICON_FA_PAUSE "##replay_play" : ICON_FA_PLAY "##replay_play", ImVec2(button, 0))){
		if(finished)
			playhead = 0.0f;
		playing = !playing;
	}
	ImGui::SetItemTooltip("%s", playing ? _("Pause") : _("Play"));
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_ROTATE_LEFT "##replay_restart", ImVec2(button, 0))){
		playhead = 0.0f;
		playing = true;
	}
	ImGui::SetItemTooltip("%s", _("Restart"));
	ImGui::SameLine();

	int shown = (int)playhead;
	char format[32];
	snprintf(format, sizeof(format), "%%d / %d", count);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
	if(ImGui::SliderInt("##replay_playhead", &shown, 0, count, format, ImGuiSliderFlags_AlwaysClamp)){
		playhead = (float)shown;
		playing = false;
	}
	ImGui::SetItemTooltip("%s", _("Elements built so far. Drag to scrub."));
	ImGui::SameLine();

	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
	if(ImGui::BeginCombo("##replay_speed", speedLabels[speedIndex], ImGuiComboFlags_HeightLarge)){
		for(int i = 0; i < NUM_SPEEDS; i++)
			if(ImGui::Selectable(speedLabels[i], i == speedIndex))
				speedIndex = i;
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("%s", _("Speed"));
	ImGui::SameLine();

	bool wasOrbiting = orbiting;
	if(wasOrbiting)
		ImGui::PushStyleColor(ImGuiCol_Button, UiAccentFill());
	if(ImGui::Button(ICON_FA_ROTATE "##replay_orbit", ImVec2(button, 0))){
		orbiting = !orbiting;
		if(orbiting)
			placeOrbitCamera();
	}
	if(wasOrbiting)
		ImGui::PopStyleColor();
	ImGui::SetItemTooltip("%s", _("Circle the map. Moving the camera stops it."));
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_XMARK "##replay_close", ImVec2(button, 0)))
		BuildReplayStop();
	ImGui::SetItemTooltip("%s", _("Close"));
}
