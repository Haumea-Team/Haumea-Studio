// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "anim.h"
#include "anim_ids.h"
#include "anim_flags.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// Skins are drawn with the building pipeline like everything else, which
// keeps picking, highlights and outlines working. An animated skin gets a
// private copy of its geometry, and each frame its vertices are skinned on
// the CPU from the original bind pose and the animated bone frames.

rw::Geometry *CloneInstanceGeometry(rw::Geometry *source);

namespace {

struct AnimKey
{
	rw::Quat rotation;
	rw::V3d translation;
	float time;
};

struct AnimTrack
{
	char name[24];
	int boneId;
	bool hasTranslation;
	std::vector<AnimKey> keys;
};

struct AnimClip
{
	char name[24];
	float duration;
	std::vector<AnimTrack> tracks;
};

struct AnimLibrary
{
	char name[32];
	// Where the IFP is: a loose file, or an entry of anim/anim.img.
	std::string file;
	uint32 offset, size;	// bytes; size 0 = the whole file
	bool attempted;
	std::vector<AnimClip> clips;
};

std::vector<AnimLibrary> gLibraries;
bool gIndexed;

void
indexLibraries(void)
{
	if(gIndexed)
		return;
	gIndexed = true;
	AnimLibrary ped = {};
	strcpy(ped.name, "PED");
	ped.file = "anim/ped.ifp";
	gLibraries.push_back(ped);

	FILE *f = fopen_ci("anim/anim.img", "rb");
	if(f == nil){
		log("Animation: anim/anim.img not found; only PED animations are available\n");
		return;
	}
	char header[8];
	if(fread(header, 1, 8, f) == 8 && memcmp(header, "VER2", 4) == 0){
		uint32 count;
		memcpy(&count, header + 4, 4);
		for(uint32 i = 0; i < count && i < 4096; i++){
			uint8 entry[32];
			if(fread(entry, 1, 32, f) != 32)
				break;
			uint32 sector, sectors;
			memcpy(&sector, entry, 4);
			sectors = entry[4] | entry[5] << 8;
			if(sectors == 0)
				sectors = entry[6] | entry[7] << 8;
			char name[25];
			memcpy(name, entry + 8, 24);
			name[24] = '\0';
			char *dot = strrchr(name, '.');
			if(dot == nil || rw::strcmp_ci(dot, ".ifp") != 0)
				continue;
			*dot = '\0';
			AnimLibrary lib = {};
			for(int c = 0; name[c] && c < 31; c++)
				lib.name[c] = (char)toupper((unsigned char)name[c]);
			lib.file = "anim/anim.img";
			lib.offset = sector * 2048;
			lib.size = sectors * 2048;
			gLibraries.push_back(lib);
		}
	}
	fclose(f);
	log("Animation: %d libraries\n", (int)gLibraries.size());
}

// ANP3, the format of SA's PC animations: compressed rotations (and root
// translations) per bone.
bool
parseIfp(const std::vector<uint8> &data, std::vector<AnimClip> &clips)
{
	size_t at = 0;
	auto read = [&](void *out, size_t n){
		if(at + n > data.size())
			return false;
		memcpy(out, &data[at], n);
		at += n;
		return true;
	};
	char ident[4];
	uint32 size, numAnims;
	char package[24];
	if(!read(ident, 4) || memcmp(ident, "ANP3", 4) != 0 || !read(&size, 4) ||
	   !read(package, 24) || !read(&numAnims, 4))
		return false;
	for(uint32 a = 0; a < numAnims; a++){
		AnimClip clip = {};
		uint32 numObjects, frameSize, unknown;
		if(!read(clip.name, 24) || !read(&numObjects, 4) || !read(&frameSize, 4) || !read(&unknown, 4))
			return false;
		clip.name[23] = '\0';
		for(uint32 o = 0; o < numObjects; o++){
			AnimTrack track = {};
			uint32 frameType, numFrames, boneId;
			if(!read(track.name, 24) || !read(&frameType, 4) || !read(&numFrames, 4) || !read(&boneId, 4))
				return false;
			track.name[23] = '\0';
			track.boneId = (int)boneId;
			track.hasTranslation = frameType == 4;
			track.keys.reserve(numFrames);
			for(uint32 k = 0; k < numFrames; k++){
				int16 q[4], t, p[3] = { 0, 0, 0 };
				if(!read(q, 8) || !read(&t, 2) || (track.hasTranslation && !read(p, 6)))
					return false;
				AnimKey key;
				// Used as stored, like the game (only ANPK files are conjugated).
				key.rotation = rw::makeQuat(q[3] / 4096.0f, q[0] / 4096.0f, q[1] / 4096.0f, q[2] / 4096.0f);
				float len = sqrtf(key.rotation.x*key.rotation.x + key.rotation.y*key.rotation.y +
					key.rotation.z*key.rotation.z + key.rotation.w*key.rotation.w);
				if(len > 0.0001f)
					key.rotation = scale(key.rotation, 1.0f/len);
				key.translation = { p[0] / 1024.0f, p[1] / 1024.0f, p[2] / 1024.0f };
				key.time = t / 60.0f;
				clip.duration = max(clip.duration, key.time);
				track.keys.push_back(key);
			}
			clip.tracks.push_back(track);
		}
		clips.push_back(clip);
	}
	return true;
}

AnimLibrary*
loadLibrary(int lib)
{
	indexLibraries();
	if(lib < 0 || lib >= (int)gLibraries.size())
		return nil;
	AnimLibrary &l = gLibraries[lib];
	if(l.attempted)
		return &l;
	l.attempted = true;
	FILE *f = fopen_ci(l.file.c_str(), "rb");
	if(f == nil){
		log("Animation: cannot open %s\n", l.file.c_str());
		return &l;
	}
	uint32 size = l.size;
	if(size == 0){
		fseek(f, 0, SEEK_END);
		size = (uint32)ftell(f);
	}
	std::vector<uint8> data(size);
	fseek(f, l.offset, SEEK_SET);
	bool ok = fread(data.data(), 1, size, f) == size;
	fclose(f);
	if(!ok || !parseIfp(data, l.clips))
		log("Animation: could not read the %s library\n", l.name);
	return &l;
}

const AnimClip*
getClip(int lib, int index)
{
	AnimLibrary *l = loadLibrary(lib);
	if(l == nil || index < 0 || index >= (int)l->clips.size())
		return nil;
	return &l->clips[index];
}

void
sampleTrack(const AnimTrack &track, float time, rw::Quat *rotation, rw::V3d *translation)
{
	const std::vector<AnimKey> &keys = track.keys;
	if(keys.size() == 1 || time <= keys[0].time){
		*rotation = keys[0].rotation;
		*translation = keys[0].translation;
		return;
	}
	for(size_t i = 1; i < keys.size(); i++){
		if(time <= keys[i].time){
			const AnimKey &a = keys[i-1], &b = keys[i];
			float span = b.time - a.time;
			float t = span > 0.0001f ? (time - a.time) / span : 0.0f;
			*rotation = slerp(a.rotation, b.rotation, t);
			*translation = add(scale(a.translation, 1.0f - t), scale(b.translation, t));
			return;
		}
	}
	*rotation = keys.back().rotation;
	*translation = keys.back().translation;
}

// Original (bind pose) vertices of each skin geometry, from before
// objectdef.cpp stood it upright.
struct BindPose
{
	std::vector<rw::V3d> vertices, normals;
};
std::map<rw::Geometry*, BindPose> gBindPoses;

struct AnimatedInstance
{
	int lib, clip;
	float delta;
	bool loop, lockX, lockY, freeze;
	int timeMs;
	float time;
	// What the pose was built for; rebuilt when the instance's clump changes.
	rw::Clump *clump;
	rw::Atomic *atomic;
	rw::Geometry *baseGeometry;	// the model's (upright) geometry
	rw::Geometry *posed;		// this instance's copy
	rw::HAnimHierarchy *hier;
	std::vector<rw::Matrix> baseLocal;
	std::vector<int> trackForNode;
	std::vector<int> stanceTrackForNode;	// PED/IDLE_stance under the clip
	bool rest;	// no animation of its own: the idle stance's first frame, posed once
	bool posedOnce;
};
std::map<int32, AnimatedInstance> gAnimated;	// by ObjectInst::m_id

// The skinned atomic of a ped clump.
rw::Atomic*
skinnedAtomic(rw::Clump *clump)
{
	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		if(atomic->geometry && rw::Skin::get(atomic->geometry))
			return atomic;
	}
	return nil;
}

void
releasePose(AnimatedInstance &a, bool restoreGeometry)
{
	if(a.posed){
		if(restoreGeometry && a.atomic && a.atomic->geometry == a.posed)
			a.atomic->setGeometry(a.baseGeometry, 0);
		a.posed->destroy();
	}
	if(restoreGeometry && a.hier)
		for(int i = 0; i < a.hier->numNodes && i < (int)a.baseLocal.size(); i++)
			if(a.hier->nodeInfo[i].frame)
				a.hier->nodeInfo[i].frame->matrix = a.baseLocal[i];
	a.posed = nil;
	a.clump = nil;
	a.atomic = nil;
	a.hier = nil;
}

// The standing pose every animation plays on top of, as in the game.
const AnimClip*
stanceClip(void)
{
	static int lib = -2, clip = -1;
	if(lib == -2){
		lib = AnimFindLibrary("PED");
		clip = lib >= 0 ? AnimFindInLibrary(lib, "IDLE_stance") : -1;
	}
	return getClip(lib, clip);
}

void
mapTracks(rw::HAnimHierarchy *hier, const AnimClip *clip, std::vector<int> &trackForNode)
{
	trackForNode.assign(hier->numNodes, -1);
	if(clip == nil)
		return;
	for(size_t t = 0; t < clip->tracks.size(); t++){
		int index = hier->getIndex(clip->tracks[t].boneId);
		if(index < 0)
			for(int i = 0; i < hier->numNodes; i++){
				rw::Frame *frame = hier->nodeInfo[i].frame;
				const char *name = frame ? gta::getNodeName(frame) : nil;
				if(name && rw::strcmp_ci(name, clip->tracks[t].name) == 0){
					index = i;
					break;
				}
			}
		if(index >= 0 && !clip->tracks[t].keys.empty())
			trackForNode[index] = (int)t;
	}
}

// Connects the animation to the instance's current clump.
bool
buildPose(ObjectInst *inst, AnimatedInstance &a)
{
	rw::Clump *clump = (rw::Clump*)inst->m_rwObject;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	const AnimClip *clip = a.rest ? stanceClip() : getClip(a.lib, a.clip);
	if(clump == nil || obj == nil || !obj->m_isPed || clip == nil)
		return false;
	rw::Atomic *atomic = skinnedAtomic(clump);
	rw::HAnimHierarchy *hier = rw::HAnimHierarchy::get(clump);
	if(atomic == nil || hier == nil || gBindPoses.find(atomic->geometry) == gBindPoses.end())
		return false;
	hier->attach();
	rw::Geometry *posed = CloneInstanceGeometry(atomic->geometry);
	if(posed == nil)
		return false;
	a.clump = clump;
	a.atomic = atomic;
	a.baseGeometry = atomic->geometry;
	a.posed = posed;
	a.hier = hier;
	a.baseLocal.resize(hier->numNodes);
	for(int i = 0; i < hier->numNodes; i++){
		rw::Frame *frame = hier->nodeInfo[i].frame;
		if(frame)
			a.baseLocal[i] = frame->matrix;
		else
			a.baseLocal[i].setIdentity();
	}
	mapTracks(hier, a.rest ? nil : clip, a.trackForNode);
	mapTracks(hier, stanceClip(), a.stanceTrackForNode);
	a.posedOnce = false;
	// Keep the base geometry alive while this instance draws its copy.
	atomic->setGeometry(posed, 0);
	return true;
}

// Where in the clip to show the pose, from the time since it started; -1
// when the animation has ended and the skin is back in the idle stance.
// The editor replays animations that end, after a pause, so their pose
// stays visible.
float
clipTime(const AnimatedInstance &a, float duration)
{
	if(duration <= 0.0f)
		return 0.0f;
	auto at = [&](float t){ return a.loop ? fmodf(t, duration) : std::min(t, duration); };
	// How long it plays: ApplyAnimation's time, or once without looping.
	float playFor = a.timeMs > 0 ? a.timeMs / 1000.0f : (a.loop ? -1.0f : duration);
	if(playFor < 0.0f)
		return at(a.time);
	const float pause = 1.5f;
	float t = fmodf(a.time, playFor + pause);
	if(t <= playFor)
		return at(t);
	return a.freeze ? at(playFor) : -1.0f;
}

void
pose(AnimatedInstance &a)
{
	if(a.rest && a.posedOnce)
		return;
	const AnimClip *clip = a.rest ? nil : getClip(a.lib, a.clip);
	const AnimClip *stance = stanceClip();
	if(clip == nil && stance == nil)
		return;
	float t = clip ? clipTime(a, clip->duration) : 0.0f;
	if(t < 0.0f)
		clip = nil;	// ended: back in the idle stance
	float st = stance && stance->duration > 0.0f && !a.rest ? fmodf(a.time, stance->duration) : 0.0f;
	rw::HAnimHierarchy *hier = a.hier;
	for(int i = 0; i < hier->numNodes; i++){
		rw::Frame *frame = hier->nodeInfo[i].frame;
		if(frame == nil)
			continue;
		rw::Matrix local = a.baseLocal[i];
		const AnimTrack *trp = nil;
		float tt = t;
		if(clip && a.trackForNode[i] >= 0)
			trp = &clip->tracks[a.trackForNode[i]];
		else if(stance && a.stanceTrackForNode[i] >= 0){
			trp = &stance->tracks[a.stanceTrackForNode[i]];
			tt = st;
		}
		if(trp){
			const AnimTrack &tr = *trp;
			rw::Quat rotation;
			rw::V3d translation;
			sampleTrack(tr, tt, &rotation, &translation);
			rw::V3d position = local.pos;
			local.rotate(rotation, rw::COMBINEREPLACE);
			local.pos = position;
			if(tr.hasTranslation){
				// Root motion stays in place unless lockX/lockY let it move.
				rw::V3d start = tr.keys[0].translation;
				if(!a.lockX) translation.x = start.x;
				if(!a.lockY) translation.y = start.y;
				local.pos = translation;
			}
		}
		frame->matrix = local;
		frame->updateObjects();
	}

	// Skin the bind pose with the posed bones, into the atomic's space.
	rw::Geometry *base = a.baseGeometry;
	rw::Skin *skin = rw::Skin::get(base);
	const BindPose &bind = gBindPoses[base];
	rw::Geometry *geo = a.posed;
	if(skin == nil || (int)bind.vertices.size() != geo->numVertices || skin->numBones != hier->numNodes)
		return;
	rw::Matrix invAtomic;
	rw::Matrix::invert(&invAtomic, a.atomic->getFrame()->getLTM());
	std::vector<rw::Matrix> bones(skin->numBones);
	for(int b = 0; b < skin->numBones; b++){
		rw::Matrix inverseBind = ((rw::Matrix*)skin->inverseMatrices)[b];
		inverseBind.flags = 0;
		rw::Matrix boneToAtomic;
		rw::Frame *frame = hier->nodeInfo[b].frame;
		if(frame)
			rw::Matrix::mult(&boneToAtomic, frame->getLTM(), &invAtomic);
		else
			boneToAtomic.setIdentity();
		rw::Matrix::mult(&bones[b], &inverseBind, &boneToAtomic);
	}
	geo->lock(rw::Geometry::LOCKVERTICES | rw::Geometry::LOCKNORMALS);
	rw::MorphTarget *mt = &geo->morphTargets[0];
	for(int v = 0; v < geo->numVertices; v++){
		rw::V3d position = { 0.0f, 0.0f, 0.0f }, normal = { 0.0f, 0.0f, 0.0f };
		float total = 0.0f;
		for(int k = 0; k < 4; k++){
			float w = skin->weights[v*4 + k];
			int bone = skin->indices[v*4 + k];
			if(w <= 0.0f || bone >= skin->numBones)
				continue;
			rw::V3d p, n;
			rw::V3d::transformPoints(&p, &bind.vertices[v], 1, &bones[bone]);
			position = add(position, scale(p, w));
			if(mt->normals && !bind.normals.empty()){
				rw::V3d::transformVectors(&n, &bind.normals[v], 1, &bones[bone]);
				normal = add(normal, scale(n, w));
			}
			total += w;
		}
		if(total <= 0.0f)
			continue;
		mt->vertices[v] = scale(position, 1.0f/total);
		if(mt->normals && !bind.normals.empty() && length(normal) > 0.0001f)
			mt->normals[v] = normalize(normal);
	}
	mt->boundingSphere = mt->calculateBoundingSphere();
	geo->unlock();
	a.posedOnce = true;
}

}	// namespace

// ---- Library queries ----

int
AnimNumLibraries(void)
{
	indexLibraries();
	return (int)gLibraries.size();
}

const char*
AnimLibraryName(int lib)
{
	indexLibraries();
	return lib >= 0 && lib < (int)gLibraries.size() ? gLibraries[lib].name : "";
}

int
AnimFindLibrary(const char *name)
{
	indexLibraries();
	for(size_t i = 0; name && i < gLibraries.size(); i++)
		if(rw::strcmp_ci(gLibraries[i].name, name) == 0)
			return (int)i;
	return -1;
}

int
AnimNumInLibrary(int lib)
{
	AnimLibrary *l = loadLibrary(lib);
	return l ? (int)l->clips.size() : 0;
}

const char*
AnimName(int lib, int index)
{
	const AnimClip *clip = getClip(lib, index);
	return clip ? clip->name : "";
}

int
AnimFindInLibrary(int lib, const char *name)
{
	AnimLibrary *l = loadLibrary(lib);
	for(size_t i = 0; l && name && i < l->clips.size(); i++)
		if(rw::strcmp_ci(l->clips[i].name, name) == 0)
			return (int)i;
	return -1;
}

int
AnimPedProblem(const char *lib, const char *name)
{
	if(lib == nil || name == nil)
		return ANIM_PED_OK;
	for(const AnimFlagEntry &e : animFlagEntries)
		if(rw::strcmp_ci(e.lib, lib) == 0 && rw::strcmp_ci(e.name, name) == 0)
			return e.objectOnly ? ANIM_PED_OBJECT_ONLY : ANIM_PED_DEFORMS;
	return ANIM_PED_OK;
}

float
AnimDuration(int lib, int index)
{
	const AnimClip *clip = getClip(lib, index);
	return clip ? clip->duration : 0.0f;
}

bool
AnimFromSampId(int id, char *lib, size_t libSize, char *name, size_t nameSize)
{
	int count = (int)(sizeof(sampAnimationNames)/sizeof(sampAnimationNames[0]));
	if(id <= 0 || id >= count)
		return false;
	const char *full = sampAnimationNames[id];
	const char *colon = strchr(full, ':');
	if(colon == nil)
		return false;
	snprintf(lib, libSize, "%.*s", (int)(colon - full), full);
	snprintf(name, nameSize, "%s", colon + 1);
	return true;
}

// ---- Instances ----

bool
SetInstanceAnimation(ObjectInst *inst, const char *lib, const char *name,
	float delta, bool loop, bool lockX, bool lockY, bool freeze, int timeMs)
{
	if(inst == nil)
		return false;
	int l = AnimFindLibrary(lib);
	int c = l >= 0 ? AnimFindInLibrary(l, name) : -1;
	if(c < 0){
		ClearInstanceAnimation(inst);
		return false;
	}
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || !obj->m_isPed)
		return false;
	AnimatedInstance &a = gAnimated[inst->m_id];
	bool same = a.posed && a.lib == l && a.clip == c;
	if(!same){
		releasePose(a, true);
		a.time = 0.0f;
	}
	if(a.rest){
		releasePose(a, true);
		a.rest = false;
		a.time = 0.0f;
		same = false;
	}
	a.lib = l;
	a.clip = c;
	a.delta = delta;
	a.loop = loop;
	a.lockX = lockX;
	a.lockY = lockY;
	a.freeze = freeze;
	if(a.timeMs != timeMs)
		a.time = 0.0f;
	a.timeMs = timeMs > 0 ? timeMs : 0;
	if(!same)
		a.posed = nil;	// built on the next update, once the clump exists
	return true;
}

void
ClearInstanceAnimation(ObjectInst *inst)
{
	if(inst == nil)
		return;
	std::map<int32, AnimatedInstance>::iterator it = gAnimated.find(inst->m_id);
	if(it == gAnimated.end())
		return;
	releasePose(it->second, (rw::Clump*)inst->m_rwObject == it->second.clump);
	gAnimated.erase(it);
}

bool
InstanceHasAnimation(const ObjectInst *inst)
{
	if(inst == nil)
		return false;
	std::map<int32, AnimatedInstance>::const_iterator it = gAnimated.find(inst->m_id);
	return it != gAnimated.end() && !it->second.rest;
}

bool
GetInstanceAnimation(const ObjectInst *inst, AnimPlayback *out)
{
	if(inst == nil || out == nil)
		return false;
	std::map<int32, AnimatedInstance>::const_iterator it = gAnimated.find(inst->m_id);
	if(it == gAnimated.end() || it->second.rest)
		return false;
	const AnimatedInstance &a = it->second;
	snprintf(out->lib, sizeof(out->lib), "%s", AnimLibraryName(a.lib));
	snprintf(out->name, sizeof(out->name), "%s", AnimName(a.lib, a.clip));
	out->delta = a.delta;
	out->loop = a.loop;
	out->lockX = a.lockX;
	out->lockY = a.lockY;
	out->freeze = a.freeze;
	out->timeMs = a.timeMs;
	return true;
}

void
AnimUpdate(float timeStep)
{
	for(std::map<int32, AnimatedInstance>::iterator it = gAnimated.begin(); it != gAnimated.end();){
		AnimatedInstance &a = it->second;
		ObjectInst *inst = GetInstanceByID(it->first);
		if(inst == nil){
			// Deleted for good: its clump went with it.
			if(a.posed)
				a.posed->destroy();
			it = gAnimated.erase(it);
			continue;
		}
		if(a.clump != (rw::Clump*)inst->m_rwObject){
			// The model changed or was reloaded; the old clump is gone.
			if(a.posed)
				a.posed->destroy();
			a.posed = nil;
			a.clump = nil;
			if(inst->m_rwObject == nil || !buildPose(inst, a)){
				++it;
				continue;
			}
		}
		if(gPlayAnimations)
			a.time += timeStep;
		if(!inst->m_isDeleted)
			pose(a);
		++it;
	}

	// Skins without an animation of their own stand in the idle stance,
	// posed once (checked twice a second so new skins pick it up).
	static float scan;
	scan -= timeStep;
	if(scan > 0.0f)
		return;
	scan = 0.5f;
	if(stanceClip() == nil)
		return;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_rwObject == nil || inst->m_isDeleted || gAnimated.count(inst->m_id))
			continue;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if(obj == nil || !obj->m_isPed)
			continue;
		AnimatedInstance &a = gAnimated[inst->m_id];
		a.rest = true;
		a.time = 0.0f;
	}
}

void
AnimNoteSkinBindPose(rw::Clump *clump)
{
	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		rw::Geometry *geo = atomic->geometry;
		if(geo == nil || rw::Skin::get(geo) == nil || geo->numMorphTargets < 1)
			continue;
		BindPose &bind = gBindPoses[geo];
		rw::MorphTarget *mt = &geo->morphTargets[0];
		bind.vertices.assign(mt->vertices, mt->vertices + geo->numVertices);
		if(mt->normals)
			bind.normals.assign(mt->normals, mt->normals + geo->numVertices);
		else
			bind.normals.clear();
	}
}
