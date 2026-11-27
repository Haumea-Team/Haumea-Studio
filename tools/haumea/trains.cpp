#include "haumea.h"
#include "trains.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

// See trains.h for where each rule comes from.

namespace {

struct TrackNode
{
	float x, y, z;	// as the game keeps them: int16 eighths of a metre
	float rail;	// distance from the first node: uint16 thirds of a metre
};

struct Track
{
	std::vector<TrackNode> nodes;
	float total;	// the loop's length, back to the first node
};

// The order InitTrains loads them in, which is the track id. Under SA-MP a
// train only ever sticks to tracks.dat or tracks3.dat (the user's finding in
// game; the patch isn't in reSA-MP's source yet), so tracks2.dat and
// tracks4.dat (ids 2 and 3) stay empty.
const char *trackFiles[] = {
	"data/Paths/tracks.dat",
	"data/Paths/tracks3.dat",
	nil,	// tracks2.dat
	nil,	// tracks4.dat
};
Track tracks[4];
bool tracksLoaded;

struct CarShape
{
	bool known;
	float length;	// the bounding box along y
	float minZ;
	float suspensionLowerLimit;
};
std::unordered_map<int, CarShape> carShapes;
std::unordered_map<std::string, float> lowerLimits;	// handling.cfg, by handling id
std::unordered_map<int, std::string> handlingIds;	// vehicles.ide, by model

// ftol: the x87 conversion GTA uses, truncating towards zero.
int
truncate(float v)
{
	return (int)v;
}

void
loadTrack(int id)
{
	Track &track = tracks[id];
	track.nodes.clear();
	track.total = 0.0f;
	if(trackFiles[id] == nil)
		return;
	FILE *f = fopen_ci(trackFiles[id], "r");
	if(f == nil)
		return;
	char line[256];
	int count = 0;
	if(fgets(line, sizeof(line), f))
		count = atoi(line);
	while((int)track.nodes.size() < count && fgets(line, sizeof(line), f)){
		float x, y, z;
		if(sscanf(line, "%f %f %f", &x, &y, &z) != 3)
			continue;
		TrackNode node;
		node.x = (int16)truncate(x * 8.0f) * 0.125f;
		node.y = (int16)truncate(y * 8.0f) * 0.125f;
		node.z = (int16)truncate(z * 8.0f) * 0.125f;
		node.rail = 0.0f;
		track.nodes.push_back(node);
	}
	fclose(f);
	// The distance is the flat (x, y) length of the segments, each node
	// keeping it as a uint16 of thirds of a metre; the last segment closes the loop.
	float distance = 0.0f;
	int n = (int)track.nodes.size();
	for(int i = 0; i < n; i++){
		track.nodes[i].rail = (uint16)truncate(distance * 3.0f) / 3.0f;
		const TrackNode &a = track.nodes[i], &b = track.nodes[(i + 1) % n];
		distance += sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
	}
	track.total = distance;
}

void
loadHandling(void)
{
	lowerLimits.clear();
	handlingIds.clear();
	char line[1024];
	if(FILE *f = fopen_ci("data/vehicles.ide", "r")){
		while(fgets(line, sizeof(line), f)){
			int model;
			char id[64];
			// id, dff, txd, type, handling id, ...
			if(sscanf(line, "%d, %*[^,], %*[^,], %*[^,], %63[^, \t]", &model, id) == 2 && IsTrainModel(model))
				handlingIds[model] = id;
		}
		fclose(f);
	}
	if(FILE *f = fopen_ci("data/handling.cfg", "r")){
		while(fgets(line, sizeof(line), f)){
			if(line[0] == ';' || line[0] == '!' || line[0] == '$' || line[0] == '%' || line[0] == '^')
				continue;
			char id[64];
			float v[23];
			// B..P (14 numbers), the drive and engine letters, then S..e:
			// the suspension lower limit (e) is the last of those.
			int got = sscanf(line, "%63s %f %f %f %f %f %f %f %f %f %f %f %f %f %f %*s %*s %f %f %f %f %f %f %f %f %f",
				id, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12],
				&v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20], &v[21], &v[22]);
			if(got == 24)
				lowerLimits[id] = v[22];
		}
		fclose(f);
	}
}

// The car's bounding box: its collision's, as GTA uses, or else its model's.
const CarShape&
carShape(int model)
{
	auto found = carShapes.find(model);
	if(found != carShapes.end() && found->second.known)
		return found->second;
	CarShape &shape = carShapes[model];
	shape = CarShape{};
	ObjectDef *obj = GetObjectDef(model);
	if(obj == nil)
		return shape;
	if(!obj->IsLoaded()){
		RequestObject(model);
		LoadAllRequestedObjects();
	}
	auto it = handlingIds.find(model);
	if(it != handlingIds.end()){
		auto limit = lowerLimits.find(it->second);
		if(limit != lowerLimits.end())
			shape.suspensionLowerLimit = limit->second;
	}
	if(obj->m_colModel){
		const CBox &box = obj->m_colModel->boundingBox;
		shape.length = box.max.y - box.min.y;
		shape.minZ = box.min.z;
		shape.known = true;
	}else if(obj->m_type == ObjectDef::CLUMP && obj->m_clump){
		bool any = false;
		float minY = 0.0f, maxY = 0.0f, minZ = 0.0f;
		FORLIST(lnk, obj->m_clump->atomics){
			rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
			rw::Geometry *g = atomic ? atomic->geometry : nil;
			if(g == nil || g->numMorphTargets == 0)
				continue;
			rw::V3d *v = g->morphTargets[0].vertices;
			for(int i = 0; i < g->numVertices; i++){
				if(!any){
					minY = maxY = v[i].y;
					minZ = v[i].z;
					any = true;
				}
				minY = fminf(minY, v[i].y);
				maxY = fmaxf(maxY, v[i].y);
				minZ = fminf(minZ, v[i].z);
			}
		}
		shape.length = maxY - minY;
		shape.minZ = minZ;
		shape.known = any;
	}
	return shape;
}

// CTrain::FindClosestTrackNode: the nearest node (3D) on any track.
int
closestNode(rw::V3d p, int *trackId)
{
	float best = 99999.9f;
	int node = 0;
	*trackId = -1;
	for(int t = 0; t < 4; t++)
		for(int i = 0; i < (int)tracks[t].nodes.size(); i++){
			const TrackNode &n = tracks[t].nodes[i];
			float d = sqrtf((p.x - n.x) * (p.x - n.x) + (p.y - n.y) * (p.y - n.y) + (p.z - n.z) * (p.z - n.z));
			if(d < best){
				best = d;
				node = i;
				*trackId = t;
			}
		}
	return node;
}

// The point on the track rail metres from its first node.
rw::V3d
pointOnTrack(const Track &track, float rail, int *nodeHint)
{
	int n = (int)track.nodes.size();
	int i = nodeHint ? *nodeHint : 0;
	for(int tries = 0; tries <= n; tries++){
		float from = track.nodes[i].rail;
		float to = i + 1 < n ? track.nodes[i + 1].rail : track.total;
		if(from <= rail && rail <= to)
			break;
		i = (i + 1) % n;
	}
	if(nodeHint)
		*nodeHint = i;
	const TrackNode &a = track.nodes[i], &b = track.nodes[(i + 1) % n];
	float from = a.rail;
	float to = i + 1 < n ? b.rail : track.total;
	float span = to - from;
	if(span < 0.0f)
		span += track.total;
	float f = span > 0.0f ? (rail - from) / span : 0.0f;
	return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
}

float
wrapRail(const Track &track, float rail)
{
	while(rail < 0.0f) rail += track.total;
	while(rail >= track.total) rail -= track.total;
	return rail;
}

} // namespace

bool
IsTrainEngineModel(int model)
{
	return model == 537 || model == 538 || model == 449;
}

bool
IsTrainCarriageModel(int model)
{
	return model == 569 || model == 570 || model == 590;
}

bool
IsTrainModel(int model)
{
	return IsTrainEngineModel(model) || IsTrainCarriageModel(model);
}

int
GetTrainCarModels(int engineModel, int *models)
{
	// CTrain::CreateMissionTrain's types 5, 3 and 9 (the table at 0x8D44F8).
	int carriage = engineModel == 538 ? 570 : engineModel == 537 ? 569 : -1;
	if(!IsTrainEngineModel(engineModel))
		return 0;
	models[0] = engineModel;
	if(carriage < 0)
		return 1;
	for(int i = 1; i < TRAIN_MAX_CARS; i++)
		models[i] = carriage;
	return TRAIN_MAX_CARS;
}

int
GetTrainVehicleIds(int engineModel)
{
	int models[TRAIN_MAX_CARS];
	return GetTrainCarModels(engineModel, models);
}

void
LoadTrainTracks(void)
{
	for(int t = 0; t < 4; t++)
		loadTrack(t);
	loadHandling();
	carShapes.clear();
	tracksLoaded = true;
	log("Trains: tracks.dat %d nodes, tracks3.dat %d nodes\n",
		(int)tracks[0].nodes.size(), (int)tracks[1].nodes.size());
}

int
GetNumTrainTracks(void)
{
	int n = 0;
	for(int t = 0; t < 4; t++)
		if(tracks[t].nodes.size() >= 2)
			n++;
	return n;
}

bool
TrainAngleIsClockwise(float angle)
{
	return angle > 180.0f;	// ConstructVehicleSlot: rotation > 180.0f
}

static float
normalizeDegrees(float angle)
{
	float a = fmodf(angle, 360.0f);
	if(a < 0.0f)
		a += 360.0f;
	return a;
}

float
TrainImportAngle(float angle)
{
	// Haumea keeps angles as rotations; written back they come out in
	// [0, 360). A train's angle only says which way it runs, so keep that:
	// -90 (not over 180) becomes 90, not 270.
	float a = normalizeDegrees(angle);
	if(TrainAngleIsClockwise(angle) != TrainAngleIsClockwise(a))
		a = TrainAngleIsClockwise(angle) ? a + 180.0f : a - 180.0f;
	return a;
}

float
TrainExportAngle(float angle)
{
	float a = normalizeDegrees(angle);
	return a == 0.0f ? 0.0f : a;
}

float
GetTrainInstAngle(const ObjectInst *inst)
{
	return TrainExportAngle(GetObjectRotationDegrees(inst->m_rotation).z);
}

int
ComputeTrainCars(int engineModel, rw::V3d position, float angle, TrainCar *cars, float *offTrack)
{
	int models[TRAIN_MAX_CARS];
	int numCars = GetTrainCarModels(engineModel, models);
	if(numCars == 0 || !tracksLoaded)
		return 0;
	int trackId;
	int node = closestNode(position, &trackId);
	if(trackId < 0)
		return 0;
	const Track &track = tracks[trackId];
	int n = (int)track.nodes.size();
	bool clockwise = TrainAngleIsClockwise(angle);

	// CreateMissionTrain starts the engine at the nearest node, then (a
	// mission train) FindPositionOnTrackFromCoors moves it to the first
	// segment within 3 m (flat) whose span covers the point, centred on it.
	float rail = track.nodes[node].rail;
	for(int i = 0; i < n; i++){
		const TrackNode &a = track.nodes[i], &b = track.nodes[(i + 1) % n];
		float dx = b.x - a.x, dy = b.y - a.y;
		float len2 = dx * dx + dy * dy;
		if(len2 <= 0.0f)
			continue;
		float t = ((position.x - a.x) * dx + (position.y - a.y) * dy) / len2;
		if(!(t > 0.001f) || !(t < 1.001f))
			continue;
		float qx = a.x + dx * t - position.x, qy = a.y + dy * t - position.y;
		if(sqrtf(qx * qx + qy * qy) < 3.0f){
			// Node i+1's distance, the first node's 0 on the closing segment, as in the game.
			rail = a.rail + t * (b.rail - a.rail) - carShape(models[0]).length * 0.5f;
			if(rail < 0.0f)
				rail += track.total;
			break;
		}
	}

	// Each carriage follows the one before (ProcessControl: its rail is the
	// previous car's plus m_fLength): back by its own length when clockwise,
	// else on by the previous car's.
	float rails[TRAIN_MAX_CARS];
	rails[0] = rail;
	for(int c = 1; c < numCars; c++){
		float step = clockwise ? -carShape(models[c]).length : carShape(models[c - 1]).length;
		rails[c] = wrapRail(track, rails[c - 1] + step);
	}

	// ProcessControl: the car stands between the track points at its rail
	// and its length further on, raised by its suspension, facing along the
	// track, the other way unless clockwise.
	int hint = 0;
	for(int c = 0; c < numCars; c++){
		const CarShape &shape = carShape(models[c]);
		rw::V3d p1 = pointOnTrack(track, wrapRail(track, rails[c]), &hint);
		int hint2 = hint;
		float farRail = rails[c] + shape.length;
		if(farRail > track.total)
			farRail -= track.total;
		rw::V3d p2 = pointOnTrack(track, farRail, &hint2);
		TrainCar &car = cars[c];
		car.model = models[c];
		car.position = rw::scale(rw::add(p1, p2), 0.5f);
		car.position.z += shape.suspensionLowerLimit - shape.minZ;
		rw::V3d forward = rw::sub(p2, p1);
		if(rw::length(forward) < 0.0001f)
			forward = { 0.0f, 1.0f, 0.0f };
		forward = rw::normalize(forward);
		car.forward = clockwise ? forward : rw::scale(forward, -1.0f);
	}
	if(offTrack){
		float dx = cars[0].position.x - position.x, dy = cars[0].position.y - position.y;
		*offTrack = sqrtf(dx * dx + dy * dy);
	}
	return numCars;
}

bool
SnapTrainToTrack(int engineModel, rw::V3d position, float angle, rw::V3d *snapped)
{
	TrainCar cars[TRAIN_MAX_CARS];
	if(ComputeTrainCars(engineModel, position, angle, cars) == 0)
		return false;
	*snapped = cars[0].position;
	return true;
}

void
TrainCarMatrix(const TrainCar &car, rw::Matrix *matrix)
{
	rw::V3d up = { 0.0f, 0.0f, 1.0f };
	rw::V3d forward = car.forward;
	rw::V3d right = rw::cross(forward, up);
	if(rw::length(right) < 0.0001f)
		right = { 1.0f, 0.0f, 0.0f };
	right = rw::normalize(right);
	matrix->setIdentity();
	matrix->right = right;
	matrix->up = forward;	// GTA's vehicles face +y
	matrix->at = rw::normalize(rw::cross(right, forward));
	matrix->pos = car.position;
	matrix->flags = rw::Matrix::TYPEORTHONORMAL;
}

// --- Drawing ---

namespace {

struct TrainVisual
{
	int engineModel;
	rw::V3d position;
	float angle;
	int color1, color2;
	int numCars;
	TrainCar cars[TRAIN_MAX_CARS];
	ObjectInst *carriages[TRAIN_MAX_CARS - 1];
};
std::unordered_map<const ObjectInst*, TrainVisual> visuals;

void
destroyCarriage(ObjectInst *car)
{
	if(car == nil)
		return;
	if(car->m_rwObject)
		((rw::Clump*)car->m_rwObject)->destroy();
	car->m_rwObject = nil;
	delete car;
}

// A carriage drawn with the engine: never in the map's instance list, so
// it isn't saved, listed or picked by itself; it picks as the engine.
ObjectInst*
makeCarriage(const ObjectInst *engine, int model)
{
	ObjectDef *obj = GetObjectDef(model);
	if(obj == nil || obj->m_type != ObjectDef::CLUMP)
		return nil;
	if(!obj->IsLoaded()){
		RequestObject(model);
		LoadAllRequestedObjects();
	}
	ObjectInst *car = new ObjectInst;
	memset(car, 0, sizeof(ObjectInst));
	car->m_id = engine->m_id;
	car->m_objectId = model;
	car->m_isVehicle = true;
	car->m_vehicleColor1 = engine->m_vehicleColor1;
	car->m_vehicleColor2 = engine->m_vehicleColor2;
	car->m_imageIndex = -1;
	car->m_binInstIndex = -1;
	car->m_lodId = -1;
	car->m_rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
	car->m_matrix.setIdentity();
	car->CreateRwObject();
	if(car->m_rwObject == nil){
		delete car;
		return nil;
	}
	return car;
}

void
placeFrame(void *rwObject, const rw::Matrix &matrix)
{
	if(rwObject == nil)
		return;
	rw::Frame *f = ((rw::Clump*)rwObject)->getFrame();
	if(f)
		f->transform(&matrix, rw::COMBINEREPLACE);
}

} // namespace

// Called by RenderInst for an engine: puts it on the track and returns the
// carriages to draw after it.
int
UpdateTrainVisual(ObjectInst *engine, ObjectInst **carriages)
{
	TrainVisual &v = visuals[engine];
	float angle = GetTrainInstAngle(engine);
	bool stale = v.engineModel != engine->m_objectId ||
		rw::length(rw::sub(v.position, engine->m_translation)) > 0.0001f ||
		TrainAngleIsClockwise(v.angle) != TrainAngleIsClockwise(angle) || v.numCars == 0;
	if(stale){
		if(v.engineModel != engine->m_objectId){
			for(int i = 0; i < TRAIN_MAX_CARS - 1; i++){
				destroyCarriage(v.carriages[i]);
				v.carriages[i] = nil;
			}
		}
		v.engineModel = engine->m_objectId;
		v.position = engine->m_translation;
		v.angle = angle;
		v.numCars = ComputeTrainCars(engine->m_objectId, engine->m_translation, angle, v.cars);
	}
	if(v.numCars == 0)
		return 0;	// no tracks: drawn where it is, alone
	rw::Matrix m;
	TrainCarMatrix(v.cars[0], &m);
	placeFrame(engine->m_rwObject, m);
	int n = 0;
	for(int c = 1; c < v.numCars; c++){
		ObjectInst *&car = v.carriages[c - 1];
		if(car == nil)
			car = makeCarriage(engine, v.cars[c].model);
		if(car == nil)
			continue;
		if(car->m_vehicleColor1 != engine->m_vehicleColor1 || car->m_vehicleColor2 != engine->m_vehicleColor2){
			car->m_vehicleColor1 = engine->m_vehicleColor1;
			car->m_vehicleColor2 = engine->m_vehicleColor2;
			car->RefreshVehicleColors();
		}
		car->m_id = engine->m_id;
		car->m_selected = engine->m_selected;
		car->m_highlight = engine->m_highlight;
		MarkInstTouched(car);
		TrainCarMatrix(v.cars[c], &car->m_matrix);
		placeFrame(car->m_rwObject, car->m_matrix);
		carriages[n++] = car;
	}
	return n;
}

void
ReleaseTrainCars(ObjectInst *engine)
{
	auto it = visuals.find(engine);
	if(it == visuals.end())
		return;
	for(int i = 0; i < TRAIN_MAX_CARS - 1; i++)
		destroyCarriage(it->second.carriages[i]);
	visuals.erase(it);
}

void
RenderTrainPlacementGhost(int engineModel, rw::V3d position, float angle)
{
	TrainCar cars[TRAIN_MAX_CARS];
	int n = ComputeTrainCars(engineModel, position, angle, cars);
	rw::RGBA col = { 80, 220, 120, 110 };
	for(int c = 0; c < n; c++){
		float heading = atan2f(-cars[c].forward.x, cars[c].forward.y) * 180.0f / 3.14159265f;
		RenderPlacementGhost(cars[c].model, cars[c].position, MakeObjectRotationDegrees({ 0.0f, 0.0f, heading }), col);
	}
}

void
RenderTrainTracksOverlay(void)
{
	bool show = gPlaceMode && IsTrainModel(GetSpawnObjectId());
	for(CPtrNode *p = selection.first; p && !show; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		show = inst->m_isVehicle && IsTrainModel(inst->m_objectId);
	}
	if(!show)
		return;
	rw::RGBA col = { 255, 210, 90, 110 };
	std::vector<rw::V3d> points;
	for(int t = 0; t < 4; t++){
		const Track &track = tracks[t];
		int n = (int)track.nodes.size();
		for(int i = 0; i < n; i++){
			const TrackNode &a = track.nodes[i], &b = track.nodes[(i + 1) % n];
			points.push_back({ a.x, a.y, a.z + 0.3f });
			points.push_back({ b.x, b.y, b.z + 0.3f });
		}
	}
	if(!points.empty())
		RenderLinesNow(points.data(), (int)points.size(), col);
}
