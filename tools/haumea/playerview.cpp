// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "playerview.h"
#include "buildreplay.h"
#include "areas.h"

#include <algorithm>
#include <vector>

bool gPlayerView;
int gPlayerViewWorld;

namespace {

// open.mp SDK values.hpp and the streamer's defaults.
const int CLIENT_OBJECTS = 1000;	// OBJECT_POOL_SIZE_037: what a client creates
const int MAX_STREAMED_VEHICLES = 700;
const int MAX_STREAMED_ACTORS = 50;

enum { PV_UNTOUCHED, PV_HIDDEN, PV_SHOWN };

PlayerViewStats stats;

struct Candidate
{
	ObjectInst *inst;
	float dist;
};

bool
byDistance(const Candidate &a, const Candidate &b)
{
	return a.dist < b.dist;
}

// Streams the nearest `limit` of the candidates in range; the rest are hidden.
int
streamNearest(std::vector<Candidate> &list, int limit)
{
	std::sort(list.begin(), list.end(), byDistance);
	for(size_t i = 0; i < list.size(); i++)
		if((int)i >= limit)
			list[i].inst->m_playerView = PV_HIDDEN;
	return min((int)list.size(), limit);
}

bool
playerInArea(const ObjectInst *area)
{
	const ObjectPawnProperties *props = GetObjectPawnProperties(area);
	if(props->virtualWorld >= 0 && props->virtualWorld != gPlayerViewWorld)
		return false;
	if(area->m_area > 0 && area->m_area != currentArea)
		return false;
	return IsPointInDynamicArea(area, TheCamera.m_position);
}

}	// namespace

void
PlayerViewUpdate(void)
{
	// Off: one pass to clear what the last frame set, then nothing.
	static bool wasOn;
	if(!gPlayerView && !wasOn)
		return;
	wasOn = gPlayerView;
	stats = PlayerViewStats();
	bool streamer = GetPawnMapFormat() == PAWN_CODE_STREAMER;
	stats.streamer = streamer;
	std::vector<Candidate> objects, vehicles, actors;
	std::vector<ObjectInst*> nativeObjects;

	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		inst->m_playerView = PV_UNTOUCHED;
		// An unchanged world building inside a model -1 removal is gone for
		// players (a changed one is recreated by the map, so it shows).
		if(gPlayerView && !inst->m_isDeleted && IsPawnWorldObject(inst) && !PawnObjectNeedsExport(inst) &&
		   InsidePawnAreaRemoval(inst)){
			inst->m_playerView = PV_HIDDEN;
			continue;
		}
		if(!gPlayerView || inst->m_isDeleted || !PawnObjectNeedsExport(inst))
			continue;
		const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
		int kind = GetSampElementKind(inst);
		bool nativeObject = !streamer && kind == SAMP_ELEMENT_OBJECT && !inst->m_isVehicle;

		// Virtual world and interior. Native objects have neither; native
		// elements have a world but no interior, except vehicles.
		if(!nativeObject){
			bool world = props->virtualWorld < 0 || props->virtualWorld == gPlayerViewWorld;
			bool usesInterior = streamer || inst->m_isVehicle;
			bool interior = !usesInterior || inst->m_area <= 0 || inst->m_area == currentArea;
			if(!world || !interior){
				inst->m_playerView = PV_HIDDEN;
				stats.otherWorld++;
				continue;
			}
		}

		// Areas are never seen; an item limited to one streams only while the
		// player is in it (by its shape, world and interior). One whose area
		// is missing is written without an areaid, so it shows everywhere.
		if(kind == SAMP_ELEMENT_AREA){
			inst->m_playerView = PV_HIDDEN;
			continue;
		}
		if(streamer && props->areaName[0]){
			ObjectInst *area = FindAreaByName(props->areaName);
			if(area && !playerInArea(area)){
				inst->m_playerView = PV_HIDDEN;
				stats.outsideArea++;
				continue;
			}
		}

		float dist = TheCamera.distanceTo(inst->m_translation);
		if(dist >= gStreamDistance){
			inst->m_playerView = PV_HIDDEN;
			continue;
		}
		inst->m_playerView = PV_SHOWN;
		if(inst->m_isVehicle)
			vehicles.push_back({ inst, dist });
		else if(kind == SAMP_ELEMENT_ACTOR)
			actors.push_back({ inst, dist });
		else if(kind == SAMP_ELEMENT_OBJECT){
			if(nativeObject) nativeObjects.push_back(inst);
			else objects.push_back({ inst, dist });
		}
	}
	if(!gPlayerView)
		return;

	// Native objects past the pool are never created; the client has the rest.
	if(!streamer){
		std::sort(nativeObjects.begin(), nativeObjects.end(),
			[](const ObjectInst *a, const ObjectInst *b){ return a->m_id < b->m_id; });
		for(size_t i = 0; i < nativeObjects.size(); i++){
			if((int)i >= CLIENT_OBJECTS)
				nativeObjects[i]->m_playerView = PV_HIDDEN;
			else
				objects.push_back({ nativeObjects[i], TheCamera.distanceTo(nativeObjects[i]->m_translation) });
		}
		stats.objectsInRange = (int)objects.size();
		stats.objectsStreamed = (int)objects.size();
	}else{
		stats.objectsInRange = (int)objects.size();
		stats.objectsStreamed = streamNearest(objects, CLIENT_OBJECTS);
	}
	stats.vehiclesStreamed = streamNearest(vehicles, MAX_STREAMED_VEHICLES);
	stats.actorsStreamed = streamNearest(actors, MAX_STREAMED_ACTORS);

	// Preview range is uniform; exported stream/draw properties stay unchanged.
	for(const Candidate &c : objects){
		if(c.inst->m_playerView != PV_SHOWN)
			continue;
		stats.objectsVisible++;
	}
}

bool
PlayerViewHides(const ObjectInst *inst)
{
	if(BuildReplayHides(inst))
		return true;
	return gPlayerView && inst->m_playerView == PV_HIDDEN && !inst->m_selected;
}

bool
PlayerViewShows(const ObjectInst *inst)
{
	return gPlayerView && inst->m_playerView == PV_SHOWN;
}

const PlayerViewStats&
GetPlayerViewStats(void)
{
	return stats;
}
