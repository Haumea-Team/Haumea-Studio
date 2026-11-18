// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "tracksnap.h"
#include "track_data.h"

#include <math.h>
#include <vector>

// Track pieces moved near another one go where the two join, exactly: SA-MP's
// tubes by the table of the tool TBR (one place for every pair of the ways
// two tubes join, after or before the other), their poles, brackets, lights
// and covers on a tube, and the stunt roads (MRoad) end to end, with their
// lights on the sides. The places come from track_data.h.
//
// Rotations are SA-MP's Euler angles in degrees: R = Rz * Rx * Ry, the same
// as MakeObjectRotationDegrees.

namespace {

const float D2R = 3.14159265358979323846f / 180.0f;
const int BENT_TUBE = 19689;	// TubeHalf45Bend4: the tool turns its angles first
const int TUBE_POLE = 19670, TUBE_BRACKET = 19671, TUBE_LIGHT = 19672;
const float MAX_DISTANCE = 8.0f;	// from where it is to where it joins
const float NEAR_PIECES = 160.0f;	// the pieces looked at

struct Mat { float m[3][3]; };
struct Pose { Mat r; rw::V3d t; };

Mat
mul(const Mat &a, const Mat &b)
{
	Mat c;
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++)
			c.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] + a.m[i][2]*b.m[2][j];
	return c;
}

rw::V3d
apply(const Mat &a, rw::V3d v)
{
	return { a.m[0][0]*v.x + a.m[0][1]*v.y + a.m[0][2]*v.z,
		a.m[1][0]*v.x + a.m[1][1]*v.y + a.m[1][2]*v.z,
		a.m[2][0]*v.x + a.m[2][1]*v.y + a.m[2][2]*v.z };
}

Mat
transpose(const Mat &a)
{
	Mat t;
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++)
			t.m[i][j] = a.m[j][i];
	return t;
}

Mat
fromEuler(rw::V3d deg)
{
	float sx = sinf(deg.x*D2R), cx = cosf(deg.x*D2R);
	float sy = sinf(deg.y*D2R), cy = cosf(deg.y*D2R);
	float sz = sinf(deg.z*D2R), cz = cosf(deg.z*D2R);
	Mat r = {{
		{ cz*cy - sz*sx*sy, -sz*cx, cz*sy + sz*sx*cy },
		{ sz*cy + cz*sx*sy, cz*cx, sz*sy - cz*sx*cy },
		{ -cx*sy, sx, cx*cy },
	}};
	return r;
}

rw::V3d
toEuler(const Mat &r)
{
	float x = asinf(fmaxf(-1.0f, fminf(1.0f, r.m[2][1]))), y, z;
	if(fabsf(r.m[2][1]) < 0.9999999f){
		y = atan2f(-r.m[2][0], r.m[2][2]);
		z = atan2f(-r.m[0][1], r.m[1][1]);
	}else{
		y = 0.0f;
		z = atan2f(r.m[1][0], r.m[0][0]);
	}
	return { x / D2R, y / D2R, z / D2R };
}

Pose
compose(const Pose &a, const Pose &b)
{
	return { mul(a.r, b.r), add(a.t, apply(a.r, b.t)) };
}

Pose
inverse(const Pose &a)
{
	Mat t = transpose(a.r);
	return { t, scale(apply(t, a.t), -1.0f) };
}

Pose
placePose(const TrackPlace &p)
{
	return { fromEuler({ p.rx, p.ry, p.rz }), { p.x, p.y, p.z } };
}

// The tool's turn of TubeHalf45Bend4's angles; it undoes itself.
rw::V3d
bentTurn(rw::V3d r)
{
	return { -r.x, -(45.0f + r.y), r.z - 180.0f };
}

// Radians between two rotations.
float
angleBetween(const Mat &a, const Mat &b)
{
	Mat d = mul(transpose(a), b);
	float c = (d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0f) * 0.5f;
	return acosf(fmaxf(-1.0f, fminf(1.0f, c)));
}

const TrackTube*
findTube(int model)
{
	for(const TrackTube &t : trackTubes)
		if(t.model == model)
			return &t;
	return nil;
}

const TrackRoad*
findRoad(int model)
{
	for(const TrackRoad &r : trackRoads)
		if(r.model == model)
			return &r;
	return nil;
}

bool
isRoadLight(int model)
{
	for(int m : trackRoadLightModels)
		if(m == model)
			return true;
	return false;
}

bool
isTubeExtra(int model)
{
	return model == TUBE_POLE || model == TUBE_BRACKET || model == TUBE_LIGHT;
}

// euler: its angles as they are, which TubeHalf45Bend4's turn needs.
struct Piece { int model; Pose pose; rw::V3d euler; };

struct Candidate { rw::V3d pos; rw::V3d rot; Mat r; };

void
addPose(const Pose &pose, int model, bool chain, std::vector<Candidate> &out)
{
	rw::V3d rot = toEuler(pose.r);
	if(chain && model == BENT_TUBE)
		rot = bentTurn(rot);
	out.push_back({ pose.t, rot, fromEuler(rot) });
}

void
tubeCandidates(int model, const Piece &q, std::vector<Candidate> &out)
{
	const TrackTube *tp = findTube(model), *tq = findTube(q.model);
	if(tq == nil)
		return;
	if(tp){
		Pose wq = q.pose;
		if(q.model == BENT_TUBE)
			wq.r = fromEuler(bentTurn(q.euler));	// the tool's frame
		for(int i = 0; i < tq->numKinds; i++)
			for(int j = 0; j < tp->numKinds; j++){
				int x = tq->kinds[i], y = tp->kinds[j];
				Pose after = placePose(trackTubeAfter[x][y]);
				addPose(compose(wq, after), model, true, out);	// after q
				Pose before = placePose(trackTubeAfter[y][x]);
				addPose(compose(wq, inverse(before)), model, true, out);	// before q
			}
	}
	// Poles, brackets, the light and covers go on a tube.
	int row = tq->row;
	if(row < 0)
		return;
	auto put = [&](bool has, const TrackPlace &at){
		if(has)
			addPose(compose(q.pose, placePose(at)), model, false, out);
	};
	if(model == TUBE_LIGHT) put(trackTubeLights[row].has, trackTubeLights[row].at);
	else if(model == TUBE_POLE) put(trackTubePoles[row].has, trackTubePoles[row].at);
	else if(model == TUBE_BRACKET){
		put(trackTubeBrackets1[row].has, trackTubeBrackets1[row].at);
		put(trackTubeBrackets2[row].has, trackTubeBrackets2[row].at);
	}else{
		// A cover is the same tube upside down on it; 19663 and 19664 cover each other.
		int cover = q.model == 19663 ? 19664 : q.model == 19664 ? 19663 : q.model;
		if(model == cover)
			put(trackTubeCovers[row].has, trackTubeCovers[row].at);
	}
}

void
roadCandidates(int model, const Piece &q, std::vector<Candidate> &out)
{
	const TrackRoad *rp = findRoad(model), *rq = findRoad(q.model);
	if(rp == nil || rq == nil)
		return;
	// End b of q meets end a of the moved piece, face to face.
	Pose flip = { fromEuler({ 0.0f, 0.0f, 180.0f }), { 0.0f, 0.0f, 0.0f } };
	for(const TrackPlace &b : rq->ends)
		for(const TrackPlace &a : rp->ends)
			addPose(compose(compose(compose(q.pose, placePose(b)), flip), inverse(placePose(a))), model, false, out);
}

void
lightCandidates(rw::V3d rot, const Piece &q, std::vector<Candidate> &out)
{
	for(const TrackRoadLight &l : trackRoadLights)
		if(l.model == q.model)
			out.push_back({ add(q.pose.t, apply(q.pose.r, rw::V3d{ l.x, l.y, l.z })), rot, fromEuler(rot) });
}

}	// namespace

bool
IsTrackPiece(int model)
{
	return findTube(model) || isTubeExtra(model) || findRoad(model) || isRoadLight(model);
}

// How far p is from the ray (only ahead of its origin).
static float
rayDistance(rw::V3d p, const rw::V3d *origin, const rw::V3d *dir)
{
	float t = dot(sub(p, *origin), *dir);
	if(t < 0.0f)
		return 1.0e30f;
	return length(sub(p, add(*origin, scale(*dir, t))));
}

bool
FindTrackSnap(int objectId, const ObjectInst *moving, rw::V3d pos, rw::V3d rot, rw::V3d *outPos, rw::V3d *outRot,
	const rw::V3d *rayOrigin, const rw::V3d *rayDir)
{
	bool ray = rayOrigin && rayDir;
	int model = PawnModelId(objectId);
	if(!IsTrackPiece(model))
		return false;
	bool light = isRoadLight(model);
	std::vector<Piece> nearby;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst == moving || inst->m_isDeleted || inst->m_isVehicle || IsPawnWorldObject(inst) ||
		   !IsPawnObjectEditable(inst))
			continue;
		int other = PawnModelId(inst->m_objectId);
		if(light ? findRoad(other) == nil : findTube(other) == nil && findRoad(other) == nil)
			continue;
		if(length(sub(inst->m_translation, pos)) > NEAR_PIECES &&
		   !(ray && rayDistance(inst->m_translation, rayOrigin, rayDir) < NEAR_PIECES))
			continue;
		rw::V3d euler = GetInstanceRotationDegrees(inst);
		nearby.push_back({ other, { fromEuler(euler), inst->m_translation }, euler });
	}
	if(nearby.empty())
		return false;
	Mat current = fromEuler(rot);
	std::vector<Candidate> list;
	float bestScore = 1.0e30f;
	bool found = false;
	for(const Piece &q : nearby){
		list.clear();
		if(light)
			lightCandidates(rot, q, list);
		else if(findRoad(model))
			roadCandidates(model, q, list);
		else
			tubeCandidates(model, q, list);
		for(const Candidate &c : list){
			float d = length(sub(c.pos, pos));
			if(ray)	// placing: the pointer passing near the place counts
				d = fminf(d, rayDistance(c.pos, rayOrigin, rayDir));
			if(d > MAX_DISTANCE)
				continue;
			// A quarter turn counts as 2.7 m.
			float score = d + angleBetween(current, c.r) / D2R * 0.03f;
			if(score >= bestScore)
				continue;
			// Not where the same piece already is.
			bool taken = false;
			for(const Piece &o : nearby)
				if(o.model == model && length(sub(o.pose.t, c.pos)) < 0.35f && angleBetween(o.pose.r, c.r) < 0.05f)
					taken = true;
			if(taken)
				continue;
			bestScore = score;
			*outPos = c.pos;
			*outRot = c.rot;
			found = true;
		}
	}
	return found;
}
