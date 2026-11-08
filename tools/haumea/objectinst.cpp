#include "haumea.h"
#include "coordinatepin.h"
#include "maptools.h"
#include "layoutpaths.h"
#include "documentxml.h"
#include "i18n.h"
#include "editoricons.h"
#include "trains.h"
#include "encoding.h"
#include "imgui/imgui_internal.h"
#include "workspace.h"
#include "textdraws.h"
#include "rec.h"
#include "anim.h"
#include "npcpreview.h"
#include "modloader.h"
#include "categories.h"
#include "sapaths.h"
#include "samp_loader.h"
#include "materialtext.h"
#include "radarart.h"
#include "tempstream.h"
#include <errno.h>
#include <sys/stat.h>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

bool ReadCdImageEntryByLogicalPath(const char *logicalPath, std::vector<uint8> &data,
                                   char *outSourcePath, size_t outSourcePathSize);

CPtrList instances;
CPtrList selection;

enum
{
	OBJECT_ANIM_HAS_ROT = 1,
	OBJECT_ANIM_HAS_TRANS = 2,
	OBJECT_ANIM_HAS_SCALE = 4
};

struct ObjectAnimKeyFrame
{
	rw::Quat rotation;
	float time;
	rw::V3d translation;
};

struct ObjectAnimTrack
{
	char name[32];
	int nodeId;
	int type;
	std::vector<ObjectAnimKeyFrame> keyFrames;
};

struct ObjectAnimAsset
{
	bool attempted;
	bool loaded;
	float duration;
	char sourcePath[256];
	char animName[MODELNAMELEN];
	std::vector<ObjectAnimTrack> tracks;
};

struct ObjectAnimState
{
	rw::HAnimHierarchy *hier;
	std::vector<rw::Matrix> baseLocalMatrices;
	std::vector<int> trackIndexByNode;
};

struct IfpChunkHeader
{
	char ident[4];
	uint32 size;
};

static ObjectAnimAsset*
GetObjectAnimAsset(const ObjectDef *obj)
{
	return (ObjectAnimAsset*)obj->m_animAsset;
}

static ObjectAnimState*
GetObjectAnimState(const ObjectInst *inst)
{
	return (ObjectAnimState*)inst->m_animState;
}

static bool
readObjectAnimExact(const uint8 *data, size_t dataSize, size_t *offset, void *buf, size_t size)
{
	if(data == nil || offset == nil || *offset > dataSize || size > dataSize - *offset)
		return false;
	memcpy(buf, data + *offset, size);
	*offset += size;
	return true;
}

static rw::Quat
normalizeObjectAnimQuat(const rw::Quat &q)
{
	float len = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
	if(len < 0.0001f)
		return rw::makeQuat(1.0f, 0.0f, 0.0f, 0.0f);
	return scale(q, 1.0f/len);
}

static bool
readObjectAnimChunkHeader(const std::vector<uint8> &data, size_t *offset, IfpChunkHeader *header)
{
	const uint8 *bytes = data.empty() ? nil : &data[0];
	return readObjectAnimExact(bytes, data.size(), offset, header, sizeof(*header));
}

static bool
readObjectAnimRoundedChunkData(const std::vector<uint8> &data, size_t *offset, uint32 size,
                               std::vector<uint8> &out)
{
	size_t rounded = (size + 3) & ~3;
	out.resize(rounded);
	if(rounded == 0)
		return true;
	const uint8 *bytes = data.empty() ? nil : &data[0];
	return readObjectAnimExact(bytes, data.size(), offset, &out[0], rounded);
}

static bool
loadObjectAnimLogicalPath(const char *logicalPath, std::vector<uint8> &data)
{
	char sourcePath[1024];
	sourcePath[0] = '\0';
	if(ReadCdImageEntryByLogicalPath(logicalPath, data, sourcePath, sizeof(sourcePath)))
		return true;

	const char *redirect = ModloaderGetSourcePath(logicalPath);
	int size = 0;
	uint8 *buf = nil;
	if(redirect)
		buf = ReadLooseFile(redirect, &size);
	else
		buf = ReadLooseFile(logicalPath, &size);
	if(buf && size > 0){
		data.assign(buf, buf + size);
		free(buf);
		return true;
	}
	if(buf)
		free(buf);

	char gameRoot[1024];
	char absPath[1024];
	if(GetGameRootDirectory(gameRoot, sizeof(gameRoot)) &&
	   BuildPath(absPath, sizeof(absPath), gameRoot, logicalPath)){
		buf = ReadLooseFile(absPath, &size);
		if(buf && size > 0){
			data.assign(buf, buf + size);
			free(buf);
			return true;
		}
		if(buf)
			free(buf);
	}
	return false;
}

static bool
loadObjectAnimationFromData(const std::vector<uint8> &data, const char *wantedName,
                            ObjectAnimAsset *asset)
{
	size_t offset = 0;
	IfpChunkHeader root;
	std::vector<uint8> chunkData;

	if(asset == nil)
		return false;
	asset->duration = 0.0f;
	asset->tracks.clear();

	if(!readObjectAnimChunkHeader(data, &offset, &root))
		return false;

	if(strncmp(root.ident, "ANP3", 4) == 0){
		char packageName[24];
		uint32 numAnimations = 0;
		if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, packageName, sizeof(packageName)) ||
		   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &numAnimations, sizeof(numAnimations)))
			return false;

		ObjectAnimAsset fallback = {};
		bool haveFallback = false;
		for(uint32 animIndex = 0; animIndex < numAnimations; animIndex++){
			char animName[24];
			uint32 numNodes = 0;
			uint32 frameDataSize = 0;
			uint32 flags = 0;
			if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, animName, sizeof(animName)) ||
			   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &numNodes, sizeof(numNodes)) ||
			   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &frameDataSize, sizeof(frameDataSize)) ||
			   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &flags, sizeof(flags)))
				return false;

			bool wanted = rw::strcmp_ci(animName, wantedName) == 0;
			ObjectAnimAsset candidate = {};
			candidate.duration = 0.0f;
			if(wanted || !haveFallback){
				strncpy(candidate.animName, animName, sizeof(candidate.animName)-1);
				candidate.tracks.reserve(numNodes);
			}

			for(uint32 nodeIndex = 0; nodeIndex < numNodes; nodeIndex++){
				char nodeName[24];
				uint32 frameType = 0;
				uint32 numKeyFrames = 0;
				uint32 boneId = 0;
				if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, nodeName, sizeof(nodeName)) ||
				   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &frameType, sizeof(frameType)) ||
				   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &numKeyFrames, sizeof(numKeyFrames)) ||
				   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &boneId, sizeof(boneId)))
					return false;

				bool hasTranslation = frameType == 2 || frameType == 4;
				bool compressed = frameType == 3 || frameType == 4;
				if(!compressed)
					return false;

				ObjectAnimTrack track = {};
				if(wanted || !haveFallback){
					strncpy(track.name, nodeName, sizeof(track.name)-1);
					track.nodeId = boneId;
					track.type = OBJECT_ANIM_HAS_ROT | (hasTranslation ? OBJECT_ANIM_HAS_TRANS : 0);
					track.keyFrames.reserve(numKeyFrames);
				}

				for(uint32 keyIndex = 0; keyIndex < numKeyFrames; keyIndex++){
					int16 qx, qy, qz, qw, dt;
					ObjectAnimKeyFrame keyFrame = {};
					if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &qx, sizeof(qx)) ||
					   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &qy, sizeof(qy)) ||
					   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &qz, sizeof(qz)) ||
					   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &qw, sizeof(qw)) ||
					   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &dt, sizeof(dt)))
						return false;

					keyFrame.rotation = normalizeObjectAnimQuat(rw::makeQuat(
						qw / 4096.0f,
						qx / 4096.0f,
						qy / 4096.0f,
						qz / 4096.0f
					));
					keyFrame.time = dt / 60.0f;

					if(hasTranslation){
						int16 tx, ty, tz;
						if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &tx, sizeof(tx)) ||
						   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &ty, sizeof(ty)) ||
						   !readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset, &tz, sizeof(tz)))
							return false;
						keyFrame.translation = { tx / 1024.0f, ty / 1024.0f, tz / 1024.0f };
					}

					if(wanted || !haveFallback)
						track.keyFrames.push_back(keyFrame);
					candidate.duration = max(candidate.duration, keyFrame.time);
				}

				if(wanted || !haveFallback)
					candidate.tracks.push_back(track);
			}

			if(wanted){
				*asset = candidate;
				return true;
			}
			if(!haveFallback){
				fallback = candidate;
				haveFallback = true;
			}
		}
		if(haveFallback){
			*asset = fallback;
			return true;
		}
		return false;
	}

	int numPackages = 0;
	if(strncmp(root.ident, "ANLF", 4) == 0){
		if(!readObjectAnimRoundedChunkData(data, &offset, root.size, chunkData) ||
		   chunkData.size() < sizeof(int32))
			return false;
		numPackages = *(int32*)&chunkData[0];
	}else if(strncmp(root.ident, "ANPK", 4) == 0){
		offset = 0;
		numPackages = 1;
	}else
		return false;

	ObjectAnimAsset fallback = {};
	bool haveFallback = false;
	for(int pkg = 0; pkg < numPackages; pkg++){
		IfpChunkHeader anpk, info;
		if(!readObjectAnimChunkHeader(data, &offset, &anpk) ||
		   !readObjectAnimChunkHeader(data, &offset, &info) ||
		   !readObjectAnimRoundedChunkData(data, &offset, info.size, chunkData) ||
		   chunkData.size() < sizeof(int32))
			return false;

		int numAnimations = *(int32*)&chunkData[0];
		for(int animIndex = 0; animIndex < numAnimations; animIndex++){
			IfpChunkHeader nameChunk, dgan, nodeInfoChunk;
			std::vector<uint8> nameData;
			if(!readObjectAnimChunkHeader(data, &offset, &nameChunk) ||
			   !readObjectAnimRoundedChunkData(data, &offset, nameChunk.size, nameData) ||
			   !readObjectAnimChunkHeader(data, &offset, &dgan) ||
			   !readObjectAnimChunkHeader(data, &offset, &nodeInfoChunk) ||
			   !readObjectAnimRoundedChunkData(data, &offset, nodeInfoChunk.size, chunkData) ||
			   chunkData.size() < 8)
				return false;

			bool wanted = rw::strcmp_ci((const char*)&nameData[0], wantedName) == 0;
			int numNodes = *(int32*)&chunkData[0];
			ObjectAnimAsset candidate = {};
			candidate.duration = 0.0f;
			if(wanted || !haveFallback){
				strncpy(candidate.animName, (const char*)&nameData[0], sizeof(candidate.animName)-1);
				candidate.tracks.reserve(numNodes);
			}

			for(int nodeIndex = 0; nodeIndex < numNodes; nodeIndex++){
				IfpChunkHeader cpan, animChunk;
				if(!readObjectAnimChunkHeader(data, &offset, &cpan) ||
				   !readObjectAnimChunkHeader(data, &offset, &animChunk) ||
				   !readObjectAnimRoundedChunkData(data, &offset, animChunk.size, chunkData) ||
				   chunkData.size() < 44)
					return false;

				int numKeyFrames = *(int32*)&chunkData[28];
				int nodeId = *(int32*)&chunkData[40];
				int type = 0;
				ObjectAnimTrack track = {};
				if(wanted || !haveFallback){
					strncpy(track.name, (const char*)&chunkData[0], sizeof(track.name)-1);
					track.nodeId = nodeId;
				}

				if(numKeyFrames <= 0){
					if(wanted || !haveFallback)
						candidate.tracks.push_back(track);
					continue;
				}

				IfpChunkHeader keyChunk;
				if(!readObjectAnimChunkHeader(data, &offset, &keyChunk))
					return false;

				if(strncmp(keyChunk.ident, "KR00", 4) == 0)
					type = OBJECT_ANIM_HAS_ROT;
				else if(strncmp(keyChunk.ident, "KRT0", 4) == 0)
					type = OBJECT_ANIM_HAS_ROT | OBJECT_ANIM_HAS_TRANS;
				else if(strncmp(keyChunk.ident, "KRTS", 4) == 0)
					type = OBJECT_ANIM_HAS_ROT | OBJECT_ANIM_HAS_TRANS | OBJECT_ANIM_HAS_SCALE;
				else
					return false;

				if(wanted || !haveFallback){
					track.type = type;
					track.keyFrames.reserve(numKeyFrames);
				}

				for(int keyIndex = 0; keyIndex < numKeyFrames; keyIndex++){
					ObjectAnimKeyFrame keyFrame = {};
					if(type & OBJECT_ANIM_HAS_ROT){
						if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset,
						                        &keyFrame.rotation, sizeof(keyFrame.rotation)))
							return false;
						keyFrame.rotation.x = -keyFrame.rotation.x;
						keyFrame.rotation.y = -keyFrame.rotation.y;
						keyFrame.rotation.z = -keyFrame.rotation.z;
						keyFrame.rotation = normalizeObjectAnimQuat(keyFrame.rotation);
					}
					if(type & OBJECT_ANIM_HAS_TRANS){
						if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset,
						                        &keyFrame.translation, sizeof(keyFrame.translation)))
							return false;
					}
					if(type & OBJECT_ANIM_HAS_SCALE){
						rw::V3d unusedScale;
						if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset,
						                        &unusedScale, sizeof(unusedScale)))
							return false;
					}
					if(!readObjectAnimExact(data.empty() ? nil : &data[0], data.size(), &offset,
					                        &keyFrame.time, sizeof(keyFrame.time)))
						return false;

					if(wanted || !haveFallback)
						track.keyFrames.push_back(keyFrame);
					candidate.duration = max(candidate.duration, keyFrame.time);
				}

				if(wanted || !haveFallback)
					candidate.tracks.push_back(track);
			}

			if(wanted){
				*asset = candidate;
				return true;
			}
			if(!haveFallback){
				fallback = candidate;
				haveFallback = true;
			}
		}
	}

	if(haveFallback){
		*asset = fallback;
		return true;
	}
	return false;
}

static bool
sampleObjectAnimTrack(const ObjectAnimTrack &track, float time,
                      rw::Quat *rotation, rw::V3d *translation)
{
	if(track.keyFrames.empty())
		return false;
	if(track.keyFrames.size() == 1){
		if(rotation) *rotation = track.keyFrames[0].rotation;
		if(translation) *translation = track.keyFrames[0].translation;
		return true;
	}

	const ObjectAnimKeyFrame *prev = &track.keyFrames[0];
	const ObjectAnimKeyFrame *next = &track.keyFrames[track.keyFrames.size()-1];
	for(size_t i = 1; i < track.keyFrames.size(); i++){
		if(time <= track.keyFrames[i].time){
			next = &track.keyFrames[i];
			prev = &track.keyFrames[i-1];
			break;
		}
	}

	float span = next->time - prev->time;
	float t = span > 0.0001f ? (time - prev->time) / span : 0.0f;
	t = clamp(t, 0.0f, 1.0f);
	if(rotation) *rotation = slerp(prev->rotation, next->rotation, t);
	if(translation) *translation = add(scale(prev->translation, 1.0f - t), scale(next->translation, t));
	return true;
}

static ObjectAnimAsset*
EnsureObjectAnimAsset(ObjectDef *obj)
{
	if(obj == nil || obj->m_animname[0] == '\0')
		return nil;

	ObjectAnimAsset *asset = GetObjectAnimAsset(obj);
	if(asset){
		if(asset->attempted && !asset->loaded)
			return nil;
		return asset->loaded ? asset : nil;
	}

	asset = new ObjectAnimAsset();
	asset->attempted = true;
	obj->m_animAsset = asset;

	std::vector<std::string> candidates;
	if(obj->m_imageIndex >= 0){
		const char *archive = GetCdImageLogicalName(obj->m_imageIndex);
		if(archive && archive[0]){
			char path[512];
			if(snprintf(path, sizeof(path), "%s/%s.ifp", archive, obj->m_animname) < (int)sizeof(path))
				candidates.push_back(path);
		}
	}
	char fallback[64];
	if(snprintf(fallback, sizeof(fallback), "anim/%s.ifp", obj->m_animname) < (int)sizeof(fallback))
		candidates.push_back(fallback);
	if(snprintf(fallback, sizeof(fallback), "models/gta3.img/%s.ifp", obj->m_animname) < (int)sizeof(fallback))
		candidates.push_back(fallback);
	if(snprintf(fallback, sizeof(fallback), "models/gta_int.img/%s.ifp", obj->m_animname) < (int)sizeof(fallback))
		candidates.push_back(fallback);

	std::vector<uint8> ifpData;
	for(size_t i = 0; i < candidates.size(); i++){
		if(loadObjectAnimLogicalPath(candidates[i].c_str(), ifpData)){
			strncpy(asset->sourcePath, candidates[i].c_str(), sizeof(asset->sourcePath)-1);
			break;
		}
	}
	if(ifpData.empty()){
		log("Object animation: IFP not found for %s (%s)\n", obj->m_name, obj->m_animname);
		return nil;
	}

	if(!loadObjectAnimationFromData(ifpData, obj->m_name, asset)){
		log("Object animation: no matching anim for %s in %s\n", obj->m_name, asset->sourcePath);
		return nil;
	}

	asset->loaded = true;
	log("Object animation: loaded %s from %s for %s\n",
	    asset->animName[0] ? asset->animName : obj->m_name,
	    asset->sourcePath[0] ? asset->sourcePath : obj->m_animname,
	    obj->m_name);
	return asset;
}

static bool
SetupAnimatedClump(ObjectInst *inst, rw::Clump *clump)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	ObjectAnimAsset *asset = EnsureObjectAnimAsset(obj);
	if(asset == nil || !asset->loaded || clump == nil)
		return false;

	rw::HAnimHierarchy *hier = rw::HAnimHierarchy::get(clump);
	if(hier == nil)
		return false;
	hier->attach();

	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		if(atomic && atomic->geometry && rw::Skin::get(atomic->geometry))
			rw::Skin::setHierarchy(atomic, hier);
	}

	ObjectAnimState *state = new ObjectAnimState();
	state->hier = hier;
	state->baseLocalMatrices.resize(hier->numNodes);
	state->trackIndexByNode.assign(hier->numNodes, -1);

	for(int i = 0; i < hier->numNodes; i++){
		if(hier->nodeInfo[i].frame)
			state->baseLocalMatrices[i] = hier->nodeInfo[i].frame->matrix;
		else
			state->baseLocalMatrices[i].setIdentity();
	}

	for(size_t i = 0; i < asset->tracks.size(); i++){
		const ObjectAnimTrack &track = asset->tracks[i];
		int index = hier->getIndex(track.nodeId);
		if(index < 0){
			for(int boneIndex = 0; boneIndex < hier->numNodes; boneIndex++){
				rw::Frame *frame = hier->nodeInfo[boneIndex].frame;
				if(frame == nil)
					continue;
				const char *boneName = gta::getNodeName(frame);
				if(boneName && rw::strcmp_ci(boneName, track.name) == 0){
					index = boneIndex;
					break;
				}
			}
		}
		if(index >= 0)
			state->trackIndexByNode[index] = (int)i;
	}

	bool haveMappedTrack = false;
	for(size_t i = 0; i < state->trackIndexByNode.size(); i++)
		if(state->trackIndexByNode[i] >= 0){
			haveMappedTrack = true;
			break;
		}
	if(!haveMappedTrack){
		delete state;
		return false;
	}

	inst->m_animState = state;
	inst->m_animTime = 0.0f;
	return true;
}

static void
ApplyAnimatedClump(ObjectInst *inst)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	ObjectAnimAsset *asset = obj ? GetObjectAnimAsset(obj) : nil;
	ObjectAnimState *state = GetObjectAnimState(inst);
	if(asset == nil || !asset->loaded || state == nil || state->hier == nil)
		return;

	float animTime = inst->m_animTime;
	if(asset->duration > 0.001f)
		animTime = std::fmod(animTime, asset->duration);

	rw::HAnimHierarchy *hier = state->hier;
	rw::Matrix rootMat;
	rw::Frame *hierRoot = hier->parentFrame;
	rw::Frame *rootParent = hierRoot ? hierRoot->getParent() : nil;
	if(hierRoot && rootParent && !(hier->flags & rw::HAnimHierarchy::LOCALSPACEMATRICES))
		rootMat = *rootParent->getLTM();
	else
		rootMat.setIdentity();

	rw::Matrix *parentMat = &rootMat;
	rw::Matrix *stack[64];
	rw::Matrix **sp = stack;
	*sp++ = parentMat;

	for(int i = 0; i < hier->numNodes; i++){
		rw::Matrix local = state->baseLocalMatrices[i];
		int trackIndex = i < (int)state->trackIndexByNode.size() ? state->trackIndexByNode[i] : -1;
		if(trackIndex >= 0 && trackIndex < (int)asset->tracks.size()){
			const ObjectAnimTrack &track = asset->tracks[trackIndex];
			rw::Quat rotation;
			rw::V3d translation;
			if(sampleObjectAnimTrack(track, animTime, &rotation, &translation)){
				rw::V3d localPos = local.pos;
				local.rotate(rotation, rw::COMBINEREPLACE);
				local.pos = localPos;
				if(track.type & OBJECT_ANIM_HAS_TRANS){
					if(i == 0 && rootParent == nil && hierRoot == hier->nodeInfo[i].frame)
						local.pos = add(localPos, translation);
					else
						local.pos = translation;
				}
			}
		}

		if(hier->nodeInfo[i].frame)
			hier->nodeInfo[i].frame->matrix = local;
		if(hier->matrices)
			rw::Matrix::mult(&hier->matrices[i], &local, parentMat);

		if(hier->nodeInfo[i].flags & rw::HAnimHierarchy::PUSH){
			if(sp < stack + 64)
				*sp++ = parentMat;
		}
		parentMat = hier->matrices ? &hier->matrices[i] : parentMat;
		if(hier->nodeInfo[i].flags & rw::HAnimHierarchy::POP){
			if(sp > stack)
				parentMat = *--sp;
			else
				parentMat = &rootMat;
		}
	}

	if(hier->parentFrame)
		hier->parentFrame->updateObjects();
}

static bool
BuildAssetExportPath(const char *dir, const char *name, const char *ext, char *dst, size_t size)
{
	char filename[128];

	if(dir == nil || name == nil || ext == nil)
		return false;
	if(snprintf(filename, sizeof(filename), "%s.%s", name, ext) >= (int)sizeof(filename))
		return false;
	return BuildPath(dst, size, dir, filename);
}

static bool
WriteBufferToPath(const char *path, const uint8 *data, int size)
{
	FILE *f;

	if(path == nil || data == nil || size <= 0)
		return false;
	if(!EnsureParentDirectoriesForPath(path))
		return false;

	f = fopen(path, "wb");
	if(f == nil)
		return false;

	size_t written = fwrite(data, 1, size, f);
	fclose(f);
	return written == (size_t)size;
}

static bool
ExportEffectiveAsset(const char *name, const char *ext, int32 imageIndex, const char *dstPath)
{
	uint8 *buffer;
	int size = 0;
	bool freeBuffer = false;

	const char *loosePath = ModloaderFindOverride(name, ext);
	if(loosePath){
		buffer = ReadLooseFile(loosePath, &size);
		freeBuffer = true;
	}else{
		if(imageIndex < 0)
			return false;
		buffer = ReadFileFromImage(imageIndex, &size);
	}

	if(buffer == nil || size <= 0){
		if(freeBuffer)
			free(buffer);
		return false;
	}

	bool ok = WriteBufferToPath(dstPath, buffer, size);
	if(freeBuffer)
		free(buffer);
	return ok;
}

static bool
HasEarlierSelectedModel(CPtrNode *upto, const char *name)
{
	for(CPtrNode *p = selection.first; p && p != upto; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		ObjectDef *obj;
		if(inst == nil || inst->m_isDeleted)
			continue;
		obj = GetObjectDef(inst->m_objectId);
		if(obj && rw::strncmp_ci(obj->m_name, name, MODELNAMELEN) == 0)
			return true;
	}
	return false;
}

static bool
HasEarlierSelectedTxd(CPtrNode *upto, const char *name)
{
	for(CPtrNode *p = selection.first; p && p != upto; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		ObjectDef *obj;
		TxdDef *txd;
		if(inst == nil || inst->m_isDeleted)
			continue;
		obj = GetObjectDef(inst->m_objectId);
		if(obj == nil)
			continue;
		txd = GetTxdDef(obj->m_txdSlot);
		if(txd && rw::strncmp_ci(txd->name, name, MODELNAMELEN) == 0)
			return true;
	}
	return false;
}

static bool
sameIplFamily(ObjectInst *a, ObjectInst *b)
{
	if(a == nil || b == nil)
		return false;
	if(a->m_iplFilterKey[0] != '\0' && b->m_iplFilterKey[0] != '\0' &&
	   rw::strcmp_ci(a->m_iplFilterKey, b->m_iplFilterKey) == 0)
		return true;
	if(a->m_file && b->m_file && LogicalPathEquals(a->m_file->name, b->m_file->name))
		return true;
	return false;
}

static ObjectInst*
resolveInstanceLod(ObjectInst *inst)
{
	if(inst == nil)
		return nil;
	if(inst->m_lodId < 0){
		inst->m_lod = nil;
		return nil;
	}
	if(inst->m_lod && inst->m_lod != inst && inst->m_lod->m_imageIndex < 0 &&
	   inst->m_lod->m_iplIndex == inst->m_lodId &&
	   sameIplFamily(inst, inst->m_lod))
		return inst->m_lod;

	// A family save temporarily rewrites LOD pointers. If a previous operation
	// left only the local IPL index behind, recover the pointer before Delete or
	// Undelete decides whether the paired LOD must follow the HD instance.
	ObjectInst *resolved = nil;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *candidate = (ObjectInst*)p->item;
		if(candidate == nil || candidate == inst || candidate->m_imageIndex >= 0)
			continue;
		if(candidate->m_iplIndex != inst->m_lodId || !sameIplFamily(inst, candidate))
			continue;
		if(resolved != nil && resolved != candidate){
			log("Ambiguous LOD index %d in IPL family; refusing delete cascade\n", inst->m_lodId);
			inst->m_lod = nil;
			return nil;
		}
		resolved = candidate;
	}
	inst->m_lod = resolved;
	return resolved;
}

static bool
instanceReferencesLod(ObjectInst *inst, ObjectInst *lodInst)
{
	// LOD indices address the parent text IPL, never a streamed instance.
	// Streamed objects can otherwise alias text index zero and delete its children.
	if(inst == nil || lodInst == nil || inst == lodInst || lodInst->m_imageIndex >= 0)
		return false;
	return inst->m_lodId >= 0 &&
	       inst->m_lodId == lodInst->m_iplIndex &&
	       sameIplFamily(inst, lodInst);
}

static int
countLiveLodChildren(ObjectInst *lodInst, ObjectInst *ignoreInst)
{
	int count = 0;
	CPtrNode *p;

	if(lodInst == nil)
		return 0;

	for(p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other == ignoreInst || other->m_isDeleted)
			continue;
		if(instanceReferencesLod(other, lodInst))
			count++;
	}
	return count;
}

// CFileLoader::LoadObjectInstance (gta-reversed FileLoader.cpp) sets only the
// heading when the IPL quaternion has |x| and |y| at most 0.05, unless the
// instance is "don't stream" and tilted on both axes. Script objects get the
// whole rotation (SET_OBJECT_ROTATION, CMatrix::SetRotate).
void
BuildInstanceMatrix(rw::Matrix *matrix, const rw::Quat &rotation, const rw::V3d &translation, bool iplRotation)
{
	if(iplRotation && isSA() && std::fabs(rotation.x) <= 0.05f && std::fabs(rotation.y) <= 0.05f){
		float w = rotation.w;
		if(w < -1.0f) w = -1.0f;
		if(w > 1.0f) w = 1.0f;
		float heading = acosf(w) * (rotation.z < 0.0f ? 2.0f : -2.0f);
		float s = sinf(heading);
		float c = cosf(heading);

		matrix->setIdentity();
		matrix->right = { c, s, 0.0f };
		matrix->up = { -s, c, 0.0f };
		matrix->at = { 0.0f, 0.0f, 1.0f };
		matrix->flags = rw::Matrix::TYPEORTHONORMAL;
	}else
		matrix->rotate(conj(rotation), rw::COMBINEREPLACE);
	matrix->translate(&translation, rw::COMBINEPOSTCONCAT);
}

void
ObjectInst::UpdateMatrix(void)
{
	// Outside an IPL document an edited world object is exported as
	// RemoveBuildingForPlayer plus CreateObject, so it is drawn as one.
	bool ipl = m_iplRotation && (IsIplMapDocumentOpen() || !PawnObjectNeedsExport(this));
	BuildInstanceMatrix(&m_matrix, m_rotation, m_translation, ipl);
	const ObjectPawnProperties *properties = GetObjectPawnProperties(this);
	if(IsPlayerAttachedObject(this)){
		m_matrix.right = scale(m_matrix.right, properties->attachScale.x);
		m_matrix.up = scale(m_matrix.up, properties->attachScale.y);
		m_matrix.at = scale(m_matrix.at, properties->attachScale.z);
		m_matrix.flags = 0;
	}
}

static bool
materialOverrideIsActive(const ObjectMaterialOverride &material)
{
	return material.active || material.textEnabled;
}

static ObjectMaterialOverride*
ensureObjectMaterialOverrides(ObjectInst *inst)
{
	if(inst == nil)
		return nil;
	if(inst->m_materialOverrides == nil){
		inst->m_materialOverrides = new ObjectMaterialOverride[MAX_PAWN_MATERIALS];
		for(int i = 0; i < MAX_PAWN_MATERIALS; i++)
			InitObjectMaterialOverride(&inst->m_materialOverrides[i], inst->m_objectId);
	}
	return inst->m_materialOverrides;
}

static void
releaseObjectMaterialOverrides(ObjectInst *inst)
{
	if(inst == nil)
		return;
	delete[] inst->m_materialOverrides;
	inst->m_materialOverrides = nil;
}

ObjectInst::~ObjectInst()
{
	releaseObjectMaterialOverrides(this);
	delete m_pawnProperties;
	m_pawnProperties = nil;
	rwFree(m_vehicleColorRoles);
	m_vehicleColorRoles = nil;
	m_numVehicleColorRoles = 0;
}

void
InitObjectMaterialOverride(ObjectMaterialOverride *material, int sourceModel)
{
	if(material == nil)
		return;
	memset(material, 0, sizeof(*material));
	material->sourceModel = sourceModel;
	material->txdSlot = -1;
	material->color = { 0, 0, 0, 0 };	// SA-MP default: keep the model's colour
	material->fontSize = 24;
	material->bold = true;
	material->textColor = { 255, 255, 255, 255 };
	material->backgroundColor = { 0, 0, 0, 0 };
	material->alignment = 0;
	material->materialSize = 90; // OBJECT_MATERIAL_SIZE_256x128
	strncpy(material->font, "Arial", sizeof(material->font)-1);
}

static void copyMaterialUVAnimationState(rw::Material *dst,
	const rw::Material *src);

// A geometry round trip recreates temporary materials before the live
// materials are cloned below. Its texture names must never be resolved through
// the current map TXD or image files. Keep those temporary handles private;
// the final material clones retain the original textures and plugin state.
static std::vector<rw::Texture*> *gGeometryReadTextures;

static rw::Texture*
findGeometryReadTexture(const char *name)
{
	if(name == nil || gGeometryReadTextures == nil)
		return nil;
	for(rw::Texture *texture : *gGeometryReadTextures)
		if(rw::strncmp_ci(texture->name, name, sizeof(texture->name)) == 0)
			return texture;
	rw::Texture *texture = rw::Texture::create(nil);
	if(texture){
		strncpy(texture->name, name, sizeof(texture->name));
		gGeometryReadTextures->push_back(texture);
	}
	return texture;
}

static rw::Texture*
skipGeometryReadImage(const char*, const char*)
{
	return nil;
}

class ScopedGeometryTextureReads
{
	rw::TexDictionary *previousDictionary;
	rw::Texture *(*previousFind)(const char*);
	rw::Texture *(*previousRead)(const char*, const char*);
	std::vector<rw::Texture*> *previousTextures;
	std::vector<rw::Texture*> textures;
public:
	ScopedGeometryTextureReads()
	{
		previousDictionary = rw::TexDictionary::getCurrent();
		previousFind = rw::Texture::findCB;
		previousRead = rw::Texture::readCB;
		previousTextures = gGeometryReadTextures;
		gGeometryReadTextures = &textures;
		rw::TexDictionary::setCurrent(nil);
		rw::Texture::findCB = findGeometryReadTexture;
		rw::Texture::readCB = skipGeometryReadImage;
	}
	~ScopedGeometryTextureReads()
	{
		rw::Texture::findCB = previousFind;
		rw::Texture::readCB = previousRead;
		rw::TexDictionary::setCurrent(previousDictionary);
		gGeometryReadTextures = previousTextures;
		for(rw::Texture *texture : textures)
			texture->destroy();
	}
};

static rw::Geometry*
cloneGeometryForMaterial(rw::Geometry *source)
{
	if(source == nil)
		return nil;

	// Geometry::streamGetSize() is the payload size. streamWrite() also
	// writes the outer ID_GEOMETRY chunk header, so leave room for it and
	// start streamRead() after that header. Without this, every clone runs
	// out of the memory stream and overrides are discarded.
	uint32 oldFlags = source->flags;
	if(oldFlags & rw::Geometry::NATIVE)
		// Some render backends keep the editable arrays around after creating
		// native data. Serialize those arrays into the private copy instead of
		// carrying the backend-only NATIVE representation across.
		source->flags &= ~rw::Geometry::NATIVE;
	uint32 payloadSize = source->streamGetSize();
	if(oldFlags & rw::Geometry::NATIVE)
		source->flags = oldFlags;
	if(payloadSize < 16 || payloadSize > 256*1024*1024 - 12)
		return nil;
	uint32 size = payloadSize + 12;

	std::vector<uint8> data(size);
	rw::StreamMemory stream;
	stream.open(data.empty() ? nil : &data[0], 0, size);
	if(oldFlags & rw::Geometry::NATIVE)
		source->flags &= ~rw::Geometry::NATIVE;
	bool written = source->streamWrite(&stream) && !stream.eof();
	if(oldFlags & rw::Geometry::NATIVE)
		source->flags = oldFlags;
	if(!written)
		return nil;
	stream.seek(12, 0);
	rw::Geometry *geometry;
	{
		ScopedGeometryTextureReads textureReads;
		geometry = rw::Geometry::streamRead(&stream);
	}
	if(geometry == nil)
		return nil;

	// The geometry stream stores UV animation references by name. Reading the
	// stream here happens outside the DFF load scope, so there may be no active
	// UV animation dictionary and librw will create a dummy animation instead.
	// Material::clone() copies the live plugin state (including UVAnim and
	// MatFX), which is what SA-MP preserves when it swaps in its text texture.
	if(geometry->matList.numMaterials != source->matList.numMaterials){
		geometry->destroy();
		return nil;
	}
	for(int i = 0; i < source->matList.numMaterials; i++){
		rw::Material *base = source->matList.materials[i];
		if(base == nil)
			continue;
		rw::Material *material = base->clone();
		if(material == nil){
			geometry->destroy();
			return nil;
		}
		copyMaterialUVAnimationState(material, base);
		rw::Material *old = geometry->matList.materials[i];
		geometry->matList.materials[i] = material;
		// The mesh header stores material pointers separately from the material
		// list.  streamRead() has already connected those pointers to the
		// materials it created, so update them before releasing the old object.
		// Leaving one of them dangling makes the renderer dereference freed
		// material/texture data on the first frame after an edit.
		if(old && geometry->meshHeader){
			rw::Mesh *mesh = geometry->meshHeader->getMeshes();
			for(uint32 j = 0; j < geometry->meshHeader->numMeshes; j++)
				if(mesh[j].material == old)
					mesh[j].material = material;
		}
		if(old)
			old->destroy();
	}
	return geometry;
}

// For anim.cpp: an animated skin's private copy of its model's geometry.
rw::Geometry*
CloneInstanceGeometry(rw::Geometry *source)
{
	return cloneGeometryForMaterial(source);
}

static void
copyMaterialUVAnimationState(rw::Material *dst, const rw::Material *src)
{
	if(dst == nil || src == nil)
		return;

	// librw's material plugin copy duplicates the interpolators, but its UV
	// matrix pointers are deliberately not copied.  A material override uses
	// exactly this clone path, so make the private material a complete copy of
	// the currently displayed animation state before changing its texture.
	rw::Material *source = const_cast<rw::Material*>(src);
	rw::UVAnim *srcAnim = PLUGINOFFSET(rw::UVAnim, source, rw::uvAnimOffset);
	rw::UVAnim *dstAnim = PLUGINOFFSET(rw::UVAnim, dst, rw::uvAnimOffset);
	bool hasInterpolator = false;

	for(int i = 0; i < 2; i++){
		if(srcAnim->uv[i]){
			if(dstAnim->uv[i] == nil)
				dstAnim->uv[i] = rw::Matrix::create();
			if(dstAnim->uv[i])
				*dstAnim->uv[i] = *srcAnim->uv[i];
		}
	}

	for(int i = 0; i < 8; i++){
		if(srcAnim->interp[i] == nil || dstAnim->interp[i] == nil)
			continue;
		hasInterpolator = true;
		rw::Animation *animation = dstAnim->interp[i]->currentAnim;
		if(animation && dstAnim->interp[i]->setCurrentAnim(animation))
			dstAnim->interp[i]->addTime(srcAnim->interp[i]->currentTime);
	}

	if(hasInterpolator)
		rw::UVAnim::applyUpdate(dst);
	else if(rw::MatFX *matfx = rw::MatFX::get(dst))
		matfx->setUVTransformMatrices(dstAnim->uv[0], dstAnim->uv[1]);
}

struct GeneratedMaterialTexture
{
	ObjectMaterialOverride material;
	rw::Texture *texture;
};

static std::vector<GeneratedMaterialTexture> generatedMaterialTextures;

static bool
sameMaterialText(const ObjectMaterialOverride &a, const ObjectMaterialOverride &b)
{
	return a.textEnabled == b.textEnabled &&
	       strcmp(a.text, b.text) == 0 &&
	       strcmp(a.font, b.font) == 0 &&
	       a.fontSize == b.fontSize &&
	       a.bold == b.bold &&
	       memcmp(&a.textColor, &b.textColor, sizeof(a.textColor)) == 0 &&
	       memcmp(&a.backgroundColor, &b.backgroundColor, sizeof(a.backgroundColor)) == 0 &&
	       a.alignment == b.alignment &&
	       a.materialSize == b.materialSize;
}

static rw::Texture*
createMaterialTextTexture(const ObjectMaterialOverride &material)
{
	rw::Image *image = RenderMaterialTextImage(material);
	if(image == nil)
		return nil;
	rw::Raster *raster = rw::Raster::createFromImage(image);
	image->destroy();
	if(raster == nil)
		return nil;
	rw::Texture *texture = rw::Texture::create(raster);
	if(texture == nil){
		raster->destroy();
		return nil;
	}
	strncpy(texture->name, "haumea_material_text", sizeof(texture->name)-1);
	texture->setFilter(rw::Texture::LINEAR);
	// SA-MP's generated material textures use the normal RenderWare default
	// addressing (wrap).  DanceFloor1/2, for example, have UVs below zero;
	// clamping the generated texture leaves those faces sampling its transparent
	// border, so text appears to work only on the first face.
	texture->setAddressU(rw::Texture::WRAP);
	texture->setAddressV(rw::Texture::WRAP);
	return texture;
}

static rw::Texture*
getMaterialTextTexture(const ObjectMaterialOverride &material)
{
	for(size_t i = 0; i < generatedMaterialTextures.size(); i++)
		if(sameMaterialText(generatedMaterialTextures[i].material, material))
			return generatedMaterialTextures[i].texture;

	// Drop generated textures that are no longer referenced by a geometry.
	// The cache itself owns the one remaining reference.
	for(size_t i = 0; i < generatedMaterialTextures.size(); ){
		GeneratedMaterialTexture &entry = generatedMaterialTextures[i];
		if(entry.texture && entry.texture->refCount == 1){
			entry.texture->destroy();
			generatedMaterialTextures.erase(generatedMaterialTextures.begin() + i);
		}else
			i++;
	}
	if(generatedMaterialTextures.size() >= 128)
		return nil;

	rw::Texture *texture = createMaterialTextTexture(material);
	if(texture == nil)
		return nil;
	GeneratedMaterialTexture entry;
	entry.material = material;
	entry.texture = texture;
	generatedMaterialTextures.push_back(entry);
	return texture;
}

rw::Texture*
GetObjectMaterialTextPreview(const ObjectMaterialOverride &material)
{
	return getMaterialTextTexture(material);
}

// SetObjectMaterial with model ID 0 assigns SA-MP's "blanktex" (a plain white
// texture from SAMP/blanktex.txd) regardless of the TXD and texture names.
static rw::Texture*
getBlankMaterialTexture(void)
{
	static rw::Texture *blank;
	if(blank)
		return blank;
	rw::Image *image = rw::Image::create(4, 4, 32);
	if(image == nil)
		return nil;
	image->allocate();
	memset(image->pixels, 0xFF, image->stride * image->height);
	rw::Raster *raster = rw::Raster::createFromImage(image);
	image->destroy();
	if(raster == nil)
		return nil;
	blank = rw::Texture::create(raster);
	if(blank == nil){
		raster->destroy();
		return nil;
	}
	strncpy(blank->name, "blanktex", sizeof(blank->name)-1);
	blank->setFilter(rw::Texture::LINEAR);
	return blank;
}

static bool
materialColorIsSet(const rw::RGBA &color)
{
	// The client only applies a non-zero colour; 0 keeps the model's own.
	return color.red || color.green || color.blue || color.alpha;
}

// Which SA-MP call the slot currently represents. The client keeps one
// texture per slot, so a later SetObjectMaterialText replaces a
// SetObjectMaterial and vice versa; the editor stores both but uses one.
enum MaterialSlotKind
{
	MATERIAL_SLOT_ORIGINAL,
	MATERIAL_SLOT_TEXTURE,
	MATERIAL_SLOT_TEXT,
};

static MaterialSlotKind
materialSlotKind(const ObjectMaterialOverride *material)
{
	if(material == nil)
		return MATERIAL_SLOT_ORIGINAL;
	if(material->textEnabled)
		// SetObjectMaterialText with an empty string is rejected by the client.
		return material->text[0] ? MATERIAL_SLOT_TEXT : MATERIAL_SLOT_ORIGINAL;
	if(!material->active)
		return MATERIAL_SLOT_ORIGINAL;
	int model = material->hasTexture ? material->sourceModel : -1;
	// SetObjectMaterialInObjectRuntime returns before touching the slot when a
	// positive model ID has no model info.
	if(model > 0 && GetObjectDef(model) == nil)
		return MATERIAL_SLOT_ORIGINAL;
	return MATERIAL_SLOT_TEXTURE;
}

static bool
materialSlotSetsColor(const ObjectMaterialOverride *material)
{
	return materialSlotKind(material) == MATERIAL_SLOT_TEXTURE &&
	       materialColorIsSet(material->color);
}

// The client drops prelighting from a geometry while any of its first 16
// materials has a material colour: it sets rpGEOMETRYMODULATEMATERIALCOLOR,
// clears rpGEOMETRYPRELIT and forces surface properties to 1/0/1. That is
// why recoloured objects lose their baked shading and glow at night.
static bool
geometryKeepsPrelight(rw::Geometry *base, const ObjectMaterialOverride *overrides)
{
	if(base == nil || !(base->flags & rw::Geometry::PRELIT))
		return false;
	if(overrides == nil)
		return true;
	int count = min(base->matList.numMaterials, (int32)MAX_PAWN_MATERIALS);
	for(int i = 0; i < count; i++)
		if(materialSlotSetsColor(&overrides[i]))
			return false;
	return true;
}

// Prelight flags only take effect when a geometry is instanced, so changing
// them needs a fresh private copy rather than an edit of the live one.
static bool
privateGeometryMatchesPrelight(rw::Geometry *geometry, rw::Geometry *base,
	const ObjectMaterialOverride *overrides)
{
	if(geometry == nil || base == nil)
		return true;
	bool prelit = (geometry->flags & rw::Geometry::PRELIT) != 0;
	return prelit == geometryKeepsPrelight(base, overrides);
}

static rw::Geometry*
clonePrivateMaterialGeometry(rw::Geometry *base, const ObjectMaterialOverride *overrides)
{
	rw::Geometry *geometry = cloneGeometryForMaterial(base);
	if(geometry && (base->flags & rw::Geometry::PRELIT) &&
	   !geometryKeepsPrelight(base, overrides)){
		geometry->flags &= ~rw::Geometry::PRELIT;
		geometry->flags |= rw::Geometry::MODULATE;
	}
	return geometry;
}

static void
copyMaterialBase(rw::Material *dst, const rw::Material *src)
{
	if(dst == nil || src == nil)
		return;
	dst->setTexture(src->texture);
	dst->color = src->color;
	dst->surfaceProps = src->surfaceProps;
	dst->pipeline = src->pipeline;
}

static void
applyMaterialOverridesToGeometry(rw::Geometry *geometry, rw::Geometry *baseGeometry,
	const ObjectMaterialOverride *overrides)
{
	if(geometry == nil)
		return;
	// The client sets rpGEOMETRYMODULATEMATERIALCOLOR whenever a slot has a
	// colour, prelit or not; without it the colour is ignored and the texture
	// shows unmultiplied (an orange leaf texture stays orange instead of the
	// green the colour makes of it).
	bool colourSet = false;
	for(int i = 0; i < geometry->matList.numMaterials && i < MAX_PAWN_MATERIALS; i++)
		if(overrides && materialSlotSetsColor(&overrides[i]))
			colourSet = true;
	if(colourSet)
		geometry->flags |= rw::Geometry::MODULATE;
	else if(baseGeometry)
		geometry->flags = (geometry->flags & ~rw::Geometry::MODULATE) |
			(baseGeometry->flags & rw::Geometry::MODULATE);
	for(int i = 0; i < geometry->matList.numMaterials; i++){
		rw::Material *dst = geometry->matList.materials[i];
		rw::Material *base = baseGeometry && i < baseGeometry->matList.numMaterials
			? baseGeometry->matList.materials[i] : nil;
		copyMaterialBase(dst, base);
		const ObjectMaterialOverride *override = overrides && i < MAX_PAWN_MATERIALS
			? &overrides[i] : nil;

		switch(materialSlotKind(override)){
		case MATERIAL_SLOT_ORIGINAL:
			break;

		case MATERIAL_SLOT_TEXT: {
			// The text call stores no material colour, so the model's own
			// colour still modulates the generated texture.
			rw::Texture *textTexture = getMaterialTextTexture(*override);
			if(textTexture)
				dst->setTexture(textTexture);
			break;
		}

		case MATERIAL_SLOT_TEXTURE: {
			int model = override->hasTexture ? override->sourceModel : -1;
			if(model == 0)
				dst->setTexture(getBlankMaterialTexture());
			else if(model > 0 && (override->txdSlot >= 0 || IsCustomModel(model))){
				// The model ID only makes the client load its TXD; the texture
				// is looked up by TXD name. A miss leaves the original texture.
				// A custom model's TXD has no stock name: its own is used.
				int txdSlot = override->txdSlot;
				ObjectDef *source = IsCustomModel(model) ? GetObjectDef(model) : nil;
				if(source){
					txdSlot = source->m_txdSlot;
					if(!IsTxdLoaded(txdSlot))
						LoadTxd(txdSlot);
				}
				rw::Texture *texture = FindTextureInTxd(txdSlot,
					override->textureName);
				if(texture)
					dst->setTexture(texture);
			}
			if(materialColorIsSet(override->color)){
				dst->color = override->color;
				dst->surfaceProps.ambient = 1.0f;
				dst->surfaceProps.specular = 0.0f;	// also drops the env map
				dst->surfaceProps.diffuse = 1.0f;
			}
			break;
		}
		}
	}
}

static void
applyMaterialOverridesToClump(rw::Clump *clump, rw::Clump *baseClump,
	const ObjectMaterialOverride *overrides)
{
	if(clump == nil)
		return;
	// The client walks every atomic and applies material index N to the Nth
	// material of each atomic's own geometry.
	rw::LLLink *baseLink = baseClump ? baseClump->atomics.link.next : nil;
	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		rw::Atomic *baseAtomic = baseClump && baseLink != baseClump->atomics.end()
		? rw::Atomic::fromClump(baseLink) : nil;
		applyMaterialOverridesToGeometry(atomic->geometry,
			baseAtomic ? baseAtomic->geometry : nil, overrides);
		if(baseClump && baseLink != baseClump->atomics.end())
			baseLink = baseLink->next;
	}
}

void
RefreshMaterialTextTextures(void)
{
	// Release the cache's references; materials keep theirs until they are
	// reapplied below with freshly rendered textures.
	for(size_t i = 0; i < generatedMaterialTextures.size(); i++)
		if(generatedMaterialTextures[i].texture)
			generatedMaterialTextures[i].texture->destroy();
	generatedMaterialTextures.clear();
	CPtrNode *p;
	for(p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(HasObjectMaterialOverrides(inst))
			inst->RefreshMaterialOverrides();
	}
}

bool
HasObjectMaterialOverrides(const ObjectInst *inst)
{
	if(inst == nil || inst->m_materialOverrides == nil)
		return false;
	for(int i = 0; i < MAX_PAWN_MATERIALS; i++)
		if(materialOverrideIsActive(inst->m_materialOverrides[i]))
			return true;
	return false;
}

ObjectMaterialOverride*
GetObjectMaterialOverride(ObjectInst *inst, int slot)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS)
		return nil;
	ObjectMaterialOverride *overrides = ensureObjectMaterialOverrides(inst);
	return overrides ? &overrides[slot] : nil;
}

const ObjectMaterialOverride*
GetObjectMaterialOverride(const ObjectInst *inst, int slot)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS)
		return nil;
	return inst->m_materialOverrides ? &inst->m_materialOverrides[slot] : nil;
}

static void
normalizeObjectMaterialOverride(ObjectMaterialOverride *material)
{
	if(material == nil)
		return;
	material->txdName[sizeof(material->txdName)-1] = '\0';
	material->textureName[sizeof(material->textureName)-1] = '\0';
	material->text[sizeof(material->text)-1] = '\0';
	material->font[sizeof(material->font)-1] = '\0';
	if(material->txdName[0] != '\0'){
		// A custom model's TXD goes by its file name in Pawn.
		material->txdSlot = FindTxdSlot(material->txdName);
		if(material->txdSlot < 0)
			material->txdSlot = FindCustomModelTxdSlot(material->txdName);
	}else
		material->txdSlot = -1;
	if(material->fontSize < 1)
		material->fontSize = 1;
	if(material->fontSize > 255)	// sent as one byte
		material->fontSize = 255;
	if(material->materialSize < 10 || material->materialSize > 140 ||
	   material->materialSize % 10 != 0)
		material->materialSize = 90; // OBJECT_MATERIAL_SIZE_256x128
	if(material->alignment < 0)
		material->alignment = 0;
	if(material->alignment > 2)
		material->alignment = 2;
}

void
SetObjectMaterialOverride(ObjectInst *inst, int slot,
	const ObjectMaterialOverride &material, bool recordUndo)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS ||
	   !IsPawnObjectEditable(inst))
		return;

	ObjectMaterialOverride normalized = material;
	normalizeObjectMaterialOverride(&normalized);
	ObjectMaterialOverride defaultMaterial;
	InitObjectMaterialOverride(&defaultMaterial, inst->m_objectId);
	const ObjectMaterialOverride *current = inst->m_materialOverrides
		? &inst->m_materialOverrides[slot] : &defaultMaterial;
	if(memcmp(current, &normalized, sizeof(normalized)) == 0)
		return;
	ObjectMaterialOverride old = *current;
	ObjectMaterialOverride *overrides = ensureObjectMaterialOverrides(inst);
	if(overrides == nil)
		return;
	overrides[slot] = normalized;
	if(recordUndo)
		UndoRecordMaterial(inst, slot, old);
	StampChangeSeq(inst);
	inst->m_isDirty = true;
	inst->RefreshMaterialOverrides();
	if(!HasObjectMaterialOverrides(inst))
		releaseObjectMaterialOverrides(inst);
}

// Shows a material on the instance without changing the map, for hover
// previews; passing the material saved before puts it back.
void
PreviewObjectMaterialOverride(ObjectInst *inst, int slot, const ObjectMaterialOverride &material)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS || !IsPawnObjectEditable(inst))
		return;
	ObjectMaterialOverride normalized = material;
	normalizeObjectMaterialOverride(&normalized);
	ObjectMaterialOverride *overrides = ensureObjectMaterialOverrides(inst);
	if(overrides == nil)
		return;
	overrides[slot] = normalized;
	inst->RefreshMaterialOverrides();
	if(!HasObjectMaterialOverrides(inst))
		releaseObjectMaterialOverrides(inst);
}

void
ClearObjectMaterialOverride(ObjectInst *inst, int slot, bool recordUndo)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS ||
	   !IsPawnObjectEditable(inst))
		return;
	ObjectMaterialOverride cleared;
	InitObjectMaterialOverride(&cleared, inst->m_objectId);
	SetObjectMaterialOverride(inst, slot, cleared, recordUndo);
}

static void updateRwFrameForInst(ObjectInst *inst);

/*
 * SA-MP object properties
 */

static bool gAttachmentListDirty = true;
static bool gAttachmentWorkspaceActive;
static bool gAttachmentWorkspaceVoid;
static bool gAttachmentWorkspacePlayerMode;
static int gAttachmentWorkspacePlayerSkin = 1;
static bool gAttachmentWorkspacePlayerSkinSet;
static ObjectInst *gAttachmentWorkspaceTarget;
static ObjectInst *gAttachmentPlayerPreview;
static bool gAttachmentTargetPickActive;
static std::vector<ObjectInst*> gAttachmentTargetPickChildren;
static int gAttachmentTargetPickBone = 2;
static rw::V3d gAttachmentTargetPickScale = { 1.0f, 1.0f, 1.0f };
static bool gAttachmentWorkspaceCameraSaved;
static rw::V3d gAttachmentWorkspaceSavedCameraPosition;
static rw::V3d gAttachmentWorkspaceSavedCameraTarget;
static rw::V3d gAttachmentWorkspaceSavedCameraUp;
static rw::V3d gAttachmentWorkspaceSavedCameraLocalUp;
static float gAttachmentWorkspaceSavedCameraFov;
static bool gAttachmentWorkspaceNeedsFocus;
static bool gAttachmentWorkspaceSelectionPending;
static int gAttachmentWorkspacePendingCameraView = ATTACHMENT_VIEW_FOCUS;
static void setInstanceElementModel(ObjectInst *inst, int modelId);
static void applyInstanceModelId(ObjectInst *inst, int editorModelId);
static void destroyInstanceRwObject(ObjectInst *inst, ObjectDef *obj);

void
InitObjectPawnProperties(ObjectPawnProperties *properties)
{
	if(properties == nil)
		return;
	memset(properties, 0, sizeof(*properties));
	properties->virtualWorld = -1;
	properties->attachParent = -1;
	properties->attachSyncRotation = true;
	properties->attachBone = -1;
	properties->attachScale = { 1.0f, 1.0f, 1.0f };
	properties->attachSkin = 1;
	properties->npcVehicle = -1;
}

const ObjectPawnProperties*
GetObjectPawnProperties(const ObjectInst *inst)
{
	static ObjectPawnProperties defaults;
	static bool initialised;
	if(!initialised){
		InitObjectPawnProperties(&defaults);
		initialised = true;
	}
	return inst && inst->m_pawnProperties ? inst->m_pawnProperties : &defaults;
}

bool
ObjectPawnPropertiesAreDefault(const ObjectPawnProperties &properties)
{
	ObjectPawnProperties defaults;
	InitObjectPawnProperties(&defaults);
	return memcmp(&properties, &defaults, sizeof(defaults)) == 0;
}

bool
IsValidPawnObjectName(const char *name)
{
	if(name == nil || name[0] == '\0' || strlen(name) >= PAWN_OBJECT_NAME_LEN)
		return false;
	if(!isalpha((unsigned char)name[0]) && name[0] != '_' && name[0] != '@')
		return false;
	for(const char *p = name; *p; p++)
		if(!isalnum((unsigned char)*p) && *p != '_' && *p != '@')
			return false;
	static const char *reserved[] = {
		"new", "static", "stock", "public", "const", "if", "else", "for", "while",
		"do", "return", "break", "continue", "switch", "case", "default", "goto",
		"sizeof", "tagof", "enum", "native", "forward", "operator", "char", "defined",
		"playerid", "tmpobjid",
	};
	for(size_t i = 0; i < sizeof(reserved)/sizeof(reserved[0]); i++)
		if(strcmp(name, reserved[i]) == 0)
			return false;
	return true;
}

ObjectInst*
FindObjectByPawnName(const char *name, const ObjectInst *except)
{
	if(name == nil || name[0] == '\0')
		return nil;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst != except && !inst->m_isDeleted && inst->m_pawnProperties &&
		   strcmp(inst->m_pawnProperties->name, name) == 0)
			return inst;
	}
	return nil;
}

// NPC names are player names: up to 24 of A-Z a-z 0-9 [ ] ( ) $ @ . _ =
bool
IsValidNpcName(const char *name)
{
	size_t len = name ? strlen(name) : 0;
	if(len < 3 || len >= PAWN_NPC_NAME_LEN)
		return false;
	for(const char *p = name; *p; p++)
		if(!isalnum((unsigned char)*p) && strchr("[]()$@._=", *p) == nil)
			return false;
	return true;
}

ObjectInst*
FindNpcByName(const char *name, const ObjectInst *except)
{
	if(name == nil || name[0] == '\0')
		return nil;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst != except && !inst->m_isDeleted && inst->m_pawnProperties &&
		   inst->m_pawnProperties->elementKind == SAMP_ELEMENT_NPC &&
		   rw::strcmp_ci(inst->m_pawnProperties->npcName, name) == 0)
			return inst;
	}
	return nil;
}

// base, then base_2, base_3... with no other NPC in the map using it.
void
MakeUniqueNpcName(const char *base, char *out, size_t outSize)
{
	char stem[PAWN_NPC_NAME_LEN];
	snprintf(stem, sizeof(stem), "%s", base && base[0] ? base : "npc");
	// Drop a _N suffix so copies of npc_2 become npc_3, not npc_2_2.
	char *underscore = strrchr(stem, '_');
	if(underscore && underscore[1] && strspn(underscore + 1, "0123456789") == strlen(underscore + 1))
		*underscore = '\0';
	char candidate[PAWN_NPC_NAME_LEN];
	snprintf(candidate, sizeof(candidate), "%s", stem);
	for(int n = 2; FindNpcByName(candidate, nil) || !IsValidNpcName(candidate); n++){
		char suffix[16];
		snprintf(suffix, sizeof(suffix), "_%d", n);
		int keep = (int)sizeof(candidate) - 1 - (int)strlen(suffix);
		snprintf(candidate, sizeof(candidate), "%.*s%s", keep, stem, suffix);
		if(n > 100000)
			break;
	}
	snprintf(out, outSize, "%s", candidate);
}

// The dynamic area whose Pawn variable is name, or nil.
ObjectInst*
FindAreaByName(const char *name)
{
	if(name == nil || name[0] == '\0')
		return nil;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && inst->m_pawnProperties &&
		   inst->m_pawnProperties->elementKind == SAMP_ELEMENT_AREA &&
		   strcmp(inst->m_pawnProperties->name, name) == 0)
			return inst;
	}
	return nil;
}

// base (or "area"), then base_2, base_3... used by no instance's variable yet.
void
MakeUniqueAreaName(const char *base, char *out, size_t outSize)
{
	char stem[PAWN_OBJECT_NAME_LEN - 8];
	snprintf(stem, sizeof(stem), "%s", base && base[0] && IsValidPawnObjectName(base) ? base : "area");
	char candidate[PAWN_OBJECT_NAME_LEN];
	snprintf(candidate, sizeof(candidate), "%s", stem);
	for(int n = 2; n < 100000; n++){
		bool used = false;
		for(CPtrNode *p = instances.first; p && !used; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			used = !inst->m_isDeleted && inst->m_pawnProperties &&
				strcmp(inst->m_pawnProperties->name, candidate) == 0;
		}
		if(!used)
			break;
		snprintf(candidate, sizeof(candidate), "%s_%d", stem, n);
	}
	snprintf(out, outSize, "%s", candidate);
}

// Kinds the streamer can show only inside a dynamic area (they take areaid).
bool
SampElementTakesArea(int kind)
{
	return kind == SAMP_ELEMENT_OBJECT || kind == SAMP_ELEMENT_PICKUP || kind == SAMP_ELEMENT_LABEL ||
		kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_MAPICON || kind == SAMP_ELEMENT_CHECKPOINT;
}

static char spawnPathName[PAWN_OBJECT_NAME_LEN] = "patrol";

void
SetSpawnPathName(const char *name)
{
	if(name && IsValidPawnObjectName(name))
		snprintf(spawnPathName, sizeof(spawnPathName), "%s", name);
}

const char*
GetSpawnPathName(void)
{
	return spawnPathName;
}

// Checkpoints placed next: plain ones, or the end of this race.
static char spawnRaceName[PAWN_OBJECT_NAME_LEN];

void
SetSpawnRaceName(const char *name)
{
	if(name == nil || name[0] == '\0')
		spawnRaceName[0] = '\0';
	else if(IsValidPawnObjectName(name))
		snprintf(spawnRaceName, sizeof(spawnRaceName), "%s", name);
}

const char*
GetSpawnRaceName(void)
{
	return spawnRaceName;
}

// Every path's points in order, rebuilt when the map changes: the viewport
// draws the paths every frame and should not scan the whole map for each.
static const std::map<std::string, std::vector<ObjectInst*>>&
npcPaths(void)
{
	static std::map<std::string, std::vector<ObjectInst*>> paths;
	static uint32 builtSeq = 0xFFFFFFFFu;
	if(builtSeq == GetLatestChangeSeq())
		return paths;
	builtSeq = GetLatestChangeSeq();
	paths.clear();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && inst->m_pawnProperties &&
		   inst->m_pawnProperties->elementKind == SAMP_ELEMENT_PATHPOINT &&
		   inst->m_pawnProperties->pathName[0])
			paths[inst->m_pawnProperties->pathName].push_back(inst);
	}
	for(auto &path : paths)
		std::stable_sort(path.second.begin(), path.second.end(), [](const ObjectInst *a, const ObjectInst *b){
			int oa = a->m_pawnProperties->pathOrder, ob = b->m_pawnProperties->pathOrder;
			return oa != ob ? oa < ob : a->m_id < b->m_id;
		});
	return paths;
}

void
GetNpcPathPoints(const char *pathName, std::vector<ObjectInst*> &out)
{
	out.clear();
	if(pathName == nil || pathName[0] == '\0')
		return;
	const std::map<std::string, std::vector<ObjectInst*>> &paths = npcPaths();
	std::map<std::string, std::vector<ObjectInst*>>::const_iterator it = paths.find(pathName);
	if(it != paths.end())
		out = it->second;
}

void
GetNpcPathNames(std::vector<std::string> &out)
{
	out.clear();
	for(auto &path : npcPaths())
		out.push_back(path.first);
}

// The order after the last point of a path, so new points go on its end.
static int
nextPathOrder(const char *pathName)
{
	std::vector<ObjectInst*> points;
	GetNpcPathPoints(pathName, points);
	return points.empty() ? 0 : points.back()->m_pawnProperties->pathOrder + 1;
}

void
GetRaceCheckpoints(const char *raceName, std::vector<ObjectInst*> &out)
{
	out.clear();
	if(raceName == nil || raceName[0] == '\0')
		return;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && inst->m_pawnProperties &&
		   inst->m_pawnProperties->elementKind == SAMP_ELEMENT_CHECKPOINT &&
		   inst->m_pawnProperties->cpKind == SAMP_CHECKPOINT_RACE &&
		   strcmp(inst->m_pawnProperties->cpRace, raceName) == 0)
			out.push_back(inst);
	}
	std::stable_sort(out.begin(), out.end(), [](const ObjectInst *a, const ObjectInst *b){
		int oa = a->m_pawnProperties->cpOrder, ob = b->m_pawnProperties->cpOrder;
		return oa != ob ? oa < ob : a->m_id < b->m_id;
	});
}

// The checkpoint after this one in its race, or nil for the last.
ObjectInst*
GetNextRaceCheckpoint(const ObjectInst *inst)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(properties->elementKind != SAMP_ELEMENT_CHECKPOINT || properties->cpKind != SAMP_CHECKPOINT_RACE)
		return nil;
	std::vector<ObjectInst*> race;
	GetRaceCheckpoints(properties->cpRace, race);
	for(size_t i = 0; i + 1 < race.size(); i++)
		if(race[i] == inst)
			return race[i + 1];
	return nil;
}

static int
nextRaceOrder(const char *raceName)
{
	std::vector<ObjectInst*> race;
	GetRaceCheckpoints(raceName, race);
	return race.empty() ? 0 : race.back()->m_pawnProperties->cpOrder + 1;
}

static void
normalizeObjectPawnProperties(ObjectPawnProperties *properties)
{
	properties->name[sizeof(properties->name)-1] = '\0';
	properties->group[sizeof(properties->group)-1] = '\0';
	properties->comment[sizeof(properties->comment)-1] = '\0';
	// Comments are written on one line after the object call.
	for(char *p = properties->comment; *p; p++)
		if(*p == '\n' || *p == '\r') *p = ' ';
	for(char *p = properties->group; *p; p++)
		if(*p == '\n' || *p == '\r') *p = ' ';
	properties->drawDistance = clamp(properties->drawDistance, 0.0f, 100000.0f);
	properties->streamDistance = clamp(properties->streamDistance, 0.0f, 100000.0f);
	if(properties->virtualWorld < -1)
		properties->virtualWorld = -1;
	if(properties->elementKind < 0 || properties->elementKind >= NUM_SAMP_ELEMENT_KINDS)
		properties->elementKind = SAMP_ELEMENT_OBJECT;
	properties->labelText[sizeof(properties->labelText)-1] = '\0';
	properties->actorHealth = clamp(properties->actorHealth, 0.0f, 100000.0f);
	properties->zoneSizeX = clamp(properties->zoneSizeX, 0.0f, 12000.0f);
	properties->zoneSizeY = clamp(properties->zoneSizeY, 0.0f, 12000.0f);
	if(properties->zoneMode < 0 || properties->zoneMode >= NUM_SAMP_ZONE_MODES)
		properties->zoneMode = SAMP_ZONE_GLOBAL;
	properties->mapIconType = clamp(properties->mapIconType, 0, 63);
	properties->mapIconStyle = clamp(properties->mapIconStyle, 0, 3);
	properties->npcName[sizeof(properties->npcName)-1] = '\0';
	properties->npcArmour = clamp(properties->npcArmour, 0.0f, 100000.0f);
	properties->npcWeapon = clamp(properties->npcWeapon, 0, 46);
	properties->npcAmmo = clamp(properties->npcAmmo, 0, 99999);
	if(properties->npcWeapon == 0)
		properties->npcAmmo = 0;
	properties->pathName[sizeof(properties->pathName)-1] = '\0';
	if(properties->pathName[0] && !IsValidPawnObjectName(properties->pathName))
		properties->pathName[0] = '\0';
	properties->pathOrder = max(properties->pathOrder, 0);
	properties->npcRecording[sizeof(properties->npcRecording)-1] = '\0';
	properties->animLib[sizeof(properties->animLib)-1] = '\0';
	properties->animName[sizeof(properties->animName)-1] = '\0';
	if(properties->animName[0] == '\0' || properties->animLib[0] == '\0' ||
	   (properties->elementKind != SAMP_ELEMENT_ACTOR && properties->elementKind != SAMP_ELEMENT_NPC)){
		properties->animLib[0] = properties->animName[0] = '\0';
		properties->animDelta = 0.0f;
		properties->animLoop = properties->animLockX = properties->animLockY = properties->animFreeze = false;
		properties->animTime = 0;
	}
	properties->animDelta = clamp(properties->animDelta, 0.0f, 100.0f);
	properties->animTime = max(properties->animTime, 0);
	properties->soundUrl[sizeof(properties->soundUrl)-1] = '\0';
	if(properties->elementKind != SAMP_ELEMENT_SOUND){
		properties->soundKind = SAMP_SOUND_STREAM;
		properties->soundUrl[0] = '\0';
		properties->soundId = 0;
		properties->soundRadius = 0.0f;
		properties->soundUsePos = properties->soundStopOnLeave = false;
	}
	properties->soundKind = clamp(properties->soundKind, (int)SAMP_SOUND_STREAM, (int)SAMP_SOUND_GAME);
	properties->soundId = max(properties->soundId, 0);
	properties->soundRadius = clamp(properties->soundRadius, 0.0f, 10000.0f);
	properties->cpRace[sizeof(properties->cpRace)-1] = '\0';
	if(properties->cpRace[0] && !IsValidPawnObjectName(properties->cpRace))
		properties->cpRace[0] = '\0';
	if(properties->elementKind != SAMP_ELEMENT_CHECKPOINT){
		properties->cpKind = SAMP_CHECKPOINT_NORMAL;
		properties->cpSize = 0.0f;
		properties->cpRaceType = 0;
	}
	properties->cpKind = clamp(properties->cpKind, (int)SAMP_CHECKPOINT_NORMAL, (int)SAMP_CHECKPOINT_RACE);
	if(properties->cpKind != SAMP_CHECKPOINT_RACE){
		properties->cpRace[0] = '\0';
		properties->cpOrder = 0;
	}
	properties->cpSize = clamp(properties->cpSize, 0.0f, 1000.0f);
	properties->cpRaceType = clamp(properties->cpRaceType, 0, 8);
	properties->cpOrder = max(properties->cpOrder, 0);
	if(properties->elementKind != SAMP_ELEMENT_AREA){
		properties->areaShape = SAMP_AREA_CIRCLE;
		properties->areaRadius = properties->areaHeight = 0.0f;
		properties->areaSize = { 0.0f, 0.0f, 0.0f };
		properties->areaPointCount = 0;
		memset(properties->areaPoints, 0, sizeof(properties->areaPoints));
	}
	properties->areaShape = clamp(properties->areaShape, 0, NUM_SAMP_AREA_SHAPES - 1);
	properties->areaRadius = clamp(properties->areaRadius, 0.0f, 20000.0f);
	properties->areaHeight = clamp(properties->areaHeight, 0.0f, 20000.0f);
	properties->areaSize.x = clamp(properties->areaSize.x, 0.0f, 40000.0f);
	properties->areaSize.y = clamp(properties->areaSize.y, 0.0f, 40000.0f);
	properties->areaSize.z = clamp(properties->areaSize.z, 0.0f, 40000.0f);
	properties->areaPointCount = clamp(properties->areaPointCount, 0, (int)PAWN_AREA_POINTS);
	properties->areaName[sizeof(properties->areaName)-1] = '\0';
	if(!SampElementTakesArea(properties->elementKind) ||
	   (properties->areaName[0] && !IsValidPawnObjectName(properties->areaName)))
		properties->areaName[0] = '\0';
	properties->npcNode = clamp(properties->npcNode, 0, 63);
	properties->npcNodePoint = clamp(properties->npcNodePoint, 0, 65535);
	properties->npcNodeRadius = clamp(properties->npcNodeRadius, 0.0f, 100.0f);
	if(properties->elementKind != SAMP_ELEMENT_NPC)
		properties->npcPlaysNode = false;
	if(properties->elementKind != SAMP_ELEMENT_NPC){
		properties->npcRecording[0] = '\0';
		properties->npcRecordingAtNpc = false;
	}
	if(properties->elementKind != SAMP_ELEMENT_NPC || properties->npcVehicle < 0){
		properties->npcVehicle = -1;
		properties->npcSeat = 0;
	}
	properties->npcSeat = clamp(properties->npcSeat, 0, 3);
	properties->pathStopRange = clamp(properties->pathStopRange, 0.0f, 1000.0f);
	properties->npcMoveType = clamp(properties->npcMoveType, 0, 5);
	if(properties->npcMoveSpeed < 0.0f)
		properties->npcMoveSpeed = -1.0f;
	// NPCs are players: they are in one world, 0 by default.
	if(properties->elementKind == SAMP_ELEMENT_NPC && properties->virtualWorld < 0)
		properties->virtualWorld = 0;
	for(int i = 0; i < PAWN_CLASS_WEAPONS; i++){
		properties->classWeapons[i] = clamp(properties->classWeapons[i], 0, 46);
		properties->classAmmo[i] = clamp(properties->classAmmo[i], 0, 99999);
		if(properties->classWeapons[i] == 0)
			properties->classAmmo[i] = 0;
	}
	// Only map objects can be attached. Player attachments use the negative
	// PAWN_ATTACH_PLAYER target; ordinary object/vehicle links use an instance ID.
	if(properties->elementKind != SAMP_ELEMENT_OBJECT)
		properties->attachParent = -1;
	if(properties->attachParent < PAWN_ATTACH_PLAYER)
		properties->attachParent = -1;
	if(properties->attachBone < -1 || properties->attachBone > 18 || properties->attachBone == 0)
		properties->attachBone = -1;
	properties->attachSlot = clamp(properties->attachSlot, 0, 9);
	properties->attachScale.x = clamp(properties->attachScale.x, 0.01f, 50.0f);
	properties->attachScale.y = clamp(properties->attachScale.y, 0.01f, 50.0f);
	properties->attachScale.z = clamp(properties->attachScale.z, 0.01f, 50.0f);
	properties->attachSkin = clamp(properties->attachSkin, 0, 311);
	if(properties->attachBone < 1){
		if(properties->attachParent == PAWN_ATTACH_PLAYER)
			properties->attachParent = PAWN_ATTACH_NONE;
		properties->attachScale = { 1.0f, 1.0f, 1.0f };
		properties->attachColor1 = properties->attachColor2 = 0;
		properties->attachSkin = 1;
	}
	if(properties->elementKind != SAMP_ELEMENT_PATHPOINT && properties->elementKind != SAMP_ELEMENT_NPC)
		properties->pathName[0] = '\0';
	// Repeating is for an NPC that walks a path.
	properties->npcPathRepeat = clamp(properties->npcPathRepeat, (int)NPC_PATH_ONCE, (int)NPC_PATH_BACK_AND_FORTH);
	if(properties->elementKind != SAMP_ELEMENT_NPC || !properties->pathName[0] ||
	   properties->npcRecording[0] || properties->npcPlaysNode)
		properties->npcPathRepeat = NPC_PATH_ONCE;
	if(properties->attachParent == -1){
		properties->attachParent = -1;
		properties->attachOffset = { 0.0f, 0.0f, 0.0f };
		properties->attachRotation = { 0.0f, 0.0f, 0.0f };
		properties->attachSyncRotation = true;
		properties->attachBone = -1;
		properties->attachSlot = 0;
		properties->attachScale = { 1.0f, 1.0f, 1.0f };
		properties->attachColor1 = properties->attachColor2 = 0;
		properties->attachSkin = 1;
	}
}

void
SetObjectPawnProperties(ObjectInst *inst, const ObjectPawnProperties &properties,
	bool recordUndo)
{
	if(inst == nil || !IsPawnObjectEditable(inst))
		return;
	ObjectPawnProperties normalized = properties;
	normalizeObjectPawnProperties(&normalized);
	const ObjectPawnProperties *current = GetObjectPawnProperties(inst);
	if(memcmp(current, &normalized, sizeof(normalized)) == 0)
		return;
	ObjectPawnProperties old = *current;
	bool attachmentChanged = old.attachParent != normalized.attachParent ||
		memcmp(&old.attachOffset, &normalized.attachOffset, sizeof(rw::V3d)) != 0 ||
		memcmp(&old.attachRotation, &normalized.attachRotation, sizeof(rw::V3d)) != 0 ||
		old.attachSyncRotation != normalized.attachSyncRotation || old.attachBone != normalized.attachBone ||
		old.attachSlot != normalized.attachSlot ||
		memcmp(&old.attachScale, &normalized.attachScale, sizeof(rw::V3d)) != 0 ||
		old.attachColor1 != normalized.attachColor1 || old.attachColor2 != normalized.attachColor2 ||
		old.attachSkin != normalized.attachSkin;
	if(ObjectPawnPropertiesAreDefault(normalized)){
		delete inst->m_pawnProperties;
		inst->m_pawnProperties = nil;
	}else{
		if(inst->m_pawnProperties == nil)
			inst->m_pawnProperties = new ObjectPawnProperties;
		*inst->m_pawnProperties = normalized;
	}
	if(attachmentChanged)
		// Snap to the (new) offset on the next attachment update.
		inst->m_attachApplied = false;
	if(memcmp(&old.attachScale, &normalized.attachScale, sizeof(rw::V3d)) != 0){
		inst->UpdateMatrix();
		updateRwFrameForInst(inst);
	}
	if(strcmp(old.animLib, normalized.animLib) != 0 || strcmp(old.animName, normalized.animName) != 0 ||
	   old.npcVehicle != normalized.npcVehicle || old.npcSeat != normalized.npcSeat ||
	   old.animLoop != normalized.animLoop || old.animLockX != normalized.animLockX ||
	   old.animLockY != normalized.animLockY || old.animFreeze != normalized.animFreeze)
		ApplyInstanceAnimation(inst);
	if(normalized.elementKind == SAMP_ELEMENT_ACTOR || normalized.elementKind == SAMP_ELEMENT_CLASS ||
	   normalized.elementKind == SAMP_ELEMENT_NPC || normalized.elementKind == SAMP_ELEMENT_PICKUP)
		setInstanceElementModel(inst, GetSampElementDisplayModel(normalized.elementKind,
			normalized.elementModel));
	gAttachmentListDirty = true;
	if(recordUndo)
		UndoRecordProperties(inst, old);
	StampChangeSeq(inst);
	inst->m_isDirty = true;
	// A renamed area keeps its items (undo restores each one by its own record).
	if(recordUndo && normalized.elementKind == SAMP_ELEMENT_AREA && old.elementKind == SAMP_ELEMENT_AREA &&
	   old.name[0] && normalized.name[0] && strcmp(old.name, normalized.name) != 0)
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *item = (ObjectInst*)p->item;
			if(item->m_isDeleted || item->m_pawnProperties == nil || strcmp(item->m_pawnProperties->areaName, old.name) != 0)
				continue;
			ObjectPawnProperties relinked = *item->m_pawnProperties;
			strcpy(relinked.areaName, normalized.name);
			SetObjectPawnProperties(item, relinked);
		}
}

// Plays the actor's or NPC's animation in the viewport, or stops it. An NPC
// in a vehicle without one of its own sits, as the driver or a passenger.
void
ApplyInstanceAnimation(ObjectInst *inst)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(properties->animName[0])
		SetInstanceAnimation(inst, properties->animLib, properties->animName, properties->animDelta,
			properties->animLoop, properties->animLockX, properties->animLockY, properties->animFreeze);
	else if(GetNpcVehicle(inst))
		SetInstanceAnimation(inst, "PED", properties->npcSeat == 0 ? "CAR_SIT" : "CAR_SITP",
			4.1f, true, false, false, false);
	else if(InstanceHasAnimation(inst))
		ClearInstanceAnimation(inst);
}

static rw::Frame*
findSeatFrameCB(rw::Frame *frame, void *data)
{
	void **find = (void**)data;
	if(find[1])
		return nil;
	const char *name = gta::getNodeName(frame);
	if(name && rw::strcmp_ci(name, (const char*)find[0]) == 0){
		find[1] = frame;
		return nil;
	}
	frame->forAllChildren(findSeatFrameCB, data);
	return frame;
}

// Where a seat is in the vehicle's own space: the model's ped_frontseat or
// ped_backseat dummy, as the game seats peds, mirrored to the left for the
// driver and the back left seat. False when the model has no such dummy.
static bool
vehicleSeatOffset(ObjectInst *vehicle, int seat, rw::V3d *out)
{
	if(vehicle->m_rwObject == nil)
		return false;
	rw::Clump *clump = (rw::Clump*)vehicle->m_rwObject;
	rw::Frame *root = clump->getFrame();
	void *find[2] = { (void*)(seat < 2 ? "ped_frontseat" : "ped_backseat"), nil };
	if(root)
		findSeatFrameCB(root, find);
	rw::Frame *dummy = (rw::Frame*)find[1];
	if(dummy == nil)
		return false;
	rw::Matrix inverse;
	rw::Matrix::invert(&inverse, root->getLTM());
	rw::V3d::transformPoints(out, &dummy->getLTM()->pos, 1, &inverse);
	out->x = seat % 2 == 0 ? -fabsf(out->x) : fabsf(out->x);
	return true;
}

// Seats NPCs that are in a vehicle, once per frame so they ride along with a
// vehicle being moved or playing a recording. Like a preview, it does not
// mark the map changed.
void
UpdateNpcVehicleSeats(void)
{
	static std::unordered_set<int32> seated;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *npc = (ObjectInst*)p->item;
		if(npc->m_isVehicle || npc->m_isDeleted || npc->m_pawnProperties == nil ||
		   npc->m_pawnProperties->elementKind != SAMP_ELEMENT_NPC)
			continue;
		ObjectInst *vehicle = GetNpcVehicle(npc);
		bool wasSeated = seated.count(npc->m_id) != 0;
		if((vehicle != nil) != wasSeated){
			if(vehicle) seated.insert(npc->m_id); else seated.erase(npc->m_id);
			ApplyInstanceAnimation(npc);
		}
		if(vehicle == nil || NpcPreviewIsActive(npc))
			continue;
		// The model's seat dummy; without one, left or right of the middle,
		// front or back, from the vehicle's collision box.
		ObjectDef *def = GetObjectDef(vehicle->m_objectId);
		rw::V3d boxMin = { -1.0f, -2.2f, -0.6f }, boxMax = { 1.0f, 2.2f, 0.8f };
		if(def && def->m_colModel){
			boxMin = def->m_colModel->boundingBox.min;
			boxMax = def->m_colModel->boundingBox.max;
		}
		int seat = npc->m_pawnProperties->npcSeat;
		rw::V3d local = {
			(seat % 2 == 0 ? boxMin.x : boxMax.x) * 0.42f,
			seat < 2 ? boxMax.y * 0.02f : boxMin.y * 0.28f,
			-0.05f
		};
		vehicleSeatOffset(vehicle, seat, &local);
		// The vehicle's matrix, not its quaternion: instance quaternions use the
		// IPL (conjugate) convention.
		rw::V3d pos;
		rw::V3d::transformPoints(&pos, &local, 1, &vehicle->m_matrix);
		if(npc->m_translation.x == pos.x && npc->m_translation.y == pos.y && npc->m_translation.z == pos.z &&
		   npc->m_rotation.x == vehicle->m_rotation.x && npc->m_rotation.y == vehicle->m_rotation.y &&
		   npc->m_rotation.z == vehicle->m_rotation.z && npc->m_rotation.w == vehicle->m_rotation.w)
			continue;
		npc->m_translation = pos;
		npc->m_rotation = vehicle->m_rotation;
		npc->UpdateMatrix();
		ObjectDef *npcDef = GetObjectDef(npc->m_objectId);
		if(npcDef && npc->m_rwObject){
			rw::Frame *f = npcDef->m_type == ObjectDef::ATOMIC ?
				((rw::Atomic*)npc->m_rwObject)->getFrame() :
				((rw::Clump*)npc->m_rwObject)->getFrame();
			f->transform(&npc->m_matrix, rw::COMBINEREPLACE);
		}
	}
}

// The vehicle an NPC sits in, or nil when it is on foot or the vehicle is gone.
ObjectInst*
GetNpcVehicle(const ObjectInst *npc)
{
	if(npc == nil || npc->m_isVehicle || npc->m_pawnProperties == nil ||
	   npc->m_pawnProperties->elementKind != SAMP_ELEMENT_NPC || npc->m_pawnProperties->npcVehicle < 0)
		return nil;
	ObjectInst *vehicle = GetInstanceByID(npc->m_pawnProperties->npcVehicle);
	return vehicle && vehicle->m_isVehicle && !vehicle->m_isDeleted ? vehicle : nil;
}

ObjectInst*
GetAttachParent(const ObjectInst *inst)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(properties->attachParent == PAWN_ATTACH_PLAYER)
		return gAttachmentWorkspaceActive && gAttachmentWorkspacePlayerMode ?
			GetAttachmentWorkspacePlayerPreview() : nil;
	if(properties->attachParent < 0)
		return nil;
	ObjectInst *parent = GetInstanceByID(properties->attachParent);
	return parent && !parent->m_isDeleted ? parent : nil;
}

bool
IsPlayerAttachmentTarget(const ObjectInst *inst)
{
	if(inst == nil || inst->m_isDeleted || inst->m_isVehicle)
		return false;
	int kind = GetSampElementKind(inst);
	return kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_NPC;
}

bool
IsPlayerAttachedObject(const ObjectInst *inst)
{
	if(inst == nil || inst->m_isVehicle ||
	   GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
		return false;
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(properties->attachParent == PAWN_ATTACH_NONE || properties->attachBone < 1)
		return false;
	if(properties->attachParent == PAWN_ATTACH_PLAYER)
		return true;
	ObjectInst *parent = GetInstanceByID(properties->attachParent);
	return IsPlayerAttachmentTarget(parent);
}

static int
playerBoneNodeId(int sampBone)
{
	// SA-MP's bone numbers mapped to the GTA HAnim node IDs used by skin DFFs.
	static const int nodes[] = {
		-1, 3, 5, 32, 22, 34, 24, 41, 51, 43, 53, 52, 42, 33, 23, 31, 21, 4, 8
	};
	return sampBone >= 1 && sampBone <= 18 ? nodes[sampBone] : -1;
}

static bool
playerBoneMatrix(const ObjectInst *parent, int sampBone, rw::Matrix *matrix)
{
	if(parent == nil || parent->m_rwObject == nil || matrix == nil)
		return false;
	ObjectDef *obj = GetObjectDef(parent->m_objectId);
	if(obj == nil || obj->m_type != ObjectDef::CLUMP || !obj->m_isPed)
		return false;
	rw::Clump *clump = (rw::Clump*)parent->m_rwObject;
	rw::HAnimHierarchy *hier = rw::HAnimHierarchy::get(clump);
	int node = playerBoneNodeId(sampBone);
	int index = hier && node >= 0 ? hier->getIndex(node) : -1;
	if(index < 0 || index >= hier->numNodes || hier->nodeInfo[index].frame == nil)
		return false;
	*matrix = *hier->nodeInfo[index].frame->getLTM();
	return true;
}

bool
GetAttachmentPlayerBoneTransform(const ObjectInst *parent, int bone, rw::Matrix *matrix)
{
	return playerBoneMatrix(parent, bone, matrix);
}

static int
nextPlayerAttachmentSlot(int32 parentId, int skin, const ObjectInst *except)
{
	bool used[10] = {};
	ObjectInst *target = parentId >= 0 ? GetInstanceByID(parentId) : nil;
	bool classTarget = target && GetSampElementKind(target) == SAMP_ELEMENT_CLASS;
	bool npcTarget = target && GetSampElementKind(target) == SAMP_ELEMENT_NPC;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == except || inst->m_isDeleted || inst->m_isVehicle ||
		   GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
			continue;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		ObjectInst *otherTarget = properties->attachParent >= 0 ? GetInstanceByID(properties->attachParent) : nil;
		int otherKind = otherTarget ? GetSampElementKind(otherTarget) : -1;
		bool samePlayer = properties->attachParent == parentId ||
			properties->attachParent == PAWN_ATTACH_PLAYER;
		if(parentId == PAWN_ATTACH_PLAYER)
			samePlayer = true; // this slot is emitted for every player skin
		else if(classTarget)
			samePlayer = samePlayer ||
				((otherKind == SAMP_ELEMENT_CLASS || otherKind == SAMP_ELEMENT_NPC) &&
				 GetObjectPawnProperties(otherTarget)->elementModel == skin);
		else if(npcTarget)
			samePlayer = samePlayer || (otherKind == SAMP_ELEMENT_CLASS &&
				GetObjectPawnProperties(otherTarget)->elementModel == skin);
		if(samePlayer && properties->attachBone >= 1 &&
		   properties->attachSlot >= 0 && properties->attachSlot < 10)
			used[properties->attachSlot] = true;
	}
	for(int slot = 0; slot < 10; slot++)
		if(!used[slot])
			return slot;
	return -1;
}

// Rendered basis of an instance (rows are the rotated X/Y/Z axes).
static rw::Matrix
attachmentBasis(const rw::Quat &stored)
{
	rw::Matrix m;
	m.rotate(conj(stored), rw::COMBINEREPLACE);
	return m;
}

static rw::V3d
basisApply(const rw::Matrix &m, const rw::V3d &v)
{
	return add(add(scale(m.right, v.x), scale(m.up, v.y)), scale(m.at, v.z));
}

static rw::V3d
basisApplyInverse(const rw::Matrix &m, const rw::V3d &v)
{
	return { dot(v, m.right), dot(v, m.up), dot(v, m.at) };
}

// GTA's CPhysical::PositionAttachedEntity: the attached matrix is the
// parent's matrix times SetRotate(offset rotation), positioned at the
// parent-space offset. Without rotation sync the offset rotation is used as
// the world rotation.
static void
computeAttachedTransform(const ObjectInst *parent, const ObjectPawnProperties &properties,
	rw::V3d *position, rw::Quat *rotation)
{
	rw::Matrix parentBasis = attachmentBasis(parent->m_rotation);
	rw::V3d parentPosition = parent->m_translation;
	if(properties.attachBone >= 1){
		rw::Matrix bone;
		if(playerBoneMatrix(parent, properties.attachBone, &bone)){
			parentBasis = bone;
			parentPosition = bone.pos;
		}
	}
	*position = add(parentPosition, basisApply(parentBasis, properties.attachOffset));
	rw::Quat offsetRotation = MakeObjectRotationDegrees(properties.attachRotation);
	if(!properties.attachSyncRotation){
		*rotation = offsetRotation;
		return;
	}
	rw::Matrix local = attachmentBasis(offsetRotation);
	rw::Matrix world;
	world.setIdentity();
	world.right = basisApply(parentBasis, local.right);
	world.up = basisApply(parentBasis, local.up);
	world.at = basisApply(parentBasis, local.at);
	*rotation = QuatFromMatrix(world);
}

static void
computeAttachmentOffset(const ObjectInst *parent, const ObjectInst *child,
	ObjectPawnProperties *properties)
{
	rw::Matrix parentBasis = attachmentBasis(parent->m_rotation);
	rw::V3d parentPosition = parent->m_translation;
	if(properties->attachBone >= 1){
		rw::Matrix bone;
		if(playerBoneMatrix(parent, properties->attachBone, &bone)){
			parentBasis = bone;
			parentPosition = bone.pos;
		}
	}
	properties->attachOffset = basisApplyInverse(parentBasis,
		sub(child->m_translation, parentPosition));
	if(!properties->attachSyncRotation){
		properties->attachRotation = GetObjectRotationDegrees(child->m_rotation);
		return;
	}
	rw::Matrix childBasis = attachmentBasis(child->m_rotation);
	rw::Matrix local;
	local.setIdentity();
	local.right = basisApplyInverse(parentBasis, childBasis.right);
	local.up = basisApplyInverse(parentBasis, childBasis.up);
	local.at = basisApplyInverse(parentBasis, childBasis.at);
	properties->attachRotation = GetObjectRotationDegrees(QuatFromMatrix(local));
}

static bool
attachmentWouldCycle(const ObjectInst *child, const ObjectInst *parent)
{
	for(int depth = 0; parent && depth < 64; depth++){
		if(parent == child)
			return true;
		parent = GetAttachParent(parent);
	}
	return parent != nil;
}

bool
AttachObjectInstance(ObjectInst *child, ObjectInst *parent, bool syncRotation,
	int playerBone, const rw::V3d *playerScale, bool recordUndo)
{
	bool vehicleTarget = parent && parent->m_isVehicle;
	bool playerTarget = IsPlayerAttachmentTarget(parent);
	bool objectTarget = parent && !parent->m_isVehicle && GetSampElementKind(parent) == SAMP_ELEMENT_OBJECT;
	if(child == nil || parent == nil || child == parent || child->m_isVehicle ||
	   (!vehicleTarget && !playerTarget && !objectTarget) ||
	   (!vehicleTarget && !playerTarget && attachmentWouldCycle(child, parent)))
		return false;
	ObjectPawnProperties properties = *GetObjectPawnProperties(child);
	properties.attachParent = parent->m_id;
	properties.attachBone = IsPlayerAttachmentTarget(parent) ?
		(playerBone >= 1 && playerBone <= 18 ? playerBone : 2) : -1;
	properties.attachSyncRotation = (properties.attachBone >= 1 || parent->m_isVehicle) ? true : syncRotation;
	if(properties.attachBone >= 1){
		if(playerScale)
			properties.attachScale = *playerScale;
		properties.attachSkin = GetObjectPawnProperties(parent)->elementModel;
		properties.attachSlot = nextPlayerAttachmentSlot(parent->m_id, properties.attachSkin, child);
		if(properties.attachSlot < 0)
			return false;
	}
	computeAttachmentOffset(parent, child, &properties);
	SetObjectPawnProperties(child, properties, recordUndo);
	return true;
}

bool
AttachmentTargetPickActive(void)
{
	return gAttachmentTargetPickActive;
}

bool
BeginAttachmentTargetPick(void)
{
	gAttachmentTargetPickChildren.clear();
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst && !inst->m_isDeleted && !inst->m_isVehicle &&
		   GetSampElementKind(inst) == SAMP_ELEMENT_OBJECT && IsPawnObjectEditable(inst))
			gAttachmentTargetPickChildren.push_back(inst);
	}
	if(gAttachmentTargetPickChildren.empty()){
		gAttachmentTargetPickActive = false;
		Toast(TOAST_SELECTION, _("Select one or more objects to attach first."));
		return false;
	}
	const ObjectPawnProperties *properties = GetObjectPawnProperties(gAttachmentTargetPickChildren.front());
	gAttachmentTargetPickBone = properties->attachBone >= 1 && properties->attachBone <= 18 ?
		properties->attachBone : 2;
	gAttachmentTargetPickScale = properties->attachScale;
	// The direct viewport picker works with the real map targets. Close the
	// optional preview workspace so its synthetic player never appears beside
	// the NPC or vehicle being picked.
	CloseAttachmentWorkspace();
	gAttachmentTargetPickActive = true;
	Toast(TOAST_SELECTION,
		_("Click a ped, NPC, vehicle, or object in the viewport. Esc or right-click cancels."));
	return true;
}

void
GetAttachmentTargetPickOptions(int *bone, rw::V3d *scale)
{
	if(bone) *bone = gAttachmentTargetPickBone;
	if(scale) *scale = gAttachmentTargetPickScale;
}

void
SetAttachmentTargetPickOptions(int bone, const rw::V3d &scale)
{
	gAttachmentTargetPickBone = clamp(bone, 1, 18);
	gAttachmentTargetPickScale = {
		std::isfinite(scale.x) ? clamp(scale.x, 0.01f, 50.0f) : 1.0f,
		std::isfinite(scale.y) ? clamp(scale.y, 0.01f, 50.0f) : 1.0f,
		std::isfinite(scale.z) ? clamp(scale.z, 0.01f, 50.0f) : 1.0f
	};
}

void
CancelAttachmentTargetPick(void)
{
	gAttachmentTargetPickActive = false;
	gAttachmentTargetPickChildren.clear();
}

int
CompleteAttachmentTargetPick(ObjectInst *target)
{
	if(!gAttachmentTargetPickActive || target == nil || target->m_isDeleted)
		return 0;
	bool validTarget = target->m_isVehicle || IsPlayerAttachmentTarget(target) ||
		GetSampElementKind(target) == SAMP_ELEMENT_OBJECT;
	if(!validTarget)
		return 0;

	int attached = 0;
	for(ObjectInst *child : gAttachmentTargetPickChildren)
		if(child && !child->m_isDeleted && child != target &&
		   AttachObjectInstance(child, target, true, gAttachmentTargetPickBone, &gAttachmentTargetPickScale))
			attached++;
	if(attached == 0)
		return 0;

	CancelAttachmentTargetPick();
	UpdateObjectAttachments();
	OpenAttachmentWorkspace(target, false);
	return attached;
}

bool
RebaseObjectAttachment(ObjectInst *child)
{
	if(child == nil)
		return false;
	ObjectInst *parent = GetAttachParent(child);
	if(parent == nil)
		return false;
	// Recalculate only the offset. Bone, slot, scale and colours belong to
	// the existing attachment and must survive a parent being recentered.
	ObjectPawnProperties properties = *GetObjectPawnProperties(child);
	computeAttachmentOffset(parent, child, &properties);
	SetObjectPawnProperties(child, properties);
	return true;
}

bool
AttachObjectToPlayer(ObjectInst *child, int previewSkin, bool recordUndo)
{
	if(child == nil || child->m_isDeleted || child->m_isVehicle ||
	   GetSampElementKind(child) != SAMP_ELEMENT_OBJECT)
		return false;
	int slot = nextPlayerAttachmentSlot(PAWN_ATTACH_PLAYER, clamp(previewSkin, 0, 311), child);
	if(slot < 0)
		return false;
	ObjectPawnProperties properties = *GetObjectPawnProperties(child);
	properties.attachParent = PAWN_ATTACH_PLAYER;
	properties.attachBone = 2;	// Head, a useful starting point for a new accessory.
	properties.attachSlot = slot;
	properties.attachOffset = { 0.0f, 0.0f, 0.0f };
	properties.attachRotation = { 0.0f, 0.0f, 0.0f };
	properties.attachScale = { 1.0f, 1.0f, 1.0f };
	properties.attachSkin = clamp(previewSkin, 0, 311);
	properties.attachSyncRotation = true;
	SetObjectPawnProperties(child, properties, recordUndo);
	return true;
}

void
DetachObjectInstance(ObjectInst *child)
{
	if(child == nil || GetObjectPawnProperties(child)->attachParent == PAWN_ATTACH_NONE)
		return;
	ObjectPawnProperties properties = *GetObjectPawnProperties(child);
	properties.attachParent = -1;
	SetObjectPawnProperties(child, properties);
}

static bool
transformDiffers(const rw::V3d &aPos, const rw::Quat &aRot, const rw::V3d &bPos,
	const rw::Quat &bRot)
{
	rw::V3d d = sub(aPos, bPos);
	if(dot(d, d) > 1e-8f)
		return true;
	float similarity = fabsf(aRot.x*bRot.x + aRot.y*bRot.y + aRot.z*bRot.z + aRot.w*bRot.w);
	return similarity < 1.0f - 1e-7f;
}

void
UpdateObjectAttachments(void)
{
	// Instances are never freed, so the cached pointers stay valid; the list
	// is rebuilt when properties change or instances are added.
	static std::vector<ObjectInst*> children;
	if(gAttachmentListDirty){
		children.clear();
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_pawnProperties &&
			   (inst->m_pawnProperties->attachParent >= 0 ||
			    inst->m_pawnProperties->attachParent == PAWN_ATTACH_PLAYER))
				children.push_back(inst);
		}
		gAttachmentListDirty = false;
	}
	if(children.empty())
		return;

	// A few passes settle chains of attachments in any list order.
	for(int pass = 0; pass < 4; pass++){
		bool moved = false;
		for(size_t i = 0; i < children.size(); i++){
			ObjectInst *child = children[i];
			ObjectPawnProperties *properties = child->m_pawnProperties;
			if(properties == nil || properties->attachParent == PAWN_ATTACH_NONE || child->m_isDeleted)
				continue;
			ObjectInst *parent = GetAttachParent(child);
			if(parent == nil)
				continue;
			if(child->m_attachApplied &&
			   transformDiffers(child->m_translation, child->m_rotation,
				child->m_attachAppliedPos, child->m_attachAppliedRot)){
				// The object itself was moved: keep it there, relative to its parent.
				computeAttachmentOffset(parent, child, properties);
			}else{
				rw::V3d position;
				rw::Quat rotation;
				computeAttachedTransform(parent, *properties, &position, &rotation);
				if(transformDiffers(position, rotation, child->m_translation, child->m_rotation)){
					RemoveInstFromSectors(child);
					child->m_translation = position;
					child->m_rotation = rotation;
					child->UpdateMatrix();
					updateRwFrameForInst(child);
					InsertInstIntoSectors(child);
					StampChangeSeq(child);
					child->m_isDirty = true;
					moved = true;
				}
			}
			child->m_attachApplied = true;
			child->m_attachAppliedPos = child->m_translation;
			child->m_attachAppliedRot = child->m_rotation;
		}
		if(!moved)
			break;
	}
}

void
ObjectInst::SetBaseGeometry(rw::Geometry *geometry)
{
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil || obj->m_type != ObjectDef::ATOMIC || m_rwObject == nil)
		return;
	rw::Atomic *atomic = (rw::Atomic*)m_rwObject;

	// A distance/LOD change can replace the base geometry. Discard the old
	// private copy before switching, otherwise its pointer would be stale after
	// Atomic::setGeometry releases it.
	if(m_materialCustomGeometry && atomic->geometry == m_materialCustomGeometry &&
	   m_materialBaseGeometry != geometry){
		m_materialCustomGeometry = nil;
		m_materialBaseGeometry = nil;
		m_materialGeometryApplied = false;
		atomic->setGeometry(geometry, 0);
	}else if(!(m_materialCustomGeometry && atomic->geometry == m_materialCustomGeometry) &&
	          atomic->geometry != geometry){
		atomic->setGeometry(geometry, 0);
	}

	if(HasObjectMaterialOverrides(this))
		RefreshMaterialOverrides();
}

// A geometry whose prelighting a material colour removed can't go through
// the building pipeline, which colours vertices only from prelighting and
// would leave it lit by the ambient alone: the default pipeline lights it,
// with the directional light, as the game lights objects. The switch only
// happens with a fresh private copy (or the return to the base geometry), so
// a geometry is never instanced by one pipeline and drawn by the other.
static void
setMaterialPipeline(rw::Atomic *atomic, bool privateCopy)
{
	if(atomic == nil || atomic->geometry == nil)
		return;
	// Only a private copy can lose its prelighting; the shared base geometry
	// stays with the building pipeline that instanced it.
	bool lit = privateCopy && !(atomic->geometry->flags & rw::Geometry::PRELIT);
	if(lit && atomic->renderCB != LitObjectRenderCB){
		atomic->pipeline = nil;
		gta::setPipelineID(atomic, 0);
		atomic->setRenderCB(LitObjectRenderCB);
	}else if(!lit && atomic->renderCB == LitObjectRenderCB){
		SetupBuildingPipe(atomic);
		atomic->setRenderCB(myRenderCB);
	}
}

void
ObjectInst::RefreshMaterialOverrides(void)
{
	refreshMaterialGeometry();
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil || m_rwObject == nil)
		return;
	if(obj->m_type == ObjectDef::ATOMIC){
		rw::Atomic *atomic = (rw::Atomic*)m_rwObject;
		setMaterialPipeline(atomic, m_materialCustomGeometry && atomic->geometry == m_materialCustomGeometry);
	}else
		FORLIST(lnk, ((rw::Clump*)m_rwObject)->atomics)
			setMaterialPipeline(rw::Atomic::fromClump(lnk), m_materialGeometryApplied);
}

bool gShowMaterialIndexes;

// Show material indexes: each slot shows its number, white on green, as
// Texture Studio paints them, so it is clear which part is which index.
static const ObjectMaterialOverride*
materialIndexOverrides(void)
{
	static ObjectMaterialOverride overrides[MAX_PAWN_MATERIALS];
	static bool made;
	if(!made){
		made = true;
		for(int i = 0; i < MAX_PAWN_MATERIALS; i++){
			InitObjectMaterialOverride(&overrides[i], -1);
			overrides[i].textEnabled = true;
			// A grid of it: models often map only part of the texture.
			snprintf(overrides[i].text, sizeof(overrides[i].text), "%d   %d   %d\n%d   %d   %d\n%d   %d   %d",
				i, i, i, i, i, i, i, i, i);
			overrides[i].fontSize = 56;
			overrides[i].bold = true;
			overrides[i].textColor = { 255, 255, 255, 255 };
			overrides[i].backgroundColor = { 40, 170, 70, 255 };
			overrides[i].alignment = 1;	// centred
			overrides[i].materialSize = 140;	// 256x256
		}
	}
	return overrides;
}

// Every frame: the selected objects show their indexes while it is on, and
// go back to their own materials when deselected or when it is turned off.
void
UpdateMaterialIndexView(void)
{
	static bool wasOn;
	if(!gShowMaterialIndexes && !wasOn)
		return;
	wasOn = gShowMaterialIndexes;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		bool want = gShowMaterialIndexes && inst->m_selected && !inst->m_isDeleted && !inst->m_isVehicle &&
			inst->m_rwObject != nil;
		if(want != inst->m_showingMaterialIndexes){
			inst->m_showingMaterialIndexes = want;
			inst->RefreshMaterialOverrides();
		}
	}
}

void
ObjectInst::refreshMaterialGeometry(void)
{
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil || m_rwObject == nil)
		return;
	// Show material indexes puts its own overrides in the place of the real ones.
	const ObjectMaterialOverride *overrides = m_showingMaterialIndexes ? materialIndexOverrides() : m_materialOverrides;
	bool anyOverride = m_showingMaterialIndexes || HasObjectMaterialOverrides(this);

	if(obj->m_type == ObjectDef::ATOMIC){
		rw::Atomic *atomic = (rw::Atomic*)m_rwObject;
		if(!anyOverride){
			if(m_materialCustomGeometry && atomic->geometry == m_materialCustomGeometry &&
			   m_materialBaseGeometry){
				rw::Geometry *base = m_materialBaseGeometry;
				m_materialCustomGeometry = nil;
				m_materialBaseGeometry = nil;
				m_materialGeometryApplied = false;
				atomic->setGeometry(base, 0);
			}else{
				m_materialGeometryApplied = false;
				m_materialCustomGeometry = nil;
				m_materialBaseGeometry = nil;
			}
			return;
		}

		bool havePrivate = m_materialCustomGeometry && atomic->geometry == m_materialCustomGeometry;
		if(!havePrivate ||
		   !privateGeometryMatchesPrelight(m_materialCustomGeometry,
			m_materialBaseGeometry, overrides)){
			if(!havePrivate)
				m_materialBaseGeometry = atomic->geometry;
			rw::Geometry *copy = clonePrivateMaterialGeometry(m_materialBaseGeometry,
				overrides);
			if(copy == nil){
				if(havePrivate)
					atomic->setGeometry(m_materialBaseGeometry, 0);
				m_materialCustomGeometry = nil;
				m_materialBaseGeometry = nil;
				m_materialGeometryApplied = false;
				log("Material override: failed to clone geometry for %s\n", obj->m_name);
				return;
			}
			// Replacing a previous private copy releases it here.
			m_materialCustomGeometry = copy;
			atomic->setGeometry(m_materialCustomGeometry, 0);
			m_materialGeometryApplied = true;
		}
		applyMaterialOverridesToGeometry(atomic->geometry, m_materialBaseGeometry,
			overrides);
		return;
	}

	// Clumps can contain several atomics. Keep their private geometries once
	// created and restore their base material values when the last override is
	// cleared; this avoids a dynamic vector in ObjectInst and keeps instances
	// safely zero-initializable by the existing loader.
	rw::Clump *clump = (rw::Clump*)m_rwObject;
	rw::LLLink *baseLink = obj->m_clump ? obj->m_clump->atomics.link.next : nil;
	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		rw::Geometry *base = nil;
		if(obj->m_clump && baseLink != obj->m_clump->atomics.end()){
			base = rw::Atomic::fromClump(baseLink)->geometry;
			baseLink = baseLink->next;
		}
		if(atomic->geometry == nil)
			continue;
		if(base == nil)
			base = atomic->geometry;
		if(m_materialGeometryApplied &&
		   privateGeometryMatchesPrelight(atomic->geometry, base, overrides))
			continue;
		rw::Geometry *copy = clonePrivateMaterialGeometry(base, overrides);
		if(copy)
			atomic->setGeometry(copy, 0);
	}
	m_materialGeometryApplied = true;
	applyMaterialOverridesToClump(clump, obj->m_clump, overrides);
}

void*
ObjectInst::CreateRwObject(void)
{
	rw::Frame *f;
	rw::Atomic *atomic;
	rw::Clump *clump;
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil) return nil;

	if(obj->m_type == ObjectDef::ATOMIC){
		if(obj->m_atomics[0] == nil)
			return nil;
		atomic = obj->m_atomics[0]->clone();
		f = rw::Frame::create();
		atomic->setFrame(f);
		f->transform(&m_matrix, rw::COMBINEREPLACE);
		m_rwObject = atomic;
	}else if(obj->m_type == ObjectDef::CLUMP){
		if(obj->m_clump == nil)
			return nil;
		clump = obj->m_clump->clone();
		f = clump->getFrame();
		f->transform(&m_matrix, rw::COMBINEREPLACE);
		SetupAnimatedClump(this, clump);
		m_rwObject = clump;
		if(m_isVehicle){
			SAPaths::PrepareEditorVehicleVisuals(clump);
			AddEditorVehicleComponents(clump, m_vehicleMods);
			bool privateGeometry = true;
			FORLIST(lnk, clump->atomics){
				rw::Atomic *vehicleAtomic = rw::Atomic::fromClump(lnk);
				if(vehicleAtomic == nil || vehicleAtomic->geometry == nil)
					continue;
				rw::Geometry *copy = cloneGeometryForMaterial(vehicleAtomic->geometry);
				if(copy)
					vehicleAtomic->setGeometry(copy, 0);
				else
					privateGeometry = false;
			}
			if(privateGeometry){
				rwFree(m_vehicleColorRoles);
				m_vehicleColorRoles = nil;
				m_numVehicleColorRoles = 0;
				SAPaths::PrepareEditorVehicleClump(clump, m_vehicleColor1,
					m_vehicleColor2, &m_vehicleColorRoles, &m_numVehicleColorRoles);
				ApplyEditorVehiclePaintjob(clump, m_objectId, m_vehiclePaintjob);
				if(m_vehiclePlate[0])
					ApplyEditorVehiclePlate(clump, m_vehiclePlate);
			}
		}
	}
	if(HasObjectMaterialOverrides(this))
		RefreshMaterialOverrides();
	return m_rwObject;
}

void
ObjectInst::RefreshVehicleColors(void)
{
	if(!m_isVehicle || m_rwObject == nil)
		return;
	rw::Clump *clump = (rw::Clump*)m_rwObject;
	SAPaths::ApplyEditorVehicleClumpColors(clump, m_vehicleColorRoles,
		m_numVehicleColorRoles, m_vehicleColor1, m_vehicleColor2);
}

// Paintjobs and parts change the model itself, so it is built again.
void
ObjectInst::RefreshVehicleUpgrades(void)
{
	if(!m_isVehicle || m_rwObject == nil)
		return;
	((rw::Clump*)m_rwObject)->destroy();
	m_rwObject = nil;
	CreateRwObject();
}

void
ObjectInst::RefreshVehiclePlate(void)
{
	if(!m_isVehicle || m_rwObject == nil)
		return;
	if(m_vehiclePlate[0] == '\0'){
		RefreshVehicleUpgrades();
		return;
	}
	ApplyEditorVehiclePlate((rw::Clump*)m_rwObject, m_vehiclePlate);
}

void
ObjectInst::Init(FileObjectInstance *fi)
{
	m_editorDocumentId = 0;
	m_objectId = fi->objectId;
	m_savedObjectId = fi->objectId;
	ObjectDef *obj = GetObjectDef(m_objectId);
	m_isVehicle = obj && obj->m_isVehicle;
	m_vehicleStatic = true;
	m_vehicleColor1 = -1;
	m_vehicleColor2 = -1;
	m_vehicleRespawnDelay = 100;
	if(fi->area & 0x100) m_isUnimportant = true;
	if(fi->area & 0x400) m_isUnderWater = true;
	if(fi->area & 0x800) m_isTunnel = true;
	if(fi->area & 0x1000) m_isTunnelTransition = true;
	m_area = fi->area & 0xFF;
	m_rotation = fi->rotation;
	m_translation = fi->position;
	m_origTranslation = fi->position;
	m_origRotation = fi->rotation;
	m_savedTranslation = fi->position;
	m_savedRotation = fi->rotation;
	m_savedStateValid = true;
	m_iplRotation = !((fi->area & 0x200) && fi->rotation.x != 0.0f && fi->rotation.y != 0.0f);
	m_wasSavedDeleted = false;
	m_gameEntityExists = true;
	m_lodId = fi->lod;
	UpdateMatrix();
}

void
ObjectInst::SetupBigBuilding(void)
{
	m_isBigBuilding = true;
}

CRect
ObjectInst::GetBoundRect(void)
{
	CRect rect;
	rw::V3d v;
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return rect;
	CColModel *col = obj->m_colModel;

	for(int ix = 0; ix < 2; ix++)
	for(int iy = 0; iy < 2; iy++)
	for(int iz = 0; iz < 2; iz++){
		v.x = ix ? col->boundingBox.max.x : col->boundingBox.min.x;
		v.y = iy ? col->boundingBox.max.y : col->boundingBox.min.y;
		v.z = iz ? col->boundingBox.max.z : col->boundingBox.min.z;
		rw::V3d::transformPoints(&v, &v, 1, &m_matrix);
		rect.ContainPoint(v);
	}

	return rect;
}

bool
ObjectInst::IsOnScreen(void)
{
	rw::Sphere sph;
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil)
		return false;
	if(obj->m_colModel == nil)
		return true;
	CColModel *col = obj->m_colModel;
	sph.center = col->boundingSphere.center;
	sph.radius = col->boundingSphere.radius;
	return TheCamera.IsSphereVisible(&sph, &m_matrix);
}

static void
updateMatFXAnim(rw::Material *m)
{
	if(!rw::UVAnim::exists(m))
		return;
	rw::UVAnim::addTime(m, timeStep);
	rw::UVAnim::applyUpdate(m);
}

void
ObjectInst::PreRender(void)
{
	int i;
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil)
		return;
	if(obj->m_type == ObjectDef::ATOMIC){
		// Model materials are shared by ordinary instances, so update those
		// once per frame. A material override owns a private geometry, however,
		// and its copied UVAnim interpolators must advance independently. The
		// model-level latch cannot be used for that instance or a text override
		// can remain stuck at its initial UV frame.
		bool hasPrivateMaterials = m_materialCustomGeometry != nil;
		if(obj->m_hasPreRendered && !hasPrivateMaterials)
			return;
		if(!hasPrivateMaterials)
			obj->m_hasPreRendered = true;

		if(gPlayAnimations){
			rw::Atomic *atm = (rw::Atomic*)m_rwObject;
			if(atm && atm->geometry)
				for(i = 0; i < atm->geometry->matList.numMaterials; i++)
					updateMatFXAnim(atm->geometry->matList.materials[i]);
		}
	}else if(obj->m_type == ObjectDef::CLUMP && m_rwObject){
		if(gPlayAnimations)
			m_animTime += timeStep;
		ApplyAnimatedClump(this);
	}
}

void
ObjectInst::JumpTo(void)
{
	rw::V3d center;
	ObjectDef *obj = GetObjectDef(m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return;
	CSphere *sph = &obj->m_colModel->boundingSphere;
	rw::V3d::transformPoints(&center, &sph->center, 1, &m_matrix);
	TheCamera.setTarget(center);
	TheCamera.setDistanceFromTarget(TheCamera.minDistToSphere(sph->radius));
}

void
ObjectInst::Select(void)
{
	if(!IsInstInIplMapDocument(this)){
		// Direct clicks and rectangle/list selection all converge here. Rate-limit
		// the explanation so a drag across many reference objects yields one toast.
		static double lastLockedReferenceToast = -100.0;
		double now = ImGui::GetTime();
		if(now - lastLockedReferenceToast >= 1.0){
			if(IsIplMapDocumentOpen())
				ToastFor(TOAST_SELECTION, 3.0f,
				         _("Reference object — only the opened IPL is editable."));
			else if(m_file && IsExternalIplMapLogicalPath(m_file->name))
				ToastFor(TOAST_SELECTION, 4.0f,
				         _("Locked external reference — reopen its source IPL to edit it."));
			lastLockedReferenceToast = now;
		}
		return;
	}
	if(m_selected) return;
	static uint32 selectSeq;
	m_selected = true;
	m_selectSeq = ++selectSeq;
	selection.InsertItem(this);
	if(isSA()){
		ObjectInst *target = m_isVehicle || IsPlayerAttachmentTarget(this) ? this : GetAttachParent(this);
		if(target && (target->m_isVehicle || IsPlayerAttachmentTarget(target)))
			gAttachmentWorkspaceSelectionPending = true;
	}
}

void
ObjectInst::Deselect(void)
{
	CPtrNode *p;
	if(!m_selected) return;
	m_selected = false;
	for(p = selection.first; p; p = p->next)
		if(p->item == this){
			selection.RemoveNode(p);
			return;
		}
}

void
ClearSelection(void)
{
	CPtrNode *p, *next;
	for(p = selection.first; p; p = next){
		next = p->next;
		((ObjectInst*)p->item)->m_selected = false;
		selection.RemoveNode(p);
	}
}

void
ObjectInst::Delete(void)
{
	if(!IsInstInIplMapDocument(this)) return;
	if(m_isDeleted) return;
	m_isDeleted = true;
	StampChangeSeq(this);
	Deselect();

	// If this HD building was the last live child of its LOD, delete the LOD too.
	ObjectInst *lodInst = resolveInstanceLod(this);
	if(lodInst && !lodInst->m_isDeleted &&
	   countLiveLodChildren(lodInst, this) == 0)
		lodInst->Delete();

	// If this IS a LOD, delete all HD buildings that reference it
	CPtrNode *p;
	for(p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(instanceReferencesLod(other, this) && !other->m_isDeleted)
			other->Delete();
	}
}

void
ObjectInst::Undelete(void)
{
	if(!IsInstInIplMapDocument(this)) return;
	if(!m_isDeleted) return;
	m_isDeleted = false;
	StampChangeSeq(this);
	if(m_imageIndex >= 0 && m_wasSavedDeleted)
		m_isDirty = true;

	// Also undelete the LOD pair
	ObjectInst *lodInst = resolveInstanceLod(this);
	if(lodInst && lodInst->m_isDeleted)
		lodInst->Undelete();

	// If this IS a LOD, undelete HD children
	CPtrNode *p;
	for(p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(instanceReferencesLod(other, this) && other->m_isDeleted)
			other->Undelete();
	}
}

static void
setDeletedWithoutCascade(ObjectInst *inst, bool deleted)
{
	if(inst == nil || inst->m_isDeleted == deleted)
		return;
	inst->m_isDeleted = deleted;
	StampChangeSeq(inst);
	if(deleted)
		inst->Deselect();
	else if(inst->m_imageIndex >= 0 && inst->m_wasSavedDeleted)
		inst->m_isDirty = true;
}

// Back to the unmodified game world: placed objects and vehicles are
// removed, world objects get their original transform, materials and
// properties back. Used by File > New and before opening a map.
static std::vector<PawnAreaRemoval> gAreaRemovals;

const std::vector<PawnAreaRemoval>&
GetPawnAreaRemovals(void)
{
	return gAreaRemovals;
}

static bool
insideRemovalSphere(const ObjectInst *inst, float x, float y, float z, float radius)
{
	float dx = inst->m_origTranslation.x - x;
	float dy = inst->m_origTranslation.y - y;
	float dz = inst->m_origTranslation.z - z;
	return dx*dx + dy*dy + dz*dz <= radius*radius;
}

void
SetPawnAreaRemovals(const std::vector<PawnAreaRemoval> &removals)
{
	gAreaRemovals = removals;
	BumpChangeSeq();
}

bool
InsidePawnAreaRemoval(const ObjectInst *inst)
{
	for(const PawnAreaRemoval &r : gAreaRemovals)
		if(insideRemovalSphere(inst, r.x, r.y, r.z, r.radius))
			return true;
	return false;
}

int
ResetPawnMap(void)
{
	SetPawnTextEncoding(PAWN_TEXT_CP1252);
	int changed = 0;
	// A new or reopened map has nothing left out yet.
	ClearLastPawnImportReport();
	ResetMapTextDraws();
	ResetMapToolsLine();
	ResetCoordinatePins();
	ResetLayoutPaths();
	ClearRadarArtLayers();
	gAreaRemovals.clear();
	ClearSelection();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		inst->m_editorDocumentId = 0;
		SetInstanceHidden(inst, false);
		bool world = IsPawnWorldObject(inst);
		if(!world && !inst->m_isDeleted){
			setDeletedWithoutCascade(inst, true);
			changed++;
		}
		if(inst->m_materialOverrides){
			releaseObjectMaterialOverrides(inst);
			inst->RefreshMaterialOverrides();
			changed++;
		}
		if(inst->m_pawnProperties){
			delete inst->m_pawnProperties;
			inst->m_pawnProperties = nil;
			changed++;
		}
		inst->m_attachApplied = false;
		if(!world)
			continue;
		if(inst->m_objectId != inst->m_savedObjectId){
			applyInstanceModelId(inst, inst->m_savedObjectId);
			changed++;
		}
		if(inst->m_isDeleted != inst->m_wasSavedDeleted){
			setDeletedWithoutCascade(inst, inst->m_wasSavedDeleted);
			changed++;
		}
		if(transformDiffers(inst->m_translation, inst->m_rotation,
			inst->m_savedTranslation, inst->m_savedRotation)){
			RemoveInstFromSectors(inst);
			inst->m_translation = inst->m_savedTranslation;
			inst->m_rotation = inst->m_savedRotation;
			inst->UpdateMatrix();
			updateRwFrameForInst(inst);
			InsertInstIntoSectors(inst);
			changed++;
		}
		inst->m_isDirty = false;
	}
	gAttachmentListDirty = true;
	ResetUndoHistory();
	return changed;
}

void
DeleteSelected(void)
{
	// Collect all instances that will be deleted (including LOD cascades)
	// First, gather the selected ones
	std::vector<ObjectInst*> toDelete;
	bool capped = false;
	CPtrNode *p, *next;
	for(p = selection.first; p; p = next){
		next = p->next;
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted){
			if((int)toDelete.size() >= MAX_BATCH_OBJECTS){
				capped = true;
				break;
			}
			toDelete.push_back(inst);
		}
	}
	if(toDelete.empty()) return;
	if(capped)
		Toast(TOAST_DELETE, _("Delete limited to first %d selected instance(s)"), MAX_BATCH_OBJECTS);

	// Record the exact transition set. Undo must not recursively undelete an HD
	// child that was already deleted before this action and shares the same LOD.
	std::vector<ObjectInst*> previouslyLive;
	for(p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst && !inst->m_isDeleted)
			previouslyLive.push_back(inst);
	}

	for(int i = 0; i < (int)toDelete.size(); i++)
		toDelete[i]->Delete();

	std::vector<ObjectInst*> newlyDeleted;
	for(size_t i = 0; i < previouslyLive.size(); i++)
		if(previouslyLive[i]->m_isDeleted)
			newlyDeleted.push_back(previouslyLive[i]);
	UndoRecordDelete(newlyDeleted.data(), (int)newlyDeleted.size());
}

int
DeleteAllInstances(void)
{
	int numDeleted = 0;
	ClearSelection();

	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted || !IsInstInIplMapDocument(inst))
			continue;
		// Whole-map delete intentionally skips per-instance LOD cascade work.
		// Every instance is being deleted in the same pass, so the recursive
		// ObjectInst::Delete path only adds O(n^2) scans on large SA maps.
		inst->m_isDeleted = true;
		StampChangeSeq(inst);
		inst->m_selected = false;
		numDeleted++;
	}
	return numDeleted;
}

// Object Spawner state
bool gPlaceMode;
bool gPlaceRepeat;
bool gPrefabPlaceMode;
float gPrefabPlaceHeading;
bool gPrefabPlaceRepeat;	// stay in placement after a click, as Shift does
bool gPrefabPlaceAsGroup;	// put what is placed in a group named after the prefab
static int spawnObjectId = -1;
static char prefabPlacePath[1024];
static GameFile *customIplFile = nil;
static const char *DEFAULT_CUSTOM_IPL_PATH = "data\\maps\\custom.ipl";
static const char *CUSTOM_IMPORT_IDE_PATH = "data/maps/haumea/custom.ide";
static const char *CUSTOM_IMPORT_IPL_PATH = "data/maps/haumea/custom.ipl";
static char currentCustomIplPath[256] = "data\\maps\\custom.ipl";
static char currentCustomIplSourcePath[1024];
// Pawn/open.mp authoring never needs to register an editor placement IPL in
// gta.dat. Keep this state false even when the editor is used without a game
// installation or a Haumea plugin runtime.
static bool currentCustomIplAppendToDat = false;

#define LOD_LOOKUP_SIZE 20000
static int lodLookup[LOD_LOOKUP_SIZE];

// Which LOD model goes with a building model, for placing and copying
// buildings: read from the loaded world's own LOD links (the IPLs' LOD
// indices), so it follows the user's game files. A model whose placements
// use different LODs gets the one most of them use, or none without a
// majority (the derricks, each with its own LOD).
void
InitLodLookup(void)
{
	memset(lodLookup, -1, sizeof(lodLookup));
	if(!isSA()) return;
	std::map<int, std::map<int, int>> uses;	// building -> LOD -> placements
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_lod && inst->m_objectId >= 0 && inst->m_objectId < LOD_LOOKUP_SIZE)
			uses[inst->m_objectId][inst->m_lod->m_objectId]++;
	}
	int linked = 0;
	for(const auto &model : uses){
		int total = 0, best = -1, bestCount = 0;
		for(const auto &lod : model.second){
			total += lod.second;
			if(lod.second > bestCount){
				best = lod.first;
				bestCount = lod.second;
			}
		}
		if(bestCount * 2 > total){
			lodLookup[model.first] = best;
			linked++;
		}
	}
	log("LOD links: %d building models from the world's IPLs\n", linked);
}

int
GetSpawnObjectId(void)
{
	return spawnObjectId;
}

static int spawnElementKind = SAMP_ELEMENT_OBJECT;

void
SetSpawnElementKind(int kind)
{
	spawnElementKind = kind >= 0 && kind < NUM_SAMP_ELEMENT_KINDS ? kind : SAMP_ELEMENT_OBJECT;
}

int
GetSpawnElementKind(void)
{
	return spawnElementKind;
}

int
GetSampElementProxyModel(int kind)
{
	switch(kind){
	case SAMP_ELEMENT_LABEL: return 1239;	// info
	case SAMP_ELEMENT_GANGZONE: return 1313;	// killfrenzy2plyr
	case SAMP_ELEMENT_MAPICON: return 1318;	// arrow
	case SAMP_ELEMENT_PICKUP: return 1239;
	case SAMP_ELEMENT_ACTOR:
	case SAMP_ELEMENT_CLASS:
	case SAMP_ELEMENT_NPC: return 7;	// male01; skin 0 (CJ) has no model file
	case SAMP_ELEMENT_PATHPOINT: return 1559;	// diamond_3, the yellow marker
	case SAMP_ELEMENT_SOUND: return 2232;	// MED_SPEAKER_4
	case SAMP_ELEMENT_CHECKPOINT: return 1317;	// Cylinder, the game's marker
	case SAMP_ELEMENT_AREA: return 1313;	// never drawn: an editor icon and its outline
	}
	return -1;
}

const char*
GetSampElementKindName(int kind)
{
	switch(kind){
	case SAMP_ELEMENT_PICKUP: return N_("Pickup");
	case SAMP_ELEMENT_LABEL: return N_("3D text label");
	case SAMP_ELEMENT_ACTOR: return N_("Actor");
	case SAMP_ELEMENT_CLASS: return N_("Player class");
	case SAMP_ELEMENT_GANGZONE: return N_("Gang zone");
	case SAMP_ELEMENT_MAPICON: return N_("Map icon");
	case SAMP_ELEMENT_NPC: return N_("NPC");
	case SAMP_ELEMENT_PATHPOINT: return N_("NPC path point");
	case SAMP_ELEMENT_SOUND: return N_("Sound");
	case SAMP_ELEMENT_CHECKPOINT: return N_("Checkpoint");
	case SAMP_ELEMENT_AREA: return N_("Dynamic area");
	}
	return N_("Object");
}

int
GetSampElementKind(const ObjectInst *inst)
{
	if(inst == nil || inst->m_isVehicle)
		return SAMP_ELEMENT_OBJECT;
	return GetObjectPawnProperties(inst)->elementKind;
}

int
GetSampElementDisplayModel(int kind, int elementModel)
{
	ObjectDef *obj = GetObjectDef(elementModel);
	if(kind == SAMP_ELEMENT_ACTOR || kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_NPC){
		if(obj && obj->m_isPed)
			return elementModel;
	}else if(kind == SAMP_ELEMENT_PICKUP){
		if(obj && !obj->m_isPed && !obj->m_isVehicle)
			return elementModel;
	}else
		return GetSampElementProxyModel(kind);
	return GetSampElementProxyModel(kind);
}

static void
destroyInstanceRwObject(ObjectInst *inst, ObjectDef *obj)
{
	if(inst->m_rwObject == nil)
		return;
	if(obj->m_type == ObjectDef::ATOMIC){
		rw::Atomic *atomic = (rw::Atomic*)inst->m_rwObject;
		rw::Frame *frame = atomic->getFrame();
		atomic->destroy();
		if(frame)
			frame->destroy();
	}else
		((rw::Clump*)inst->m_rwObject)->destroy();
	inst->m_rwObject = nil;
}

ObjectInst*
GetAttachmentWorkspacePlayerPreview(void)
{
	if(!gAttachmentWorkspacePlayerMode)
		return nil;
	int model = GetSampElementDisplayModel(SAMP_ELEMENT_CLASS, gAttachmentWorkspacePlayerSkin);
	if(model < 0)
		return nil;
	if(gAttachmentPlayerPreview == nil){
		gAttachmentPlayerPreview = new ObjectInst;
		memset(gAttachmentPlayerPreview, 0, sizeof(*gAttachmentPlayerPreview));
		gAttachmentPlayerPreview->m_id = 0;
		gAttachmentPlayerPreview->m_lodId = -1;
		gAttachmentPlayerPreview->m_translation = TheCamera.m_target;
		gAttachmentPlayerPreview->m_rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		gAttachmentPlayerPreview->m_objectId = model;
		gAttachmentPlayerPreview->m_vehicleColor1 = -1;
		gAttachmentPlayerPreview->m_vehicleColor2 = -1;
		gAttachmentPlayerPreview->UpdateMatrix();
	}else if(gAttachmentPlayerPreview->m_objectId != model){
		destroyInstanceRwObject(gAttachmentPlayerPreview,
			GetObjectDef(gAttachmentPlayerPreview->m_objectId));
		gAttachmentPlayerPreview->m_objectId = model;
		gAttachmentPlayerPreview->m_translation = TheCamera.m_target;
		gAttachmentPlayerPreview->UpdateMatrix();
	}
	ObjectDef *obj = GetObjectDef(model);
	if(obj && !obj->IsLoaded())
		RequestObject(model);
	if(obj && obj->IsLoaded() && gAttachmentPlayerPreview->m_rwObject == nil)
		gAttachmentPlayerPreview->CreateRwObject();
	return gAttachmentPlayerPreview;
}

ObjectInst*
GetAttachmentWorkspaceTarget(void)
{
	return gAttachmentWorkspaceActive ? gAttachmentWorkspaceTarget : nil;
}

static void
saveAttachmentWorkspaceCamera(void)
{
	if(gAttachmentWorkspaceCameraSaved)
		return;
	gAttachmentWorkspaceSavedCameraPosition = TheCamera.m_position;
	gAttachmentWorkspaceSavedCameraTarget = TheCamera.m_target;
	gAttachmentWorkspaceSavedCameraUp = TheCamera.m_up;
	gAttachmentWorkspaceSavedCameraLocalUp = TheCamera.m_localup;
	gAttachmentWorkspaceSavedCameraFov = TheCamera.m_fov;
	gAttachmentWorkspaceCameraSaved = true;
}

static void
restoreAttachmentWorkspaceCamera(void)
{
	if(!gAttachmentWorkspaceCameraSaved)
		return;
	TheCamera.m_position = gAttachmentWorkspaceSavedCameraPosition;
	TheCamera.m_target = gAttachmentWorkspaceSavedCameraTarget;
	TheCamera.m_up = gAttachmentWorkspaceSavedCameraUp;
	TheCamera.m_localup = gAttachmentWorkspaceSavedCameraLocalUp;
	TheCamera.m_fov = gAttachmentWorkspaceSavedCameraFov;
	TheCamera.update();
	gAttachmentWorkspaceCameraSaved = false;
	gAttachmentWorkspaceNeedsFocus = false;
}

static bool
focusAttachmentWorkspaceTarget(int view)
{
	ObjectInst *target = GetAttachmentWorkspacePlayerPreview();
	if(target == nil)
		target = gAttachmentWorkspaceTarget;
	if(target == nil || target->m_isDeleted)
		return false;

	ObjectDef *obj = GetObjectDef(target->m_objectId);
	if(obj == nil)
		return false;
	if(!obj->IsLoaded()){
		RequestObject(target->m_objectId);
		return false;
	}

	rw::V3d center = target->m_translation;
	float radius = obj->m_isPed ? 1.0f : 2.0f;
	if(obj->m_colModel){
		const CSphere &sphere = obj->m_colModel->boundingSphere;
		rw::V3d::transformPoints(&center, &sphere.center, 1, &target->m_matrix);
		float scaleX = length(target->m_matrix.right);
		float scaleY = length(target->m_matrix.up);
		float scaleZ = length(target->m_matrix.at);
		radius = sphere.radius * max(scaleX, max(scaleY, scaleZ));
	}
	if(!std::isfinite(radius) || radius < 0.25f)
		radius = 1.0f;

	TheCamera.setTarget(center);
	float distance = max(TheCamera.minDistToSphere(radius * 1.15f), radius * 1.5f);
	TheCamera.setDistanceFromTarget(distance);
	const float d = TheCamera.distanceToTarget();
	// GTA models face local +Y; local +X is the model's right side.
	rw::V3d forward = normalize(target->m_matrix.up);
	rw::V3d right = normalize(target->m_matrix.right);
	rw::V3d up = normalize(target->m_matrix.at);
	switch(view){
	case ATTACHMENT_VIEW_FRONT:
		TheCamera.m_up = TheCamera.m_localup = up;
		TheCamera.m_position = add(center, scale(forward, d));
		break;
	case ATTACHMENT_VIEW_BACK:
		TheCamera.m_up = TheCamera.m_localup = up;
		TheCamera.m_position = sub(center, scale(forward, d));
		break;
	case ATTACHMENT_VIEW_LEFT:
		TheCamera.m_up = TheCamera.m_localup = up;
		TheCamera.m_position = sub(center, scale(right, d));
		break;
	case ATTACHMENT_VIEW_RIGHT:
		TheCamera.m_up = TheCamera.m_localup = up;
		TheCamera.m_position = add(center, scale(right, d));
		break;
	case ATTACHMENT_VIEW_TOP:
		TheCamera.m_up = TheCamera.m_localup = { 0.0f, 1.0f, 0.0f };
		TheCamera.m_position = { center.x, center.y, center.z + d };
		break;
	default:
		break;
	}
	TheCamera.update();
	return true;
}

void
OpenAttachmentWorkspace(ObjectInst *target, bool voidView)
{
	if(target && target->m_isDeleted){
		CloseAttachmentWorkspace();
		return;
	}
	if(target && GetObjectPawnProperties(target)->attachParent == PAWN_ATTACH_PLAYER){
		OpenPlayerAttachmentWorkspace(GetObjectPawnProperties(target)->attachSkin, voidView);
		return;
	}
	CancelAttachmentTargetPick();
	if(gAttachmentWorkspaceActive && !gAttachmentWorkspacePlayerMode && gAttachmentWorkspaceTarget == target){
		SetAttachmentWorkspaceVoid(voidView && target != nil);
		return;
	}
	if(gAttachmentWorkspaceVoid)
		restoreAttachmentWorkspaceCamera();
	gAttachmentWorkspaceActive = true;
	gAttachmentWorkspaceVoid = false;
	gAttachmentWorkspacePlayerMode = false;
	gAttachmentWorkspaceTarget = target;
	gAttachmentWorkspaceNeedsFocus = false;
	if(voidView && target)
		SetAttachmentWorkspaceVoid(true);
}

void
OpenPlayerAttachmentWorkspace(int skin, bool voidView)
{
	if(gAttachmentWorkspaceVoid)
		restoreAttachmentWorkspaceCamera();
	gAttachmentWorkspaceActive = true;
	gAttachmentWorkspaceVoid = false;
	gAttachmentWorkspacePlayerMode = true;
	gAttachmentWorkspacePlayerSkin = clamp(skin, 0, 311);
	gAttachmentWorkspacePlayerSkinSet = true;
	gAttachmentWorkspaceTarget = GetAttachmentWorkspacePlayerPreview();
	if(gAttachmentWorkspaceTarget){
		RequestObject(gAttachmentWorkspaceTarget->m_objectId);
	}
	gAttachmentWorkspaceNeedsFocus = false;
	if(voidView)
		SetAttachmentWorkspaceVoid(true);
	UpdateObjectAttachments();
}

void
CloseAttachmentWorkspace(void)
{
	gAttachmentWorkspaceSelectionPending = false;
	restoreAttachmentWorkspaceCamera();
	gAttachmentWorkspaceActive = false;
	gAttachmentWorkspaceVoid = false;
	gAttachmentWorkspacePlayerMode = false;
	gAttachmentWorkspaceTarget = nil;
	gAttachmentWorkspaceNeedsFocus = false;
}

bool
IsAttachmentWorkspaceActive(void)
{
	return gAttachmentWorkspaceActive;
}

bool
AttachmentWorkspaceHidesWorld(void)
{
	return gAttachmentWorkspaceActive && gAttachmentWorkspaceVoid;
}

void
SetAttachmentWorkspaceVoid(bool voidView)
{
	if(!gAttachmentWorkspaceActive || gAttachmentWorkspaceVoid == voidView)
		return;
	if(voidView){
		saveAttachmentWorkspaceCamera();
		gAttachmentWorkspaceVoid = true;
		gAttachmentWorkspaceNeedsFocus = true;
		gAttachmentWorkspacePendingCameraView = ATTACHMENT_VIEW_FOCUS;
	}else{
		gAttachmentWorkspaceVoid = false;
		restoreAttachmentWorkspaceCamera();
	}
}

void
SetAttachmentWorkspaceCameraView(int view)
{
	if(!gAttachmentWorkspaceActive || view < ATTACHMENT_VIEW_FOCUS || view > ATTACHMENT_VIEW_TOP)
		return;
	gAttachmentWorkspacePendingCameraView = view;
	gAttachmentWorkspaceNeedsFocus = true;
}

void
UpdateAttachmentWorkspace(void)
{
	// Selection helpers may visit a ped or vehicle in the middle of a range.
	// Wait for the complete selection before opening a window over the list.
	if(gAttachmentWorkspaceSelectionPending){
		gAttachmentWorkspaceSelectionPending = false;
		if(selection.first && selection.first->next == nil){
			ObjectInst *selected = (ObjectInst*)selection.first->item;
			ObjectInst *target = selected->m_isVehicle || IsPlayerAttachmentTarget(selected) ?
				selected : GetAttachParent(selected);
			if(target && !target->m_isDeleted && (target->m_isVehicle || IsPlayerAttachmentTarget(target)))
				OpenAttachmentWorkspace(target, AttachmentWorkspaceHidesWorld());
		}
	}
	if(!gAttachmentWorkspaceActive)
		return;
	if(gAttachmentWorkspaceTarget && gAttachmentWorkspaceTarget->m_isDeleted){
		CloseAttachmentWorkspace();
		return;
	}
	if(gAttachmentWorkspacePlayerMode)
		gAttachmentWorkspaceTarget = GetAttachmentWorkspacePlayerPreview();
	if(gAttachmentWorkspaceTarget && !gAttachmentWorkspaceTarget->m_isDeleted){
		ObjectDef *obj = GetObjectDef(gAttachmentWorkspaceTarget->m_objectId);
		if(obj && !obj->IsLoaded())
			RequestObject(gAttachmentWorkspaceTarget->m_objectId);
	}
	if(gAttachmentWorkspaceNeedsFocus &&
	   focusAttachmentWorkspaceTarget(gAttachmentWorkspacePendingCameraView))
		gAttachmentWorkspaceNeedsFocus = false;
}

int
GetAttachmentWorkspacePlayerSkin(void)
{
	if(gAttachmentWorkspacePlayerSkinSet)
		return gAttachmentWorkspacePlayerSkin;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(!inst->m_isDeleted && properties->attachParent == PAWN_ATTACH_PLAYER)
			return properties->attachSkin;
	}
	return 1;
}

void
SetAttachmentWorkspacePlayerSkin(int skin)
{
	gAttachmentWorkspacePlayerSkin = clamp(skin, 0, 311);
	gAttachmentWorkspacePlayerSkinSet = true;
	if(gAttachmentWorkspacePlayerMode){
		gAttachmentWorkspaceTarget = GetAttachmentWorkspacePlayerPreview();
		if(gAttachmentWorkspaceTarget){
			RequestObject(gAttachmentWorkspaceTarget->m_objectId);
		}
		if(gAttachmentWorkspaceVoid){
			gAttachmentWorkspaceNeedsFocus = true;
			gAttachmentWorkspacePendingCameraView = ATTACHMENT_VIEW_FOCUS;
		}
	}
	UpdateObjectAttachments();
}

bool
AttachmentWorkspaceShowsInstance(const ObjectInst *inst)
{
	if(inst == nil)
		return false;
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(!gAttachmentWorkspaceActive || !gAttachmentWorkspaceVoid)
		return true;
	if(properties->attachParent == PAWN_ATTACH_PLAYER)
		return gAttachmentWorkspacePlayerMode;
	if(inst == gAttachmentWorkspaceTarget)
		return true;
	for(int depth = 0; depth < 64; depth++){
		ObjectInst *parent = GetAttachParent(inst);
		if(parent == nil)
			return false;
		if(parent == gAttachmentWorkspaceTarget)
			return true;
		inst = parent;
	}
	return false;
}

// Swaps the model drawing a pickup, actor or player class. Elements have no
// LODs or material edits, so only the RW object and sector entry change.
static void
setInstanceElementModel(ObjectInst *inst, int modelId)
{
	ObjectDef *obj = GetObjectDef(modelId);
	ObjectDef *old = GetObjectDef(inst->m_objectId);
	if(obj == nil || old == nil || modelId == inst->m_objectId ||
	   obj->m_isVehicle || old->m_isVehicle || HasObjectMaterialOverrides(inst))
		return;
	destroyInstanceRwObject(inst, old);
	RemoveInstFromSectors(inst);
	inst->m_objectId = modelId;
	if(!obj->IsLoaded()){
		RequestObject(modelId);
		LoadAllRequestedObjects();
	}
	inst->CreateRwObject();
	if(obj->m_colModel)
		InsertInstIntoSectors(inst);
	else
		outOfBoundsSector.buildings.InsertItem(inst);
}

bool
CanChangeInstanceModelId(ObjectInst *inst)
{
	if(inst == nil || inst->m_isDeleted || inst->m_isVehicle ||
	   GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT || !IsInstInIplMapDocument(inst) ||
	   inst->m_lodId >= 0 || inst->m_lod != nil)
		return false;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other != inst && !other->m_isDeleted && instanceReferencesLod(other, inst))
			return false;
	}
	return true;
}

static void
applyInstanceModelId(ObjectInst *inst, int editorModelId)
{
	ObjectDef *old = GetObjectDef(inst->m_objectId);
	ObjectDef *obj = GetObjectDef(editorModelId);
	if(old == nil || obj == nil || inst->m_objectId == editorModelId)
		return;
	if(!obj->IsLoaded()){
		RequestObject(editorModelId);
		LoadAllRequestedObjects();
	}

	RemoveInstFromSectors(inst);
	if(inst->m_animState){
		delete (ObjectAnimState*)inst->m_animState;
		inst->m_animState = nil;
	}
	inst->m_animTime = 0.0f;
	destroyInstanceRwObject(inst, old);
	// Material overrides are cleared in the same undo group as the model swap.
	// The private geometry pointers belong to the old rendered object.
	inst->m_materialGeometryApplied = false;
	inst->m_materialBaseGeometry = nil;
	inst->m_materialCustomGeometry = nil;
	inst->m_objectId = editorModelId;
	inst->m_isBigBuilding = false;
	if(obj->m_isBigBuilding)
		inst->SetupBigBuilding();
	inst->UpdateMatrix();
	inst->CreateRwObject();
	inst->RefreshMaterialOverrides();
	if(obj->m_colModel)
		InsertInstIntoSectors(inst);
	else{
		CPtrList *list = inst->m_isBigBuilding ? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
		list->InsertItem(inst);
	}
	StampChangeSeq(inst);
	inst->m_isDirty = true;
}

bool
ChangeInstanceModelId(ObjectInst *inst, int editorModelId, bool recordUndo)
{
	if(!CanChangeInstanceModelId(inst))
		return false;
	ObjectDef *obj = GetObjectDef(editorModelId);
	if(obj == nil || obj->m_isVehicle)
		return false;
	int oldModelId = inst->m_objectId;
	if(oldModelId == editorModelId)
		return true;

	if(recordUndo)
		UndoBeginGroup();
	if(recordUndo)
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
			const ObjectMaterialOverride *material = GetObjectMaterialOverride((const ObjectInst*)inst, slot);
			if(material && (material->active || material->textEnabled))
				ClearObjectMaterialOverride(inst, slot, true);
		}
	applyInstanceModelId(inst, editorModelId);
	if(recordUndo){
		UndoRecordModelId(inst, oldModelId);
		UndoEndGroup();
	}
	return true;
}

void
InitSampElementProperties(ObjectPawnProperties *properties, int kind)
{
	if(properties == nil)
		return;
	properties->elementKind = kind;
	switch(kind){
	case SAMP_ELEMENT_PICKUP:
		properties->pickupType = 1;
		break;
	case SAMP_ELEMENT_LABEL:
		if(properties->labelText[0] == '\0')
			strcpy(properties->labelText, "3D text label");
		properties->labelColor = 0xFFFFFFFF;
		if(properties->drawDistance <= 0.0f)
			properties->drawDistance = 20.0f;
		break;
	case SAMP_ELEMENT_ACTOR:
		properties->actorInvulnerable = true;
		properties->actorHealth = 100.0f;
		break;
	case SAMP_ELEMENT_CLASS:
		properties->classTeam = -1;
		break;
	case SAMP_ELEMENT_GANGZONE:
		properties->zoneSizeX = 50.0f;
		properties->zoneSizeY = 50.0f;
		properties->zoneColor = 0xFF000088;
		break;
	case SAMP_ELEMENT_MAPICON:
		properties->mapIconType = 56;	// the red radar dot
		properties->mapIconColor = 0xFFFFFFFF;
		properties->mapIconStyle = 0;
		break;
	case SAMP_ELEMENT_NPC:
		// NPC_Spawn's own defaults: full health, no armour, fists.
		properties->actorInvulnerable = false;
		properties->actorHealth = 100.0f;
		properties->virtualWorld = 0;
		if(properties->npcName[0] == '\0')
			MakeUniqueNpcName("npc", properties->npcName, sizeof(properties->npcName));
		properties->npcMoveType = 2;	// NPC_MOVE_TYPE_JOG, the native's default
		properties->npcMoveSpeed = -1.0f;
		properties->npcNodeSetAngle = true;
		break;
	case SAMP_ELEMENT_PATHPOINT:
		if(properties->pathName[0] == '\0')
			snprintf(properties->pathName, sizeof(properties->pathName), "%s", spawnPathName);
		properties->pathOrder = nextPathOrder(properties->pathName);
		properties->pathStopRange = 0.2f;	// NPC_Move's default
		break;
	case SAMP_ELEMENT_SOUND:
		properties->soundKind = SAMP_SOUND_GAME;
		properties->soundId = 1097;	// background music
		properties->soundRadius = 30.0f;
		properties->soundUsePos = true;
		properties->soundStopOnLeave = true;
		break;
	case SAMP_ELEMENT_CHECKPOINT:
		if(properties->cpSize <= 0.0f)
			properties->cpSize = 5.0f;
		if(properties->cpKind == SAMP_CHECKPOINT_NORMAL && properties->cpRace[0] == '\0' &&
		   spawnRaceName[0]){
			properties->cpKind = SAMP_CHECKPOINT_RACE;
			snprintf(properties->cpRace, sizeof(properties->cpRace), "%s", spawnRaceName);
		}
		if(properties->cpKind == SAMP_CHECKPOINT_RACE){
			if(properties->cpRace[0] == '\0')
				strcpy(properties->cpRace, "race");
			properties->cpOrder = nextRaceOrder(properties->cpRace);
		}
		break;
	case SAMP_ELEMENT_AREA:
		// A 10 m box, 5 m tall, named so items can be shown only inside it.
		if(properties->areaRadius <= 0.0f)
			properties->areaRadius = 10.0f;
		if(properties->areaSize.x <= 0.0f || properties->areaSize.y <= 0.0f || properties->areaSize.z <= 0.0f){
			properties->areaShape = SAMP_AREA_CUBOID;
			properties->areaSize = { 10.0f, 10.0f, 5.0f };
		}
		if(properties->areaHeight <= 0.0f)
			properties->areaHeight = 5.0f;
		if(properties->name[0] == '\0')
			MakeUniqueAreaName(nil, properties->name, sizeof(properties->name));
		break;
	}
}

void
SetCustomPlacementIpl(const char *logicalPath, const char *sourcePath, bool addToDat)
{
	char normalizedLogical[256];
	char resolvedSource[1024];
	// Open Map owns the active placement target. Do not redirect inserts to the
	// legacy custom placement file while editing a document.
	if(IsIplMapDocumentOpen() &&
	   (logicalPath == nil || !LogicalPathEquals(logicalPath, GetIplMapDocumentLogicalPath())))
		return;
	// Placement files are editor bookkeeping for Pawn/open.mp authoring. Never
	// register one in the GTA data file, even if an old caller requests it.
	addToDat = false;

	if(logicalPath == nil || logicalPath[0] == '\0')
		logicalPath = DEFAULT_CUSTOM_IPL_PATH;

	strncpy(normalizedLogical, logicalPath, sizeof(normalizedLogical)-1);
	normalizedLogical[sizeof(normalizedLogical)-1] = '\0';
	for(char *p = normalizedLogical; *p; p++)
		if(*p == '/')
			*p = '\\';

	resolvedSource[0] = '\0';
	if(sourcePath && sourcePath[0]){
		strncpy(resolvedSource, sourcePath, sizeof(resolvedSource)-1);
		resolvedSource[sizeof(resolvedSource)-1] = '\0';
	}else if(!addToDat){
		BuildModloaderLogicalExportPath(logicalPath, resolvedSource, sizeof(resolvedSource));
	}

	bool sameLogical = LogicalPathEquals(currentCustomIplPath, normalizedLogical);
	bool sameSource = strcmp(currentCustomIplSourcePath, resolvedSource) == 0;
	if(!sameLogical || !sameSource || currentCustomIplAppendToDat != addToDat)
		customIplFile = nil;

	strncpy(currentCustomIplPath, normalizedLogical, sizeof(currentCustomIplPath)-1);
	currentCustomIplPath[sizeof(currentCustomIplPath)-1] = '\0';
	strncpy(currentCustomIplSourcePath, resolvedSource, sizeof(currentCustomIplSourcePath)-1);
	currentCustomIplSourcePath[sizeof(currentCustomIplSourcePath)-1] = '\0';
	currentCustomIplAppendToDat = addToDat;
}

void
SetSpawnObjectId(int id)
{
	spawnObjectId = id;
	ObjectDef *spawnObj = GetObjectDef(id);
	spawnElementKind = spawnObj && spawnObj->m_isPed ? SAMP_ELEMENT_ACTOR : SAMP_ELEMENT_OBJECT;
	// Open Map owns the placement target for the duration of the document.
	// Choosing a model in the Browser must not silently send new objects back
	// to the legacy custom.ipl file.
	if(IsIplMapDocumentOpen())
		return;
	ObjectDef *obj = GetObjectDef(id);
	if(obj && obj->m_file && LogicalPathEquals(obj->m_file->name, CUSTOM_IMPORT_IDE_PATH))
		SetCustomPlacementIpl(CUSTOM_IMPORT_IPL_PATH, nil, false);
	else
		SetCustomPlacementIpl(DEFAULT_CUSTOM_IPL_PATH, nil, false);
}

int
GetLodForObject(int id)
{
	if(isSA()){
		if(id >= 0 && id < LOD_LOOKUP_SIZE)
			return lodLookup[id];
		return -1;
	}else{
		ObjectDef *obj = GetObjectDef(id);
		if(obj && obj->m_relatedModel && !obj->m_isBigBuilding)
			return obj->m_relatedModel->m_id;
		return -1;
	}
}

static const char*
GetDatFilename(void)
{
	switch(gameversion){
	case GAME_III: return "data/gta3.dat";
	case GAME_VC:  return "data/gta_vc.dat";
	case GAME_SA:  return "data/gta.dat";
	case GAME_LCS: return "data/gta_lcs.dat";
	case GAME_VCS: return "data/gta_vcs.dat";
	default:       return nil;
	}
}

static bool
IplEntryExistsInDat(const char *datfile, const char *iplpath)
{
	FILE *f = fopen_ci(datfile, "r");
	if(f == nil) return false;
	char line[2048];
	while(fgets(line, sizeof(line), f)){
		// strip newline
		char *p = line;
		while(*p && *p != '\r' && *p != '\n') p++;
		*p = '\0';
		if(strncmp(line, "IPL ", 4) == 0){
			if(strcmp(line+4, iplpath) == 0){
				fclose(f);
				return true;
			}
		}
	}
	fclose(f);
	return false;
}

static void
AppendIplToDat(const char *iplpath)
{
	const char *datfile = GetDatFilename();
	if(datfile == nil) return;
	if(IplEntryExistsInDat(datfile, iplpath)) return;

	const char *targetPath = ModloaderGetSourcePath(datfile);
	if(targetPath == nil)
		targetPath = getPath(datfile);
	if(IsInGameDir(targetPath))
		return;	// Haumea never writes to the game folder
	FILE *f = fopen(targetPath, "a");
	if(f == nil){
		log("WARNING: could not append to %s\n", datfile);
		return;
	}
	fprintf(f, "\nIPL %s\n", iplpath);
	fclose(f);
	log("Added IPL %s to %s\n", iplpath, datfile);
}

static GameFile*
GetOrCreateCustomIplFile(void)
{
	if(customIplFile)
		return customIplFile;
	char path[256];
	strncpy(path, currentCustomIplPath, sizeof(path));
	path[sizeof(path)-1] = '\0';
	customIplFile = NewGameFile(path);
	if(currentCustomIplSourcePath[0]){
		free(customIplFile->sourcePath);
		customIplFile->sourcePath = strdup(currentCustomIplSourcePath);
	}
	if(currentCustomIplAppendToDat)
		AppendIplToDat(currentCustomIplPath);
	return customIplFile;
}

GameFile*
GetOrCreateCurrentPlacementIplFile(void)
{
	return GetOrCreateCustomIplFile();
}

static int
GetMaxIplIndexForFile(GameFile *file)
{
	int maxIplIndex = -1;
	CPtrNode *p;
	for(p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other->m_file && file &&
		   LogicalPathEquals(other->m_file->name, file->name) &&
		   other->m_imageIndex < 0){
			if(other->m_iplIndex > maxIplIndex)
				maxIplIndex = other->m_iplIndex;
		}
	}
	return maxIplIndex;
}

static ObjectInst*
createSpawnedInstance(int objectId, rw::V3d position, GameFile *file, int iplIndex,
	const rw::Quat *orientation = nil)
{
	ObjectInst *inst = AddInstance();
	inst->m_objectId = objectId;
	ObjectDef *obj = GetObjectDef(objectId);
	inst->m_isVehicle = obj && obj->m_isVehicle;
	inst->m_vehicleStatic = true;
	inst->m_vehicleColor1 = -1;
	inst->m_vehicleColor2 = -1;
	inst->m_vehicleRespawnDelay = 100;
	inst->m_area = currentArea;
	if(orientation){
		inst->m_rotation = *orientation;
	}else{
		inst->m_rotation.x = 0;
		inst->m_rotation.y = 0;
		inst->m_rotation.z = 0;
		inst->m_rotation.w = 1;
	}
	inst->m_translation = position;
	inst->m_lodId = -1;
	inst->m_lod = nil;
	inst->m_numChildren = 0;
	inst->m_file = file;
	inst->m_imageIndex = -1;
	inst->m_iplIndex = -1;
	inst->m_binInstIndex = -1;
	inst->m_iplIndex = iplIndex;
	SetInstIplFilterKey(inst, file ? file->name : nil);
	inst->m_isAdded = true;
	inst->m_iplRotation = IsIplMapDocumentOpen();
	inst->m_isDirty = true;
	inst->m_savedStateValid = false;
	inst->m_wasSavedDeleted = false;
	inst->m_gameEntityExists = false;
	StampChangeSeq(inst);

	if(obj && obj->m_isBigBuilding)
		inst->SetupBigBuilding();

	inst->UpdateMatrix();

	if(obj && !obj->IsLoaded()){
		RequestObject(objectId);
		LoadAllRequestedObjects();
	}

	inst->CreateRwObject();

	if(obj && obj->m_colModel)
		InsertInstIntoSectors(inst);
	else{
		CPtrList *list = inst->m_isBigBuilding
			? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
		list->InsertItem(inst);
	}

	return inst;
}

ObjectInst *SpawnDocumentInstance(int model, const rw::V3d &position, const rw::Quat &rotation)
{
	if (!GetObjectDef(model))
		return nil;
	SetCustomPlacementIpl(DEFAULT_CUSTOM_IPL_PATH, nil, false);
	GameFile *file = GetOrCreateCustomIplFile();
	return createSpawnedInstance(model, position, file, GetMaxIplIndexForFile(file) + 1, &rotation);
}
void RestoreDocumentTransform(ObjectInst *inst, const rw::V3d &position, const rw::Quat &rotation, bool deleted)
{
	RemoveInstFromSectors(inst);
	inst->m_translation = position;
	inst->m_rotation = rotation;
	inst->m_isDeleted = deleted;
	inst->m_isDirty = true;
	StampChangeSeq(inst);
	inst->UpdateMatrix();
	updateRwFrameForInst(inst);
	if (!deleted)
		InsertInstIntoSectors(inst);
}

static void
finalizeLinkedLod(ObjectInst *hdInst, ObjectInst *lodInst)
{
	ObjectDef *hdObj, *lodObj;

	hdInst->m_lodId = lodInst->m_iplIndex;
	hdInst->m_lod = lodInst;
	lodInst->m_numChildren = 1;

	if(!lodInst->m_isBigBuilding)
		lodInst->SetupBigBuilding();

	hdObj = GetObjectDef(hdInst->m_objectId);
	lodObj = GetObjectDef(lodInst->m_objectId);
	if(hdObj && lodObj && lodInst->m_numChildren == 1 && hdObj->m_colModel){
		lodObj->m_colModel = hdObj->m_colModel;
		lodObj->m_gotChildCol = true;
	}

	RemoveInstFromSectors(lodInst);
	InsertInstIntoSectors(lodInst);
}

// Core spawn helper: creates HD (+ optional LOD), applies orientation,
// selects the HD, and fills outInsts. Does NOT clear selection, does NOT
// record undo, does NOT emit a toast — caller handles those so scatter/brush
// can batch an entire stroke into one undo entry.
// Returns the number of insts written to outInsts (1 or 2).
int
SpawnPlaceObjectNoUndo(rw::V3d position, const rw::Quat *orientation,
	ObjectInst **outInsts, int outCapacity)
{
	if(spawnObjectId < 0) return 0;
	ObjectDef *obj = GetObjectDef(spawnObjectId);
	if(obj == nil) return 0;
	if(outInsts == nil || outCapacity <= 0) return 0;

	GameFile *file = GetOrCreateCurrentPlacementIplFile();
	int maxIdx = GetMaxIplIndexForFile(file);

	int lodObjId = -1;
	if(spawnElementKind != SAMP_ELEMENT_OBJECT){
		// Elements are single scripted entities without LODs.
	}else if(isSA()){
		if(spawnObjectId < LOD_LOOKUP_SIZE)
			lodObjId = lodLookup[spawnObjectId];
	}else{
		if(obj->m_relatedModel && !obj->m_isBigBuilding)
			lodObjId = obj->m_relatedModel->m_id;
	}

	int n = 0;
	ObjectInst *lodInst = nil;
	if(lodObjId >= 0 && GetObjectDef(lodObjId) && n < outCapacity){
		lodInst = createSpawnedInstance(lodObjId, position, file, ++maxIdx, orientation);
		outInsts[n++] = lodInst;
	}

	if(n >= outCapacity) return n;
	ObjectInst *hdInst = createSpawnedInstance(spawnObjectId, position, file, ++maxIdx, orientation);
	if(lodInst)
		finalizeLinkedLod(hdInst, lodInst);
	if(spawnElementKind != SAMP_ELEMENT_OBJECT && !hdInst->m_isVehicle){
		ObjectPawnProperties properties;
		InitObjectPawnProperties(&properties);
		InitSampElementProperties(&properties, spawnElementKind);
		properties.elementModel = spawnObjectId;
		SetObjectPawnProperties(hdInst, properties, false);
	}
	hdInst->Select();
	outInsts[n++] = hdInst;
	if(GetSampElementProxyModel(spawnElementKind) != spawnObjectId ||
	   spawnElementKind == SAMP_ELEMENT_OBJECT)
		NoteRecentObject(spawnObjectId);	// not the stand-ins for labels, zones and icons

	return n;
}

void
SpawnPlaceObject(rw::V3d position, const rw::Quat *orientation)
{
	if(spawnObjectId < 0) return;
	ObjectDef *obj = GetObjectDef(spawnObjectId);
	if(obj == nil) return;

	ObjectInst *pasted[4];
	ClearSelection();
	int numPasted = SpawnPlaceObjectNoUndo(position, orientation, pasted, 4);
	if(numPasted == 0) return;

	UndoRecordPaste(pasted, numPasted);
	Toast(TOAST_SPAWN, _("Placed %s"), obj->m_name);
}

void
SpawnExitPlaceMode(void)
{
	gPlaceMode = false;
	spawnObjectId = -1;
}

void
EnterPrefabPlaceMode(const char *path)
{
	if(path == nil || path[0] == '\0')
		return;
	strncpy(prefabPlacePath, path, sizeof(prefabPlacePath));
	prefabPlacePath[sizeof(prefabPlacePath)-1] = '\0';
	gPrefabPlaceMode = true;
	gPlaceMode = false;
}

void
ExitPrefabPlaceMode(void)
{
	gPrefabPlaceMode = false;
	prefabPlacePath[0] = '\0';
}

const char*
GetPrefabPlacePath(void)
{
	return prefabPlacePath;
}

// Object Browser categories: categories.cpp

// Favourites
static bool favourites[NUMOBJECTDEFS];

void
LoadFavourites(void)
{
	memset(favourites, 0, sizeof(favourites));
	FILE *f = fopenHaumeaDataRead("favourites.txt", "favourites.txt");
	if(f == nil) return;
	char line[64];
	while(fgets(line, sizeof(line), f)){
		int id = atoi(line);
		if(id >= 0 && id < NUMOBJECTDEFS)
			favourites[id] = true;
	}
	fclose(f);
}

void
SaveFavourites(void)
{
	FILE *f = fopenHaumeaDataWrite("favourites.txt");
	if(f == nil) return;
	for(int i = 0; i < NUMOBJECTDEFS; i++)
		if(favourites[i])
			fprintf(f, "%d\n", i);
	fclose(f);
}

bool
IsFavourite(int id)
{
	if(id < 0 || id >= NUMOBJECTDEFS) return false;
	return favourites[id];
}

void
ToggleFavourite(int id)
{
	if(id < 0 || id >= NUMOBJECTDEFS) return;
	favourites[id] = !favourites[id];
	SaveFavourites();
}

// Models the user placed most recently, newest first.
static int recentObjects[MAX_RECENT_OBJECTS];
static int numRecentObjects;

void
LoadRecentObjects(void)
{
	numRecentObjects = 0;
	FILE *f = fopenHaumeaDataRead("recent_objects.txt", "recent_objects.txt");
	if(f == nil) return;
	char line[64];
	while(numRecentObjects < MAX_RECENT_OBJECTS && fgets(line, sizeof(line), f)){
		int id = atoi(line);
		if(id >= 0 && id < NUMOBJECTDEFS)
			recentObjects[numRecentObjects++] = id;
	}
	fclose(f);
}

static void
SaveRecentObjects(void)
{
	FILE *f = fopenHaumeaDataWrite("recent_objects.txt");
	if(f == nil) return;
	for(int i = 0; i < numRecentObjects; i++)
		fprintf(f, "%d\n", recentObjects[i]);
	fclose(f);
}

// Moves id to the front. Brush strokes place the same model over and over,
// so the file is only written when the order changes.
void
NoteRecentObject(int id)
{
	if(id < 0 || id >= NUMOBJECTDEFS) return;
	if(numRecentObjects > 0 && recentObjects[0] == id) return;
	int i;
	for(i = 0; i < numRecentObjects; i++)
		if(recentObjects[i] == id)
			break;
	if(i == numRecentObjects && numRecentObjects < MAX_RECENT_OBJECTS)
		numRecentObjects++;
	if(i == numRecentObjects)
		i = numRecentObjects - 1;
	memmove(&recentObjects[1], &recentObjects[0], i * sizeof(int));
	recentObjects[0] = id;
	SaveRecentObjects();
}

int
GetRecentObjects(const int **ids)
{
	*ids = recentObjects;
	return numRecentObjects;
}

// Diff viewer — monotonic change counter for chronological ordering
static uint32 gChangeSeqCounter = 0;

uint32
BumpChangeSeq(void)
{
	return ++gChangeSeqCounter;
}

void
StampChangeSeq(ObjectInst *inst)
{
	inst->m_changeSeq = BumpChangeSeq();
}

uint32
GetLatestChangeSeq(void)
{
	return gChangeSeqCounter;
}

float
GetQuaternionSimilarity(const rw::Quat &a, const rw::Quat &b)
{
	float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
	float lenSqA = a.x*a.x + a.y*a.y + a.z*a.z + a.w*a.w;
	float lenSqB = b.x*b.x + b.y*b.y + b.z*b.z + b.w*b.w;
	if(lenSqA <= 1.0e-12f || lenSqB <= 1.0e-12f){
		float direct = (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) +
		               (a.z-b.z)*(a.z-b.z) + (a.w-b.w)*(a.w-b.w);
		float negated = (a.x+b.x)*(a.x+b.x) + (a.y+b.y)*(a.y+b.y) +
		                (a.z+b.z)*(a.z+b.z) + (a.w+b.w)*(a.w+b.w);
		return direct <= 1.0e-12f || negated <= 1.0e-12f ? 1.0f : 0.0f;
	}
	float similarity = fabsf(dot) / sqrtf(lenSqA*lenSqB);
	return similarity < 1.0f ? similarity : 1.0f;
}

// Diff viewer — compute bitmask of changes since last save
int
GetInstanceDiffFlags(ObjectInst *inst)
{
	if(!inst->m_savedStateValid){
		// Never saved — it's new (if not deleted)
		if(!inst->m_isDeleted)
			return DIFF_ADDED | (HasObjectMaterialOverrides(inst) ? DIFF_MATERIAL : 0) |
				(inst->m_pawnProperties ? DIFF_PROPERTIES : 0);
		return 0;	// created then deleted before save — ignore
	}
	if(inst->m_isDeleted && !inst->m_wasSavedDeleted)
		return DIFF_DELETED;
	if(inst->m_isDeleted)
		return 0;	// was deleted at save, still deleted

	int flags = 0;
	if(!inst->m_isDeleted && inst->m_wasSavedDeleted)
		flags |= DIFF_RESTORED;
	if(inst->m_objectId != inst->m_savedObjectId)
		flags |= DIFF_MODEL;
	// Position edits are stored directly, so any different component is a
	// real change. A distance tolerance loses small inspector edits (even
	// 0.001 can round just below that threshold in float storage).
	if(inst->m_translation.x != inst->m_savedTranslation.x ||
	   inst->m_translation.y != inst->m_savedTranslation.y ||
	   inst->m_translation.z != inst->m_savedTranslation.z)
		flags |= DIFF_MOVED;
	if(GetQuaternionSimilarity(inst->m_rotation, inst->m_savedRotation) < 0.9999f)
		flags |= DIFF_ROTATED;
	if(HasObjectMaterialOverrides(inst))
		flags |= DIFF_MATERIAL;
	if(inst->m_pawnProperties)
		flags |= DIFF_PROPERTIES;
	return flags;
}

bool
PawnObjectNeedsExport(const ObjectInst *inst)
{
	if(inst == nil || (inst->m_isDeleted && !IsPawnWorldObject((ObjectInst*)inst)))
		return false;
	if(inst->m_isVehicle)
		return !inst->m_isDeleted;

	// Scripted objects are already represented by Pawn calls, so every live
	// scripted instance belongs in the export. Built-in world objects are
	// reference geometry and only become exportable when the editor changed
	// something Pawn cannot change in place.
	if(!IsPawnWorldObject((ObjectInst*)inst))
		return true;

	int flags = GetInstanceDiffFlags((ObjectInst*)inst);
	return (flags & (DIFF_DELETED | DIFF_MODEL | DIFF_MOVED | DIFF_ROTATED | DIFF_MATERIAL |
		DIFF_PROPERTIES)) != 0;
}

static bool prefabNextToken(const char **cursor, char *out, size_t outSize);
static bool prefabParseIntToken(const char *token, int *out);
static bool prefabParseColorToken(const char *token, rw::RGBA *out);

struct PrefabEntry {
	int objectId;
	char modelName[MODELNAMELEN];
	float relX, relY, relZ;
	float rotX, rotY, rotZ, rotW;
	int area;
	int lodRef;
	ObjectMaterialOverride materials[MAX_PAWN_MATERIALS];
	ObjectPawnProperties properties;
	int attachParentIndex;	// local index within the prefab, or -1
	int npcVehicleIndex;	// the vehicle an NPC sits in, local index, or -1
	bool hasVehicle;
	bool vehicleStatic;
	int vehicleColor1, vehicleColor2, vehicleRespawnDelay, vehiclePaintjob;
	uint16 vehicleMods[VEHICLE_MOD_SLOTS];
	char vehiclePlate[VEHICLE_PLATE_LEN];
};

static bool
ReadPrefabEntries(const char *path, PrefabEntry *entries, int maxEntries, int *numEntriesOut, bool showToasts)
{
	FILE *f = fopen(path, "r");
	if(f == nil){
		log("ReadPrefabEntries: failed to open %s\n", path);
		return false;
	}

	char line[4096];	// room for a full 3D text label
	int version = 0, game = -1, count = 0;
	while(fgets(line, sizeof(line), f)){
		if(strncmp(line, "HAUMEA_PREFAB", 13) == 0)
			sscanf(line, "%*s %d", &version);
		else if(strncmp(line, "game", 4) == 0)
			sscanf(line, "game %d", &game);
		else if(strncmp(line, "count", 5) == 0){
			sscanf(line, "count %d", &count);
			break;
		}
	}

	if(version < 1 || count <= 0){
		fclose(f);
		log("ReadPrefabEntries: invalid prefab header in %s\n", path);
		return false;
	}

	if(game >= 0 && game != (int)params.map){
		fclose(f);
		if(showToasts)
			Toast(TOAST_SPAWN, _("Prefab is for a different game"));
		return false;
	}

	if(count > maxEntries){
		if(showToasts)
			Toast(TOAST_SPAWN, _("Prefab too large (%d instances, max %d)"), count, maxEntries);
		count = maxEntries;
	}

	int numEntries = 0;
	while(numEntries < count && fgets(line, sizeof(line), f)){
		PrefabEntry *e = &entries[numEntries];
		memset(e, 0, sizeof(*e));
		e->lodRef = -1;
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++)
			InitObjectMaterialOverride(&e->materials[slot], -1);
		InitObjectPawnProperties(&e->properties);
		e->attachParentIndex = -1;
		e->npcVehicleIndex = -1;
		int n = sscanf(line, "%d , %29s %f , %f , %f , %f , %f , %f , %f , %d , %d",
			&e->objectId, e->modelName, &e->relX, &e->relY, &e->relZ,
			&e->rotX, &e->rotY, &e->rotZ, &e->rotW,
			&e->area, &e->lodRef);
		if(n >= 9){
			e->objectId = EditorModelId(e->objectId);	// 0.3.DL custom models
			char *comma = strchr(e->modelName, ',');
			if(comma) *comma = '\0';
			if(n < 11) e->lodRef = -1;
			numEntries++;
		}
	}
	// Version 2 stores instance-local material edits after the object records.
	// Version 1 files simply have no material lines and remain fully compatible.
	if(version >= 2){
		while(fgets(line, sizeof(line), f)){
			const char *cursor = line;
			char type[32];
			if(!prefabNextToken(&cursor, type, sizeof(type)))
				continue;
			int objectIndex, slot;
			char token[128];
			if(strcmp(type, "props") == 0){
				// props <index> <draw> <stream> <world> "name" "group" "comment"
				ObjectPawnProperties properties;
				InitObjectPawnProperties(&properties);
				char draw[64], stream[64];
				if(!prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries ||
				   !prefabNextToken(&cursor, draw, sizeof(draw)) ||
				   !prefabNextToken(&cursor, stream, sizeof(stream)) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) ||
				   !prefabParseIntToken(token, &properties.virtualWorld) ||
				   !prefabNextToken(&cursor, properties.name, sizeof(properties.name)) ||
				   !prefabNextToken(&cursor, properties.group, sizeof(properties.group)) ||
				   !prefabNextToken(&cursor, properties.comment, sizeof(properties.comment)))
					continue;
				properties.drawDistance = (float)atof(draw);
				properties.streamDistance = (float)atof(stream);
				entries[objectIndex].properties = properties;
				continue;
			}
			if(strcmp(type, "element") == 0){
				// Written after the props line of the same object.
				char values[21][64];
				int numValues = 0;
				while(numValues < 21 && prefabNextToken(&cursor, values[numValues], sizeof(values[0])))
					numValues++;
				char text[PAWN_LABEL_TEXT_LEN];
				if(numValues < 21 || !prefabNextToken(&cursor, text, sizeof(text)) ||
				   !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				int v = 1;
				auto nextInt = [&](){ int x = 0; prefabParseIntToken(values[v++], &x); return x; };
				auto nextColor = [&](){ return (uint32)strtoul(values[v++], nil, 0); };
				el.elementKind = nextInt();
				el.pickupType = nextInt();
				el.elementModel = nextInt();
				el.actorInvulnerable = nextInt() != 0;
				el.actorHealth = (float)atof(values[v++]);
				el.classTeam = nextInt();
				for(int w = 0; w < PAWN_CLASS_WEAPONS; w++){
					el.classWeapons[w] = nextInt();
					el.classAmmo[w] = nextInt();
				}
				el.zoneSizeX = (float)atof(values[v++]);
				el.zoneSizeY = (float)atof(values[v++]);
				el.zoneColor = nextColor();
				el.mapIconType = nextInt();
				el.mapIconColor = nextColor();
				el.mapIconStyle = nextInt();
				el.labelColor = nextColor();
				el.labelTestLOS = nextInt() != 0;
				strncpy(el.labelText, text, sizeof(el.labelText)-1);
				el.labelText[sizeof(el.labelText)-1] = '\0';
				continue;
			}
			if(strcmp(type, "npc") == 0){
				// Written after the element line of the same object.
				char armour[64], weapon[64], ammo[64];
				if(!prefabNextToken(&cursor, token, sizeof(token)) ||
				   !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries ||
				   !prefabNextToken(&cursor, armour, sizeof(armour)) ||
				   !prefabNextToken(&cursor, weapon, sizeof(weapon)) ||
				   !prefabNextToken(&cursor, ammo, sizeof(ammo)))
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				el.npcArmour = (float)atof(armour);
				prefabParseIntToken(weapon, &el.npcWeapon);
				prefabParseIntToken(ammo, &el.npcAmmo);
				prefabNextToken(&cursor, el.npcName, sizeof(el.npcName));
				continue;
			}
			if(strcmp(type, "anim") == 0){
				char values[7][64];
				bool valid = true;
				for(int v = 0; valid && v < 7; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				int flags[4] = {};
				el.animDelta = (float)atof(values[1]);
				for(int k = 0; k < 4; k++)
					prefabParseIntToken(values[2 + k], &flags[k]);
				el.animLoop = flags[0] != 0;
				el.animLockX = flags[1] != 0;
				el.animLockY = flags[2] != 0;
				el.animFreeze = flags[3] != 0;
				prefabParseIntToken(values[6], &el.animTime);
				prefabNextToken(&cursor, el.animLib, sizeof(el.animLib));
				prefabNextToken(&cursor, el.animName, sizeof(el.animName));
				continue;
			}
			if(strcmp(type, "sound") == 0){
				char values[6][64];
				bool valid = true;
				for(int v = 0; valid && v < 6; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				int usePos = 0, stop = 0;
				prefabParseIntToken(values[1], &el.soundKind);
				prefabParseIntToken(values[2], &el.soundId);
				el.soundRadius = (float)atof(values[3]);
				prefabParseIntToken(values[4], &usePos);
				prefabParseIntToken(values[5], &stop);
				el.soundUsePos = usePos != 0;
				el.soundStopOnLeave = stop != 0;
				prefabNextToken(&cursor, el.soundUrl, sizeof(el.soundUrl));
				continue;
			}
			if(strcmp(type, "zone") == 0){
				char mode[64];
				if(!prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries || !prefabNextToken(&cursor, mode, sizeof(mode)))
					continue;
				prefabParseIntToken(mode, &entries[objectIndex].properties.zoneMode);
				continue;
			}
			if(strcmp(type, "checkpoint") == 0){
				char values[5][64];
				bool valid = true;
				for(int v = 0; valid && v < 5; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				prefabParseIntToken(values[1], &el.cpKind);
				el.cpSize = (float)atof(values[2]);
				prefabParseIntToken(values[3], &el.cpRaceType);
				prefabParseIntToken(values[4], &el.cpOrder);
				prefabNextToken(&cursor, el.cpRace, sizeof(el.cpRace));
				continue;
			}
			if(strcmp(type, "area") == 0){
				char values[8][64];
				bool valid = true;
				for(int v = 0; valid && v < 8; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				prefabParseIntToken(values[1], &el.areaShape);
				el.areaRadius = (float)atof(values[2]);
				el.areaSize = { (float)atof(values[3]), (float)atof(values[4]), (float)atof(values[5]) };
				el.areaHeight = (float)atof(values[6]);
				int count = 0;
				prefabParseIntToken(values[7], &count);
				count = clamp(count, 0, (int)PAWN_AREA_POINTS);
				el.areaPointCount = 0;
				for(int p = 0; p < count; p++){
					char x[64], y[64];
					if(!prefabNextToken(&cursor, x, sizeof(x)) || !prefabNextToken(&cursor, y, sizeof(y)))
						break;
					el.areaPoints[p][0] = (float)atof(x);
					el.areaPoints[p][1] = (float)atof(y);
					el.areaPointCount = p + 1;
				}
				continue;
			}
			if(strcmp(type, "inarea") == 0){
				if(!prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				prefabNextToken(&cursor, entries[objectIndex].properties.areaName,
					sizeof(entries[objectIndex].properties.areaName));
				continue;
			}
			if(strcmp(type, "interior0") == 0){
				if(prefabNextToken(&cursor, token, sizeof(token)) && prefabParseIntToken(token, &objectIndex) &&
				   objectIndex >= 0 && objectIndex < numEntries)
					entries[objectIndex].properties.interiorZeroOnly = true;
				continue;
			}
			if(strcmp(type, "node") == 0){
				char values[7][64];
				bool valid = true;
				for(int v = 0; valid && v < 7; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				int setAngle = 1;
				el.npcPlaysNode = true;
				prefabParseIntToken(values[1], &el.npcNode);
				prefabParseIntToken(values[2], &el.npcNodePoint);
				el.npcNodeRadius = (float)atof(values[3]);
				prefabParseIntToken(values[4], &setAngle);
				el.npcNodeSetAngle = setAngle != 0;
				prefabParseIntToken(values[5], &el.npcMoveType);
				el.npcMoveSpeed = (float)atof(values[6]);
				continue;
			}
			if(strcmp(type, "npcveh") == 0){
				int vehicleIndex = -1, seat = 0;
				if(!prefabNextToken(&cursor, token, sizeof(token)) ||
				   !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries ||
				   !prefabNextToken(&cursor, token, sizeof(token)) ||
				   !prefabParseIntToken(token, &vehicleIndex) ||
				   vehicleIndex < 0 || vehicleIndex >= numEntries || vehicleIndex == objectIndex ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &seat))
					continue;
				entries[objectIndex].npcVehicleIndex = vehicleIndex;
				entries[objectIndex].properties.npcSeat = clamp(seat, 0, 3);
				continue;
			}
			if(strcmp(type, "rec") == 0){
				char atNpc[64];
				if(!prefabNextToken(&cursor, token, sizeof(token)) ||
				   !prefabParseIntToken(token, &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries ||
				   !prefabNextToken(&cursor, atNpc, sizeof(atNpc)))
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				int value = 0;
				prefabParseIntToken(atNpc, &value);
				el.npcRecordingAtNpc = value != 0;
				prefabNextToken(&cursor, el.npcRecording, sizeof(el.npcRecording));
				continue;
			}
			if(strcmp(type, "repeat") == 0){
				char values[2][32];
				int objectIndex, mode;
				if(prefabNextToken(&cursor, values[0], sizeof(values[0])) &&
				   prefabNextToken(&cursor, values[1], sizeof(values[1])) &&
				   prefabParseIntToken(values[0], &objectIndex) && prefabParseIntToken(values[1], &mode) &&
				   objectIndex >= 0 && objectIndex < numEntries)
					entries[objectIndex].properties.npcPathRepeat = mode;
				continue;
			}
			if(strcmp(type, "path") == 0){
				char values[6][64];
				bool valid = true;
				for(int v = 0; valid && v < 6; v++)
					valid = prefabNextToken(&cursor, values[v], sizeof(values[v]));
				if(!valid || !prefabParseIntToken(values[0], &objectIndex) ||
				   objectIndex < 0 || objectIndex >= numEntries)
					continue;
				ObjectPawnProperties &el = entries[objectIndex].properties;
				prefabParseIntToken(values[1], &el.pathOrder);
				el.pathStopRange = (float)atof(values[2]);
				prefabParseIntToken(values[3], &el.npcMoveType);
				el.npcMoveSpeed = (float)atof(values[4]);
				int reversed = 0;
				prefabParseIntToken(values[5], &reversed);
				el.npcPathReversed = reversed != 0;
				prefabNextToken(&cursor, el.pathName, sizeof(el.pathName));
				continue;
			}
			if(strcmp(type, "vehicle") == 0){
				// vehicle <index> <static> <color1> <color2> <respawn> <paintjob + 1>
				//     <component x16> "plate"
				int values[6 + VEHICLE_MOD_SLOTS];
				bool valid = true;
				for(int v = 0; valid && v < 6 + VEHICLE_MOD_SLOTS; v++)
					valid = prefabNextToken(&cursor, token, sizeof(token)) &&
						prefabParseIntToken(token, &values[v]);
				objectIndex = values[0];
				if(!valid || objectIndex < 0 || objectIndex >= numEntries)
					continue;
				PrefabEntry *e = &entries[objectIndex];
				e->hasVehicle = true;
				e->vehicleStatic = values[1] != 0;
				e->vehicleColor1 = clamp(values[2], -1, 255);
				e->vehicleColor2 = clamp(values[3], -1, 255);
				e->vehicleRespawnDelay = values[4];
				e->vehiclePaintjob = clamp(values[5], 0, 3);
				for(int m = 0; m < VEHICLE_MOD_SLOTS; m++){
					int component = values[6 + m];
					e->vehicleMods[m] = GetVehicleComponentSlot(component) == m ? (uint16)component : 0;
				}
				prefabNextToken(&cursor, e->vehiclePlate, sizeof(e->vehiclePlate));
				continue;
			}
			if(strcmp(type, "attach") == 0){
				// v3: attach <index> <parent index> <x y z> <rx ry rz> <sync>
				// v4 adds bone, slot, scale, material colours and preview skin.
				int parentIndex, sync;
				float values[6];
				bool valid = prefabNextToken(&cursor, token, sizeof(token)) &&
					prefabParseIntToken(token, &objectIndex) &&
					prefabNextToken(&cursor, token, sizeof(token)) &&
					prefabParseIntToken(token, &parentIndex);
				for(int v = 0; valid && v < 6; v++){
					valid = prefabNextToken(&cursor, token, sizeof(token));
					values[v] = valid ? (float)atof(token) : 0.0f;
				}
				valid = valid && prefabNextToken(&cursor, token, sizeof(token)) &&
					prefabParseIntToken(token, &sync);
				if(!valid || objectIndex < 0 || objectIndex >= numEntries ||
				   (parentIndex != PAWN_ATTACH_PLAYER &&
				    (parentIndex < 0 || parentIndex >= numEntries || parentIndex == objectIndex)))
					continue;
				PrefabEntry *child = &entries[objectIndex];
				child->attachParentIndex = parentIndex;
				child->properties.attachOffset = { values[0], values[1], values[2] };
				child->properties.attachRotation = { values[3], values[4], values[5] };
				child->properties.attachSyncRotation = sync != 0;
				if(prefabNextToken(&cursor, token, sizeof(token)))
					prefabParseIntToken(token, &child->properties.attachBone);
				if(prefabNextToken(&cursor, token, sizeof(token)))
					prefabParseIntToken(token, &child->properties.attachSlot);
				float scale[3] = { 1.0f, 1.0f, 1.0f };
				for(int v = 0; v < 3; v++)
					if(prefabNextToken(&cursor, token, sizeof(token)))
						scale[v] = (float)atof(token);
				child->properties.attachScale = { scale[0], scale[1], scale[2] };
				if(prefabNextToken(&cursor, token, sizeof(token)))
					child->properties.attachColor1 = (uint32)strtoul(token, nil, 0);
				if(prefabNextToken(&cursor, token, sizeof(token)))
					child->properties.attachColor2 = (uint32)strtoul(token, nil, 0);
				if(prefabNextToken(&cursor, token, sizeof(token)))
					prefabParseIntToken(token, &child->properties.attachSkin);
				continue;
			}
			if(strcmp(type, "material") == 0){
				int sourceModel, hasTexture;
				char color[64], txd[MODELNAMELEN], texture[PAWN_TEXTURE_NAME_LEN];
				if(!prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &objectIndex) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &slot) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &sourceModel) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &hasTexture) ||
				   !prefabNextToken(&cursor, color, sizeof(color)) ||
				   !prefabNextToken(&cursor, txd, sizeof(txd)) ||
				   !prefabNextToken(&cursor, texture, sizeof(texture)) ||
				   objectIndex < 0 || objectIndex >= numEntries || slot < 0 || slot >= MAX_PAWN_MATERIALS ||
				   !prefabParseColorToken(color, &entries[objectIndex].materials[slot].color))
					continue;
				sourceModel = EditorModelId(sourceModel);
				ObjectMaterialOverride *material = &entries[objectIndex].materials[slot];
				material->active = true;
				material->hasTexture = hasTexture != 0 && texture[0] != '\0';
				material->sourceModel = sourceModel;
				material->txdSlot = FindTxdSlot(txd);
				strncpy(material->txdName, txd, sizeof(material->txdName)-1);
				material->txdName[sizeof(material->txdName)-1] = '\0';
				strncpy(material->textureName, texture, sizeof(material->textureName)-1);
				material->textureName[sizeof(material->textureName)-1] = '\0';
			}else if(strcmp(type, "material_text") == 0){
				int materialSize, fontSize, bold, alignment;
				char textColor[64], backgroundColor[64], font[PAWN_MATERIAL_FONT_LEN], text[PAWN_MATERIAL_TEXT_LEN];
				if(!prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &objectIndex) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &slot) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &materialSize) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &fontSize) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &bold) ||
				   !prefabNextToken(&cursor, textColor, sizeof(textColor)) ||
				   !prefabNextToken(&cursor, backgroundColor, sizeof(backgroundColor)) ||
				   !prefabNextToken(&cursor, token, sizeof(token)) || !prefabParseIntToken(token, &alignment) ||
				   !prefabNextToken(&cursor, font, sizeof(font)) ||
				   !prefabNextToken(&cursor, text, sizeof(text)) ||
				   objectIndex < 0 || objectIndex >= numEntries || slot < 0 || slot >= MAX_PAWN_MATERIALS ||
				   !prefabParseColorToken(textColor, &entries[objectIndex].materials[slot].textColor) ||
				   !prefabParseColorToken(backgroundColor, &entries[objectIndex].materials[slot].backgroundColor))
					continue;
				ObjectMaterialOverride *material = &entries[objectIndex].materials[slot];
				material->textEnabled = true;
				material->materialSize = materialSize;
				material->fontSize = fontSize;
				material->bold = bold != 0;
				material->alignment = alignment;
				strncpy(material->font, font, sizeof(material->font)-1);
				material->font[sizeof(material->font)-1] = '\0';
				strncpy(material->text, text, sizeof(material->text)-1);
				material->text[sizeof(material->text)-1] = '\0';
			}
		}
	}
	fclose(f);

	if(numEntries == 0)
		return false;
	if(numEntriesOut)
		*numEntriesOut = numEntries;
	return true;
}

// Keep preview passes isolated from the main scene camera. On GL3, endUpdate()
// does not restore the framebuffer, viewport, matrices, or other device state.
rw::Texture *gPreviewTexture;
static rw::Raster *previewColorRaster;
static rw::Raster *previewDepthRaster;
static rw::Camera *previewCamera;
static bool previewInited;
static bool previewInitFailed;

#define PREVIEW_SIZE 256
#define OBJECT_THUMBNAIL_SIZE 128
#define OBJECT_THUMBNAIL_SLOTS 64
#define OBJECT_THUMBNAILS_PER_FRAME 3
#define PREFAB_THUMBNAIL_SIZE 128
#define PREFAB_THUMBNAIL_SLOTS 32
#define PREFAB_THUMBNAILS_PER_FRAME 1

struct ObjectThumbnailSlot
{
	int objectId;
	bool ready;
	bool failed;
	uint32 lastUsed;
	int lastRequestedFrame;
	rw::Raster *colorRaster;
	rw::Raster *depthRaster;
	rw::Texture *texture;
};

static ObjectThumbnailSlot objectThumbnailSlots[OBJECT_THUMBNAIL_SLOTS];
static uint32 objectThumbnailUseSeq;
static bool objectThumbnailSlotsInited;
static bool objectThumbnailInitFailed;

struct PrefabThumbnailSlot
{
	char path[512];
	time_t mtime;	// of the file when it was rendered; a newer file renders again
	int checkedFrame;
	bool ready;
	bool failed;
	uint32 lastUsed;
	int lastRequestedFrame;
	rw::Raster *colorRaster;
	rw::Raster *depthRaster;
	rw::Texture *texture;
};

static PrefabThumbnailSlot prefabThumbnailSlots[PREFAB_THUMBNAIL_SLOTS];
static uint32 prefabThumbnailUseSeq;
static bool prefabThumbnailInitFailed;

static rw::Camera*
getPreviewCamera(void)
{
	if(previewCamera)
		return previewCamera;

	previewCamera = rw::Camera::create();
	if(previewCamera == nil)
		return nil;
	rw::Frame *frame = rw::Frame::create();
	if(frame == nil){
		previewCamera->destroy();
		previewCamera = nil;
		return nil;
	}
	previewCamera->setFrame(frame);
	if(Scene.world)
		Scene.world->addCamera(previewCamera);
	return previewCamera;
}

static void
initObjectThumbnailSlots(void)
{
	if(objectThumbnailSlotsInited)
		return;
	for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++)
		objectThumbnailSlots[i].objectId = -1;
	objectThumbnailSlotsInited = true;
}

static void
destroyObjectThumbnailSlot(ObjectThumbnailSlot *slot)
{
	if(slot->texture){
		slot->texture->raster = nil;
		slot->texture->destroy();
		slot->texture = nil;
	}
	if(slot->colorRaster){ slot->colorRaster->destroy(); slot->colorRaster = nil; }
	if(slot->depthRaster){ slot->depthRaster->destroy(); slot->depthRaster = nil; }
	slot->objectId = -1;
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = 0;
	slot->lastRequestedFrame = 0;
}

static void
destroyPrefabThumbnailSlot(PrefabThumbnailSlot *slot)
{
	if(slot->texture){
		slot->texture->raster = nil;
		slot->texture->destroy();
		slot->texture = nil;
	}
	if(slot->colorRaster){ slot->colorRaster->destroy(); slot->colorRaster = nil; }
	if(slot->depthRaster){ slot->depthRaster->destroy(); slot->depthRaster = nil; }
	slot->path[0] = '\0';
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = 0;
	slot->lastRequestedFrame = 0;
}

static bool
initObjectThumbnailSlot(ObjectThumbnailSlot *slot)
{
	if(slot->texture && slot->colorRaster && slot->depthRaster)
		return true;
	if(objectThumbnailInitFailed)
		return false;

	destroyObjectThumbnailSlot(slot);
	slot->colorRaster = rw::Raster::create(OBJECT_THUMBNAIL_SIZE, OBJECT_THUMBNAIL_SIZE, 0, rw::Raster::CAMERATEXTURE);
	if(slot->colorRaster == nil){
		objectThumbnailInitFailed = true;
		return false;
	}
	slot->depthRaster = rw::Raster::create(OBJECT_THUMBNAIL_SIZE, OBJECT_THUMBNAIL_SIZE, 0, rw::Raster::ZBUFFER);
	if(slot->depthRaster == nil){
		objectThumbnailInitFailed = true;
		return false;
	}
	slot->texture = rw::Texture::create(slot->colorRaster);
	if(slot->texture == nil){
		objectThumbnailInitFailed = true;
		return false;
	}
	slot->texture->setFilter(rw::Texture::LINEAR);
	slot->objectId = -1;
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = 0;
	slot->lastRequestedFrame = 0;
	return true;
}

static bool
initPrefabThumbnailSlot(PrefabThumbnailSlot *slot)
{
	if(slot->texture && slot->colorRaster && slot->depthRaster)
		return true;
	if(prefabThumbnailInitFailed)
		return false;

	char path[512];
	strncpy(path, slot->path, sizeof(path));
	path[sizeof(path)-1] = '\0';
	destroyPrefabThumbnailSlot(slot);
	strncpy(slot->path, path, sizeof(slot->path));
	slot->path[sizeof(slot->path)-1] = '\0';

	slot->colorRaster = rw::Raster::create(PREFAB_THUMBNAIL_SIZE, PREFAB_THUMBNAIL_SIZE, 0, rw::Raster::CAMERATEXTURE);
	if(slot->colorRaster == nil){
		prefabThumbnailInitFailed = true;
		return false;
	}
	slot->depthRaster = rw::Raster::create(PREFAB_THUMBNAIL_SIZE, PREFAB_THUMBNAIL_SIZE, 0, rw::Raster::ZBUFFER);
	if(slot->depthRaster == nil){
		prefabThumbnailInitFailed = true;
		return false;
	}
	slot->texture = rw::Texture::create(slot->colorRaster);
	if(slot->texture == nil){
		prefabThumbnailInitFailed = true;
		return false;
	}
	slot->texture->setFilter(rw::Texture::LINEAR);
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = 0;
	slot->lastRequestedFrame = 0;
	return true;
}

static void
BuildPreviewMatrix(rw::Matrix *matrix, const rw::Quat &rotation, const rw::V3d &translation)
{
	if(isSA() && std::fabs(rotation.x) <= 0.05f && std::fabs(rotation.y) <= 0.05f){
		float w = rotation.w;
		if(w < -1.0f) w = -1.0f;
		if(w > 1.0f) w = 1.0f;
		float heading = acosf(w) * (rotation.z < 0.0f ? 2.0f : -2.0f);
		float s = sinf(heading);
		float c = cosf(heading);

		matrix->setIdentity();
		matrix->right = { c, s, 0.0f };
		matrix->up = { -s, c, 0.0f };
		matrix->at = { 0.0f, 0.0f, 1.0f };
		matrix->flags = rw::Matrix::TYPEORTHONORMAL;
	}else
		matrix->rotate(conj(rotation), rw::COMBINEREPLACE);
	matrix->translate(&translation, rw::COMBINEPOSTCONCAT);
}

static void
setObjectPreviewError(char *error, size_t errorSize, const char *message)
{
	if(error == nil || errorSize == 0)
		return;
	strncpy(error, message, errorSize - 1);
	error[errorSize - 1] = '\0';
}

// Centre and radius of a model's visible geometry. Collision spheres are often
// much larger than the model, which left small or thin objects lost in previews.
static bool
previewGeometryBounds(rw::Atomic *atm, rw::Clump *clump, rw::V3d *center, float *radius, bool preciseVertices = false)
{
	rw::V3d boundsMin = { 1e30f, 1e30f, 1e30f }, boundsMax = { -1e30f, -1e30f, -1e30f };
	bool haveBounds = false;
	auto addSphere = [&](const rw::Sphere &sphere){
		if(sphere.radius <= 0.0f)
			return;
		boundsMin.x = min(boundsMin.x, sphere.center.x - sphere.radius);
		boundsMin.y = min(boundsMin.y, sphere.center.y - sphere.radius);
		boundsMin.z = min(boundsMin.z, sphere.center.z - sphere.radius);
		boundsMax.x = max(boundsMax.x, sphere.center.x + sphere.radius);
		boundsMax.y = max(boundsMax.y, sphere.center.y + sphere.radius);
		boundsMax.z = max(boundsMax.z, sphere.center.z + sphere.radius);
		haveBounds = true;
	};
	rw::Matrix ident;
	ident.setIdentity();
	if(atm){
		atm->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		if(atm->geometry && atm->geometry->numMorphTargets > 0)
			addSphere(atm->geometry->morphTargets[0].boundingSphere);
	}else if(clump){
		clump->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		FORLIST(lnk, clump->atomics){
			rw::Atomic *part = rw::Atomic::fromClump(lnk);
			if(part->geometry && part->geometry->numMorphTargets > 0){
				if(preciseVertices && part->geometry->morphTargets[0].vertices){
					// A skin's sphere is deliberately generous for animation.
					// Frame its actual posed vertices for a readable portrait.
					const rw::Matrix *matrix = part->getFrame()->getLTM();
					for(int i = 0; i < part->geometry->numVertices; i++){
						rw::V3d point;
						rw::V3d::transformPoints(&point, &part->geometry->morphTargets[0].vertices[i], 1, matrix);
						boundsMin.x = min(boundsMin.x, point.x); boundsMax.x = max(boundsMax.x, point.x);
						boundsMin.y = min(boundsMin.y, point.y); boundsMax.y = max(boundsMax.y, point.y);
						boundsMin.z = min(boundsMin.z, point.z); boundsMax.z = max(boundsMax.z, point.z);
						haveBounds = true;
					}
				}else
					addSphere(*part->getWorldBoundingSphere());
			}
		}
	}
	if(!haveBounds)
		return false;
	*center = scale(add(boundsMin, boundsMax), 0.5f);
	*radius = max(length(sub(boundsMax, boundsMin)) * 0.5f, 0.25f);
	return true;
}

// Vehicle models carry placeholder paint (bright green/pink) that the game
// replaces with car colours. Give the preview its own geometry and paint it
// with the vehicle's default colours, as placed vehicles are.
static void
paintPreviewVehicle(ObjectDef *obj, rw::Clump *clump, int colour1 = -1, int colour2 = -1)
{
	if(obj == nil || !obj->m_isVehicle || clump == nil)
		return;
	SAPaths::PrepareEditorVehicleVisuals(clump);
	FORLIST(lnk, clump->atomics){
		rw::Atomic *part = rw::Atomic::fromClump(lnk);
		if(part->geometry == nil)
			continue;
		rw::Geometry *copy = cloneGeometryForMaterial(part->geometry);
		if(copy == nil)
			return;	// never repaint shared model geometry
		part->setGeometry(copy, 0);
		copy->destroy();	// the atomic holds the remaining reference
	}
	uint8 *roles = nil;
	int numRoles = 0;
	SAPaths::PrepareEditorVehicleClump(clump, colour1, colour2, &roles, &numRoles);
	rwFree(roles);
}

// Preview surfaces match the active theme (the RW camera texture is opaque).
static rw::RGBA
previewBackground(void)
{
	return GetUiTheme() == UI_THEME_LIGHT ? rw::RGBA{ 235, 239, 244, 255 } : rw::RGBA{ 31, 41, 48, 255 };
}

static bool
renderObjectToRaster(int objectId, rw::Raster *colorRaster, rw::Raster *depthRaster,
	int size, float angle, char *error = nil, size_t errorSize = 0, bool pngOrientation = false)
{
	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil){
		setObjectPreviewError(error, errorSize, "object definition is missing");
		return false;
	}

	rw::Atomic *atm = nil;
	rw::Clump *clump = nil;
	if(!CreateObjectPreviewRwObject(objectId, &atm, &clump)){
		setObjectPreviewError(error, errorSize, "preview geometry clone failed");
		return false;
	}

	paintPreviewVehicle(obj, clump);

	rw::V3d target = { 0.0f, 0.0f, 0.0f };
	float radius = 5.0f;
	// Namaka's asset index was built from collision-framed PNG captures.
	bool haveBounds = !pngOrientation && previewGeometryBounds(atm, clump, &target, &radius, obj->m_isPed);
	if(!haveBounds && obj->m_colModel){
		target = obj->m_colModel->boundingSphere.center;
		radius = obj->m_colModel->boundingSphere.radius;
	}
	if(radius < 0.25f) radius = 0.25f;
	rw::V3d eye;
	float dist;
	if(pngOrientation){
		if(radius < 1.0f) radius = 1.0f;
		dist = radius * 2.5f;
		eye = { target.x + dist*cosf(angle), target.y + dist*sinf(angle), target.z + dist*0.4f };
	}else{
		// The sphere just fits the 55 degree view at r / sin(27.5).
		dist = radius / 0.4617f * 1.02f;
		rw::V3d viewDir = normalize(rw::V3d{ cosf(angle), sinf(angle), 0.45f });
		eye = add(target, scale(viewDir, dist));
	}
	// UI camera textures use their historical inverted orientation. PNG readback
	// has no UI UV transform and needs a world-up camera for upright asset views.
	rw::V3d up = { 0.0f, 0.0f, pngOrientation ? 1.0f : -1.0f };
	rw::V3d dir = normalize(sub(target, eye));

	rw::Camera *cam = getPreviewCamera();
	if(cam == nil){
		setObjectPreviewError(error, errorSize, "preview camera creation failed");
		if(atm){ atm->getFrame()->destroy(); atm->destroy(); }
		if(clump){ clump->destroy(); }
		return false;
	}
	rw::Frame *camFrame = cam->getFrame();

	cam->frameBuffer = colorRaster;
	cam->zBuffer = depthRaster;
	cam->setNearPlane(0.01f);
	cam->setFarPlane(300.0f);
	cam->fogPlane = 300.0f;
	rw::V2d window = { 0.5f, 0.5f };
	cam->setViewWindow(&window);
	camFrame->matrix.pos = eye;
	camFrame->matrix.lookAt(dir, up);
	camFrame->updateObjects();

	// Editor thumbnails use a mid tone so dark and light models both read;
	// PNG captures keep the background Namaka's previews were made with.
	rw::RGBA clearCol = pngOrientation ? rw::RGBA{ 34, 34, 34, 255 } : previewBackground();
	cam->clear(&clearCol, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	cam->beginUpdate();

	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	// One white ambient light, no directional one, and no culling, so the
	// one-sided faces of wedges and panels show from either side.
	uint32 oldCull = rw::GetRenderState(rw::CULLMODE);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	pAmbient->setColor(1.0f, 1.0f, 1.0f);
	pDirect->setColor(0.0f, 0.0f, 0.0f);

	rw::Matrix ident;
	ident.setIdentity();
	if(atm){
		atm->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		atm->render();
	}else if(clump){
		clump->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		clump->render();
	}

	cam->endUpdate();

	if(atm){ atm->getFrame()->destroy(); atm->destroy(); }
	if(clump){ clump->destroy(); }
	Timecycle::SetLights();
	return true;
}

// A SA-MP model preview textdraw (TextDrawSetPreviewModel/Rot/VehCol): the
// model turned by rot degrees, seen from its front, zoom bringing it closer.
bool
RenderModelPreviewToRaster(int objectId, rw::V3d rot, float zoom, int colour1, int colour2,
	rw::RGBA background, rw::Raster *colorRaster, rw::Raster *depthRaster)
{
	// The client draws these four as 18631 (a question mark), turned
	// 180 about Z whatever the rotation (RenderStreamedModelPreviewToTextureSurface).
	if(objectId == 1373 || objectId == 3118 || objectId == 3552 || objectId == 3553){
		objectId = 18631;
		rot.x = 0.0f; rot.y = 0.0f; rot.z = 180.0f;
	}
	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil)
		return false;
	if(!obj->IsLoaded()){
		RequestObject(objectId);
		LoadAllRequestedObjects();
	}
	rw::Atomic *atm = nil;
	rw::Clump *clump = nil;
	if(!obj->IsLoaded() || !CreateObjectPreviewRwObject(objectId, &atm, &clump))
		return false;
	paintPreviewVehicle(obj, clump, colour1, colour2);
	// The client's preview scene (reSA-MP 0.3.7 InitializePreviewSceneResources,
	// RenderStreamedModelPreviewToTextureSurface, RenderCharacterLikePreview...):
	// a camera looking along -Y with a 0.5 view window, the model's collision
	// sphere setting the distance, zoom moving it away, and the rotations
	// turning the model about its own origin, Z first, then Y, then X.
	rw::V3d center = { 0.0f, 0.0f, 0.0f };
	float radius = 1.0f;
	if(obj->m_colModel){
		center = obj->m_colModel->boundingSphere.center;
		radius = obj->m_colModel->boundingSphere.radius;
	}else
		previewGeometryBounds(atm, clump, &center, &radius);
	// Where each kind is put in front of the camera: a skin at a fixed
	// distance (RenderModelPreviewToTextureSurface, 0.05 above the camera,
	// skin 162 0.15), a vehicle by its size and the zoom, a plane by its size
	// alone (RenderCharacterLikePreviewToTextureSurface), anything else by
	// its collision sphere (RenderStreamedModelPreviewToTextureSurface).
	rw::V3d pos;
	if(obj->m_isPed)
		pos = { 0.0f, -2.25f * zoom, objectId == 162 ? 0.15f : 0.05f };
	else if(obj->m_isVehicle && rw::strncmp_ci(obj->m_vehicleType, "plane", 16) == 0)
		pos = { 0.0f, -5.5f - radius * 2.5f, 0.0f };
	else if(obj->m_isVehicle)
		pos = { 0.0f, -(1.0f + radius * 2.0f) * zoom, 0.0f };
	else
		pos = { -center.x, -(0.1f + radius * 2.25f) * zoom, -center.z };
	rw::Matrix m;
	m.setIdentity();
	m.translate(&pos, rw::COMBINEREPLACE);
	rw::V3d xaxis = { 1.0f, 0.0f, 0.0f }, yaxis = { 0.0f, 1.0f, 0.0f }, zaxis = { 0.0f, 0.0f, 1.0f };
	if(rot.x != 0.0f) m.rotate(&xaxis, rot.x, rw::COMBINEPRECONCAT);
	if(rot.y != 0.0f) m.rotate(&yaxis, rot.y, rw::COMBINEPRECONCAT);
	if(rot.z != 0.0f) m.rotate(&zaxis, rot.z, rw::COMBINEPRECONCAT);

	rw::Camera *cam = getPreviewCamera();
	if(cam == nil){
		if(atm){ atm->getFrame()->destroy(); atm->destroy(); }
		if(clump) clump->destroy();
		return false;
	}
	rw::V3d eye = { 0.0f, 0.0f, 0.0f };
	rw::V3d dir = { 0.0f, -1.0f, 0.0f };
	rw::V3d up = { 0.0f, 0.0f, -1.0f };	// UI textures are drawn flipped
	rw::Raster *oldColor = cam->frameBuffer, *oldDepth = cam->zBuffer;
	cam->frameBuffer = colorRaster;
	cam->zBuffer = depthRaster;
	cam->setNearPlane(0.01f);
	cam->setFarPlane(300.0f);
	cam->fogPlane = 300.0f;
	rw::V2d window = { 0.5f, 0.5f };
	cam->setViewWindow(&window);
	rw::Frame *camFrame = cam->getFrame();
	camFrame->matrix.pos = eye;
	camFrame->matrix.lookAt(dir, up);
	camFrame->updateObjects();
	cam->clear(&background, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	cam->beginUpdate();
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	// One white ambient light, no directional one, and no culling, so the
	// one-sided faces of wedges and panels show from either side.
	uint32 oldCull = rw::GetRenderState(rw::CULLMODE);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	pAmbient->setColor(1.0f, 1.0f, 1.0f);
	pDirect->setColor(0.0f, 0.0f, 0.0f);
	if(atm){
		atm->getFrame()->transform(&m, rw::COMBINEREPLACE);
		atm->render();
	}else if(clump){
		clump->getFrame()->transform(&m, rw::COMBINEREPLACE);
		clump->render();
	}
	rw::SetRenderState(rw::CULLMODE, oldCull);
	cam->endUpdate();
	cam->frameBuffer = oldColor;
	cam->zBuffer = oldDepth;
	if(atm){ atm->getFrame()->destroy(); atm->destroy(); }
	if(clump) clump->destroy();
	Timecycle::SetLights();
	return true;
}

bool
CaptureObjectPreviewPng(int objectId, const char *path, int size, float angle,
	char *error, size_t errorSize)
{
	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil || path == nil || path[0] == '\0'){
		setObjectPreviewError(error, errorSize, "invalid model or output path");
		return false;
	}
	if(!obj->IsLoaded()){
		RequestObject(objectId);
		LoadAllRequestedObjects();
	}
	if(!obj->IsLoaded()){
		setObjectPreviewError(error, errorSize, "model stream load failed");
		return false;
	}
	if(!EnsureParentDirectoriesForPath(path)){
		setObjectPreviewError(error, errorSize, "preview output directory could not be created");
		return false;
	}
	size = max(96, min(size, 1024));
	// Explicit format bits are needed by Raster::toImage() on the GL3 backend.
	rw::Raster *color = rw::Raster::create(size, size, 24,
		rw::Raster::C888|rw::Raster::CAMERATEXTURE);
	rw::Raster *depth = rw::Raster::create(size, size, 0, rw::Raster::ZBUFFER);
	if(color == nil || depth == nil){
		setObjectPreviewError(error, errorSize,
			color == nil ? "preview color raster creation failed" : "preview depth raster creation failed");
		if(color) color->destroy();
		if(depth) depth->destroy();
		return false;
	}

	// Rendering borrows the shared preview camera; restore its thumbnail targets
	// before releasing these temporary rasters.
	rw::Camera *camera = getPreviewCamera();
	rw::Raster *oldColor = camera ? camera->frameBuffer : nil;
	rw::Raster *oldDepth = camera ? camera->zBuffer : nil;
	bool ok = renderObjectToRaster(objectId, color, depth, size, angle,
		error, errorSize, true);
	if(ok){
		rw::Image *image = color->toImage();
		ok = image != nil;
		if(image){
			rw::writePNG(image, path);
			image->destroy();
		}else
			setObjectPreviewError(error, errorSize, "preview framebuffer readback failed");
	}
	if(camera){
		camera->frameBuffer = oldColor;
		camera->zBuffer = oldDepth;
	}
	if(ok)
		setObjectPreviewError(error, errorSize, "");
	color->destroy();
	depth->destroy();
	return ok;
}

// One object of a thumbnail: a prefab's, or a map's for the start screen.
struct ThumbnailObject
{
	int objectId;
	float relX, relY, relZ;
	float rotX, rotY, rotZ, rotW;
	int elementKind;	// labels, sounds... are drawn as editor icons
};
static const int MAX_THUMBNAIL_OBJECTS = 256;
static bool readMapThumbnailObjects(const char *path, std::vector<ThumbnailObject> &out);

static bool
isPrefabPath(const char *path)
{
	size_t n = strlen(path);
	return n > 7 && rw::strncmp_ci(path + n - 7, ".haumea", 7) == 0;
}

static bool
renderPrefabToRaster(const char *path, rw::Raster *colorRaster, rw::Raster *depthRaster, int size)
{
	std::vector<ThumbnailObject> objects;
	if(isPrefabPath(path)){
		static PrefabEntry prefab[MAX_THUMBNAIL_OBJECTS];
		int numPrefab = 0;
		if(!ReadPrefabEntries(path, prefab, MAX_THUMBNAIL_OBJECTS, &numPrefab, false))
			return false;
		for(int i = 0; i < numPrefab; i++)
			objects.push_back({ prefab[i].objectId, prefab[i].relX, prefab[i].relY, prefab[i].relZ,
				prefab[i].rotX, prefab[i].rotY, prefab[i].rotZ, prefab[i].rotW, prefab[i].properties.elementKind });
	}else if(!readMapThumbnailObjects(path, objects))
		return false;
	const ThumbnailObject *entries = objects.data();
	int numEntries = (int)objects.size();

	rw::V3d minPt = { 1.0e30f, 1.0e30f, 1.0e30f };
	rw::V3d maxPt = { -1.0e30f, -1.0e30f, -1.0e30f };
	int numBounds = 0;
	for(int i = 0; i < numEntries; i++){
		ObjectDef *obj = GetObjectDef(entries[i].objectId);
		if(obj == nil)
			continue;
		float radius = obj->m_colModel ? obj->m_colModel->boundingSphere.radius : 1.0f;
		if(radius < 1.0f) radius = 1.0f;
		rw::V3d center = { entries[i].relX, entries[i].relY, entries[i].relZ };
		if(obj->m_colModel)
			center = add(center, obj->m_colModel->boundingSphere.center);
		minPt.x = min(minPt.x, center.x - radius);
		minPt.y = min(minPt.y, center.y - radius);
		minPt.z = min(minPt.z, center.z - radius);
		maxPt.x = max(maxPt.x, center.x + radius);
		maxPt.y = max(maxPt.y, center.y + radius);
		maxPt.z = max(maxPt.z, center.z + radius);
		numBounds++;
	}
	if(numBounds == 0)
		return false;

	rw::V3d target = scale(add(minPt, maxPt), 0.5f);
	float radius = 1.0f;
	for(int i = 0; i < numEntries; i++){
		ObjectDef *obj = GetObjectDef(entries[i].objectId);
		if(obj == nil)
			continue;
		float objRadius = obj->m_colModel ? obj->m_colModel->boundingSphere.radius : 1.0f;
		rw::V3d center = { entries[i].relX, entries[i].relY, entries[i].relZ };
		if(obj->m_colModel)
			center = add(center, obj->m_colModel->boundingSphere.center);
		float dist = length(sub(center, target)) + objRadius;
		if(dist > radius)
			radius = dist;
	}
	if(radius < 2.0f) radius = 2.0f;

	struct PrefabPreviewModel
	{
		int objectId;
		rw::Atomic *atomic;
		rw::Clump *clump;
	};
	PrefabPreviewModel models[256];
	int numModels = 0, numIcons = 0;
	for(int i = 0; i < numEntries; i++){
		// Labels, sounds and the like are drawn as editor icons, not models.
		if(ElementKindUsesIcon(entries[i].elementKind)){
			numIcons++;
			continue;
		}
		int modelIndex = -1;
		for(int j = 0; j < numModels; j++)
			if(models[j].objectId == entries[i].objectId){
				modelIndex = j;
				break;
			}
		if(modelIndex >= 0)
			continue;

		rw::Atomic *atomic = nil;
		rw::Clump *clump = nil;
		if(!CreateObjectPreviewRwObject(entries[i].objectId, &atomic, &clump))
			continue;
		models[numModels].objectId = entries[i].objectId;
		models[numModels].atomic = atomic;
		models[numModels].clump = clump;
		numModels++;
	}
	if(numModels == 0 && numIcons == 0)
		return false;

	uint32 hash = 2166136261u;
	for(const char *s = path; *s; s++)
		hash = (hash ^ (uint8)*s) * 16777619u;
	float angle = 0.65f + (hash % 19) * 0.045f;
	float dist = radius * 2.8f;
	rw::V3d eye = { target.x + dist*cosf(angle), target.y + dist*sinf(angle), target.z + dist*0.45f };
	rw::V3d up = { 0.0f, 0.0f, -1.0f };
	rw::V3d dir = normalize(sub(target, eye));

	rw::Camera *cam = getPreviewCamera();
	if(cam == nil){
		for(int i = 0; i < numModels; i++){
			if(models[i].atomic){ models[i].atomic->getFrame()->destroy(); models[i].atomic->destroy(); }
			if(models[i].clump){ models[i].clump->destroy(); }
		}
		return false;
	}
	rw::Frame *camFrame = cam->getFrame();

	cam->frameBuffer = colorRaster;
	cam->zBuffer = depthRaster;
	cam->setNearPlane(0.1f);
	cam->setFarPlane(max(1000.0f, radius * 8.0f));
	cam->fogPlane = max(900.0f, radius * 7.0f);
	cam->setFOV(58.0f, 1.0f);
	camFrame->matrix.pos = eye;
	camFrame->matrix.lookAt(dir, up);
	camFrame->updateObjects();

	rw::RGBA clearCol = { 34, 34, 34, 255 };
	cam->clear(&clearCol, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	cam->beginUpdate();

	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	pAmbient->setColor(0.72f, 0.72f, 0.72f);
	pDirect->setColor(0.52f, 0.52f, 0.52f);

	for(int i = 0; i < numEntries; i++){
		if(ElementKindUsesIcon(entries[i].elementKind)){
			rw::V3d at = { entries[i].relX, entries[i].relY, entries[i].relZ };
			RenderElementIconBillboard(entries[i].elementKind, at, dir, max(0.6f, radius*0.18f));
			continue;
		}
		PrefabPreviewModel *model = nil;
		for(int j = 0; j < numModels; j++)
			if(models[j].objectId == entries[i].objectId){
				model = &models[j];
				break;
			}
		if(model == nil)
			continue;

		rw::Quat rotation = { entries[i].rotX, entries[i].rotY, entries[i].rotZ, entries[i].rotW };
		rw::V3d translation = { entries[i].relX, entries[i].relY, entries[i].relZ };
		rw::Matrix matrix;
		BuildPreviewMatrix(&matrix, rotation, translation);
		if(model->atomic){
			model->atomic->getFrame()->transform(&matrix, rw::COMBINEREPLACE);
			model->atomic->render();
		}else{
			model->clump->getFrame()->transform(&matrix, rw::COMBINEREPLACE);
			model->clump->render();
		}
	}

	cam->endUpdate();

	for(int i = 0; i < numModels; i++){
		if(models[i].atomic){ models[i].atomic->getFrame()->destroy(); models[i].atomic->destroy(); }
		if(models[i].clump){ models[i].clump->destroy(); }
	}

	Timecycle::SetLights();
	return true;
}

void
InitPreviewRenderer(void)
{
	if(previewInited || previewInitFailed) return;
	if(getPreviewCamera() == nil){ previewInitFailed = true; return; }

	previewColorRaster = rw::Raster::create(PREVIEW_SIZE, PREVIEW_SIZE, 0, rw::Raster::CAMERATEXTURE);
	if(previewColorRaster == nil){ previewInitFailed = true; return; }
	previewDepthRaster = rw::Raster::create(PREVIEW_SIZE, PREVIEW_SIZE, 0, rw::Raster::ZBUFFER);
	if(previewDepthRaster == nil){ previewInitFailed = true; return; }
	gPreviewTexture = rw::Texture::create(previewColorRaster);
	gPreviewTexture->setFilter(rw::Texture::LINEAR);
	previewInited = true;
}

void
ShutdownPreviewRenderer(void)
{
	initObjectThumbnailSlots();
	for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++)
		destroyObjectThumbnailSlot(&objectThumbnailSlots[i]);
	objectThumbnailUseSeq = 0;
	objectThumbnailInitFailed = false;
	for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS; i++)
		destroyPrefabThumbnailSlot(&prefabThumbnailSlots[i]);
	prefabThumbnailUseSeq = 0;
	prefabThumbnailInitFailed = false;

	if(gPreviewTexture){
		gPreviewTexture->raster = nil;
		gPreviewTexture->destroy();
		gPreviewTexture = nil;
	}
	if(previewColorRaster){ previewColorRaster->destroy(); previewColorRaster = nil; }
	if(previewDepthRaster){ previewDepthRaster->destroy(); previewDepthRaster = nil; }
	if(previewCamera){
		rw::Frame *frame = previewCamera->getFrame();
		if(previewCamera->world)
			previewCamera->world->removeCamera(previewCamera);
		previewCamera->destroy();
		previewCamera = nil;
		if(frame) frame->destroy();
	}
	previewInited = false;
}

rw::Texture*
GetObjectThumbnailTexture(int objectId)
{
	initObjectThumbnailSlots();
	static int thumbnailTheme = -1;
	if(thumbnailTheme != GetUiTheme()){
		thumbnailTheme = GetUiTheme();
		for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++){
			objectThumbnailSlots[i].ready = false;
			objectThumbnailSlots[i].failed = false;
		}
	}
	if(objectId < 0 || GetObjectDef(objectId) == nil)
		return nil;

	int requestFrame = ImGui::GetFrameCount();
	for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++){
		ObjectThumbnailSlot *slot = &objectThumbnailSlots[i];
		if(slot->objectId == objectId){
			slot->lastUsed = ++objectThumbnailUseSeq;
			slot->lastRequestedFrame = requestFrame;
			return slot->ready ? slot->texture : nil;
		}
	}

	ObjectThumbnailSlot *slot = nil;
	for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++)
		if(objectThumbnailSlots[i].objectId < 0){
			slot = &objectThumbnailSlots[i];
			break;
		}
	if(slot == nil){
		for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS; i++){
			if(objectThumbnailSlots[i].lastRequestedFrame == requestFrame)
				continue;
			if(slot == nil || objectThumbnailSlots[i].lastUsed < slot->lastUsed)
				slot = &objectThumbnailSlots[i];
		}
		if(slot == nil)
			return nil;
	}

	slot->objectId = objectId;
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = ++objectThumbnailUseSeq;
	slot->lastRequestedFrame = requestFrame;
	return nil;
}

void
RenderRequestedObjectThumbnails(void)
{
	initObjectThumbnailSlots();
	if(objectThumbnailInitFailed)
		return;

	int attempted = 0;
	for(int i = 0; i < OBJECT_THUMBNAIL_SLOTS && attempted < OBJECT_THUMBNAILS_PER_FRAME; i++){
		ObjectThumbnailSlot *slot = &objectThumbnailSlots[i];
		if(slot->objectId < 0 || slot->ready || slot->failed)
			continue;
		attempted++;
		int objectId = slot->objectId;
		if(!initObjectThumbnailSlot(slot))
			return;
		slot->objectId = objectId;
		float angle = 0.8f + (objectId % 11) * 0.08f;
		if(renderObjectToRaster(objectId, slot->colorRaster, slot->depthRaster, OBJECT_THUMBNAIL_SIZE, angle)){
			slot->ready = true;
		}else{
			ObjectDef *obj = GetObjectDef(objectId);
			if(obj == nil)
				slot->objectId = -1;
			else
				slot->failed = true;
		}
	}
}

rw::Texture*
GetPrefabThumbnailTexture(const char *path)
{
	static int thumbnailTheme = -1;
	if(thumbnailTheme != GetUiTheme()){
		thumbnailTheme = GetUiTheme();
		for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS; i++){
			prefabThumbnailSlots[i].ready = false;
			prefabThumbnailSlots[i].failed = false;
		}
	}
	if(path == nil || path[0] == '\0')
		return nil;

	int requestFrame = ImGui::GetFrameCount();
	for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS; i++){
		PrefabThumbnailSlot *slot = &prefabThumbnailSlots[i];
		if(slot->path[0] != '\0' && strcmp(slot->path, path) == 0){
			slot->lastUsed = ++prefabThumbnailUseSeq;
			slot->lastRequestedFrame = requestFrame;
			// Saved over since it was drawn: draw it again (checked once a second).
			if(requestFrame - slot->checkedFrame > 60){
				slot->checkedFrame = requestFrame;
				struct stat st;
				if(stat(path, &st) == 0 && st.st_mtime != slot->mtime && (slot->ready || slot->failed)){
					slot->ready = false;
					slot->failed = false;
				}
			}
			return slot->ready ? slot->texture : nil;
		}
	}

	PrefabThumbnailSlot *slot = nil;
	for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS; i++)
		if(prefabThumbnailSlots[i].path[0] == '\0'){
			slot = &prefabThumbnailSlots[i];
			break;
		}
	if(slot == nil){
		for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS; i++){
			if(prefabThumbnailSlots[i].lastRequestedFrame == requestFrame)
				continue;
			if(slot == nil || prefabThumbnailSlots[i].lastUsed < slot->lastUsed)
				slot = &prefabThumbnailSlots[i];
		}
		if(slot == nil)
			return nil;
	}

	strncpy(slot->path, path, sizeof(slot->path));
	slot->path[sizeof(slot->path)-1] = '\0';
	slot->ready = false;
	slot->failed = false;
	slot->lastUsed = ++prefabThumbnailUseSeq;
	slot->lastRequestedFrame = requestFrame;
	return nil;
}

void
RenderRequestedPrefabThumbnails(void)
{
	if(prefabThumbnailInitFailed)
		return;

	int attempted = 0;
	for(int i = 0; i < PREFAB_THUMBNAIL_SLOTS && attempted < PREFAB_THUMBNAILS_PER_FRAME; i++){
		PrefabThumbnailSlot *slot = &prefabThumbnailSlots[i];
		if(slot->path[0] == '\0' || slot->ready || slot->failed)
			continue;
		attempted++;
		char path[512];
		strncpy(path, slot->path, sizeof(path));
		path[sizeof(path)-1] = '\0';
		if(!initPrefabThumbnailSlot(slot))
			return;
		strncpy(slot->path, path, sizeof(slot->path));
		slot->path[sizeof(slot->path)-1] = '\0';
		struct stat st;
		slot->mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
		slot->checkedFrame = ImGui::GetFrameCount();
		if(renderPrefabToRaster(path, slot->colorRaster, slot->depthRaster, PREFAB_THUMBNAIL_SIZE))
			slot->ready = true;
		else if(!doesFileExist(path))
			slot->path[0] = '\0';
		else
			slot->failed = true;
	}
}

void
RenderPreviewObject(int objectId)
{
	if(previewInitFailed) return;
	if(objectId < 0) return;
	if(!previewInited) InitPreviewRenderer();
	if(!previewInited) return;

	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil) return;
	if(!obj->IsLoaded()){
		RequestObject(objectId);
		return;
	}

	// Clone model for preview (persistent, rebuilt when selection changes)
	static rw::Atomic *previewAtm = nil;
	static rw::Clump *previewClump = nil;
	static int previewCloneId = -1;

	if(previewCloneId != objectId){
		if(previewAtm){ previewAtm->getFrame()->destroy(); previewAtm->destroy(); previewAtm = nil; }
		if(previewClump){ previewClump->destroy(); previewClump = nil; }
		previewCloneId = objectId;

		if(obj->m_type == ObjectDef::ATOMIC && obj->m_atomics[0]){
			previewAtm = obj->m_atomics[0]->clone();
			rw::Frame *f = rw::Frame::create();
			previewAtm->setFrame(f);
		}else if(obj->m_type == ObjectDef::CLUMP && obj->m_clump){
			previewClump = obj->m_clump->clone();
			paintPreviewVehicle(obj, previewClump);
		}
	}
	if(previewAtm == nil && previewClump == nil) return;

	// Camera setup: same framing as the browser thumbnails
	rw::V3d target = { 0.0f, 0.0f, 0.0f };
	float radius = 5.0f;
	if(!previewGeometryBounds(previewAtm, previewClump, &target, &radius) && obj->m_colModel){
		target = obj->m_colModel->boundingSphere.center;
		radius = max(obj->m_colModel->boundingSphere.radius, 0.25f);
	}
	// The sphere just fits the 60 degree view at r / sin(30).
	float dist = radius / 0.5f * 1.02f;

	static float angle = 0.0f;
	angle += 0.01f;
	if(angle > 2.0f * 3.14159f) angle -= 2.0f * 3.14159f;

	rw::V3d viewDir = normalize(rw::V3d{ cosf(angle), sinf(angle), 0.45f });
	rw::V3d eye = add(target, scale(viewDir, dist));
	rw::V3d up = { 0.0f, 0.0f, -1.0f };  // negative Z because FBO is Y-flipped
	rw::V3d dir = normalize(sub(target, eye));

	rw::Camera *cam = getPreviewCamera();
	if(cam == nil)
		return;
	rw::Frame *camFrame = cam->getFrame();

	// Set preview camera state
	cam->frameBuffer = previewColorRaster;
	cam->zBuffer = previewDepthRaster;
	cam->setNearPlane(max(0.05f, dist - radius * 1.5f));
	cam->setFarPlane(dist + radius * 2.0f + 10.0f);
	cam->fogPlane = dist + radius * 2.0f;
	cam->setFOV(60.0f, 1.0f);
	camFrame->matrix.pos = eye;
	camFrame->matrix.lookAt(dir, up);
	camFrame->updateObjects();

	rw::RGBA clearCol = previewBackground();
	cam->clear(&clearCol, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	cam->beginUpdate();

	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	// One white ambient light, no directional one, and no culling, so the
	// one-sided faces of wedges and panels show from either side.
	uint32 oldCull = rw::GetRenderState(rw::CULLMODE);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	pAmbient->setColor(1.0f, 1.0f, 1.0f);
	pDirect->setColor(0.0f, 0.0f, 0.0f);

	rw::Matrix ident;
	ident.setIdentity();
	if(previewAtm){
		previewAtm->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		previewAtm->render();
	}else if(previewClump){
		previewClump->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		previewClump->render();
	}

	cam->endUpdate();

	Timecycle::SetLights();
}

// Clipboard
static std::vector<ObjectInst*> clipboard;
static bool clipboardIsCut;

// Undo/Redo system
#define MAX_UNDO 128
static UndoAction undoStack[MAX_UNDO];
static int undoCount;	// number of actions in stack
static int undoPos;	// current position (next undo)

void
ResetUndoHistory(void)
{
	for(int i = 0; i < undoCount; i++)
		undoStack[i] = UndoAction();
	undoCount = 0;
	undoPos = 0;
}

static void
updateRwFrameForInst(ObjectInst *inst)
{
	if(inst->m_rwObject == nil) return;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return;
	rw::Frame *f;
	if(obj->m_type == ObjectDef::ATOMIC)
		f = ((rw::Atomic*)inst->m_rwObject)->getFrame();
	else
		f = ((rw::Clump*)inst->m_rwObject)->getFrame();
	f->transform(&inst->m_matrix, rw::COMBINEREPLACE);
}

static void
applyUndoTransform(const UndoTransform &t, bool useNewState)
{
	ObjectInst *inst = t.inst;
	if(inst == nil)
		return;

	bool refreshSectors = (t.flags & (UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT)) != 0;
	if(refreshSectors)
		RemoveInstFromSectors(inst);
	if(t.flags & UNDO_TRANSFORM_POS)
		inst->m_translation = useNewState ? t.newPos : t.oldPos;
	if(t.flags & UNDO_TRANSFORM_ROT)
		inst->m_rotation = useNewState ? t.newRot : t.oldRot;

	StampChangeSeq(inst);
	inst->m_isDirty = true;
	inst->UpdateMatrix();
	updateRwFrameForInst(inst);

	if(refreshSectors)
		InsertInstIntoSectors(inst);
}

static int undoGroupDepth;
static int undoCurrentGroup;
static int undoNextGroup = 1;

void
UndoBeginGroup(void)
{
	if(undoGroupDepth++ == 0)
		undoCurrentGroup = undoNextGroup++;
}

void
UndoEndGroup(void)
{
	if(undoGroupDepth > 0 && --undoGroupDepth == 0)
		undoCurrentGroup = 0;
}

// While one widget stays active (a drag, typing in a field), its edits
// are one step: a new step for the same instance and kind updates the one
// recorded since the widget became active instead of stacking up.
static ImGuiID undoCoalesceWidget;
static int undoCoalesceStart;

static bool
coalesceUndo(UndoAction *a)
{
	ImGuiID widget = ImGui::GetCurrentContext() ? ImGui::GetActiveID() : 0;
	if(widget == 0 || widget != undoCoalesceWidget || undoPos != undoCount){
		undoCoalesceWidget = widget;
		undoCoalesceStart = undoCount;
		return false;
	}
	for(int i = undoCount - 1; i >= undoCoalesceStart && i >= 0; i--){
		UndoAction *top = &undoStack[i];
		if(top->type != a->type || top->inst != a->inst)
			continue;
		switch(a->type){
		case UNDO_PROPERTIES: top->newProperties = a->newProperties; return true;
		case UNDO_VEHICLE: top->newVehicle = a->newVehicle; return true;
		case UNDO_MODEL_ID: top->newModelId = a->newModelId; return true;
		case UNDO_MATERIAL:
			if(top->materialSlot != a->materialSlot)
				continue;
			top->newMaterial = a->newMaterial;
			return true;
		}
		return false;
	}
	return false;
}

// Undo steps recorded so far: a group counts once.
static int undoStepsRecorded;
static int undoLastPushedGroup;

int
GetUndoPosition(void)
{
	return undoStepsRecorded;
}

bool
MapHasUndoHistory(void)
{
	return undoCount > 0;
}

static void
pushUndo(UndoAction *a)
{
	if(coalesceUndo(a))
		return;
	a->group = undoGroupDepth > 0 ? undoCurrentGroup : 0;
	if(a->group == 0 || a->group != undoLastPushedGroup)
		undoStepsRecorded++;
	undoLastPushedGroup = a->group;
	// If we're not at the top, discard redo history
	for(int i = undoPos; i < undoCount; i++)
		undoStack[i] = UndoAction();
	undoCount = undoPos;
	if(undoCount >= MAX_UNDO){
		// Shift everything down
		for(int i = 1; i < MAX_UNDO; i++)
			undoStack[i-1] = std::move(undoStack[i]);
		undoStack[MAX_UNDO-1] = UndoAction();
		undoCount--;
		undoPos--;
		if(undoCoalesceStart > 0)
			undoCoalesceStart--;
	}
	undoStack[undoCount] = std::move(*a);
	undoCount++;
	undoPos = undoCount;
	if(undoCoalesceStart > undoCount - 1)
		undoCoalesceStart = undoCount - 1;
}

// Takes out the steps forget() empties (it may also trim them), keeping the
// rest in order.
template <typename Forget> static void
forgetUndoSteps(Forget forget)
{
	int kept = 0, keptBeforePos = 0;
	for(int i = 0; i < undoCount; i++){
		if(forget(undoStack[i]))
			continue;
		if(i < undoPos)
			keptBeforePos++;
		if(kept != i)
			undoStack[kept] = std::move(undoStack[i]);
		kept++;
	}
	for(int i = kept; i < undoCount; i++)
		undoStack[i] = UndoAction();
	undoCount = kept;
	undoPos = keptBeforePos;
	undoCoalesceWidget = 0;	// its indices may have moved
}

// Someone else changed inst (multiplayer): a step that would put it back
// would throw their change away, so the steps lose it, and a step left with
// nothing to do goes.
void
UndoForgetInstance(ObjectInst *inst)
{
	auto drop = [inst](std::vector<ObjectInst*> &list) {
		list.erase(std::remove(list.begin(), list.end(), inst), list.end());
		return (int)list.size();
	};
	forgetUndoSteps([&](UndoAction &a) {
		switch(a.type){
		case UNDO_MOVE:
			if(a.lodInst == inst)
				a.lodInst = nil;
			return a.inst == inst;
		case UNDO_ROTATE:
		case UNDO_MATERIAL:
		case UNDO_PROPERTIES:
		case UNDO_VEHICLE:
		case UNDO_MODEL_ID:
			return a.inst == inst;
		case UNDO_DELETE:
			return (a.numDeleted = drop(a.deletedInsts)) == 0;
		case UNDO_PASTE:
			return (a.numPasted = drop(a.pastedInsts)) == 0;
		case UNDO_WORLD_CONVERSION:
			return drop(a.convertedWorldObjects) == 0;
		case UNDO_TRANSFORM_BATCH:
			a.transforms.erase(std::remove_if(a.transforms.begin(), a.transforms.end(),
				[inst](const UndoTransform &t) { return t.inst == inst; }), a.transforms.end());
			a.numTransforms = (int)a.transforms.size();
			return a.numTransforms == 0;
		}
		return false;
	});
}

// Someone else replaced a whole list (radar art, 3D paths): its steps are
// snapshots of it, which would throw their change away.
void
UndoForgetKind(int type)
{
	forgetUndoSteps([type](UndoAction &a) { return a.type == type; });
}

void
UndoRecordMove(ObjectInst *inst, rw::V3d oldPos, ObjectInst *lodInst, rw::V3d lodOldPos)
{
	UndoAction a = {};
	a.type = UNDO_MOVE;
	a.inst = inst;
	a.oldPos = oldPos;
	a.newPos = inst->m_translation;
	a.lodInst = lodInst;
	a.lodOldPos = lodOldPos;
	if(lodInst)
		a.lodNewPos = lodInst->m_translation;
	pushUndo(&a);
}

void
UndoRecordRotate(ObjectInst *inst, rw::Quat oldRot)
{
	UndoAction a = {};
	a.type = UNDO_ROTATE;
	a.inst = inst;
	a.oldRot = oldRot;
	a.newRot = inst->m_rotation;
	pushUndo(&a);
}

void
UndoRecordDelete(ObjectInst **insts, int num)
{
	UndoAction a = {};
	a.type = UNDO_DELETE;
	a.numDeleted = num;
	a.deletedInsts.assign(insts, insts + num);
	pushUndo(&a);
}

void
UndoRecordPaste(ObjectInst **insts, int num)
{
	UndoAction a = {};
	a.type = UNDO_PASTE;
	a.numPasted = num;
	a.pastedInsts.assign(insts, insts + num);
	pushUndo(&a);
}

void
UndoRecordTransformBatch(UndoTransform *transforms, int num)
{
	if(num <= 0)
		return;

	UndoAction a = {};
	a.type = UNDO_TRANSFORM_BATCH;
	a.numTransforms = num;
	a.transforms.assign(transforms, transforms + num);
	pushUndo(&a);
}

void
UndoRecordMaterial(ObjectInst *inst, int slot, const ObjectMaterialOverride &oldMaterial)
{
	if(inst == nil || slot < 0 || slot >= MAX_PAWN_MATERIALS)
		return;
	UndoAction a = {};	// it holds vectors: never memset it
	a.type = UNDO_MATERIAL;
	a.inst = inst;
	a.materialSlot = slot;
	a.oldMaterial = oldMaterial;
	a.newMaterial = inst->m_materialOverrides[slot];
	pushUndo(&a);
}

void
UndoRecordProperties(ObjectInst *inst, const ObjectPawnProperties &oldProperties)
{
	if(inst == nil)
		return;
	UndoAction a = {};
	a.type = UNDO_PROPERTIES;
	a.inst = inst;
	a.oldProperties = oldProperties;
	a.newProperties = *GetObjectPawnProperties(inst);
	pushUndo(&a);
}

int
ConvertWorldObjectsToMapObjects(const std::vector<ObjectInst*> &objects)
{
	if(IsIplMapDocumentOpen())
		return 0;
	UndoAction action = {};
	action.type = UNDO_WORLD_CONVERSION;
	for(ObjectInst *inst : objects){
		if(inst == nil || inst->m_isDeleted || !IsPawnWorldObject(inst) || !IsInstInIplMapDocument(inst))
			continue;
		ObjectPawnProperties properties = *GetObjectPawnProperties(inst);
		if(properties.forceWorldReplacement)
			continue;
		properties.forceWorldReplacement = true;
		SetObjectPawnProperties(inst, properties, false);
		action.convertedWorldObjects.push_back(inst);
	}
	int count = (int)action.convertedWorldObjects.size();
	if(count)
		pushUndo(&action);
	return count;
}

VehicleState
GetVehicleState(const ObjectInst *inst)
{
	VehicleState s = {};
	s.isStatic = inst->m_vehicleStatic;
	s.color1 = inst->m_vehicleColor1;
	s.color2 = inst->m_vehicleColor2;
	s.respawnDelay = inst->m_vehicleRespawnDelay;
	s.paintjob = inst->m_vehiclePaintjob;
	memcpy(s.plate, inst->m_vehiclePlate, sizeof(s.plate));
	memcpy(s.mods, inst->m_vehicleMods, sizeof(s.mods));
	return s;
}

void
SetVehicleState(ObjectInst *inst, const VehicleState &s)
{
	inst->m_vehicleStatic = s.isStatic;
	inst->m_vehicleColor1 = s.color1;
	inst->m_vehicleColor2 = s.color2;
	inst->m_vehicleRespawnDelay = s.respawnDelay;
	inst->m_vehiclePaintjob = s.paintjob;
	memcpy(inst->m_vehiclePlate, s.plate, sizeof(s.plate));
	memcpy(inst->m_vehicleMods, s.mods, sizeof(s.mods));
	StampChangeSeq(inst);
	inst->m_isDirty = true;
	inst->RefreshVehicleUpgrades();
	inst->RefreshVehicleColors();
}

void
UndoRecordVehicle(ObjectInst *inst, const VehicleState &before)
{
	VehicleState after = GetVehicleState(inst);
	if(memcmp(&before, &after, sizeof(before)) == 0)
		return;
	UndoAction a = {};
	a.type = UNDO_VEHICLE;
	a.inst = inst;
	a.oldVehicle = before;
	a.newVehicle = after;
	pushUndo(&a);
}

void
UndoRecordModelId(ObjectInst *inst, int oldModelId)
{
	if(inst == nil)
		return;
	UndoAction a = {};
	a.type = UNDO_MODEL_ID;
	a.inst = inst;
	a.oldModelId = oldModelId;
	a.newModelId = inst->m_objectId;
	pushUndo(&a);
}

void
UndoRecordNodes(const std::vector<int> &areas, const std::vector<std::shared_ptr<void>> &before,
	const std::vector<std::shared_ptr<void>> &after)
{
	UndoAction a = {};
	a.type = UNDO_NODES;
	a.nodeAreas = areas;
	a.nodesBefore = before;
	a.nodesAfter = after;
	pushUndo(&a);
}

void
UndoRecordRadarArt(const std::shared_ptr<void> &before, const std::shared_ptr<void> &after)
{
	UndoAction a = {};
	a.type = UNDO_RADAR_ART;
	a.radarBefore = before;
	a.radarAfter = after;
	pushUndo(&a);
}

void UndoRecordLayoutPaths(const std::shared_ptr<void> &before, const std::shared_ptr<void> &after)
{
	UndoAction a = {};
	a.type = UNDO_LAYOUT_PATHS;
	a.radarBefore = before;
	a.radarAfter = after;
	pushUndo(&a);
}

void SetLayoutObjectState(ObjectInst *inst, const rw::V3d &position, const rw::Quat &rotation, bool deleted)
{
	if(!inst) return;
	RemoveInstFromSectors(inst);
	inst->m_translation = position;
	inst->m_rotation = rotation;
	setDeletedWithoutCascade(inst, deleted);
	inst->m_isDirty = true;
	inst->UpdateMatrix();
	updateRwFrameForInst(inst);
	StampChangeSeq(inst);
	if(!deleted) InsertInstIntoSectors(inst);
}

static void undoStep(void);
static void redoStep(void);

void
Undo(void)
{
	if(undoPos <= 0) return;
	int group = undoStack[undoPos-1].group;
	do
		undoStep();
	while(group != 0 && undoPos > 0 && undoStack[undoPos-1].group == group);
}

void
Redo(void)
{
	if(undoPos >= undoCount) return;
	int group = undoStack[undoPos].group;
	do
		redoStep();
	while(group != 0 && undoPos < undoCount && undoStack[undoPos].group == group);
}

static void
undoStep(void)
{
	undoPos--;
	UndoAction *a = &undoStack[undoPos];

	switch(a->type){
	case UNDO_MOVE:
		a->inst->m_translation = a->oldPos;
		a->inst->UpdateMatrix();
		updateRwFrameForInst(a->inst);
		if(a->lodInst){
			a->lodInst->m_translation = a->lodOldPos;
			a->lodInst->UpdateMatrix();
			updateRwFrameForInst(a->lodInst);
		}
		break;
	case UNDO_ROTATE:
		a->inst->m_rotation = a->oldRot;
		a->inst->UpdateMatrix();
		updateRwFrameForInst(a->inst);
		break;
	case UNDO_DELETE:
		for(int i = 0; i < a->numDeleted; i++)
			setDeletedWithoutCascade(a->deletedInsts[i], false);
		break;
	case UNDO_PASTE:
		for(int i = 0; i < a->numPasted; i++){
			a->pastedInsts[i]->Deselect();
			a->pastedInsts[i]->Delete();
		}
		break;
	case UNDO_TRANSFORM_BATCH:
		for(int i = 0; i < a->numTransforms; i++)
			applyUndoTransform(a->transforms[i], false);
		break;
	case UNDO_MATERIAL:
		SetObjectMaterialOverride(a->inst, a->materialSlot, a->oldMaterial, false);
		break;
	case UNDO_PROPERTIES:
		SetObjectPawnProperties(a->inst, a->oldProperties, false);
		break;
	case UNDO_WORLD_CONVERSION:
		for(ObjectInst *inst : a->convertedWorldObjects){
			ObjectPawnProperties properties = *GetObjectPawnProperties(inst);
			properties.forceWorldReplacement = false;
			SetObjectPawnProperties(inst, properties, false);
		}
		break;
	case UNDO_VEHICLE:
		SetVehicleState(a->inst, a->oldVehicle);
		break;
	case UNDO_MODEL_ID:
		ChangeInstanceModelId(a->inst, a->oldModelId, false);
		break;
	case UNDO_NODES:
		SAPaths::RestoreNodeAreas(a->nodeAreas, a->nodesBefore);
		break;
	case UNDO_RADAR_ART:
		RestoreRadarArtLayers(a->radarBefore);
		break;
	case UNDO_LAYOUT_PATHS:
		RestoreLayoutPaths(a->radarBefore);
		break;
	}
}

static void
redoStep(void)
{
	UndoAction *a = &undoStack[undoPos];
	undoPos++;

	switch(a->type){
	case UNDO_MOVE:
		a->inst->m_translation = a->newPos;
		a->inst->UpdateMatrix();
		updateRwFrameForInst(a->inst);
		if(a->lodInst){
			a->lodInst->m_translation = a->lodNewPos;
			a->lodInst->UpdateMatrix();
			updateRwFrameForInst(a->lodInst);
		}
		break;
	case UNDO_ROTATE:
		a->inst->m_rotation = a->newRot;
		a->inst->UpdateMatrix();
		updateRwFrameForInst(a->inst);
		break;
	case UNDO_DELETE:
		for(int i = 0; i < a->numDeleted; i++)
			setDeletedWithoutCascade(a->deletedInsts[i], true);
		break;
	case UNDO_PASTE:
		for(int i = 0; i < a->numPasted; i++){
			a->pastedInsts[i]->Undelete();
			a->pastedInsts[i]->Select();
		}
		break;
	case UNDO_TRANSFORM_BATCH:
		for(int i = 0; i < a->numTransforms; i++)
			applyUndoTransform(a->transforms[i], true);
		break;
	case UNDO_MATERIAL:
		SetObjectMaterialOverride(a->inst, a->materialSlot, a->newMaterial, false);
		break;
	case UNDO_PROPERTIES:
		SetObjectPawnProperties(a->inst, a->newProperties, false);
		break;
	case UNDO_WORLD_CONVERSION:
		for(ObjectInst *inst : a->convertedWorldObjects){
			ObjectPawnProperties properties = *GetObjectPawnProperties(inst);
			properties.forceWorldReplacement = true;
			SetObjectPawnProperties(inst, properties, false);
		}
		break;
	case UNDO_VEHICLE:
		SetVehicleState(a->inst, a->newVehicle);
		break;
	case UNDO_MODEL_ID:
		ChangeInstanceModelId(a->inst, a->newModelId, false);
		break;
	case UNDO_NODES:
		SAPaths::RestoreNodeAreas(a->nodeAreas, a->nodesAfter);
		break;
	case UNDO_RADAR_ART:
		RestoreRadarArtLayers(a->radarAfter);
		break;
	case UNDO_LAYOUT_PATHS:
		RestoreLayoutPaths(a->radarAfter);
		break;
	}
}

// Instances are never taken out of the list, so ids map to them for good.
static std::unordered_map<int32, ObjectInst*> instancesById;

ObjectInst*
GetInstanceByID(int32 id)
{
	std::unordered_map<int32, ObjectInst*>::iterator it = instancesById.find(id);
	return it == instancesById.end() ? nil : it->second;
}

ObjectInst*
AddInstance(void)
{
	static int32 id = 1;

	ObjectInst *inst = new ObjectInst;
	memset(inst, 0, sizeof(ObjectInst));
	inst->m_id = id++;
	instancesById[inst->m_id] = inst;
	inst->m_imageIndex = -1;
	inst->m_binInstIndex = -1;
	instances.InsertItem(inst);
	gAttachmentListDirty = true;
	return inst;
}

void
CopySelected(void)
{
	clipboard.clear();
	clipboardIsCut = false;
	CPtrNode *p;
	bool capped = false;
	for(p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted){
			if((int)clipboard.size() >= MAX_BATCH_OBJECTS){
				capped = true;
				break;
			}
			clipboard.push_back(inst);
		}
	}
	if(capped)
		Toast(TOAST_COPY_PASTE, _("Copy limited to first %d selected instance(s)"), MAX_BATCH_OBJECTS);
	if(!clipboard.empty())
		log("Copied %d instance(s)\n", (int)clipboard.size());
}

void
CutSelected(void)
{
	clipboard.clear();
	clipboardIsCut = false;
	CPtrNode *p;
	bool capped = false;
	for(p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted){
			if((int)clipboard.size() >= MAX_BATCH_OBJECTS){
				capped = true;
				break;
			}
			clipboard.push_back(inst);
		}
	}
	if(capped)
		Toast(TOAST_COPY_PASTE, _("Cut limited to first %d selected instance(s)"), MAX_BATCH_OBJECTS);
	if(clipboard.empty())
		return;

	clipboardIsCut = true;
	DeleteSelected();
	log("Cut %d instance(s)\n", (int)clipboard.size());
}

static ObjectInst*
cloneInstanceWithObjectId(ObjectInst *src, GameFile *dstFile, int iplIndex, rw::V3d offset,
	int objectId, bool preserveBigBuildingState)
{
	ObjectInst *inst = AddInstance();
	inst->m_objectId = objectId;
	inst->m_area = src->m_area;
	inst->m_rotation = src->m_rotation;
	inst->m_translation.x = src->m_translation.x + offset.x;
	inst->m_translation.y = src->m_translation.y + offset.y;
	inst->m_translation.z = src->m_translation.z + offset.z;
	inst->m_isUnimportant = src->m_isUnimportant;
	inst->m_isUnderWater = src->m_isUnderWater;
	inst->m_isTunnel = src->m_isTunnel;
	inst->m_isTunnelTransition = src->m_isTunnelTransition;
	inst->m_isVehicle = src->m_isVehicle;
	inst->m_vehicleStatic = src->m_vehicleStatic;
	inst->m_vehicleColor1 = src->m_vehicleColor1;
	inst->m_vehicleColor2 = src->m_vehicleColor2;
	inst->m_vehicleRespawnDelay = src->m_vehicleRespawnDelay;
	inst->m_vehiclePaintjob = src->m_vehiclePaintjob;
	memcpy(inst->m_vehiclePlate, src->m_vehiclePlate, sizeof(inst->m_vehiclePlate));
	memcpy(inst->m_vehicleMods, src->m_vehicleMods, sizeof(inst->m_vehicleMods));
	inst->m_isBigBuilding = preserveBigBuildingState ? src->m_isBigBuilding : false;
	inst->m_lodId = -1;
	inst->m_lod = nil;
	inst->m_numChildren = 0;
	inst->m_file = dstFile;
	inst->m_imageIndex = -1;
	inst->m_binInstIndex = -1;
	inst->m_iplIndex = iplIndex;
	SetInstIplFilterKey(inst, dstFile ? dstFile->name : nil);
	inst->m_isAdded = true;
	inst->m_iplRotation = IsIplMapDocumentOpen();
	inst->m_isDirty = true;
	inst->m_savedStateValid = false;
	inst->m_wasSavedDeleted = false;
	inst->m_gameEntityExists = false;
	if(src->m_materialOverrides){
		ObjectMaterialOverride *overrides = ensureObjectMaterialOverrides(inst);
		memcpy(overrides, src->m_materialOverrides,
		       sizeof(ObjectMaterialOverride) * MAX_PAWN_MATERIALS);
	}
	inst->m_materialGeometryApplied = false;
	inst->m_materialBaseGeometry = nil;
	inst->m_materialCustomGeometry = nil;
	if(src->m_pawnProperties){
		// A copy keeps its group, comment and distances. Names are unique and
		// a copy placed elsewhere would snap back onto its source's parent.
		ObjectPawnProperties copy = *src->m_pawnProperties;
		copy.name[0] = '\0';
		copy.attachParent = -1;
		copy.attachOffset = { 0.0f, 0.0f, 0.0f };
		copy.attachRotation = { 0.0f, 0.0f, 0.0f };
		copy.attachSyncRotation = true;
		// Two NPCs cannot share a player name, or one seat.
		if(copy.elementKind == SAMP_ELEMENT_NPC){
			MakeUniqueNpcName(copy.npcName, copy.npcName, sizeof(copy.npcName));
			copy.npcVehicle = -1;
			copy.npcSeat = 0;
		}
		if(copy.elementKind == SAMP_ELEMENT_PATHPOINT)
			copy.pathOrder = nextPathOrder(copy.pathName);
		if(copy.elementKind == SAMP_ELEMENT_CHECKPOINT && copy.cpKind == SAMP_CHECKPOINT_RACE)
			copy.cpOrder = nextRaceOrder(copy.cpRace);
		// Items name their area by its variable, so an area always has one.
		if(copy.elementKind == SAMP_ELEMENT_AREA)
			MakeUniqueAreaName(src->m_pawnProperties->name, copy.name, sizeof(copy.name));
		if(!ObjectPawnPropertiesAreDefault(copy))
			inst->m_pawnProperties = new ObjectPawnProperties(copy);
	}
	StampChangeSeq(inst);
	ObjectDef *obj = GetObjectDef(objectId);
	if(!preserveBigBuildingState && obj && obj->m_isBigBuilding)
		inst->SetupBigBuilding();
	inst->UpdateMatrix();
	if(obj && !obj->IsLoaded()){
		RequestObject(objectId);
		LoadAllRequestedObjects();
	}
	inst->CreateRwObject();
	InsertInstIntoSectors(inst);
	return inst;
}

static ObjectInst*
cloneInstance(ObjectInst *src, GameFile *dstFile, int iplIndex, rw::V3d offset)
{
	return cloneInstanceWithObjectId(src, dstFile, iplIndex, offset, src->m_objectId, true);
}

static GameFile*
findPasteDestinationFile(ObjectInst *src)
{
	if(src == nil)
		return nil;

	// A paste creates a new placement. Keep it out of the source map IPL so
	// copying one original-map object does not force a full replacement export
	// of that area's IPL.
	return GetOrCreateCurrentPlacementIplFile();
}

static float
instanceDistanceSq(ObjectInst *a, ObjectInst *b)
{
	rw::V3d d = sub(a->m_translation, b->m_translation);
	return dot(d, d);
}

static int
getAssociatedLodObjectId(ObjectInst *src)
{
	if(src == nil || !isSA())
		return -1;
	int lodObjId = GetLodForObject(src->m_objectId);
	if(lodObjId < 0 || lodObjId == src->m_objectId || GetObjectDef(lodObjId) == nil)
		return -1;
	return lodObjId;
}

static ObjectInst*
findNearbyAssociatedLod(ObjectInst *src, int lodObjId)
{
	if(src == nil || lodObjId < 0)
		return nil;

	ObjectInst *bestSameFamily = nil;
	ObjectInst *bestAnyFamily = nil;
	float bestSameFamilyDistSq = 1.0e30f;
	float bestAnyFamilyDistSq = 1.0e30f;

	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *other = (ObjectInst*)p->item;
		if(other == nil || other == src || other->m_isDeleted)
			continue;
		if(other->m_objectId != lodObjId)
			continue;

		float distSq = instanceDistanceSq(src, other);
		if(sameIplFamily(src, other) && distSq < bestSameFamilyDistSq){
			bestSameFamily = other;
			bestSameFamilyDistSq = distSq;
		}
		if(distSq < bestAnyFamilyDistSq){
			bestAnyFamily = other;
			bestAnyFamilyDistSq = distSq;
		}
	}

	// LOD pairs normally sit at the same placement. Keep the fallback close
	// enough to avoid borrowing a repeated LOD model from another zone.
	if(bestSameFamily && bestSameFamilyDistSq <= sq(250.0f))
		return bestSameFamily;
	if(bestAnyFamily && bestAnyFamilyDistSq <= sq(25.0f))
		return bestAnyFamily;
	return nil;
}

static ObjectInst*
findPasteSourceLod(ObjectInst *src, int *lodObjIdOut, bool *usedAssociationOut)
{
	if(lodObjIdOut)
		*lodObjIdOut = -1;
	if(usedAssociationOut)
		*usedAssociationOut = false;
	if(src == nil || !isSA())
		return nil;
	if(src->m_lod && !src->m_lod->m_isDeleted)
		return src->m_lod;

	int lodObjId = getAssociatedLodObjectId(src);
	if(lodObjIdOut)
		*lodObjIdOut = lodObjId;
	if(lodObjId < 0)
		return nil;

	ObjectInst *lod = findNearbyAssociatedLod(src, lodObjId);
	if(lod && usedAssociationOut)
		*usedAssociationOut = true;
	return lod;
}

static rw::V3d
getClipboardPasteOffset(ObjectInst **toPaste, int numToPaste, bool pasteInPlace)
{
	if(pasteInPlace)
		return { 0.0f, 0.0f, 0.0f };

	rw::V3d anchor = { 0.0f, 0.0f, 0.0f };
	int numAnchors = 0;

	for(int i = 0; i < numToPaste; i++){
		ObjectInst *inst = toPaste[i];
		if(inst == nil || inst->m_isDeleted)
			continue;
		anchor.x += inst->m_translation.x;
		anchor.y += inst->m_translation.y;
		anchor.z += inst->m_translation.z;
		numAnchors++;
	}

	if(numAnchors == 0)
		return { 10.0f, 0.0f, 0.0f };

	float invCount = 1.0f / (float)numAnchors;
	anchor.x *= invCount;
	anchor.y *= invCount;
	anchor.z *= invCount;

	rw::V3d pastePos = TheCamera.m_target;
	rw::V3d groundHit;
	if(gPlaceSnapToGround && GetGroundPlacementSurface(pastePos, &groundHit, nil, true))
		pastePos = groundHit;

	return sub(pastePos, anchor);
}

static int
PasteCutClipboard(void)
{
	std::vector<ObjectInst*> restored;

	ClearSelection();
	for(int i = 0; i < (int)clipboard.size(); i++){
		ObjectInst *inst = clipboard[i];
		if(inst == nil)
			continue;
		if(inst->m_isDeleted)
			inst->Undelete();
		inst->Select();
		restored.push_back(inst);
	}

	if(!restored.empty()){
		UndoRecordPaste(restored.data(), (int)restored.size());
		log("Restored %d cut instance(s)\n", (int)restored.size());
	}
	clipboardIsCut = false;
	return (int)restored.size();
}

static int
pasteClipboard(bool pasteInPlace)
{
	if(clipboard.empty()) return 0;
	if(clipboardIsCut)
		return PasteCutClipboard();

	std::vector<ObjectInst*> pasted;

	// Deduplicate: skip LODs that are already the m_lod of another
	// clipboard entry (they'll be auto-copied with their HD parent)
	std::vector<ObjectInst*> toPaste;
	std::unordered_set<ObjectInst*> lodsCopiedWithParent;
	lodsCopiedWithParent.reserve(clipboard.size());

	for(int i = 0; i < (int)clipboard.size(); i++){
		ObjectInst *lod = findPasteSourceLod(clipboard[i], nil, nil);
		if(lod)
			lodsCopiedWithParent.insert(lod);
	}
	for(int i = 0; i < (int)clipboard.size(); i++)
		if(lodsCopiedWithParent.find(clipboard[i]) == lodsCopiedWithParent.end())
			toPaste.push_back(clipboard[i]);

	rw::V3d offset = getClipboardPasteOffset(toPaste.data(), (int)toPaste.size(), pasteInPlace);

	ClearSelection();

	int recoveredLods = 0;
	int generatedLods = 0;
	std::unordered_map<GameFile*, int> maxIplIndexByFile;

	for(int i = 0; i < (int)toPaste.size(); i++){
		ObjectInst *src = toPaste[i];
		int associatedLodObjId = -1;
		bool usedAssociatedSource = false;
		ObjectInst *srcLod = findPasteSourceLod(src, &associatedLodObjId, &usedAssociatedSource);

		GameFile *dstFile = findPasteDestinationFile(src);

		std::unordered_map<GameFile*, int>::iterator maxIt = maxIplIndexByFile.find(dstFile);
		if(maxIt == maxIplIndexByFile.end()){
			int maxIplIndex = -1;
			CPtrNode *p;
			for(p = instances.first; p; p = p->next){
				ObjectInst *other = (ObjectInst*)p->item;
				if(other->m_file == dstFile && other->m_imageIndex < 0){
					if(other->m_iplIndex > maxIplIndex)
						maxIplIndex = other->m_iplIndex;
				}
			}
			maxIt = maxIplIndexByFile.insert(std::make_pair(dstFile, maxIplIndex)).first;
		}

		// Paste LOD first (if any) so we have its iplIndex for the HD's lodId
		ObjectInst *newLod = nil;
		if(isSA() && srcLod && !srcLod->m_isDeleted){
			newLod = cloneInstance(srcLod, dstFile, ++maxIt->second, offset);
			pasted.push_back(newLod);
			if(usedAssociatedSource)
				recoveredLods++;
		}else if(isSA() && associatedLodObjId >= 0){
			newLod = cloneInstanceWithObjectId(src, dstFile, ++maxIt->second, offset,
				associatedLodObjId, false);
			pasted.push_back(newLod);
			generatedLods++;
		}

		// Paste HD
		ObjectInst *newHd = cloneInstance(src, dstFile, ++maxIt->second, offset);

		// Link LOD
		if(newLod)
			finalizeLinkedLod(newHd, newLod);

		newHd->Select();
		pasted.push_back(newHd);
	}

	if(!pasted.empty()){
		UndoRecordPaste(pasted.data(), (int)pasted.size());
		log("Pasted %d instance(s)\n", (int)pasted.size());
		if(recoveredLods > 0)
			log("Recovered %d LOD link(s) from nearby associated instances\n", recoveredLods);
			if(generatedLods > 0)
				log("Generated %d associated LOD instance(s) during paste\n", generatedLods);
	}
	return (int)pasted.size();
}

int
PasteClipboard(void)
{
	return pasteClipboard(false);
}

int
PasteClipboardInPlace(void)
{
	return pasteClipboard(true);
}

int
DuplicateSelected(void)
{
	std::vector<ObjectInst*> savedClipboard = clipboard;
	bool savedIsCut = clipboardIsCut;
	clipboard.clear();
	clipboardIsCut = false;
	for(CPtrNode *p = selection.first; p && (int)clipboard.size() < MAX_BATCH_OBJECTS; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted)
			clipboard.push_back(inst);
	}
	int duplicated = pasteClipboard(true);
	clipboard = savedClipboard;
	clipboardIsCut = savedIsCut;
	return duplicated;
}

static bool hiddenCountStale = true;

bool
IsInstanceHidden(ObjectInst *inst)
{
	return inst && inst->m_editorHidden;
}

void
SetInstanceHidden(ObjectInst *inst, bool hidden)
{
	if(inst == nil)
		return;
	// The LOD goes with its building, or it would be drawn in its place.
	ObjectInst *both[2] = { inst, inst->m_lod };
	for(int i = 0; i < 2; i++){
		if(both[i] == nil || both[i]->m_editorHidden == hidden)
			continue;
		both[i]->m_editorHidden = hidden;
		BumpChangeSeq();
		hiddenCountStale = true;
		if(hidden && both[i]->m_selected)
			both[i]->Deselect();
	}
}

int
HideSelected(void)
{
	std::vector<ObjectInst*> hide;
	for(CPtrNode *p = selection.first; p; p = p->next)
		hide.push_back((ObjectInst*)p->item);
	for(ObjectInst *inst : hide)
		SetInstanceHidden(inst, true);
	return (int)hide.size();
}

int
IsolateSelected(void)
{
	if(selection.first == nil)
		return 0;
	int hidden = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_selected || inst->m_editorHidden || inst->m_isDeleted)
			continue;
		// Keep the LODs of what stays.
		bool lodOfSelected = false;
		for(CPtrNode *s = selection.first; s && !lodOfSelected; s = s->next)
			lodOfSelected = ((ObjectInst*)s->item)->m_lod == inst;
		if(lodOfSelected)
			continue;
		inst->m_editorHidden = true;
		hiddenCountStale = true;
		hidden++;
	}
	if(hidden)
		BumpChangeSeq();
	return hidden;
}

int
ShowAllInstances(void)
{
	int shown = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_editorHidden){
			inst->m_editorHidden = false;
			shown++;
		}
		hiddenCountStale = true;
	}
	if(shown)
		BumpChangeSeq();
	return shown;
}

int
CountHiddenInstances(void)
{
	// The status bar asks every frame; count again only after a change.
	static int hidden;
	static uint32 countedSeq = 0xFFFFFFFFu;
	if(!hiddenCountStale && countedSeq == GetLatestChangeSeq())
		return hidden;
	hiddenCountStale = false;
	countedSeq = GetLatestChangeSeq();
	hidden = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_editorHidden && !inst->m_isDeleted)
			hidden++;
	}
	return hidden;
}

void
FocusSelection(void)
{
	// A sphere around every selected object's bounding sphere.
	rw::V3d lo = { 0.0f, 0.0f, 0.0f }, hi = lo;
	bool any = false;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		rw::V3d center = inst->m_translation;
		float radius = 1.0f;
		if(obj && obj->m_colModel){
			CSphere *sph = &obj->m_colModel->boundingSphere;
			rw::V3d::transformPoints(&center, &sph->center, 1, &inst->m_matrix);
			radius = sph->radius;
		}
		rw::V3d a = { center.x - radius, center.y - radius, center.z - radius };
		rw::V3d b = { center.x + radius, center.y + radius, center.z + radius };
		if(!any){
			lo = a;
			hi = b;
			any = true;
		}else{
			lo.x = min(lo.x, a.x); lo.y = min(lo.y, a.y); lo.z = min(lo.z, a.z);
			hi.x = max(hi.x, b.x); hi.y = max(hi.y, b.y); hi.z = max(hi.z, b.z);
		}
	}
	if(!any)
		return;
	rw::V3d center = scale(add(lo, hi), 0.5f);
	float radius = length(sub(hi, lo)) * 0.5f;
	TheCamera.setTarget(center);
	TheCamera.setDistanceFromTarget(TheCamera.minDistToSphere(radius));
}

static void pawnWriteQuotedString(FILE *f, const char *value);
static uint32 pawnColorToInt(const rw::RGBA &color);

int
ExportPrefab(const char *path)
{
	return ExportPrefabEx(path, PREFAB_ORIGIN_CENTRE, nil);
}

int
ExportPrefabEx(const char *path, int origin, const char *tags)
{
	// Collect selected non-deleted instances + their LODs
	ObjectInst *insts[256];
	int numInsts = 0;
	bool overflow = false;

	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		if(numInsts >= 256){ overflow = true; break; }
		insts[numInsts++] = inst;
	}

	// Auto-include linked LODs that weren't explicitly selected
	int numExplicit = numInsts;
	for(int i = 0; i < numExplicit; i++){
		ObjectInst *lod = insts[i]->m_lod;
		if(lod == nil || lod->m_isDeleted)
			continue;
		// Check if LOD is already in the list
		bool found = false;
		for(int j = 0; j < numInsts; j++){
			if(insts[j] == lod){ found = true; break; }
		}
		if(!found){
			if(numInsts >= 256){ overflow = true; break; }
			insts[numInsts++] = lod;
		}
	}

	if(numInsts == 0)
		return 0;
	if(overflow)
		Toast(TOAST_SAVE, _("Prefab truncated at 256 instances (selection too large)"));

	// Compute centroid
	rw::V3d centroid = {0.0f, 0.0f, 0.0f};
	for(int i = 0; i < numInsts; i++){
		centroid.x += insts[i]->m_translation.x;
		centroid.y += insts[i]->m_translation.y;
		centroid.z += insts[i]->m_translation.z;
	}
	float invCount = 1.0f / (float)numInsts;
	centroid.x *= invCount;
	centroid.y *= invCount;
	centroid.z *= invCount;
	// Where the prefab is held when placed: the middle, the first object, or
	// the middle at the lowest object, so it stands on the ground.
	// The selection list keeps the newest first, so the first selected is last.
	if(origin == PREFAB_ORIGIN_FIRST)
		centroid = insts[numExplicit - 1]->m_translation;
	else if(origin == PREFAB_ORIGIN_LOWEST)
		// The bottom of the lowest model, not its origin: each model's origin
		// sits its ground offset above where it touches the ground.
		for(int i = 0; i < numInsts; i++){
			float bottom = insts[i]->m_translation.z -
				GetElementPlacementBaseOffset(GetSampElementKind(insts[i]), insts[i]->m_objectId);
			centroid.z = i == 0 ? bottom : min(centroid.z, bottom);
		}

	// Build local index map for LOD references within the prefab
	// insts[i] -> local index i, so we can resolve m_lod pointers
	if(!EnsureParentDirectoriesForPath(path)){
		log("ExportPrefab: failed to create parent directories for %s\n", path);
		return 0;
	}

	FILE *f = fopen(path, "w");
	if(f == nil){
		log("ExportPrefab: failed to open %s for writing\n", path);
		return 0;
	}

	fprintf(f, "HAUMEA_PREFAB 4\n");
	fprintf(f, "game %d\n", (int)params.map);
	// Before count, where older readers skip what they don't know.
	if(tags && tags[0]){
		fputs("tags ", f);
		for(const char *t = tags; *t; t++)
			fputc(*t == '\n' || *t == '\r' ? ' ' : *t, f);
		fputc('\n', f);
	}
	fprintf(f, "count %d\n", numInsts);

	for(int i = 0; i < numInsts; i++){
		ObjectInst *inst = insts[i];
		ObjectDef *obj = GetObjectDef(inst->m_objectId);

		// Resolve LOD reference as local index within this prefab
		int lodRef = -1;
		if(inst->m_lod){
			for(int j = 0; j < numInsts; j++){
				if(insts[j] == inst->m_lod){
					lodRef = j;
					break;
				}
			}
		}

		int area = inst->m_area;
		if(isSA()){
			if(inst->m_isUnimportant) area |= 0x100;
			if(inst->m_isUnderWater) area |= 0x400;
			if(inst->m_isTunnel) area |= 0x800;
			if(inst->m_isTunnelTransition) area |= 0x1000;
		}

		rw::V3d rel = sub(inst->m_translation, centroid);

		// objectId, modelName, relX, relY, relZ, rotX, rotY, rotZ, rotW, area, lodRef
		fprintf(f, "%d, %s, %f, %f, %f, %f, %f, %f, %f, %d, %d\n",
			PawnModelId(inst->m_objectId),
			obj ? obj->m_name : "unknown",
			rel.x, rel.y, rel.z,
			inst->m_rotation.x, inst->m_rotation.y, inst->m_rotation.z, inst->m_rotation.w,
			area, lodRef);

		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
			const ObjectMaterialOverride *material = GetObjectMaterialOverride(inst, slot);
			if(material == nil || !materialOverrideIsActive(*material))
				continue;
			if(material->active){
				fprintf(f, "material %d %d %d %d 0x%08X ", i, slot,
					PawnModelId(material->sourceModel), material->hasTexture ? 1 : 0,
					pawnColorToInt(material->color));
				pawnWriteQuotedString(f, material->txdName);
				fputc(' ', f);
				pawnWriteQuotedString(f, material->textureName);
				fputc('\n', f);
			}
			if(material->textEnabled){
				fprintf(f, "material_text %d %d %d %d %d 0x%08X 0x%08X %d ",
					i, slot, material->materialSize, material->fontSize,
					material->bold ? 1 : 0, pawnColorToInt(material->textColor),
					pawnColorToInt(material->backgroundColor), material->alignment);
				pawnWriteQuotedString(f, material->font);
				fputc(' ', f);
				pawnWriteQuotedString(f, material->text);
				fputc('\n', f);
			}
		}

		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(inst->m_pawnProperties){
			fprintf(f, "props %d %.3f %.3f %d ", i, properties->drawDistance,
				properties->streamDistance, properties->virtualWorld);
			pawnWriteQuotedString(f, properties->name);
			fputc(' ', f);
			pawnWriteQuotedString(f, properties->group);
			fputc(' ', f);
			pawnWriteQuotedString(f, properties->comment);
			fputc('\n', f);
		}
		if(properties->elementKind != SAMP_ELEMENT_OBJECT){
			// element <index> <kind> <pickup type> <skin> <invulnerable> <health>
			//     <team> <weapon ammo x3> <zone x y color> <icon type color style>
			//     <label color> <label LOS> "label text"
			fprintf(f, "element %d %d %d %d %d %.2f %d", i, properties->elementKind,
				properties->pickupType, properties->elementModel,
				properties->actorInvulnerable ? 1 : 0, properties->actorHealth,
				properties->classTeam);
			for(int w = 0; w < PAWN_CLASS_WEAPONS; w++)
				fprintf(f, " %d %d", properties->classWeapons[w], properties->classAmmo[w]);
			fprintf(f, " %.3f %.3f 0x%08X %d 0x%08X %d 0x%08X %d ",
				properties->zoneSizeX, properties->zoneSizeY, properties->zoneColor,
				properties->mapIconType, properties->mapIconColor, properties->mapIconStyle,
				properties->labelColor, properties->labelTestLOS ? 1 : 0);
			pawnWriteQuotedString(f, properties->labelText);
			fputc('\n', f);
		}
		if(properties->elementKind == SAMP_ELEMENT_NPC){
			// npc <index> <armour> <weapon> <ammo> "name"
			fprintf(f, "npc %d %.2f %d %d ", i, properties->npcArmour,
				properties->npcWeapon, properties->npcAmmo);
			pawnWriteQuotedString(f, properties->npcName);
			fputc('\n', f);
		}
		if(properties->animName[0]){
			// anim <index> <delta> <loop> <lockx> <locky> <freeze> <time> "lib" "name"
			fprintf(f, "anim %d %.3f %d %d %d %d %d ", i, properties->animDelta,
				properties->animLoop ? 1 : 0, properties->animLockX ? 1 : 0,
				properties->animLockY ? 1 : 0, properties->animFreeze ? 1 : 0, properties->animTime);
			pawnWriteQuotedString(f, properties->animLib);
			fputc(' ', f);
			pawnWriteQuotedString(f, properties->animName);
			fputc('\n', f);
		}
		if(properties->elementKind == SAMP_ELEMENT_SOUND){
			// sound <index> <kind> <id> <radius> <use pos> <stop on leave> "url"
			fprintf(f, "sound %d %d %d %.3f %d %d ", i, properties->soundKind, properties->soundId,
				properties->soundRadius, properties->soundUsePos ? 1 : 0,
				properties->soundStopOnLeave ? 1 : 0);
			pawnWriteQuotedString(f, properties->soundUrl);
			fputc('\n', f);
		}
		if(properties->elementKind == SAMP_ELEMENT_GANGZONE && properties->zoneMode != SAMP_ZONE_GLOBAL)
			// zone <index> <mode>
			fprintf(f, "zone %d %d\n", i, properties->zoneMode);
		if(properties->elementKind == SAMP_ELEMENT_CHECKPOINT){
			// checkpoint <index> <kind> <size> <race type> <order> "race"
			fprintf(f, "checkpoint %d %d %.3f %d %d ", i, properties->cpKind, properties->cpSize,
				properties->cpRaceType, properties->cpOrder);
			pawnWriteQuotedString(f, properties->cpRace);
			fputc('\n', f);
		}
		if(properties->elementKind == SAMP_ELEMENT_AREA){
			// area <index> <shape> <radius> <size x> <size y> <size z> <height> <points> x y ...
			fprintf(f, "area %d %d %.4f %.4f %.4f %.4f %.4f %d", i, properties->areaShape,
				properties->areaRadius, properties->areaSize.x, properties->areaSize.y,
				properties->areaSize.z, properties->areaHeight, properties->areaPointCount);
			for(int p = 0; p < properties->areaPointCount; p++)
				fprintf(f, " %.4f %.4f", properties->areaPoints[p][0], properties->areaPoints[p][1]);
			fputc('\n', f);
		}
		if(properties->areaName[0]){
			// inarea <index> "area": shown only inside that dynamic area
			fprintf(f, "inarea %d ", i);
			pawnWriteQuotedString(f, properties->areaName);
			fputc('\n', f);
		}
		if(properties->interiorZeroOnly)
			// interior0 <index>: the streamer's interiorid 0, not -1
			fprintf(f, "interior0 %d\n", i);
		if(properties->npcPlaysNode){
			// node <index> <node> <point> <radius> <set angle> <move type> <speed>
			fprintf(f, "node %d %d %d %.3f %d %d %.4f\n", i, properties->npcNode,
				properties->npcNodePoint, properties->npcNodeRadius,
				properties->npcNodeSetAngle ? 1 : 0, properties->npcMoveType,
				properties->npcMoveSpeed);
		}
		if(properties->npcRecording[0]){
			// rec <index> <at npc> "recording"
			fprintf(f, "rec %d %d ", i, properties->npcRecordingAtNpc ? 1 : 0);
			pawnWriteQuotedString(f, properties->npcRecording);
			fputc('\n', f);
		}
		if(properties->pathName[0]){
			// path <index> <order> <stop range> <move type> <speed> <reversed> "path"
			fprintf(f, "path %d %d %.3f %d %.4f %d ", i, properties->pathOrder,
				properties->pathStopRange, properties->npcMoveType, properties->npcMoveSpeed,
				properties->npcPathReversed ? 1 : 0);
			pawnWriteQuotedString(f, properties->pathName);
			fputc('\n', f);
			if(properties->npcPathRepeat != NPC_PATH_ONCE)
				// repeat <index> <1 loop, 2 back and forth>
				fprintf(f, "repeat %d %d\n", i, properties->npcPathRepeat);
		}
		if(inst->m_isVehicle){
			fprintf(f, "vehicle %d %d %d %d %d %d", i, inst->m_vehicleStatic ? 1 : 0,
				inst->m_vehicleColor1, inst->m_vehicleColor2, inst->m_vehicleRespawnDelay,
				inst->m_vehiclePaintjob);
			for(int m = 0; m < VEHICLE_MOD_SLOTS; m++)
				fprintf(f, " %d", inst->m_vehicleMods[m]);
			fputc(' ', f);
			pawnWriteQuotedString(f, inst->m_vehiclePlate);
			fputc('\n', f);
		}
		// npcveh <index> <vehicle index> <seat>
		ObjectInst *npcVehicle = GetNpcVehicle(inst);
		for(int j = 0; npcVehicle && j < numInsts; j++)
			if(insts[j] == npcVehicle)
				fprintf(f, "npcveh %d %d %d\n", i, j, inst->m_pawnProperties->npcSeat);
		int parentIndex = -1;
		ObjectInst *parent = GetAttachParent(inst);
		if(properties->attachParent == PAWN_ATTACH_PLAYER)
			parentIndex = PAWN_ATTACH_PLAYER;
		else
			for(int j = 0; parent && j < numInsts; j++)
				if(insts[j] == parent){ parentIndex = j; break; }
		if(parentIndex != -1)
			fprintf(f, "attach %d %d %f %f %f %f %f %f %d %d %d %f %f %f 0x%08X 0x%08X %d\n", i, parentIndex,
				properties->attachOffset.x, properties->attachOffset.y,
				properties->attachOffset.z, properties->attachRotation.x,
				properties->attachRotation.y, properties->attachRotation.z,
				properties->attachSyncRotation ? 1 : 0, properties->attachBone, properties->attachSlot,
				properties->attachScale.x, properties->attachScale.y, properties->attachScale.z,
				properties->attachColor1, properties->attachColor2, properties->attachSkin);
	}

	fclose(f);
	log("ExportPrefab: wrote %d instance(s) to %s\n", numInsts, path);
	return numInsts;
}

int
ExportSelectedDffs(const char *dir, int *numFailed)
{
	int exported = 0;
	int failed = 0;
	char path[1024];

	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		ObjectDef *obj;

		if(inst == nil || inst->m_isDeleted)
			continue;
		obj = GetObjectDef(inst->m_objectId);
		if(obj == nil || obj->m_name[0] == '\0'){
			failed++;
			continue;
		}
		if(HasEarlierSelectedModel(p, obj->m_name))
			continue;
		if(!BuildAssetExportPath(dir, obj->m_name, "dff", path, sizeof(path)) ||
		   !ExportEffectiveAsset(obj->m_name, "dff", obj->m_imageIndex, path)){
			log("ExportSelectedDffs: failed to export %s\n", obj->m_name);
			failed++;
			continue;
		}
		exported++;
	}

	if(numFailed)
		*numFailed = failed;
	return exported;
}

int
ExportSelectedTxds(const char *dir, int *numFailed)
{
	int exported = 0;
	int failed = 0;
	char path[1024];

	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		ObjectDef *obj;
		TxdDef *txd;

		if(inst == nil || inst->m_isDeleted)
			continue;
		obj = GetObjectDef(inst->m_objectId);
		if(obj == nil){
			failed++;
			continue;
		}
		txd = GetTxdDef(obj->m_txdSlot);
		if(txd == nil || txd->name[0] == '\0'){
			failed++;
			continue;
		}
		if(HasEarlierSelectedTxd(p, txd->name))
			continue;
		if(!BuildAssetExportPath(dir, txd->name, "txd", path, sizeof(path)) ||
		   !ExportEffectiveAsset(txd->name, "txd", txd->imageIndex, path)){
			log("ExportSelectedTxds: failed to export %s\n", txd->name);
			failed++;
			continue;
		}
		exported++;
	}

	if(numFailed)
		*numFailed = failed;
	return exported;
}

struct PawnEulerRotation
{
	double x, y, z;
};

bool
InstanceUsesSourceDegrees(const ObjectInst *inst)
{
	if(inst == nil || !inst->m_hasSourceDegrees)
		return false;
	const rw::Quat &a = inst->m_rotation, &b = inst->m_sourceRotation;
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

rw::V3d
GetInstanceRotationDegrees(const ObjectInst *inst)
{
	if(InstanceUsesSourceDegrees(inst))
		return { (float)inst->m_sourceDegrees[0], (float)inst->m_sourceDegrees[1], (float)inst->m_sourceDegrees[2] };
	return GetObjectRotationDegrees(inst->m_rotation);
}

static PawnEulerRotation
pawnGetEulerRotation(const ObjectInst *inst)
{
	if(InstanceUsesSourceDegrees(inst)){
		PawnEulerRotation e = { inst->m_sourceDegrees[0], inst->m_sourceDegrees[1], inst->m_sourceDegrees[2] };
		return e;	// the file's numbers, unrounded: 89.999992 stays 89.999992
	}
	rw::V3d degrees = GetObjectRotationDegrees(inst->m_rotation);
	// Snap float noise so files read 0.0/90.0 rather than -0.000000/90.000008,
	// and write a half turn as 180: -180 and 180 alternated between saves.
	float *axes[3] = { &degrees.x, &degrees.y, &degrees.z };
	for(int i = 0; i < 3; i++){
		float rounded = floorf(*axes[i] * 10000.0f + 0.5f) / 10000.0f;
		*axes[i] = rounded == 0.0f ? 0.0f : rounded == -180.0f ? 180.0f : rounded;
	}
	PawnEulerRotation e = { degrees.x, degrees.y, degrees.z };
	return e;
}

static rw::Quat
pawnMakeStoredQuaternion(float x, float y, float z)
{
	return MakeObjectRotationDegrees({ x, y, z });
}

static bool
pawnPathLooksCustom(const char *path)
{
	if(path == nil || path[0] == '\0')
		return false;

	char normalized[1024];
	size_t i = 0;
	for(; path[i] && i < sizeof(normalized)-1; i++){
		char c = path[i];
		if(c == '\\') c = '/';
		if(c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
		normalized[i] = c;
	}
	normalized[i] = '\0';

	if(strcmp(normalized, "data/maps/custom.ipl") == 0 ||
	   strcmp(normalized, "data/maps/haumea/custom.ipl") == 0 ||
	   strcmp(normalized, "custom.ipl") == 0 ||
	   strcmp(normalized, "data/maps/haumea") == 0)
		return true;
	return strstr(normalized, "/haumea/") != nil ||
	       (strlen(normalized) >= 11 &&
	        strcmp(normalized + strlen(normalized) - 11, "/custom.ipl") == 0);
}

bool
IsPawnWorldObject(ObjectInst *inst)
{
	if(inst == nil || inst->m_isVehicle || inst->m_isAdded || !inst->m_savedStateValid)
		return false;
	if(inst->m_file == nil || inst->m_file->name == nil)
		return true;
	return !pawnPathLooksCustom(inst->m_file->name);
}

// The map's own objects, the opposite of IsPawnWorldObject, cheap enough
// for every instance every frame: a file's answer is kept.
bool
IsUserMapObject(ObjectInst *inst)
{
	if(inst->m_isVehicle || inst->m_isAdded || !inst->m_savedStateValid)
		return true;
	GameFile *file = inst->m_file;
	if(file == nil || file->name == nil)
		return false;
	if(file->custom < 0)
		file->custom = pawnPathLooksCustom(file->name);
	return file->custom != 0;
}

bool
IsPawnObjectSelectable(ObjectInst *inst)
{
	// There is no separate Pawn editing layer. World objects stay selectable
	// as reference geometry and can be changed; export decides whether a
	// change needs a scripted replacement.
	return inst != nil;
}

bool
IsPawnObjectEditable(ObjectInst *inst)
{
	return inst != nil;
}

void
SetPawnEditMode(bool enabled)
{
	(void)enabled;
	// Kept as a compatibility entry point for older UI state/configuration.
	// The open.mp workflow has one editing layer and never enters this mode.
	gPawnEditMode = false;
	gPawnWorldDeleteMode = false;
}

void
SetPawnWorldDeleteMode(bool enabled)
{
	(void)enabled;
	// World deletion is handled by the normal Delete action and exported from
	// the instance diff; it does not need a separate selection layer.
	gPawnWorldDeleteMode = false;
}

int
SelectAllPawnObjects(void)
{
	ClearSelection();
	int count = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted || IsPawnWorldObject(inst))
			continue;
		inst->Select();
		count++;
	}
	return count;
}

int
SelectAllWorldObjectsForDeletion(void)
{
	ClearSelection();
	int count = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted || !IsPawnWorldObject(inst))
			continue;
		inst->Select();
		count++;
	}
	return count;
}

static bool
pawnIsWorldObject(const ObjectInst *inst)
{
	return IsPawnWorldObject((ObjectInst*)inst);
}

static void
pawnCollectInstances(bool selectedOnly, std::vector<ObjectInst*> &out, int *numSkipped)
{
	if(numSkipped)
		*numSkipped = 0;

	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil)
			continue;
		bool worldObject = pawnIsWorldObject(inst);
		bool include = PawnObjectNeedsExport(inst);

		if(selectedOnly){
			bool selected = inst->m_selected;
			// Delete() clears selection. Retain deleted world objects as Pawn
			// removal entries so a deletion remains exportable.
			bool deletedWorld = worldObject && inst->m_isDeleted;
			include = (selected || deletedWorld) && include;
		}
		if(!include)
			continue;
		if(GetObjectDef(inst->m_objectId) == nil){
			if(numSkipped) (*numSkipped)++;
			continue;
		}
		out.push_back(inst);
	}
	// The instance list is newest first; write maps in creation order.
	std::sort(out.begin(), out.end(), [](const ObjectInst *a, const ObjectInst *b){
		return a->m_id < b->m_id;
	});
}

static void
pawnBuildFunctionPrefix(const char *path, char *prefix, size_t size)
{
	const char *base = path ? strrchr(path, '/') : nil;
	const char *backslash = path ? strrchr(path, '\\') : nil;
	if(backslash && (base == nil || backslash > base))
		base = backslash;
	base = base ? base + 1 : path;
	if(base == nil || base[0] == '\0')
		base = "objects";

	if(size == 0)
		return;
	strncpy(prefix, "Haumea_", size-1);
	prefix[size-1] = '\0';
	size_t used = strlen(prefix);
	for(const char *p = base; *p && used + 1 < size; p++){
		if(*p == '.')
			break;
		unsigned char c = (unsigned char)*p;
		prefix[used++] = (isalnum(c) || c == '_') ? (char)c : '_';
	}
	if(used == strlen("Haumea_")){
		const char *fallback = "objects";
		for(const char *p = fallback; *p && used + 1 < size; p++)
			prefix[used++] = *p;
	}
	prefix[used] = '\0';
}

// Weapon constants. The writer uses the first name for an ID, which is
// SA-MP 0.3.7's spelling; open.mp also knows those. 0.3.7 has no name for
// 0, 44 and 45, so a map that uses them defines the open.mp name itself.
static const struct { const char *name; int value; } pawnWeaponNames[] = {
	{ "WEAPON_FIST", 0 },
	{ "WEAPON_BRASSKNUCKLE", 1 }, { "WEAPON_GOLFCLUB", 2 }, { "WEAPON_NITESTICK", 3 },
	{ "WEAPON_KNIFE", 4 }, { "WEAPON_BAT", 5 }, { "WEAPON_SHOVEL", 6 },
	{ "WEAPON_POOLSTICK", 7 }, { "WEAPON_KATANA", 8 }, { "WEAPON_CHAINSAW", 9 },
	{ "WEAPON_DILDO", 10 }, { "WEAPON_DILDO2", 11 }, { "WEAPON_VIBRATOR", 12 },
	{ "WEAPON_VIBRATOR2", 13 }, { "WEAPON_FLOWER", 14 }, { "WEAPON_CANE", 15 },
	{ "WEAPON_GRENADE", 16 }, { "WEAPON_TEARGAS", 17 }, { "WEAPON_MOLTOV", 18 },
	{ "WEAPON_COLT45", 22 }, { "WEAPON_SILENCED", 23 }, { "WEAPON_DEAGLE", 24 },
	{ "WEAPON_SHOTGUN", 25 }, { "WEAPON_SAWEDOFF", 26 }, { "WEAPON_SHOTGSPA", 27 },
	{ "WEAPON_UZI", 28 }, { "WEAPON_MP5", 29 }, { "WEAPON_AK47", 30 },
	{ "WEAPON_M4", 31 }, { "WEAPON_TEC9", 32 }, { "WEAPON_RIFLE", 33 },
	{ "WEAPON_SNIPER", 34 }, { "WEAPON_ROCKETLAUNCHER", 35 }, { "WEAPON_HEATSEEKER", 36 },
	{ "WEAPON_FLAMETHROWER", 37 }, { "WEAPON_MINIGUN", 38 }, { "WEAPON_SATCHEL", 39 },
	{ "WEAPON_BOMB", 40 }, { "WEAPON_SPRAYCAN", 41 }, { "WEAPON_FIREEXTINGUISHER", 42 },
	{ "WEAPON_CAMERA", 43 }, { "WEAPON_PARACHUTE", 46 },
	{ "WEAPON_NIGHT_VISION_GOGGLES", 44 }, { "WEAPON_THERMAL_GOGGLES", 45 },
	{ "WEAPON_NIGHTSTICK", 3 }, { "WEAPON_MOLOTOV", 18 },
};

static bool
pawnWeaponIsOpenMpOnly(int weapon)
{
	return weapon == 0 || weapon == 44 || weapon == 45;
}

static const char *
pawnWeaponName(int weapon)
{
	for(size_t i = 0; i < sizeof(pawnWeaponNames)/sizeof(pawnWeaponNames[0]); i++)
		if(pawnWeaponNames[i].value == weapon)
			return pawnWeaponNames[i].name;
	return nil;
}

static void
pawnWriteQuotedString(FILE *f, const char *value)
{
	fputc('"', f);
	if(value){
		for(const char *p = value; *p; p++){
			switch(*p){
			case '\\': fputs("\\\\", f); break;
			case '"':  fputs("\\\"", f); break;
			case '\n': fputs("\\n", f); break;
			case '\r': fputs("\\r", f); break;
			case '\t': fputs("\\t", f); break;
			default: fputc(*p, f); break;
			}
		}
	}
	fputc('"', f);
}

static uint32
pawnColorToInt(const rw::RGBA &color)
{
	return ((uint32)color.alpha << 24) |
	       ((uint32)color.red << 16) |
	       ((uint32)color.green << 8) |
	       (uint32)color.blue;
}

static rw::RGBA
pawnColorFromInt(uint32 color)
{
	rw::RGBA rgba = {
		(uint8)((color >> 16) & 0xFF),
		(uint8)((color >> 8) & 0xFF),
		(uint8)(color & 0xFF),
		(uint8)((color >> 24) & 0xFF)
	};
	return rgba;
}

// Named constants for tagged arguments: open.mp's includes warn about plain
// numbers there, and SA-MP 0.3.7's define the same names.
static void
pawnWriteMaterialSize(FILE *f, int materialSize)
{
	int width, height;
	if(GetMaterialTextDimensions(materialSize, &width, &height))
		fprintf(f, "OBJECT_MATERIAL_SIZE_%dx%d", width, height);
	else
		fprintf(f, "%d", materialSize);
}

static void
pawnWriteTextAlignment(FILE *f, int alignment)
{
	static const char *names[] = {
		"OBJECT_MATERIAL_TEXT_ALIGN_LEFT", "OBJECT_MATERIAL_TEXT_ALIGN_CENTER",
		"OBJECT_MATERIAL_TEXT_ALIGN_RIGHT",
	};
	if(alignment >= 0 && alignment < 3)
		fputs(names[alignment], f);
	else
		fprintf(f, "%d", alignment);
}

static void
pawnWriteMapIconStyle(FILE *f, int style)
{
	static const char *names[] = {
		"MAPICON_LOCAL", "MAPICON_GLOBAL", "MAPICON_LOCAL_CHECKPOINT", "MAPICON_GLOBAL_CHECKPOINT",
	};
	if(style >= 0 && style < 4)
		fputs(names[style], f);
	else
		fprintf(f, "%d", style);
}

static void
pawnWriteWeapon(FILE *f, int weapon)
{
	const char *name = pawnWeaponName(weapon);
	if(name)
		fputs(name, f);
	else
		fprintf(f, "%d", weapon);
}

static void
pawnWriteMaterialCalls(FILE *f, const ObjectInst *inst, PawnCodeFormat format,
	const char *handle)
{
	if(inst == nil || handle == nil || handle[0] == '\0')
		return;
	const char *materialName = format == PAWN_CODE_STREAMER
		? "SetDynamicObjectMaterial" : "SetObjectMaterial";
	const char *textName = format == PAWN_CODE_STREAMER
		? "SetDynamicObjectMaterialText" : "SetObjectMaterialText";

	for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
		const ObjectMaterialOverride *material = GetObjectMaterialOverride(inst, slot);
		if(material == nil || !materialOverrideIsActive(*material))
			continue;

		// Only one call per slot survives in game, so write just that one.
		if(material->textEnabled){
			if(material->text[0] == '\0')
				continue;	// the client ignores empty material text
		}else if(material->active){
			int sourceModel = material->hasTexture ? material->sourceModel : -1;
			const char *txd = material->hasTexture && material->txdName[0]
				? material->txdName : "none";
			const char *texture = material->hasTexture && material->textureName[0]
				? material->textureName : "none";
			fprintf(f, "\t%s(%s, %d, %d, ", materialName, handle, slot, PawnModelId(sourceModel));
			pawnWriteQuotedString(f, txd);
			fputs(", ", f);
			pawnWriteQuotedString(f, texture);
			fprintf(f, ", 0x%08X);\n", pawnColorToInt(material->color));
		}

		if(material->textEnabled){
			fprintf(f, "\t%s(%s, ", textName, handle);
			if(format == PAWN_CODE_STREAMER){
				fprintf(f, "%d, ", slot);
				pawnWriteQuotedString(f, material->text);
				fputs(", ", f);
			}else{
				pawnWriteQuotedString(f, material->text);
				fprintf(f, ", %d, ", slot);
			}
			pawnWriteMaterialSize(f, material->materialSize);
			fputs(", ", f);
			pawnWriteQuotedString(f, material->font[0] ? material->font : "Arial");
			fprintf(f, ", %d, %s, 0x%08X, 0x%08X, ",
				material->fontSize, material->bold ? "true" : "false",
				pawnColorToInt(material->textColor),
				pawnColorToInt(material->backgroundColor));
			pawnWriteTextAlignment(f, material->alignment);
			fputs(");\n", f);
		}
	}
}

// The comment after a call: the model name, then Haumea's group and note.
static void
pawnWriteTrailingTags(FILE *f, const char *name, const char *extra,
	const ObjectPawnProperties *)
{
	// Editor metadata belongs in the paired XML document.
	fprintf(f, " // %s%s", name, extra);
	fputc('\n', f);
}

static void
pawnWriteDistance(FILE *f, float distance, const char *defaultName)
{
	if(distance > 0.0f)
		fprintf(f, "%.2f", distance);
	else
		fputs(defaultName, f);
}

// Gang zone extent as GangZoneCreate's minx, miny, maxx, maxy.
static void
pawnGangZoneBounds(const ObjectInst *inst, const ObjectPawnProperties *properties,
	float *minX, float *minY, float *maxX, float *maxY)
{
	*minX = inst->m_translation.x - properties->zoneSizeX * 0.5f;
	*minY = inst->m_translation.y - properties->zoneSizeY * 0.5f;
	*maxX = inst->m_translation.x + properties->zoneSizeX * 0.5f;
	*maxY = inst->m_translation.y + properties->zoneSizeY * 0.5f;
}

// "lib", "name", delta, loop, lockX, lockY, freeze, time); and the line end.
// The natives take bools; the streamer's ApplyDynamicActorAnimation ints.
static void
pawnWriteAnimationArgs(FILE *f, const ObjectPawnProperties *properties, bool bools)
{
	pawnWriteQuotedString(f, properties->animLib);
	fputs(", ", f);
	pawnWriteQuotedString(f, properties->animName);
	const char *t = bools ? "true" : "1", *n = bools ? "false" : "0";
	fprintf(f, ", %.2f, %s, %s, %s, %s, %d);\n", properties->animDelta,
		properties->animLoop ? t : n, properties->animLockX ? t : n,
		properties->animLockY ? t : n, properties->animFreeze ? t : n, properties->animTime);
}

// SA-MP elements other than objects and vehicles. Gang zones and native map
// icons are per player, so their show calls go to <prefix>_ShowForPlayer.
// Dynamic areas in the map being written: area variable name -> its handle.
static std::unordered_map<std::string, std::string> gPawnAreaHandles;

// ", <area handle>" for a streamer item limited to an area that the map has
// (its areaid argument), else nothing.
static void
pawnWriteAreaArg(FILE *f, const ObjectPawnProperties *properties)
{
	if(properties->areaName[0] == '\0')
		return;
	std::unordered_map<std::string, std::string>::iterator it = gPawnAreaHandles.find(properties->areaName);
	if(it != gPawnAreaHandles.end())
		fprintf(f, ", %s", it->second.c_str());
}

static bool
pawnHasArea(const ObjectPawnProperties *properties)
{
	return properties->areaName[0] && gPawnAreaHandles.count(properties->areaName);
}

// The streamer's interiorid for an item: its interior, or -1 (every
// interior) or 0 (only interior 0) as the map had it.
static int
pawnStreamerInterior(const ObjectInst *inst)
{
	if(inst->m_area > 0)
		return inst->m_area;
	return GetObjectPawnProperties(inst)->interiorZeroOnly ? 0 : -1;
}

// CreateDynamicCircle .. CreateDynamicPolygon for a dynamic area (streamer only).
static void
pawnWriteAreaCall(FILE *f, const ObjectInst *inst, const char *handle)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	const rw::V3d &p = inst->m_translation;
	int interior = pawnStreamerInterior(inst);
	float r = properties->areaRadius, h = properties->areaHeight;
	float hx = properties->areaSize.x * 0.5f, hy = properties->areaSize.y * 0.5f;
	fprintf(f, "\tnew const %s = ", handle);	// as hand-written maps do
	switch(properties->areaShape){
	case SAMP_AREA_CIRCLE:
		fprintf(f, "CreateDynamicCircle(%.6f, %.6f, %.6f", p.x, p.y, r);
		break;
	case SAMP_AREA_CYLINDER:
		fprintf(f, "CreateDynamicCylinder(%.6f, %.6f, %.6f, %.6f, %.6f", p.x, p.y, p.z, p.z + h, r);
		break;
	case SAMP_AREA_SPHERE:
		fprintf(f, "CreateDynamicSphere(%.6f, %.6f, %.6f, %.6f", p.x, p.y, p.z, r);
		break;
	case SAMP_AREA_RECTANGLE:
		fprintf(f, "CreateDynamicRectangle(%.6f, %.6f, %.6f, %.6f", p.x - hx, p.y - hy, p.x + hx, p.y + hy);
		break;
	case SAMP_AREA_CUBOID:
	case SAMP_AREA_CUBE:
		fprintf(f, "%s(%.6f, %.6f, %.6f, %.6f, %.6f, %.6f",
			properties->areaShape == SAMP_AREA_CUBE ? "CreateDynamicCube" : "CreateDynamicCuboid",
			p.x - hx, p.y - hy, p.z, p.x + hx, p.y + hy, p.z + properties->areaSize.z);
		break;
	case SAMP_AREA_POLYGON:
		fputs("CreateDynamicPolygon(Float:{", f);
		for(int i = 0; i < properties->areaPointCount; i++)
			fprintf(f, "%s%.6f, %.6f", i ? ", " : "", p.x + properties->areaPoints[i][0],
				p.y + properties->areaPoints[i][1]);
		if(h > 0.0f)
			fprintf(f, "}, %.6f, %.6f, %d", p.z, p.z + h, properties->areaPointCount * 2);
		else
			fprintf(f, "}, -FLOAT_INFINITY, FLOAT_INFINITY, %d", properties->areaPointCount * 2);
		break;
	}
	fprintf(f, ", %d, %d);", properties->virtualWorld, interior);
	pawnWriteTrailingTags(f, "area", " [dynamic area]", properties);
}

static void
pawnWriteElementCall(FILE *f, const ObjectInst *inst, PawnCodeFormat format,
	const char *handle)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	const char *name = obj && obj->m_name[0] ? obj->m_name : "unknown";
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	char skinName[32];
	if((properties->elementKind == SAMP_ELEMENT_ACTOR || properties->elementKind == SAMP_ELEMENT_CLASS ||
	    properties->elementKind == SAMP_ELEMENT_NPC || properties->elementKind == SAMP_ELEMENT_PICKUP) &&
	   properties->elementModel != inst->m_objectId){
		// Drawn with a stand-in (CJ has no model file); name the real one.
		snprintf(skinName, sizeof(skinName), properties->elementModel == 0 &&
			properties->elementKind != SAMP_ELEMENT_PICKUP ? "CJ" : "model %d", properties->elementModel);
		name = skinName;
	}
	PawnEulerRotation e = pawnGetEulerRotation(inst);
	const rw::V3d &p = inst->m_translation;
	int interior = pawnStreamerInterior(inst);
	bool streamer = format == PAWN_CODE_STREAMER;
	int kind = properties->elementKind;

	fputs("\t", f);
	if(handle && handle[0])
		fprintf(f, kind == SAMP_ELEMENT_GANGZONE ? "%s = " : "new %s = ", handle);
	switch(kind){
	case SAMP_ELEMENT_PICKUP:
		if(streamer){
			fprintf(f, "CreateDynamicPickup(%d, %d, %.6f, %.6f, %.6f, %d, %d, -1, ",
				properties->elementModel, properties->pickupType, p.x, p.y, p.z,
				properties->virtualWorld, interior);
			pawnWriteDistance(f, properties->streamDistance, "STREAMER_PICKUP_SD");
			pawnWriteAreaArg(f, properties);
			fputs(");", f);
		}else
			fprintf(f, "CreatePickup(%d, %d, %.6f, %.6f, %.6f, %d);",
				properties->elementModel, properties->pickupType, p.x, p.y, p.z,
				properties->virtualWorld);
		pawnWriteTrailingTags(f, name, " [pickup]", properties);
		break;
	case SAMP_ELEMENT_LABEL:
		fputs(streamer ? "CreateDynamic3DTextLabel(" : "Create3DTextLabel(", f);
		pawnWriteQuotedString(f, properties->labelText);
		fprintf(f, ", 0x%08X, %.6f, %.6f, %.6f, %.2f, ", properties->labelColor,
			p.x, p.y, p.z, properties->drawDistance > 0.0f ? properties->drawDistance : 20.0f);
		if(streamer){
			fprintf(f, "INVALID_PLAYER_ID, INVALID_VEHICLE_ID, %s, %d, %d, -1, ",
				properties->labelTestLOS ? "true" : "false", properties->virtualWorld, interior);
			pawnWriteDistance(f, properties->streamDistance, "STREAMER_3D_TEXT_LABEL_SD");
			pawnWriteAreaArg(f, properties);
			fputs(");", f);
		}else
			// Native labels have no "every world" value; world 0 is the default.
			fprintf(f, "%d, %s);", properties->virtualWorld > 0 ? properties->virtualWorld : 0,
				properties->labelTestLOS ? "true" : "false");
		pawnWriteTrailingTags(f, name, " [3D text label]", properties);
		break;
	case SAMP_ELEMENT_ACTOR:
		if(streamer){
			fprintf(f, "CreateDynamicActor(%d, %.6f, %.6f, %.6f, %.6f, %d, %.2f, %d, %d, -1, ",
				properties->elementModel, p.x, p.y, p.z, e.z, properties->actorInvulnerable ? 1 : 0,
				properties->actorHealth, properties->virtualWorld, interior);
			pawnWriteDistance(f, properties->streamDistance, "STREAMER_ACTOR_SD");
			pawnWriteAreaArg(f, properties);
			fputs(");", f);
			pawnWriteTrailingTags(f, name, " [actor]", properties);
			if(handle && handle[0] && properties->animName[0]){
				fprintf(f, "\tApplyDynamicActorAnimation(%s, ", handle);
				pawnWriteAnimationArgs(f, properties, false);
			}
		}else{
			fprintf(f, "CreateActor(%d, %.6f, %.6f, %.6f, %.6f);",
				properties->elementModel, p.x, p.y, p.z, e.z);
			pawnWriteTrailingTags(f, name, " [actor]", properties);
			if(handle && handle[0]){
				if(properties->virtualWorld > 0)
					fprintf(f, "\tSetActorVirtualWorld(%s, %d);\n", handle, properties->virtualWorld);
				if(!properties->actorInvulnerable)
					fprintf(f, "\tSetActorInvulnerable(%s, false);\n", handle);
				if(properties->actorHealth != 100.0f)
					fprintf(f, "\tSetActorHealth(%s, %.2f);\n", handle, properties->actorHealth);
				if(properties->animName[0]){
					fprintf(f, "\tApplyActorAnimation(%s, ", handle);
					pawnWriteAnimationArgs(f, properties, true);
				}
			}
		}
		break;
	case SAMP_ELEMENT_CLASS:
		if(properties->classTeam >= 0)
			fprintf(f, "AddPlayerClassEx(%d, ", properties->classTeam);
		else
			fputs("AddPlayerClass(", f);
		fprintf(f, "%d, %.6f, %.6f, %.6f, %.6f", properties->elementModel, p.x, p.y, p.z, e.z);
		for(int i = 0; i < PAWN_CLASS_WEAPONS; i++){
			fputs(", ", f);
			pawnWriteWeapon(f, properties->classWeapons[i]);
			fprintf(f, ", %d", properties->classAmmo[i]);
		}
		fputs(");", f);
		pawnWriteTrailingTags(f, name, " [player class]", properties);
		break;
	case SAMP_ELEMENT_GANGZONE: {
		float minX, minY, maxX, maxY;
		pawnGangZoneBounds(inst, properties, &minX, &minY, &maxX, &maxY);
		fprintf(f, "GangZoneCreate(%.6f, %.6f, %.6f, %.6f);", minX, minY, maxX, maxY);
		if(IsRadarArtStandIn(inst))
			pawnWriteTrailingTags(f, "gangzone", "", properties);
		else
			pawnWriteTrailingTags(f, "gangzone", " [gang zone]", properties);
		break;
	}
	case SAMP_ELEMENT_CHECKPOINT:
		// Only reached with the streamer, for plain checkpoints.
		fprintf(f, "CreateDynamicCP(%.6f, %.6f, %.6f, %.2f, %d, %d, -1, ", p.x, p.y, p.z,
			properties->cpSize, properties->virtualWorld, interior);
		pawnWriteDistance(f, properties->streamDistance, "STREAMER_CP_SD");
		pawnWriteAreaArg(f, properties);
		fputs(");", f);
		pawnWriteTrailingTags(f, "checkpoint", "", properties);
		break;
	case SAMP_ELEMENT_MAPICON:
		// Only reached with the streamer; native icons are set per player.
		fprintf(f, "CreateDynamicMapIcon(%.6f, %.6f, %.6f, %d, 0x%08X, %d, %d, -1, ",
			p.x, p.y, p.z, properties->mapIconType, properties->mapIconColor,
			properties->virtualWorld, interior);
		pawnWriteDistance(f, properties->streamDistance, "STREAMER_MAP_ICON_SD");
		fputs(", ", f);
		pawnWriteMapIconStyle(f, properties->mapIconStyle);
		pawnWriteAreaArg(f, properties);
		fputs(");", f);
		pawnWriteTrailingTags(f, "mapicon", " [map icon]", properties);
		break;
	case SAMP_ELEMENT_NPC:
		// open.mp only. NPC_Spawn resets health, armour and weapon, so the
		// setters follow it. Natives and the streamer write the same calls.
		fputs("NPC_Create(", f);
		pawnWriteQuotedString(f, properties->npcName);
		fputs(");", f);
		pawnWriteTrailingTags(f, name, " [NPC]", properties);
		fprintf(f, "\tNPC_Spawn(%s);\n", handle);
		fprintf(f, "\tNPC_SetSkin(%s, %d);\n", handle, properties->elementModel);
		fprintf(f, "\tNPC_SetPos(%s, %.6f, %.6f, %.6f);\n", handle, p.x, p.y, p.z);
		fprintf(f, "\tNPC_SetFacingAngle(%s, %.6f);\n", handle, e.z);
		if(inst->m_area > 0)
			fprintf(f, "\tNPC_SetInterior(%s, %d);\n", handle, inst->m_area);
		if(properties->virtualWorld > 0)
			fprintf(f, "\tNPC_SetVirtualWorld(%s, %d);\n", handle, properties->virtualWorld);
		if(properties->actorHealth != 100.0f)
			fprintf(f, "\tNPC_SetHealth(%s, %.2f);\n", handle, properties->actorHealth);
		if(properties->npcArmour > 0.0f)
			fprintf(f, "\tNPC_SetArmour(%s, %.2f);\n", handle, properties->npcArmour);
		if(properties->npcWeapon > 0){
			fprintf(f, "\tNPC_SetWeapon(%s, WEAPON:%d);\n", handle, properties->npcWeapon);
			fprintf(f, "\tNPC_SetAmmo(%s, %d);\n", handle, properties->npcAmmo);
		}
		if(properties->actorInvulnerable)
			fprintf(f, "\tNPC_SetInvulnerable(%s, true);\n", handle);
		if(properties->animName[0]){
			fprintf(f, "\tNPC_ApplyAnimation(%s, ", handle);
			pawnWriteAnimationArgs(f, properties, true);
		}
		break;
	}
}

// Interiors, paintjobs, parts and plates are set on the vehicle's handle.
static bool
vehicleNeedsHandle(const ObjectInst *inst)
{
	if(inst->m_area > 0 || inst->m_vehiclePaintjob > 0 || inst->m_vehiclePlate[0])
		return true;
	for(int slot = 0; slot < VEHICLE_MOD_SLOTS; slot++)
		if(inst->m_vehicleMods[slot])
			return true;
	return false;
}

static void
pawnWriteObjectCall(FILE *f, const ObjectInst *inst, PawnCodeFormat format,
	const char *handle)
{
	// A SetPlayerAttachedObject entry is a model/slot call, not a CreateObject.
	if(IsPlayerAttachedObject(inst))
		return;
	if(GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT){
		pawnWriteElementCall(f, inst, format, handle);
		return;
	}
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	PawnEulerRotation e = pawnGetEulerRotation(inst);
	const char *name = obj && obj->m_name[0] ? obj->m_name : "unknown";
	const char *worldTag = pawnIsWorldObject(inst) ? " [world object replacement]" : "";
	if(inst->m_isVehicle){
		// A train's angle only says which way it runs (over 180 or not).
		if(IsTrainEngineModel(inst->m_objectId))
			e.z = TrainExportAngle(e.z);
		if(handle && handle[0])
			fprintf(f, "\tnew %s = ", handle);
		else
			fputs("\t", f);
		if(inst->m_vehicleStatic && inst->m_vehicleRespawnDelay < 0){
			fprintf(f, "AddStaticVehicle(%d, %.6f, %.6f, %.6f, %.6f, %d, %d); // %s\n",
				inst->m_objectId,
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				e.z, inst->m_vehicleColor1, inst->m_vehicleColor2, name);
		}else{
			fprintf(f, "%s(%d, %.6f, %.6f, %.6f, %.6f, %d, %d, %d); // %s\n",
				inst->m_vehicleStatic ? "AddStaticVehicleEx" : "CreateVehicle",
				inst->m_objectId,
				inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				e.z, inst->m_vehicleColor1, inst->m_vehicleColor2,
				inst->m_vehicleRespawnDelay, name);
		}
		if(handle && handle[0] && inst->m_area > 0)
			fprintf(f, "\tLinkVehicleToInterior(%s, %d);\n", handle, inst->m_area);
		if(handle && handle[0]){
			if(inst->m_vehiclePaintjob > 0)
				fprintf(f, "\tChangeVehiclePaintjob(%s, %d);\n", handle, inst->m_vehiclePaintjob - 1);
			for(int slot = 0; slot < VEHICLE_MOD_SLOTS; slot++)
				if(inst->m_vehicleMods[slot])
					fprintf(f, "\tAddVehicleComponent(%s, %d);\n", handle, inst->m_vehicleMods[slot]);
			if(inst->m_vehiclePlate[0]){
				fprintf(f, "\tSetVehicleNumberPlate(%s, ", handle);
				pawnWriteQuotedString(f, inst->m_vehiclePlate);
				fputs(");\n", f);
			}
		}
		return;
	}
	if(handle && handle[0])
		fprintf(f, "\tnew %s = ", handle);
	else
		fputs("\t", f);
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	if(format == PAWN_CODE_STREAMER){
		int interior = pawnStreamerInterior(inst);
		fprintf(f, "CreateDynamicObject(%d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %d, %d, -1",
			PawnModelId(inst->m_objectId),
			inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
			e.x, e.y, e.z, properties->virtualWorld, interior);
		// areaid comes after both distances, so an area writes them out.
		if(properties->streamDistance > 0.0f || properties->drawDistance > 0.0f || pawnHasArea(properties)){
			if(properties->streamDistance > 0.0f)
				fprintf(f, ", %.2f", properties->streamDistance);
			else
				fputs(", STREAMER_OBJECT_SD", f);
			if(properties->drawDistance > 0.0f)
				fprintf(f, ", %.2f", properties->drawDistance);
			else
				fputs(", STREAMER_OBJECT_DD", f);
			pawnWriteAreaArg(f, properties);
		}
		fputs(");", f);
	}else{
		fprintf(f, "CreateObject(%d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f",
			PawnModelId(inst->m_objectId),
			inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
			e.x, e.y, e.z);
		if(properties->drawDistance > 0.0f)
			fprintf(f, ", %.2f", properties->drawDistance);
		fputs(");", f);
	}
	pawnWriteTrailingTags(f, name, worldTag, properties);
}

static void
pawnWritePlayerAttachedObject(FILE *f, const ObjectInst *inst, const char *playerVar)
{
	const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
	fprintf(f, "\tSetPlayerAttachedObject(%s, %d, %d, %d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, 0x%08X, 0x%08X);\n",
		playerVar, properties->attachSlot, PawnModelId(inst->m_objectId), properties->attachBone,
		properties->attachOffset.x, properties->attachOffset.y, properties->attachOffset.z,
		properties->attachRotation.x, properties->attachRotation.y, properties->attachRotation.z,
		properties->attachScale.x, properties->attachScale.y, properties->attachScale.z,
		properties->attachColor1, properties->attachColor2);
}

// The table behind <prefix>_AddSound and the code that plays each game
// sound when a player walks in: a tick with IsPlayerInRangeOfPoint natively,
// a dynamic sphere and its enter/leave callbacks with the streamer. Sound 0
// stops the sound playing, for places that stop when the player walks out.
static void
pawnWriteSoundHelpers(FILE *f, const char *prefix, int count, PawnCodeFormat format)
{
	const char *p = prefix;
	bool streamer = format == PAWN_CODE_STREAMER;
	fprintf(f, "static %s_soundId[%d], Float:%s_soundAt[%d][4], bool:%s_soundStop[%d], %s_numSounds;\n",
		p, count, p, count, p, count, p);
	if(streamer)
		// Untagged, with _: casts: streamer.inc versions disagree on whether
		// STREAMER_TAG_AREA includes the colon, and either spelling breaks one.
		fprintf(f, "static %s_soundArea[%d];\n\n", p, count);
	else
		fprintf(f, "static %s_soundPlace[%d][2], bool:%s_soundIn[MAX_PLAYERS][%d];\n\n", p, count, p, count);
	fprintf(f, "stock %s_AddSound(soundid, Float:x, Float:y, Float:z, Float:radius, bool:stopOnLeave,\n"
		"\tworldid = -1, interiorid = -1)\n{\n", p);
	fprintf(f, "\tif(%s_numSounds == sizeof %s_soundId)\n\t\treturn;\n", p, p);
	fprintf(f, "\tnew i = %s_numSounds++;\n", p);
	fprintf(f, "\t%s_soundId[i] = soundid;\n", p);
	fprintf(f, "\t%s_soundAt[i][0] = x; %s_soundAt[i][1] = y; %s_soundAt[i][2] = z; %s_soundAt[i][3] = radius;\n",
		p, p, p, p);
	fprintf(f, "\t%s_soundStop[i] = stopOnLeave;\n", p);
	if(streamer)
		fprintf(f, "\t%s_soundArea[i] = _:CreateDynamicSphere(x, y, z, radius, worldid, interiorid);\n}\n\n", p);
	else
		fprintf(f, "\t%s_soundPlace[i][0] = worldid; %s_soundPlace[i][1] = interiorid;\n}\n\n", p, p);
	if(streamer){
		fprintf(f, "stock %s_SoundArea(playerid, {DynamicArea, _}:areaid, bool:entered)\n{\n", p);
		fprintf(f, "\tfor(new i = 0; i < %s_numSounds; i++){\n", p);
		fprintf(f, "\t\tif(%s_soundArea[i] != _:areaid)\n\t\t\tcontinue;\n", p);
		fprintf(f, "\t\tif(entered)\n\t\t\tPlayerPlaySound(playerid, %s_soundId[i], %s_soundAt[i][0], "
			"%s_soundAt[i][1], %s_soundAt[i][2]);\n", p, p, p, p);
		fprintf(f, "\t\telse if(%s_soundStop[i])\n\t\t\tPlayerPlaySound(playerid, 0, 0.0, 0.0, 0.0);\n", p);
		fputs("\t}\n}\n\n", f);
		return;
	}
	fprintf(f, "stock %s_SoundTick(playerid)\n{\n", p);
	fprintf(f, "\tfor(new i = 0; i < %s_numSounds; i++){\n", p);
	fprintf(f, "\t\tnew bool:inside = (%s_soundPlace[i][0] == -1 || %s_soundPlace[i][0] == GetPlayerVirtualWorld(playerid)) &&\n"
		"\t\t\t(%s_soundPlace[i][1] == -1 || %s_soundPlace[i][1] == GetPlayerInterior(playerid)) &&\n"
		"\t\t\tIsPlayerInRangeOfPoint(playerid, %s_soundAt[i][3], %s_soundAt[i][0], %s_soundAt[i][1], %s_soundAt[i][2]);\n",
		p, p, p, p, p, p, p, p);
	fprintf(f, "\t\tif(inside && !%s_soundIn[playerid][i])\n\t\t\tPlayerPlaySound(playerid, %s_soundId[i], "
		"%s_soundAt[i][0], %s_soundAt[i][1], %s_soundAt[i][2]);\n", p, p, p, p, p);
	fprintf(f, "\t\telse if(!inside && %s_soundIn[playerid][i] && %s_soundStop[i])\n"
		"\t\t\tPlayerPlaySound(playerid, 0, 0.0, 0.0, 0.0);\n", p, p);
	fprintf(f, "\t\t%s_soundIn[playerid][i] = inside;\n", p);
	fputs("\t}\n}\n\n", f);
}

// The table behind <prefix>_MoveByPathRepeat: open.mp walks a path once,
// so <prefix>_OnNPCFinishMovePath starts it again (the other way round for
// back and forth) for the NPCs that repeat it.
static void
pawnWriteNpcRepeatHelpers(FILE *f, const char *prefix, int count)
{
	const char *p = prefix;
	fputs("#if !defined NPC_PATH_LOOP\n\t#define NPC_PATH_LOOP (1)\n\t#define NPC_PATH_BACK_AND_FORTH (2)\n#endif\n\n", f);
	fprintf(f, "static %s_repeatNpc[%d], %s_repeatPath[%d], NPC_MOVE_TYPE:%s_repeatType[%d],\n"
		"\tFloat:%s_repeatSpeed[%d], bool:%s_repeatReversed[%d], %s_repeatMode[%d], %s_numRepeats;\n\n",
		p, count, p, count, p, count, p, count, p, count, p, count, p);
	fprintf(f, "stock bool:%s_MoveByPathRepeat(npcid, pathid, NPC_MOVE_TYPE:moveType, Float:moveSpeed,\n"
		"\tbool:reversed, mode)\n{\n", p);
	fprintf(f, "\tif(%s_numRepeats < sizeof %s_repeatNpc){\n", p, p);
	fprintf(f, "\t\tnew i = %s_numRepeats++;\n", p);
	fprintf(f, "\t\t%s_repeatNpc[i] = npcid; %s_repeatPath[i] = pathid; %s_repeatType[i] = moveType;\n", p, p, p);
	fprintf(f, "\t\t%s_repeatSpeed[i] = moveSpeed; %s_repeatReversed[i] = reversed; %s_repeatMode[i] = mode;\n",
		p, p, p);
	fputs("\t}\n\treturn NPC_MoveByPath(npcid, pathid, moveType, moveSpeed, reversed);\n}\n\n", f);
	fprintf(f, "stock bool:%s_OnNPCFinishMovePath(npcid, pathid)\n{\n", p);
	fprintf(f, "\tfor(new i = 0; i < %s_numRepeats; i++){\n", p);
	fprintf(f, "\t\tif(%s_repeatNpc[i] != npcid || %s_repeatPath[i] != pathid)\n\t\t\tcontinue;\n", p, p);
	fprintf(f, "\t\tif(%s_repeatMode[i] == NPC_PATH_BACK_AND_FORTH)\n"
		"\t\t\t%s_repeatReversed[i] = !%s_repeatReversed[i];\n", p, p, p);
	fprintf(f, "\t\treturn NPC_MoveByPath(npcid, pathid, %s_repeatType[i], %s_repeatSpeed[i], %s_repeatReversed[i]);\n",
		p, p, p);
	fputs("\t}\n\treturn false;\n}\n\n", f);
}

// Per-player gang zones (open.mp only): <prefix>_AddPlayerZone keeps them
// and _ShowForPlayer makes them for each player. open.mp destroys a
// player's zones when they leave.
static void
pawnWritePlayerZoneHelpers(FILE *f, const char *prefix, int count)
{
	const char *p = prefix;
	fprintf(f, "static Float:%s_pzoneAt[%d][4], %s_pzoneColour[%d], %s_numPZones;\n\n", p, count, p, count, p);
	fprintf(f, "stock %s_AddPlayerZone(Float:minx, Float:miny, Float:maxx, Float:maxy, colour)\n{\n", p);
	fprintf(f, "\tif(%s_numPZones == sizeof %s_pzoneColour)\n\t\treturn;\n", p, p);
	fprintf(f, "\tnew i = %s_numPZones++;\n", p);
	fprintf(f, "\t%s_pzoneAt[i][0] = minx; %s_pzoneAt[i][1] = miny; %s_pzoneAt[i][2] = maxx; %s_pzoneAt[i][3] = maxy;\n",
		p, p, p, p);
	fprintf(f, "\t%s_pzoneColour[i] = colour;\n}\n\n", p);
}

// Streamed gang zones (open.mp only, with the streamer): a streamer
// rectangle for each STREAMED_ZONE_CELL cell of the grid. When a player
// walks into one, <prefix>_ZoneUpdate makes their zones in the cell they
// are in and the eight around it and destroys the rest, as LongWayDrivers'
// minimap does.
static void
pawnWriteStreamedZoneHelpers(FILE *f, const char *prefix, int count, const StreamedZoneGrid &grid)
{
	const char *p = prefix;
	int cells = grid.cols * grid.rows;
	int capacity = std::max(1, std::min(grid.capacity, 1024));
	float cell = STREAMED_ZONE_CELL;
	fprintf(f, "static Float:%s_szoneAt[%d][4], %s_szoneColour[%d], %s_szoneCell[%d], %s_szoneNext[%d], %s_numSZones;\n",
		p, count, p, count, p, count, p, count, p);
	// Untagged areas with _: casts, as for sounds: streamer.inc versions
	// disagree on STREAMER_TAG_AREA.
	fprintf(f, "static %s_cellFirst[%d] = { -1, ... }, %s_cellArea[%d], bool:%s_cellsMade;\n", p, cells, p, cells, p);
	fprintf(f, "static bool:%s_cellLoaded[MAX_PLAYERS][%d];\n", p, cells);
	fprintf(f, "static %s_liveZone[MAX_PLAYERS][%d], %s_liveIndex[MAX_PLAYERS][%d], %s_numLive[MAX_PLAYERS];\n\n",
		p, capacity, p, capacity, p);
	fprintf(f, "static stock %s_ZoneCellAt(Float:at, Float:origin, cells)\n{\n"
		"\treturn clamp(floatround((at - origin) / %.1f, floatround_floor), 0, cells - 1);\n}\n\n", p, cell);
	fprintf(f, "stock %s_AddStreamedZone(Float:minx, Float:miny, Float:maxx, Float:maxy, colour)\n{\n", p);
	fprintf(f, "\tif(!%s_cellsMade){\n\t\t%s_cellsMade = true;\n", p, p);
	fprintf(f, "\t\tfor(new c = 0; c < sizeof %s_cellArea; c++){\n", p);
	fprintf(f, "\t\t\tnew Float:x = %.1f + float(c %% %d) * %.1f, Float:y = %.1f + float(c / %d) * %.1f;\n",
		grid.x0, grid.cols, cell, grid.y0, grid.cols, cell);
	fprintf(f, "\t\t\t%s_cellArea[c] = _:CreateDynamicRectangle(x, y, x + %.1f, y + %.1f);\n\t\t}\n\t}\n", p, cell, cell);
	fprintf(f, "\tif(%s_numSZones == sizeof %s_szoneColour)\n\t\treturn;\n", p, p);
	fprintf(f, "\tnew i = %s_numSZones++;\n", p);
	fprintf(f, "\t%s_szoneAt[i][0] = minx; %s_szoneAt[i][1] = miny; %s_szoneAt[i][2] = maxx; %s_szoneAt[i][3] = maxy;\n",
		p, p, p, p);
	fprintf(f, "\t%s_szoneColour[i] = colour;\n", p);
	fprintf(f, "\t%s_szoneCell[i] = %s_ZoneCellAt((miny + maxy) / 2.0, %.1f, %d) * %d +\n"
		"\t\t%s_ZoneCellAt((minx + maxx) / 2.0, %.1f, %d);\n",
		p, p, grid.y0, grid.rows, grid.cols, p, grid.x0, grid.cols);
	fprintf(f, "\t%s_szoneNext[i] = %s_cellFirst[%s_szoneCell[i]];\n\t%s_cellFirst[%s_szoneCell[i]] = i;\n}\n\n",
		p, p, p, p, p);
	fprintf(f, "stock %s_ZoneReset(playerid)\n{\n", p);
	fprintf(f, "\tfor(new c = 0; c < sizeof %s_cellArea; c++)\n\t\t%s_cellLoaded[playerid][c] = false;\n", p, p);
	fprintf(f, "\t%s_numLive[playerid] = 0;\n}\n\n", p);
	fprintf(f, "stock %s_ZoneUpdate(playerid)\n{\n", p);
	fprintf(f, "\tnew Float:x, Float:y, Float:z;\n\tGetPlayerPos(playerid, x, y, z);\n");
	fprintf(f, "\tnew col = floatround((x %c %.1f) / %.1f, floatround_floor), row = floatround((y %c %.1f) / %.1f, floatround_floor);\n",
		grid.x0 < 0.0f ? '+' : '-', fabsf(grid.x0), cell, grid.y0 < 0.0f ? '+' : '-', fabsf(grid.y0), cell);
	// Unload first, so the zones a player leaves make room for the new ones.
	fprintf(f, "\tfor(new pass = 0; pass < 2; pass++){\n");
	fprintf(f, "\t\tfor(new c = 0; c < sizeof %s_cellArea; c++){\n", p);
	fprintf(f, "\t\t\tnew dx = c %% %d - col, dy = c / %d - row;\n", grid.cols, grid.cols);
	fprintf(f, "\t\t\tnew bool:near = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;\n");
	fprintf(f, "\t\t\tif(pass == 0 && %s_cellLoaded[playerid][c] && !near){\n", p);
	fprintf(f, "\t\t\t\tfor(new k = %s_numLive[playerid] - 1; k >= 0; k--){\n", p);
	fprintf(f, "\t\t\t\t\tif(%s_szoneCell[%s_liveIndex[playerid][k]] != c)\n\t\t\t\t\t\tcontinue;\n", p, p);
	fprintf(f, "\t\t\t\t\tPlayerGangZoneDestroy(playerid, %s_liveZone[playerid][k]);\n", p);
	fprintf(f, "\t\t\t\t\tnew last = --%s_numLive[playerid];\n", p);
	fprintf(f, "\t\t\t\t\t%s_liveZone[playerid][k] = %s_liveZone[playerid][last];\n", p, p);
	fprintf(f, "\t\t\t\t\t%s_liveIndex[playerid][k] = %s_liveIndex[playerid][last];\n\t\t\t\t}\n", p, p);
	fprintf(f, "\t\t\t\t%s_cellLoaded[playerid][c] = false;\n", p);
	fprintf(f, "\t\t\t}else if(pass == 1 && !%s_cellLoaded[playerid][c] && near){\n", p);
	fprintf(f, "\t\t\t\t%s_cellLoaded[playerid][c] = true;\n", p);
	fprintf(f, "\t\t\t\tfor(new i = %s_cellFirst[c]; i != -1 && %s_numLive[playerid] < sizeof %s_liveZone[];"
		" i = %s_szoneNext[i]){\n", p, p, p, p);
	fprintf(f, "\t\t\t\t\tnew zone = CreatePlayerGangZone(playerid, %s_szoneAt[i][0], %s_szoneAt[i][1],"
		" %s_szoneAt[i][2], %s_szoneAt[i][3]);\n", p, p, p, p);
	fprintf(f, "\t\t\t\t\tif(zone == INVALID_GANG_ZONE)\n\t\t\t\t\t\tbreak;\n");
	fprintf(f, "\t\t\t\t\tPlayerGangZoneShow(playerid, zone, %s_szoneColour[i]);\n", p);
	fprintf(f, "\t\t\t\t\tnew k = %s_numLive[playerid]++;\n", p);
	fprintf(f, "\t\t\t\t\t%s_liveZone[playerid][k] = zone;\n\t\t\t\t\t%s_liveIndex[playerid][k] = i;\n", p, p);
	fputs("\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n\n", f);
	fprintf(f, "stock bool:%s_ZoneArea(playerid, {DynamicArea, _}:areaid)\n{\n", p);
	fprintf(f, "\tfor(new c = 0; c < sizeof %s_cellArea; c++){\n", p);
	fprintf(f, "\t\tif(%s_cellArea[c] == _:areaid){\n\t\t\t%s_ZoneUpdate(playerid);\n\t\t\treturn true;\n\t\t}\n", p, p);
	fputs("\t}\n\treturn false;\n}\n\n", f);
}

static int
pawnWriteCode(FILE *f, const char *path, PawnCodeFormat format, bool selectedOnly,
	int *numSkipped)
{
	std::vector<ObjectInst*> insts;
	pawnCollectInstances(selectedOnly, insts, numSkipped);
	// The radar art layers' zones, written as the gang zones they become.
	struct StandIns {
		std::vector<ObjectInst*> list;
		~StandIns() { FreeRadarArtStandIns(list); }
	} standIns;
	if(!selectedOnly){
		MakeRadarArtStandIns(standIns.list);
		insts.insert(insts.end(), standIns.list.begin(), standIns.list.end());
	}

	char prefix[96];
	pawnBuildFunctionPrefix(path, prefix, sizeof(prefix));
	// A deleted building inside a -1 removal is covered by that one line.
	const std::vector<PawnAreaRemoval> &areaRemovals = selectedOnly ? std::vector<PawnAreaRemoval>() : gAreaRemovals;
	auto coveredByArea = [&](ObjectInst *inst){
		return inst->m_isDeleted && !selectedOnly && InsidePawnAreaRemoval(inst);
	};
	int numWorldObjects = (int)areaRemovals.size();
	for(size_t i = 0; i < insts.size(); i++)
		if(pawnIsWorldObject(insts[i]) && !coveredByArea(insts[i]))
			numWorldObjects++;
	// Gang zones shown per player (open.mp) or streamed, by mode.
	std::vector<ObjectInst*> playerZones, streamedZones;
	for(size_t i = 0; i < insts.size(); i++){
		if(insts[i]->m_isDeleted || GetSampElementKind(insts[i]) != SAMP_ELEMENT_GANGZONE)
			continue;
		int mode = GetObjectPawnProperties(insts[i])->zoneMode;
		if(mode == SAMP_ZONE_PLAYER)
			playerZones.push_back(insts[i]);
		else if(mode == SAMP_ZONE_STREAMED)
			streamedZones.push_back(insts[i]);
	}
	StreamedZoneGrid zoneGrid = {};
	if(!streamedZones.empty()){
		std::vector<float> bounds;
		for(size_t i = 0; i < streamedZones.size(); i++){
			float b[4];
			pawnGangZoneBounds(streamedZones[i], GetObjectPawnProperties(streamedZones[i]), &b[0], &b[1], &b[2], &b[3]);
			bounds.insert(bounds.end(), b, b + 4);
		}
		GetStreamedZoneGrid(bounds, &zoneGrid);
	}

	fprintf(f, "// Generated by Haumea Studio from GTA San Andreas map objects.\n");
	fprintf(f, "// This file uses absolute world coordinates and XYZ rotation degrees.\n");
	fprintf(f, "// Unchanged world objects are omitted from this file.\n");
	fprintf(f, "// A moved, rotated, deleted, or material-edited world object is exported\n");
	fprintf(f, "// as RemoveBuildingForPlayer plus a scripted replacement when it is not deleted.\n");
	// Sound places: streams start in _ShowForPlayer, game sounds play when
	// a player walks in (a timer tick natively, area callbacks with the streamer).
	std::vector<ObjectInst*> audioStreams, gameSounds;
	for(size_t i = 0; i < insts.size(); i++)
		if(!insts[i]->m_isDeleted && GetSampElementKind(insts[i]) == SAMP_ELEMENT_SOUND)
			(GetObjectPawnProperties(insts[i])->soundKind == SAMP_SOUND_STREAM ?
				audioStreams : gameSounds).push_back(insts[i]);
	bool nativeGameSounds = !gameSounds.empty() && format != PAWN_CODE_STREAMER;
	bool hasPlayerElements = !audioStreams.empty() || nativeGameSounds;
	for(size_t i = 0; i < insts.size(); i++){
		int kind = GetSampElementKind(insts[i]);
		if(!insts[i]->m_isDeleted && (kind == SAMP_ELEMENT_GANGZONE ||
		   (kind == SAMP_ELEMENT_MAPICON && format != PAWN_CODE_STREAMER)))
			hasPlayerElements = true;
	}
	fprintf(f, "// Call %s_CreateObjects() from OnGameModeInit (it also adds the player classes).\n", prefix);
	if(hasPlayerElements)
		fprintf(f, "// Call %s_ShowForPlayer(playerid) from OnPlayerConnect for gang zones, map icons and sounds.\n", prefix);
	if(!audioStreams.empty())
		fprintf(f, "// A player hears one audio stream at a time: the last one started plays.\n");
	if(nativeGameSounds)
		fprintf(f, "// Game sounds play when a player walks in: call %s_SoundTick(playerid) for each\n"
			"// player from a timer, e.g. every 500 ms.\n", prefix);
	else if(!gameSounds.empty())
		fprintf(f, "// Game sounds play when a player walks in: call %s_SoundArea(playerid, areaid, true)\n"
			"// from OnPlayerEnterDynamicArea and %s_SoundArea(playerid, areaid, false) from\n"
			"// OnPlayerLeaveDynamicArea.\n", prefix, prefix);
	bool hasNpcs = false;
	for(size_t i = 0; i < insts.size(); i++){
		int kind = GetSampElementKind(insts[i]);
		if(!insts[i]->m_isDeleted && (kind == SAMP_ELEMENT_NPC || kind == SAMP_ELEMENT_PATHPOINT))
			hasNpcs = true;
	}
	if(hasNpcs)
		fprintf(f, "// The NPC_* calls need open.mp with its NPCs component; SA-MP 0.3.7 has no NPC natives.\n");
	if(!playerZones.empty() || !streamedZones.empty())
		fprintf(f, "// Per-player gang zones (CreatePlayerGangZone) need open.mp; SA-MP 0.3.7 has none.\n");
	if(!streamedZones.empty())
		fprintf(f, "// Streamed gang zones need the streamer: call %s_ZoneArea(playerid, areaid) from\n"
			"// OnPlayerEnterDynamicArea and OnPlayerLeaveDynamicArea, and %s_ZoneUpdate(playerid)\n"
			"// from OnPlayerSpawn.\n", prefix, prefix);
	// NPCs that walk their path again need the end-of-path callback.
	int numRepeating = 0;
	for(size_t i = 0; i < insts.size(); i++){
		const ObjectPawnProperties *p = GetObjectPawnProperties(insts[i]);
		if(!insts[i]->m_isDeleted && p->elementKind == SAMP_ELEMENT_NPC && p->npcPathRepeat != NPC_PATH_ONCE)
			numRepeating++;
	}
	if(numRepeating > 0)
		fprintf(f, "// NPCs that walk their path again: call %s_OnNPCFinishMovePath(npcid, pathid)\n"
			"// from OnNPCFinishMovePath(npcid, pathid), or they stop at the end.\n", prefix);
	int numCheckpoints = 0;
	for(size_t i = 0; i < insts.size(); i++)
		if(!insts[i]->m_isDeleted && GetSampElementKind(insts[i]) == SAMP_ELEMENT_CHECKPOINT)
			numCheckpoints++;
	if(numCheckpoints > 0 && format != PAWN_CODE_STREAMER)
		fprintf(f, "// %d checkpoint(s) not written: map checkpoints need the streamer (natively a player\n"
			"// has one checkpoint at a time, set with SetPlayerCheckpoint). Save as streamer code.\n",
			numCheckpoints);
	bool hasCustomModels = false;
	for(size_t i = 0; i < insts.size() && !hasCustomModels; i++){
		const ObjectInst *inst = insts[i];
		if(inst->m_isDeleted)
			continue;
		hasCustomModels = IsCustomModel(inst->m_objectId) ||
			IsCustomModel(GetObjectPawnProperties(inst)->elementModel);
		for(int slot = 0; slot < MAX_PAWN_MATERIALS && !hasCustomModels; slot++){
			const ObjectMaterialOverride *material = GetObjectMaterialOverride(inst, slot);
			hasCustomModels = material && material->active && material->hasTexture &&
				IsCustomModel(material->sourceModel);
		}
	}
	if(hasCustomModels)
		fprintf(f, "// Uses custom models: needs SA-MP 0.3.DL or open.mp with the server's models/artconfig.txt.\n");
	if(numWorldObjects > 0){
		fprintf(f, "// Call %s_RemoveWorldObjectsForPlayer(playerid) from OnPlayerConnect.\n", prefix);
		fprintf(f, "// Deleted world objects have a removal line but no CreateObject call.\n");
	}
	if(!selectedOnly)
		WriteTextDrawsPawnHeader(f, prefix);
	// open.mp's own include declares the NPC natives. Without NPCs the map
	// takes open.mp's include when there is one (its <a_samp> is a wrapper
	// that warns) and SA-MP 0.3.7's a_samp otherwise.
	bool needsOpenMp = hasNpcs || !playerZones.empty() || !streamedZones.empty();
	fprintf(f, needsOpenMp ? "\n#include <open.mp>\n" :
		"\n#tryinclude <open.mp>\n#if !defined _INC_open_mp\n\t#include <a_samp>\n#endif\n");
	if(format == PAWN_CODE_STREAMER || !streamedZones.empty())
		fprintf(f, "#include <streamer>\n");
	fprintf(f, "\n");
	// Weapon names only open.mp defines, for player classes on SA-MP 0.3.7.
	std::vector<int> openMpWeapons;
	for(size_t i = 0; i < insts.size(); i++){
		const ObjectPawnProperties *properties = GetObjectPawnProperties(insts[i]);
		if(insts[i]->m_isDeleted || properties->elementKind != SAMP_ELEMENT_CLASS)
			continue;
		for(int w = 0; w < PAWN_CLASS_WEAPONS; w++){
			int weapon = properties->classWeapons[w];
			if(pawnWeaponIsOpenMpOnly(weapon) &&
			   std::find(openMpWeapons.begin(), openMpWeapons.end(), weapon) == openMpWeapons.end())
				openMpWeapons.push_back(weapon);
		}
	}
	std::sort(openMpWeapons.begin(), openMpWeapons.end());
	for(size_t i = 0; i < openMpWeapons.size(); i++){
		const char *name = pawnWeaponName(openMpWeapons[i]);
		fprintf(f, "#if !defined %s\n\t#define %s (%d)\n#endif\n", name, name, openMpWeapons[i]);
	}
	if(!openMpWeapons.empty())
		fputc('\n', f);
	if(!selectedOnly)
		WriteTextDrawsPawnGlobals(f, prefix);

	if(numWorldObjects > 0){
		fprintf(f, "stock %s_RemoveWorldObjectsForPlayer(playerid)\n{\n", prefix);
		for(const PawnAreaRemoval &r : areaRemovals)
			fprintf(f, "\tRemoveBuildingForPlayer(playerid, -1, %.6f, %.6f, %.6f, %.6f); // everything in the area\n",
				r.x, r.y, r.z, r.radius);
		for(size_t i = 0; i < insts.size(); i++){
			ObjectInst *inst = insts[i];
			if(!pawnIsWorldObject(inst) || coveredByArea(inst))
				continue;
			ObjectDef *obj = GetObjectDef(inst->m_objectId);
			const char *name = obj && obj->m_name[0] ? obj->m_name : "unknown";
			fprintf(f, "\tRemoveBuildingForPlayer(playerid, %d, %.6f, %.6f, %.6f, 0.25); // %s\n",
				inst->m_objectId,
				inst->m_origTranslation.x, inst->m_origTranslation.y, inst->m_origTranslation.z,
				name);
		}
		fprintf(f, "}\n\n");
	}

	// Attachments whose parent is part of this export. Both objects need a
	// variable, and the attach calls go last so every parent exists. Player
	// bone slots do not create an object-pool handle.
	std::unordered_map<const ObjectInst*, const ObjectInst*> attachParents;
	std::vector<ObjectInst*> playerAttachments;
	std::unordered_set<const ObjectInst*> exported(insts.begin(), insts.end());
	for(size_t i = 0; i < insts.size(); i++){
		ObjectInst *inst = insts[i];
		if(inst->m_isDeleted)
			continue;
		if(IsPlayerAttachedObject(inst)){
			if(inst->m_pawnProperties->attachParent == PAWN_ATTACH_PLAYER){
				playerAttachments.push_back(inst);
				continue;
			}
			ObjectInst *parent = GetInstanceByID(inst->m_pawnProperties->attachParent);
			if(parent && exported.count(parent))
				playerAttachments.push_back(inst);
			continue;
		}
		ObjectInst *parent = GetAttachParent(inst);
		if(parent && exported.count(parent))
			attachParents[insts[i]] = parent;
	}
	// Vehicles an exported NPC sits in need a variable for NPC_PutInVehicle.
	std::unordered_set<const ObjectInst*> npcVehicles;
	for(size_t i = 0; i < insts.size(); i++){
		ObjectInst *vehicle = insts[i]->m_isDeleted ? nil : GetNpcVehicle(insts[i]);
		if(vehicle && exported.count(vehicle))
			npcVehicles.insert(vehicle);
	}
	std::unordered_set<const ObjectInst*> needsHandle;
	for(std::unordered_map<const ObjectInst*, const ObjectInst*>::iterator it = attachParents.begin();
	    it != attachParents.end(); ++it){
		needsHandle.insert(it->first);
		needsHandle.insert(it->second);
	}

	// Objects are written group by group, in order of first appearance.
	std::vector<std::string> groups;
	for(size_t i = 0; i < insts.size(); i++){
		std::string group = GetObjectPawnProperties(insts[i])->group;
		if(std::find(groups.begin(), groups.end(), group) == groups.end())
			groups.push_back(group);
	}
	std::stable_sort(groups.begin(), groups.end(), [](const std::string &a, const std::string &b){
		return a.empty() && !b.empty();	// ungrouped objects first
	});

	// Variables, in file order so generated names stay stable. Generated
	// names count in write order, which reopening the file keeps.
	std::unordered_map<const ObjectInst*, std::string> handles;
	std::unordered_set<std::string> usedHandles;
	int generatedHandles = 0;
	std::unordered_set<const ObjectInst*> unusedHandles;	// pawncc would warn (204)
	for(size_t g = 0; g < groups.size(); g++)
		for(size_t i = 0; i < insts.size(); i++){
			ObjectInst *inst = insts[i];
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			if(inst->m_isDeleted || groups[g] != properties->group)
				continue;
			int kind = GetSampElementKind(inst);
			// Animated actors need a handle in both formats.
			bool nativeActorExtras = kind == SAMP_ELEMENT_ACTOR &&
				(properties->animName[0] || (format != PAWN_CODE_STREAMER &&
				 (properties->virtualWorld > 0 || !properties->actorInvulnerable ||
				  properties->actorHealth != 100.0f)));
			// Whether a later call uses the variable; a generated one is only
			// made then, a hand-written one is kept anyway.
			bool usedLater = (inst->m_isVehicle && (vehicleNeedsHandle(inst) || npcVehicles.count(inst) || needsHandle.count(inst))) ||
				kind == SAMP_ELEMENT_GANGZONE || nativeActorExtras || kind == SAMP_ELEMENT_NPC ||
				(kind == SAMP_ELEMENT_OBJECT && !inst->m_isVehicle && !IsPlayerAttachedObject(inst) &&
				 (HasObjectMaterialOverrides(inst) || needsHandle.count(inst)));
			char handle[128];
			handle[0] = '\0';
			if(kind == SAMP_ELEMENT_CLASS || kind == SAMP_ELEMENT_PATHPOINT || kind == SAMP_ELEMENT_SOUND ||
			   kind == SAMP_ELEMENT_AREA ||
			   (kind == SAMP_ELEMENT_GANGZONE && properties->zoneMode != SAMP_ZONE_GLOBAL) ||
			   (kind == SAMP_ELEMENT_CHECKPOINT &&
			    (format != PAWN_CODE_STREAMER || properties->cpKind == SAMP_CHECKPOINT_RACE)) ||
			   (kind == SAMP_ELEMENT_MAPICON && format != PAWN_CODE_STREAMER))
				;	// AddPlayerClass and SetPlayerMapIcon return nothing to keep;
					// path points are written with their path
			else if(properties->name[0] && usedHandles.count(properties->name) == 0)
				strncpy(handle, properties->name, sizeof(handle)-1);
			else if(inst->m_isVehicle && (vehicleNeedsHandle(inst) || npcVehicles.count(inst) || needsHandle.count(inst)))
				snprintf(handle, sizeof(handle), "%s_vehicle_%d", prefix, generatedHandles++);
			else if(kind == SAMP_ELEMENT_GANGZONE)
				snprintf(handle, sizeof(handle), "%s_zone_%d", prefix, generatedHandles++);
			else if(nativeActorExtras)
				snprintf(handle, sizeof(handle), "%s_actor_%d", prefix, generatedHandles++);
			else if(kind == SAMP_ELEMENT_NPC)
				snprintf(handle, sizeof(handle), "%s_npc_%d", prefix, generatedHandles++);
			else if(kind == SAMP_ELEMENT_OBJECT && !inst->m_isVehicle && !IsPlayerAttachedObject(inst) &&
				(HasObjectMaterialOverrides(inst) || needsHandle.count(inst) || properties->name[0]))
				snprintf(handle, sizeof(handle), "%s_obj_%d", prefix, generatedHandles++);
			if(handle[0]){
				handles[inst] = handle;
				usedHandles.insert(handle);
				if(!usedLater)
					unusedHandles.insert(inst);
			}
		}

	CaptureMapXMLExport(insts, handles, (int)format, selectedOnly);

	// Gang zones are shown per player, so their IDs live at file scope.
	std::vector<ObjectInst*> gangZones, nativeMapIcons;
	for(size_t i = 0; i < insts.size(); i++){
		if(insts[i]->m_isDeleted)
			continue;
		int kind = GetSampElementKind(insts[i]);
		if(kind == SAMP_ELEMENT_GANGZONE && GetObjectPawnProperties(insts[i])->zoneMode == SAMP_ZONE_GLOBAL)
			gangZones.push_back(insts[i]);
		else if(kind == SAMP_ELEMENT_MAPICON && format != PAWN_CODE_STREAMER)
			nativeMapIcons.push_back(insts[i]);
	}
	if(!gangZones.empty()){
		for(size_t i = 0; i < gangZones.size(); i++)
			fprintf(f, "static %s = -1;\n", handles[gangZones[i]].c_str());
		fputc('\n', f);
	}

	// NPC paths, by name, each point in its order. The path's variable is
	// its name, unless an object already took that name.
	std::vector<std::string> pathNames;
	std::unordered_map<std::string, std::vector<ObjectInst*>> pathPoints;
	for(size_t i = 0; i < insts.size(); i++){
		ObjectInst *inst = insts[i];
		if(inst->m_isDeleted || GetSampElementKind(inst) != SAMP_ELEMENT_PATHPOINT)
			continue;
		std::string name = GetObjectPawnProperties(inst)->pathName;
		if(name.empty())
			continue;
		if(pathPoints.find(name) == pathPoints.end())
			pathNames.push_back(name);
		pathPoints[name].push_back(inst);
	}
	std::sort(pathNames.begin(), pathNames.end());
	std::unordered_map<std::string, std::string> pathHandles;
	for(size_t i = 0; i < pathNames.size(); i++){
		std::string handle = pathNames[i];
		if(usedHandles.count(handle))
			handle = std::string(prefix) + "_path_" + handle;
		pathHandles[pathNames[i]] = handle;
		usedHandles.insert(handle);
		std::stable_sort(pathPoints[pathNames[i]].begin(), pathPoints[pathNames[i]].end(),
			[](const ObjectInst *a, const ObjectInst *b){
				return GetObjectPawnProperties(a)->pathOrder < GetObjectPawnProperties(b)->pathOrder;
			});
	}

	if(!gameSounds.empty())
		pawnWriteSoundHelpers(f, prefix, (int)gameSounds.size(), format);
	if(numRepeating > 0)
		pawnWriteNpcRepeatHelpers(f, prefix, numRepeating);
	if(!playerZones.empty())
		pawnWritePlayerZoneHelpers(f, prefix, (int)playerZones.size());
	if(!streamedZones.empty())
		pawnWriteStreamedZoneHelpers(f, prefix, (int)streamedZones.size(), zoneGrid);

	fprintf(f, "stock %s_CreateObjects()\n{\n", prefix);
	// Dynamic areas first (streamer only): items name them as their areaid.
	gPawnAreaHandles.clear();
	std::vector<ObjectInst*> areas;
	for(size_t i = 0; i < insts.size() && format == PAWN_CODE_STREAMER; i++)
		if(!insts[i]->m_isDeleted && GetSampElementKind(insts[i]) == SAMP_ELEMENT_AREA)
			areas.push_back(insts[i]);
	if(!areas.empty()){
		std::unordered_set<std::string> usedAreas;
		for(size_t i = 0; i < insts.size(); i++){
			const ObjectPawnProperties *properties = GetObjectPawnProperties(insts[i]);
			if(!insts[i]->m_isDeleted && properties->areaName[0] && !insts[i]->m_isVehicle)
				usedAreas.insert(properties->areaName);
		}
		fputs("\t// ---- dynamic areas (streamer) ----\n", f);
		for(size_t i = 0; i < areas.size(); i++){
			const ObjectPawnProperties *properties = GetObjectPawnProperties(areas[i]);
			std::string handle = properties->name;
			if(handle.empty() || usedHandles.count(handle) || gPawnAreaHandles.count(handle))
				handle = std::string(prefix) + "_area_" + std::to_string(generatedHandles++);
			usedHandles.insert(handle);
			if(properties->name[0])
				gPawnAreaHandles[properties->name] = handle;
			pawnWriteAreaCall(f, areas[i], handle.c_str());
			if(!properties->name[0] || !usedAreas.count(properties->name))
				fprintf(f, "\t#pragma unused %s\n", handle.c_str());
		}
		fputc('\n', f);
	}
	if(!pathNames.empty()){
		fputs("\t// ---- NPC paths (open.mp) ----\n", f);
		for(size_t i = 0; i < pathNames.size(); i++){
			const char *handle = pathHandles[pathNames[i]].c_str();
			fprintf(f, "\tnew %s = NPC_CreatePath();\n", handle);
			const std::vector<ObjectInst*> &points = pathPoints[pathNames[i]];
			for(size_t j = 0; j < points.size(); j++){
				const rw::V3d &p = points[j]->m_translation;
				const ObjectPawnProperties *properties = GetObjectPawnProperties(points[j]);
				fprintf(f, "\tNPC_AddPointToPath(%s, %.6f, %.6f, %.6f, %.2f);", handle,
					p.x, p.y, p.z, properties->pathStopRange);
				pawnWriteTrailingTags(f, "path point", "", properties);
			}
		}
		fputc('\n', f);
	}
	// Race checkpoints, race by race in order, each pointing at the next.
	std::vector<std::string> raceNames;
	std::unordered_map<std::string, std::vector<ObjectInst*>> races;
	for(size_t i = 0; i < insts.size() && format == PAWN_CODE_STREAMER; i++){
		const ObjectPawnProperties *properties = GetObjectPawnProperties(insts[i]);
		if(insts[i]->m_isDeleted || properties->elementKind != SAMP_ELEMENT_CHECKPOINT ||
		   properties->cpKind != SAMP_CHECKPOINT_RACE)
			continue;
		if(races.find(properties->cpRace) == races.end())
			raceNames.push_back(properties->cpRace);
		races[properties->cpRace].push_back(insts[i]);
	}
	std::sort(raceNames.begin(), raceNames.end());
	if(!raceNames.empty()){
		fputs("\t// ---- race checkpoints (streamer) ----\n", f);
		for(size_t r = 0; r < raceNames.size(); r++){
			std::vector<ObjectInst*> &race = races[raceNames[r]];
			std::stable_sort(race.begin(), race.end(), [](const ObjectInst *a, const ObjectInst *b){
				return GetObjectPawnProperties(a)->cpOrder < GetObjectPawnProperties(b)->cpOrder;
			});
			for(size_t j = 0; j < race.size(); j++){
				const ObjectPawnProperties *properties = GetObjectPawnProperties(race[j]);
				const rw::V3d &p = race[j]->m_translation;
				rw::V3d next = { 0.0f, 0.0f, 0.0f };	// the last one points nowhere
				if(j + 1 < race.size())
					next = race[j + 1]->m_translation;
				fprintf(f, "\tCreateDynamicRaceCP(%d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.2f, %d, %d, -1, ",
					properties->cpRaceType, p.x, p.y, p.z, next.x, next.y, next.z, properties->cpSize,
					properties->virtualWorld, race[j]->m_area > 0 ? race[j]->m_area : -1);
				pawnWriteDistance(f, properties->streamDistance, "STREAMER_RACE_CP_SD");
				pawnWriteAreaArg(f, properties);
				fputs(");", f);
				pawnWriteTrailingTags(f, "checkpoint", "", properties);
			}
		}
		fputc('\n', f);
	}
	for(size_t g = 0; g < groups.size(); g++){
		for(size_t i = 0; i < insts.size(); i++){
			ObjectInst *inst = insts[i];
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			if(inst->m_isDeleted || groups[g] != properties->group)
				continue;
			int kind = GetSampElementKind(inst);
			if(kind == SAMP_ELEMENT_MAPICON && format != PAWN_CODE_STREAMER)
				continue;	// written to _ShowForPlayer
			if(kind == SAMP_ELEMENT_PATHPOINT || kind == SAMP_ELEMENT_AREA)
				continue;	// written with its path, or with the areas, above
			if(kind == SAMP_ELEMENT_CHECKPOINT &&
			   (format != PAWN_CODE_STREAMER || properties->cpKind == SAMP_CHECKPOINT_RACE))
				continue;	// streamer only; races are written above
			if(kind == SAMP_ELEMENT_SOUND){
				if(properties->soundKind == SAMP_SOUND_GAME){
					const rw::V3d &p = inst->m_translation;
					fprintf(f, "\t%s_AddSound(%d, %.6f, %.6f, %.6f, %.2f, %s, %d, %d);", prefix,
						properties->soundId, p.x, p.y, p.z, properties->soundRadius,
						properties->soundStopOnLeave ? "true" : "false", properties->virtualWorld,
						inst->m_area > 0 ? inst->m_area : -1);
					pawnWriteTrailingTags(f, "sound", "", properties);
				}
				continue;	// streams are written to _ShowForPlayer
			}
			if(kind == SAMP_ELEMENT_GANGZONE && properties->zoneMode != SAMP_ZONE_GLOBAL){
				float minX, minY, maxX, maxY;
				pawnGangZoneBounds(inst, properties, &minX, &minY, &maxX, &maxY);
				fprintf(f, "\t%s_%s(%.6f, %.6f, %.6f, %.6f, 0x%08X);", prefix,
					properties->zoneMode == SAMP_ZONE_PLAYER ? "AddPlayerZone" : "AddStreamedZone",
					minX, minY, maxX, maxY, properties->zoneColor);
				pawnWriteTrailingTags(f, "gangzone", "", properties);
				continue;
			}
			std::unordered_map<const ObjectInst*, std::string>::iterator it = handles.find(inst);
			const char *handle = it != handles.end() ? it->second.c_str() : "";
			pawnWriteObjectCall(f, inst, format, handle);
			if(!inst->m_isVehicle && kind == SAMP_ELEMENT_OBJECT)
				pawnWriteMaterialCalls(f, inst, format, handle);
			if(unusedHandles.count(inst))
				fprintf(f, "\t#pragma unused %s\n", handle);
		}
	}
	if(!attachParents.empty()){
		fputs("\n\t// ---- attachments ----\n", f);
		for(size_t i = 0; i < insts.size(); i++){
			std::unordered_map<const ObjectInst*, const ObjectInst*>::iterator it =
				attachParents.find(insts[i]);
			if(it == attachParents.end())
				continue;
			const ObjectPawnProperties *properties = GetObjectPawnProperties(insts[i]);
			bool vehicle = it->second->m_isVehicle;
			const char *attachName = vehicle ? (format == PAWN_CODE_STREAMER ?
				"AttachDynamicObjectToVehicle" : "AttachObjectToVehicle") :
				(format == PAWN_CODE_STREAMER ? "AttachDynamicObjectToObject" : "AttachObjectToObject");
			fprintf(f, "\t%s(%s, %s, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f", attachName,
				handles[it->first].c_str(), handles[it->second].c_str(),
				properties->attachOffset.x, properties->attachOffset.y, properties->attachOffset.z,
				properties->attachRotation.x, properties->attachRotation.y,
				properties->attachRotation.z);
			if(!vehicle)
				fprintf(f, ", %s", properties->attachSyncRotation ? "true" : "false");
			fputs(");\n", f);
		}
	}
	if(!playerAttachments.empty()){
		bool wroteNpc = false;
		for(ObjectInst *inst : playerAttachments){
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			ObjectInst *parent = properties->attachParent >= 0 ?
				GetInstanceByID(properties->attachParent) : nil;
			if(parent == nil || GetSampElementKind(parent) != SAMP_ELEMENT_NPC ||
			   handles.find(parent) == handles.end())
				continue;
			if(!wroteNpc){
				fputs("\n\t// ---- NPC attached objects (open.mp) ----\n", f);
				wroteNpc = true;
			}
			pawnWritePlayerAttachedObject(f, inst, handles[parent].c_str());
		}
	}
	// NPCs get in their vehicles once both exist, before any recording starts.
	bool vehicleHeader = false;
	for(size_t i = 0; i < insts.size(); i++){
		ObjectInst *inst = insts[i];
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(inst->m_isDeleted || properties->elementKind != SAMP_ELEMENT_NPC)
			continue;
		ObjectInst *vehicle = GetNpcVehicle(inst);
		if(vehicle == nil || !npcVehicles.count(vehicle) || !handles.count(vehicle))
			continue;
		if(!vehicleHeader)
			fputs("\n\t// ---- NPCs in vehicles ----\n", f);
		vehicleHeader = true;
		fprintf(f, "\tNPC_PutInVehicle(%s, %s, %d);\n", handles[inst].c_str(),
			handles[vehicle].c_str(), properties->npcSeat);
	}
	// NPCs start walking once every NPC and path exists.
	bool movementHeader = false;
	static const char *moveTypes[] = {
		"NPC_MOVE_TYPE_NONE", "NPC_MOVE_TYPE_WALK", "NPC_MOVE_TYPE_JOG",
		"NPC_MOVE_TYPE_SPRINT", "NPC_MOVE_TYPE_DRIVE", "NPC_MOVE_TYPE_AUTO",
	};
	for(size_t i = 0; i < insts.size(); i++){
		ObjectInst *inst = insts[i];
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(inst->m_isDeleted || properties->elementKind != SAMP_ELEMENT_NPC)
			continue;
		bool playback = properties->npcRecording[0] != '\0';
		if(!playback && !properties->npcPlaysNode &&
		   pathHandles.find(properties->pathName) == pathHandles.end())
			continue;
		if(!movementHeader){
			fputs("\n\t// ---- NPC movement ----\n", f);
			// Every node file an NPC walks, opened once.
			std::vector<int> nodes;
			for(size_t j = 0; j < insts.size(); j++){
				const ObjectPawnProperties *other = GetObjectPawnProperties(insts[j]);
				if(!insts[j]->m_isDeleted && other->elementKind == SAMP_ELEMENT_NPC &&
				   other->npcPlaysNode && !other->npcRecording[0] &&
				   std::find(nodes.begin(), nodes.end(), other->npcNode) == nodes.end())
					nodes.push_back(other->npcNode);
			}
			std::sort(nodes.begin(), nodes.end());
			for(size_t j = 0; j < nodes.size(); j++)
				fprintf(f, "\tNPC_OpenNode(%d); // scriptfiles/NPCs/nodes/NODES%d.DAT\n", nodes[j], nodes[j]);
		}
		movementHeader = true;
		if(playback){
			// A zero start position plays the route where it was recorded.
			rw::V3d start = { 0.0f, 0.0f, 0.0f };
			if(properties->npcRecordingAtNpc)
				start = inst->m_translation;
			fprintf(f, "\tNPC_StartPlayback(%s, ", handles[inst].c_str());
			pawnWriteQuotedString(f, properties->npcRecording);
			fprintf(f, ", true, %.6f, %.6f, %.6f, 0.0, 0.0, 0.0);\n", start.x, start.y, start.z);
			continue;
		}
		if(properties->npcPlaysNode){
			// The node's current point is shared, so set it right before each NPC starts.
			fprintf(f, "\tNPC_SetNodePoint(%d, %d);\n", properties->npcNode, properties->npcNodePoint);
			fprintf(f, "\tNPC_PlayNode(%s, %d, %s, ", handles[inst].c_str(), properties->npcNode,
				moveTypes[clamp(properties->npcMoveType, 0, 5)]);
			if(properties->npcMoveSpeed < 0.0f)
				fputs("NPC_MOVE_SPEED_AUTO", f);
			else
				fprintf(f, "%.4f", properties->npcMoveSpeed);
			fprintf(f, ", %.2f, %s);\n", properties->npcNodeRadius,
				properties->npcNodeSetAngle ? "true" : "false");
			continue;
		}
		// A repeating NPC goes through the helper, which remembers it for
		// <prefix>_OnNPCFinishMovePath.
		if(properties->npcPathRepeat != NPC_PATH_ONCE)
			fprintf(f, "\t%s_MoveByPathRepeat(%s, %s, %s, ", prefix, handles[inst].c_str(),
				pathHandles[properties->pathName].c_str(), moveTypes[clamp(properties->npcMoveType, 0, 5)]);
		else
			fprintf(f, "\tNPC_MoveByPath(%s, %s, %s, ", handles[inst].c_str(),
				pathHandles[properties->pathName].c_str(), moveTypes[clamp(properties->npcMoveType, 0, 5)]);
		if(properties->npcMoveSpeed < 0.0f)
			fputs("NPC_MOVE_SPEED_AUTO", f);
		else
			fprintf(f, "%.4f", properties->npcMoveSpeed);
		if(properties->npcPathRepeat != NPC_PATH_ONCE)
			fprintf(f, ", %s, %s);\n", properties->npcPathReversed ? "true" : "false",
				properties->npcPathRepeat == NPC_PATH_LOOP ? "NPC_PATH_LOOP" : "NPC_PATH_BACK_AND_FORTH");
		else
			fprintf(f, ", %s);\n", properties->npcPathReversed ? "true" : "false");
	}
	fprintf(f, "}\n");
	bool playerAttachmentHelper = false;
	for(ObjectInst *inst : playerAttachments){
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		ObjectInst *parent = properties->attachParent >= 0 ? GetInstanceByID(properties->attachParent) : nil;
		if(parent == nil || GetSampElementKind(parent) == SAMP_ELEMENT_CLASS)
			playerAttachmentHelper = true;
	}
	if(playerAttachmentHelper){
		fprintf(f, "\n// Call %s_ApplyPlayerAttachments(playerid) from OnPlayerSpawn.\n"
			"stock %s_ApplyPlayerAttachments(playerid)\n{\n", prefix, prefix);
		for(ObjectInst *inst : playerAttachments){
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			ObjectInst *parent = properties->attachParent >= 0 ? GetInstanceByID(properties->attachParent) : nil;
			if(parent && GetSampElementKind(parent) != SAMP_ELEMENT_CLASS)
				continue;
			if(parent){
				fprintf(f, "\tif(GetPlayerSkin(playerid) == %d){\n", GetObjectPawnProperties(parent)->elementModel);
				pawnWritePlayerAttachedObject(f, inst, "playerid");
				fputs("\t}\n", f);
			}else
				pawnWritePlayerAttachedObject(f, inst, "playerid");
		}
		fputs("}\n", f);
	}

	if(hasPlayerElements){
		fprintf(f, "\nstock %s_ShowForPlayer(playerid)\n{\n", prefix);
		if(nativeGameSounds)
			fprintf(f, "\tfor(new i = 0; i < sizeof %s_soundIn[]; i++)\n"
				"\t\t%s_soundIn[playerid][i] = false;\n", prefix, prefix);
		if(!playerZones.empty()){
			fprintf(f, "\tfor(new i = 0; i < %s_numPZones; i++){\n", prefix);
			fprintf(f, "\t\tnew zone = CreatePlayerGangZone(playerid, %s_pzoneAt[i][0], %s_pzoneAt[i][1],"
				" %s_pzoneAt[i][2], %s_pzoneAt[i][3]);\n", prefix, prefix, prefix, prefix);
			fprintf(f, "\t\tif(zone != INVALID_GANG_ZONE)\n\t\t\tPlayerGangZoneShow(playerid, zone, %s_pzoneColour[i]);\n"
				"\t}\n", prefix);
		}
		if(!streamedZones.empty())
			fprintf(f, "\t%s_ZoneReset(playerid);\n", prefix);
		for(size_t i = 0; i < gangZones.size(); i++){
			const ObjectPawnProperties *properties = GetObjectPawnProperties(gangZones[i]);
			fprintf(f, "\tGangZoneShowForPlayer(playerid, %s, 0x%08X);",
				handles[gangZones[i]].c_str(), properties->zoneColor);
			pawnWriteTrailingTags(f, "gangzone", "", properties);
		}
		for(size_t i = 0; i < nativeMapIcons.size(); i++){
			ObjectInst *inst = nativeMapIcons[i];
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			fprintf(f, "\tSetPlayerMapIcon(playerid, %d, %.6f, %.6f, %.6f, %d, 0x%08X, ",
				(int)i, inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
				properties->mapIconType, properties->mapIconColor);
			pawnWriteMapIconStyle(f, properties->mapIconStyle);
			fputs(");", f);
			pawnWriteTrailingTags(f, "mapicon", " [map icon]", properties);
		}
		for(size_t i = 0; i < audioStreams.size(); i++){
			ObjectInst *inst = audioStreams[i];
			const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
			fputs("\tPlayAudioStreamForPlayer(playerid, ", f);
			pawnWriteQuotedString(f, properties->soundUrl);
			fprintf(f, ", %.6f, %.6f, %.6f, %.2f, %s);", inst->m_translation.x, inst->m_translation.y,
				inst->m_translation.z, properties->soundRadius, properties->soundUsePos ? "true" : "false");
			pawnWriteTrailingTags(f, "sound", "", properties);
		}
		fprintf(f, "}\n");
	}
	if(!selectedOnly && HasTextDraws()){
		fputc('\n', f);
		WriteTextDrawsPawnFunctions(f, prefix);
	}
	return (int)insts.size();
}

// Moves src over dst in one step, replacing dst.
static bool
replaceFile(const char *src, const char *dst)
{
#ifdef _WIN32
	return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(src, dst) == 0;
#endif
}

int
ExportPawnCode(const char *path, PawnCodeFormat format, bool selectedOnly, int *numSkipped)
{
	if(numSkipped)
		*numSkipped = 0;
	if(path == nil || path[0] == '\0' ||
	   (format != PAWN_CODE_NATIVE && format != PAWN_CODE_STREAMER))
		return -1;
	if(!isSA()){
		log("ExportPawnCode: Pawn object code is only supported for GTA San Andreas\n");
		return -1;
	}
	if(!EnsureParentDirectoriesForPath(path)){
		log("ExportPawnCode: failed to create parent directories for %s: %s\n", path, strerror(errno));
		return -1;
	}
	// The recording player moves elements without marking them; put them back first.
	RecPlayerStopAll();
	NpcPreviewStopAll();
	std::string error;
	if(!CanSaveEditorDocument(path,"map",&error)){
		log("ExportPawnCode: %s\n",error.c_str());return -1;
	}
	BeginMapXMLExport();
	std::string code;
	int count=BuildPawnCode(path,format,selectedOnly,code,numSkipped);
	EditorXML::Node document=FinishMapXMLExport();
	if(count<0)return -1;
	code=EncodePawnFileText(code,GetPawnTextEncoding());
	if(!ValidateEditorDocument(document,&error)||!EditorXML::WriteDocument(path,code,std::move(document),&error)){
		log("ExportPawnCode: %s\n",error.c_str());return -1;
	}
	log("ExportPawnCode: wrote %d object(s) to %s\n", count, path);
	return count;
}

int
BuildPawnCode(const char *pathHint, PawnCodeFormat format, bool selectedOnly,
	std::string &code, int *numSkipped)
{
	code.clear();
	if(numSkipped)
		*numSkipped = 0;
	if(format != PAWN_CODE_NATIVE && format != PAWN_CODE_STREAMER)
		return -1;
	if(!isSA())
		return -1;
	FILE *f = OpenTemporaryCodeStream();
	if(f == nil)
		return -1;
	const char *hint = pathHint && pathHint[0] ? pathHint : "haumea-map.pwn";
	int count = pawnWriteCode(f, hint, format, selectedOnly, numSkipped);
	if(fflush(f) != 0 || ferror(f) || fseek(f, 0, SEEK_END) != 0){
		fclose(f);
		return -1;
	}
	long size = ftell(f);
	if(size < 0 || fseek(f, 0, SEEK_SET) != 0){
		fclose(f);
		return -1;
	}
	code.resize((size_t)size);
	if(size > 0 && fread(&code[0], 1, (size_t)size, f) != (size_t)size){
		code.clear();
		fclose(f);
		return -1;
	}
	fclose(f);
	return count;
}

struct PawnImportEntry
{
	int objectId;
	float x, y, z;
	float rx, ry, rz;
	double sourceDegrees[3];	// rx, ry, rz as written (objects only)
	int interior;
	bool isVehicle;
	bool vehicleStatic;
	int vehicleColor1;
	int vehicleColor2;
	int vehicleRespawnDelay;
	char handle[64];
	// SA-MP object properties
	float drawDistance;
	float streamDistance;
	int world;
	char group[PAWN_OBJECT_GROUP_LEN];
	char note[PAWN_OBJECT_COMMENT_LEN];
	bool radarArt;	// "// radar art": a zone of a radar art layer
	// SA-MP elements: kind and settings, applied over the fields above.
	int elementKind;
	ObjectPawnProperties element;
	// CreateDynamicRaceCP's next point, to find the race it belongs to.
	float cpNext[3];
	// NPC_PutInVehicle: the vehicle's create entry plus one; 0 on foot, so a
	// zeroed entry is on foot.
	int npcVehicleEntry;
	// A streamer item's areaid: the dynamic area's create entry plus one (the
	// latest area with that variable before the item), 0 for none.
	int areaEntry;
};

static std::string pawnTrimString(const std::string &value);
static bool pawnCharIsIdentifier(char c);

// A streamer item's areaid: the argument at index (or a named ".areaid = x"),
// when it names a dynamic area created before it: that entry plus one, else 0.
// A name used for several areas means the latest one, as in the script.
static int
pawnAreaEntryFor(const std::vector<std::string> &args, int index,
	const std::map<std::string, int> &handleEntries, const std::vector<PawnImportEntry> &entries)
{
	std::string value;
	for(const std::string &a : args){
		std::string t = pawnTrimString(a);
		if(t.compare(0, 7, ".areaid") == 0 && t.find('=') != std::string::npos){
			value = pawnTrimString(t.substr(t.find('=') + 1));
			break;
		}
	}
	if(value.empty() && index >= 0 && index < (int)args.size() && pawnTrimString(args[index])[0] != '.')
		value = pawnTrimString(args[index]);
	size_t tag = value.find(':');	// STREAMER_TAG_AREA:areaid
	if(tag != std::string::npos)
		value = pawnTrimString(value.substr(tag + 1));
	if(value.empty() || !(isalpha((unsigned char)value[0]) || value[0] == '_'))
		return 0;
	for(char c : value)
		if(!pawnCharIsIdentifier(c))
			return 0;
	std::map<std::string, int>::const_iterator it = handleEntries.find(value);
	if(it == handleEntries.end() || it->second < 0 || it->second >= (int)entries.size() ||
	   entries[it->second].elementKind != SAMP_ELEMENT_AREA)
		return 0;
	return it->second + 1;
}

// Calls that change an element after it was created, by its variable:
// SetActorVirtualWorld/Invulnerable/Health and GangZoneShowFor*'s colour.
struct PawnElementSetterEntry
{
	int entryIndex;
	enum { WORLD, INVULNERABLE, HEALTH, ZONE_COLOR } field;
	float value;
	uint32 color;
};

struct PawnAttachImportEntry
{
	int child, parent;	// indices into the create entries; player uses PAWN_ATTACH_PLAYER
	float x, y, z, rx, ry, rz;
	bool syncRotation;
	bool toVehicle;
	int bone, slot;
	rw::V3d scale;
	uint32 color1, color2;
};

struct PawnRemoveEntry
{
	int objectId;
	float x, y, z;
	float radius;
};

// Calls on a vehicle's handle: LinkVehicleToInterior, ChangeVehiclePaintjob,
// AddVehicleComponent and SetVehicleNumberPlate.
struct PawnVehicleInteriorEntry
{
	char target[64];
	int entryIndex;
	enum { INTERIOR, PAINTJOB, COMPONENT, PLATE } field;
	int interior;	// or the paintjob or component
	char plate[VEHICLE_PLATE_LEN];
};

struct PawnMaterialImportEntry
{
	int entryIndex;	// the object the handle referred to at that point
	int slot;
	bool hasMaterial;
	bool hasText;
	bool textIsLast;	// the client keeps whichever call came last
	ObjectMaterialOverride material;
};

static bool
pawnCharIsIdentifier(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static bool
pawnNameEqualsAt(const std::string &source, size_t offset, const char *name)
{
	size_t len = strlen(name);
	if(offset + len > source.size() ||
	   (offset > 0 && pawnCharIsIdentifier(source[offset-1])) ||
	   (offset + len < source.size() && pawnCharIsIdentifier(source[offset+len])))
		return false;
	for(size_t i = 0; i < len; i++){
		char a = source[offset+i];
		char b = name[i];
		if(a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
		if(b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
		if(a != b)
			return false;
	}
	return true;
}

static std::string
pawnTrimString(const std::string &value)
{
	size_t first = 0;
	while(first < value.size() && isspace((unsigned char)value[first])) first++;
	size_t last = value.size();
	while(last > first && isspace((unsigned char)value[last-1])) last--;
	return value.substr(first, last-first);
}

static bool
pawnParseDouble(const std::string &value, double *out)
{
	std::string token = pawnTrimString(value);
	if(token.size() >= 5 &&
	   (token[0] == 'F' || token[0] == 'f') &&
	   (token[1] == 'L' || token[1] == 'l') &&
	   (token[2] == 'O' || token[2] == 'o') &&
	   (token[3] == 'A' || token[3] == 'a') && token[4] == ':')
		token = pawnTrimString(token.substr(5));
	while(token.size() >= 2 && token[0] == '(' && token[token.size()-1] == ')'){
		int depth = 0;
		bool enclosesAll = true;
		for(size_t i = 0; i < token.size(); i++){
			if(token[i] == '(') depth++;
			else if(token[i] == ')' && --depth == 0 && i + 1 < token.size()){
				enclosesAll = false;
				break;
			}
		}
		if(!enclosesAll || depth != 0)
			break;
		token = pawnTrimString(token.substr(1, token.size()-2));
	}
	if(token.empty())
		return false;
	char *end = nil;
	const char *start = token.c_str();
	double parsed = strtod(start, &end);
	if(end == start || !std::isfinite((float)parsed))
		return false;
	while(*end && isspace((unsigned char)*end)) end++;
	if(*end != '\0')
		return false;
	if(out) *out = parsed;
	return true;
}

static bool
pawnParseFloat(const std::string &value, float *out)
{
	double parsed;
	if(!pawnParseDouble(value, &parsed))
		return false;
	if(out) *out = (float)parsed;
	return true;
}

static bool
pawnParseInt(const std::string &value, int *out)
{
	std::string token = pawnTrimString(value);
	if(!token.empty()){
		char *end = nil;
		long parsed = strtol(token.c_str(), &end, 0);
		while(*end && isspace((unsigned char)*end)) end++;
		if(end != token.c_str() && *end == '\0' &&
		   parsed >= LONG_MIN && parsed <= LONG_MAX){
			if(parsed >= INT_MIN && parsed <= INT_MAX){
				if(out) *out = (int)parsed;
				return true;
			}
			return false;
		}
	}
	float parsed;
	if(!pawnParseFloat(value, &parsed) || parsed < (float)INT_MIN || parsed > (float)INT_MAX)
		return false;
	float rounded = parsed >= 0.0f ? floorf(parsed + 0.5f) : ceilf(parsed - 0.5f);
	if(fabsf(parsed - rounded) > 0.0001f)
		return false;
	if(out) *out = (int)rounded;
	return true;
}

static bool
pawnParseUInt32(const std::string &value, uint32 *out)
{
	std::string token = pawnTrimString(value);
	if(token.empty())
		return false;
	char *end = nil;
	long long signedValue = strtoll(token.c_str(), &end, 0);
	while(*end && isspace((unsigned char)*end)) end++;
	if(end != token.c_str() && *end == '\0'){
		if(out) *out = (uint32)signedValue;
		return true;
	}
	return false;
}

// Drops a leading Pawn tag such as "OBJECT_MATERIAL_SIZE:" or "bool:".
static std::string
pawnStripTag(const std::string &value)
{
	std::string token = pawnTrimString(value);
	size_t colon = token.find(':');
	if(colon != std::string::npos && colon > 0){
		bool identifier = true;
		for(size_t i = 0; i < colon; i++)
			if(!isalnum((unsigned char)token[i]) && token[i] != '_')
				identifier = false;
		if(identifier)
			return pawnTrimString(token.substr(colon + 1));
	}
	return token;
}

// Integers in material calls, including the named constants scripts use.
static bool
pawnParseMaterialInt(const std::string &value, int *out)
{
	std::string token = pawnStripTag(value);
	if(pawnParseInt(token, out))
		return true;
	std::string upper = token;
	for(size_t i = 0; i < upper.size(); i++)
		upper[i] = (char)toupper((unsigned char)upper[i]);
	if(upper == "TRUE"){ *out = 1; return true; }
	if(upper == "FALSE"){ *out = 0; return true; }
	if(upper == "OBJECT_MATERIAL_TEXT_ALIGN_LEFT"){ *out = 0; return true; }
	if(upper == "OBJECT_MATERIAL_TEXT_ALIGN_CENTER" ||
	   upper == "OBJECT_MATERIAL_TEXT_ALIGN_CENTRE" ||
	   upper == "OBJECT_MATERIAL_TEXT_ALIGN_CENT"){ *out = 1; return true; }
	if(upper == "OBJECT_MATERIAL_TEXT_ALIGN_RIGHT"){ *out = 2; return true; }
	int width, height;
	char tail;
	if(sscanf(upper.c_str(), "OBJECT_MATERIAL_SIZE_%dX%d%c", &width, &height, &tail) == 2){
		for(int code = 10; code <= 140; code += 10){
			int w, h;
			if(GetMaterialTextDimensions(code, &w, &h) && w == width && h == height){
				*out = code;
				return true;
			}
		}
	}
	return false;
}

static bool
pawnParseMaterialColor(const std::string &value, uint32 *out)
{
	return pawnParseUInt32(pawnStripTag(value), out);
}

// String #defines the importer resolves: the map's own, then the workspace's
// sources and includes, where a gamemode keeps its colour macros
// (#define LIGHTRED "{FF6347}"). Names it can't find are listed.
static std::map<std::string, std::string> gPawnStringDefines;
static std::vector<std::string> gPawnUnresolvedMacros;

static bool
pawnIsIdentifierStart(char c)
{
	return isalpha((unsigned char)c) || c == '_' || c == '@';
}

static bool
pawnIsIdentifierChar(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '@';
}

// "#define NAME value" lines whose value is a string expression (quotes or
// names); a name already known is kept, so the map's own wins.
static void
pawnCollectStringDefines(const std::string &source, std::map<std::string, std::string> &out)
{
	size_t start = 0;
	while(start < source.size()){
		size_t end = source.find('\n', start);
		if(end == std::string::npos)
			end = source.size();
		std::string line = pawnTrimString(source.substr(start, end - start));
		start = end + 1;
		if(line.compare(0, 7, "#define") != 0 || line.size() < 9 || !isspace((unsigned char)line[7]))
			continue;
		size_t i = 7;
		while(i < line.size() && isspace((unsigned char)line[i])) i++;
		size_t nameStart = i;
		if(i >= line.size() || !pawnIsIdentifierStart(line[i]))
			continue;
		while(i < line.size() && pawnIsIdentifierChar(line[i])) i++;
		std::string name = line.substr(nameStart, i - nameStart);
		std::string value = line.substr(i);
		size_t comment = value.find("//");
		if(comment != std::string::npos && value.find('"') > comment)
			value.resize(comment);
		value = pawnTrimString(value);
		if(!value.empty() && (value[0] == '"' || pawnIsIdentifierStart(value[0])) && !out.count(name))
			out[name] = value;
	}
}

static void
pawnCollectFileDefines(const std::string &path, std::map<std::string, std::string> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return;
	std::string text;
	char buffer[65536];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), f)) > 0 && text.size() < 4u * 1024 * 1024)
		text.append(buffer, n);
	fclose(f);
	pawnCollectStringDefines(text, out);
}

static bool
pawnIsSourceFile(const std::string &name)
{
	if(name.size() < 5)
		return false;
	std::string ext = name.substr(name.size() - 4);
	for(char &c : ext)
		c = (char)tolower((unsigned char)c);
	return ext == ".pwn" || ext == ".inc";
}

// The workspace's gamemodes, filterscripts and include folders, read once
// per workspace.
static const std::map<std::string, std::string>&
pawnWorkspaceDefines(void)
{
	static std::string forWorkspace;
	static std::map<std::string, std::string> defines;
	std::string workspace = GetWorkspaceDir();
	if(workspace == forWorkspace)
		return defines;
	forWorkspace = workspace;
	defines.clear();
	if(workspace.empty())
		return defines;
	static const char *folders[] = { "gamemodes", "filterscripts", "include", "pawno/include", "qawno/include" };
	int budget = 400;
	for(const char *folder : folders){
		std::string dir = workspace + "/" + folder;
#ifdef _WIN32
		WIN32_FIND_DATAA entry;
		HANDLE handle = FindFirstFileA((dir + "\\*").c_str(), &entry);
		if(handle == INVALID_HANDLE_VALUE)
			continue;
		do{
			std::string name = entry.cFileName;
			if(budget > 0 && name.size() > 4 && pawnIsSourceFile(name)){
				budget--;
				pawnCollectFileDefines(dir + "/" + name, defines);
			}
		}while(FindNextFileA(handle, &entry));
		FindClose(handle);
#else
		DIR *d = opendir(dir.c_str());
		if(d == nil)
			continue;
		while(dirent *ent = readdir(d)){
			std::string name = ent->d_name;
			if(budget > 0 && name.size() > 4 && pawnIsSourceFile(name)){
				budget--;
				pawnCollectFileDefines(dir + "/" + name, defines);
			}
		}
		closedir(d);
#endif
	}
	return defines;
}

// A string expression: literals side by side (Pawn joins them) and names
// that expand to more of the same. Unknown names are left out and listed;
// at least one literal is needed, so a plain variable still isn't text.
static bool
pawnExpandStringExpression(const std::string &expr, std::string &out, int depth, bool *sawLiteral)
{
	size_t i = 0, n = expr.size();
	bool any = false;
	while(i < n){
		char c = expr[i];
		if(isspace((unsigned char)c)){
			i++;
			continue;
		}
		if(c == '"'){
			bool closed = false;
			for(i++; i < n; i++){
				char d = expr[i];
				if(d == '"'){
					closed = true;
					i++;
					break;
				}
				if(d == '\\' && i + 1 < n){
					char escaped = expr[++i];
					d = escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped == 't' ? '\t' : escaped;
				}
				out += d;
			}
			if(!closed)
				return false;
			*sawLiteral = true;
			any = true;
			continue;
		}
		if(!pawnIsIdentifierStart(c))
			return false;
		size_t start = i;
		while(i < n && pawnIsIdentifierChar(expr[i])) i++;
		std::string name = expr.substr(start, i - start);
		std::map<std::string, std::string>::const_iterator it = gPawnStringDefines.find(name);
		if(it == gPawnStringDefines.end()){
			const std::map<std::string, std::string> &workspace = pawnWorkspaceDefines();
			it = workspace.find(name);
			if(it == workspace.end()){
				if(std::find(gPawnUnresolvedMacros.begin(), gPawnUnresolvedMacros.end(), name) == gPawnUnresolvedMacros.end())
					gPawnUnresolvedMacros.push_back(name);
				any = true;
				continue;
			}
		}
		if(depth > 8 || !pawnExpandStringExpression(it->second, out, depth + 1, sawLiteral))
			return false;
		any = true;
	}
	return any;
}

static bool
pawnParseStringExpression(const std::string &token, char *out, size_t outSize)
{
	std::string text;
	bool sawLiteral = false;
	size_t unresolvedBefore = gPawnUnresolvedMacros.size();
	if(!pawnExpandStringExpression(token, text, 0, &sawLiteral) || !sawLiteral){
		gPawnUnresolvedMacros.resize(unresolvedBefore);	// not text after all
		return false;
	}
	if(out && outSize > 0)
		snprintf(out, outSize, "%s", text.c_str());
	return true;
}

static bool
pawnParseString(const std::string &value, char *out, size_t outSize)
{
	std::string token = pawnTrimString(value);
	if(token.size() < 2 || token[0] != '"')
		return false;
	size_t write = 0;
	bool closed = false;
	for(size_t i = 1; i < token.size(); i++){
		char c = token[i];
		if(c == '"'){
			// More after the closing quote: a joined or macro string.
			if(i + 1 < token.size() && !pawnTrimString(token.substr(i+1)).empty())
				return pawnParseStringExpression(token, out, outSize);
			closed = true;
			break;
		}
		if(c == '\\' && i + 1 < token.size()){
			char escaped = token[++i];
			switch(escaped){
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case '\\': c = '\\'; break;
			case '"': c = '"'; break;
			default: c = escaped; break;
			}
		}
		if(out && outSize > 1 && write + 1 < outSize)
			out[write++] = c;
	}
	if(!closed)
		return pawnParseStringExpression(token, out, outSize);
	if(out && outSize > 0)
		out[write < outSize ? write : outSize-1] = '\0';
	return true;
}

static bool
pawnParseToken(const std::string &value, char *out, size_t outSize)
{
	std::string token = pawnTrimString(value);
	if(token.empty() || (outSize == 0 && out != nil))
		return false;
	if(out && outSize > 0){
		strncpy(out, token.c_str(), outSize-1);
		out[outSize-1] = '\0';
	}
	return true;
}

static std::string
pawnStripComments(const std::string &source)
{
	std::string out;
	bool inString = false;
	bool escaped = false;
	for(size_t i = 0; i < source.size(); i++){
		char c = source[i];
		if(inString){
			out.push_back(c);
			if(escaped)
				escaped = false;
			else if(c == '\\')
				escaped = true;
			else if(c == '"')
				inString = false;
			continue;
		}
		if(c == '"'){
			inString = true;
			out.push_back(c);
		}else if(c == '/' && i + 1 < source.size() && source[i+1] == '/'){
			i++;
			while(i + 1 < source.size() && source[i+1] != '\n'){
				i++;
				out.push_back(' ');
			}
			if(i + 1 < source.size())
				out.push_back('\n');
		}else if(c == '/' && i + 1 < source.size() && source[i+1] == '*'){
			i += 2;
			while(i < source.size()){
				if(source[i] == '\n') out.push_back('\n');
				else out.push_back(' ');
				if(source[i] == '*' && i + 1 < source.size() && source[i+1] == '/'){
					out.push_back(' ');
					i++;
					break;
				}
				i++;
			}
		}else
			out.push_back(c);
	}
	return out;
}

static bool
pawnPositionInString(const std::string &source, size_t position)
{
	bool inString = false;
	bool escaped = false;
	for(size_t i = 0; i < position; i++){
		char c = source[i];
		if(inString){
			if(escaped) escaped = false;
			else if(c == '\\') escaped = true;
			else if(c == '"') inString = false;
		}else if(c == '"'){
			inString = true;
		}
	}
	return inString;
}

// pawnPositionInString for positions that only move forward: scans each
// character once instead of from the start every time.
struct PawnStringScanner
{
	size_t at = 0;
	bool inString = false, escaped = false;

	bool inStringAt(const std::string &source, size_t position)
	{
		if(position < at){
			at = 0;
			inString = escaped = false;
		}
		for(; at < position; at++){
			char c = source[at];
			if(inString){
				if(escaped) escaped = false;
				else if(c == '\\') escaped = true;
				else if(c == '"') inString = false;
			}else if(c == '"')
				inString = true;
		}
		return inString;
	}
};

static void
pawnCaptureAssignment(const std::string &source, size_t callPosition, char *out, size_t outSize)
{
	if(out == nil || outSize == 0)
		return;
	out[0] = '\0';
	size_t begin = callPosition;
	while(begin > 0 && source[begin-1] != ';' && source[begin-1] != '\n' && source[begin-1] != '{')
		begin--;
	std::string lhs = pawnTrimString(source.substr(begin, callPosition-begin));
	size_t equals = lhs.rfind('=');
	if(equals == std::string::npos)
		return;
	lhs = pawnTrimString(lhs.substr(0, equals));
	// new, static and const in any order: "new const areaid = ...", "static x = ..."
	for(bool again = true; again;){
		again = false;
		for(const char *word : { "new ", "static ", "const " })
			if(lhs.compare(0, strlen(word), word) == 0){
				lhs = pawnTrimString(lhs.substr(strlen(word)));
				again = true;
			}
	}
	size_t tag = lhs.find(':');	// new STREAMER_TAG_OBJECT:name = ...
	if(tag != std::string::npos)
		lhs = pawnTrimString(lhs.substr(tag + 1));
	if(lhs.empty())
		return;
	for(size_t i = 0; i < lhs.size(); i++)
		if(!pawnCharIsIdentifier(lhs[i]) || (i == 0 && isdigit((unsigned char)lhs[i])))
			return;
	strncpy(out, lhs.c_str(), outSize-1);
	out[outSize-1] = '\0';
}

// CreateDynamicPolygon's points: a literal "Float:{x, y, ...}", or an array
// variable declared with one before the call. Returns x, y pairs.
static bool
pawnParsePolygonPoints(const std::string &arg, const std::string &source, size_t before,
	std::vector<float> &out)
{
	out.clear();
	std::string text = pawnTrimString(pawnStripTag(arg));
	if(text.empty())
		return false;
	if(text[0] != '{'){
		// An array variable: its last "name[...] = {...}" before the call.
		std::string name = text;
		for(char c : name)
			if(!pawnCharIsIdentifier(c))
				return false;
		size_t at = std::string::npos;
		for(size_t p = source.rfind(name, before); p != std::string::npos && p > 0; p = source.rfind(name, p - 1)){
			size_t q = p + name.size();
			while(q < source.size() && isspace((unsigned char)source[q])) q++;
			if((p == 0 || !pawnCharIsIdentifier(source[p-1])) && q < source.size() && source[q] == '['){
				at = q;
				break;
			}
			if(p == 0)
				break;
		}
		if(at == std::string::npos)
			return false;
		size_t open = source.find('{', at), close = open == std::string::npos ? open : source.find('}', open);
		if(open == std::string::npos || close == std::string::npos || open > before)
			return false;
		text = source.substr(open, close - open + 1);
	}
	if(text.size() < 2 || text[0] != '{' || text[text.size()-1] != '}')
		return false;
	std::string body = text.substr(1, text.size() - 2);
	size_t start = 0;
	while(start <= body.size()){
		size_t comma = body.find(',', start);
		std::string part = pawnTrimString(body.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if(!part.empty()){
			float value;
			if(!pawnParseFloat(pawnStripTag(part), &value))
				return false;
			out.push_back(value);
		}
		if(comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out.size() >= 6 && out.size() % 2 == 0;
}

static PawnMaterialImportEntry*
pawnFindMaterialImport(std::vector<PawnMaterialImportEntry> &materials,
	int entryIndex, int slot)
{
	for(size_t i = 0; i < materials.size(); i++)
		if(materials[i].slot == slot && materials[i].entryIndex == entryIndex)
			return &materials[i];
	PawnMaterialImportEntry entry;
	memset(&entry, 0, sizeof(entry));
	entry.entryIndex = entryIndex;
	entry.slot = slot;
	InitObjectMaterialOverride(&entry.material, -1);
	materials.push_back(entry);
	return &materials.back();
}

static bool
pawnSplitArguments(const std::string &source, size_t begin, size_t end,
	std::vector<std::string> &args)
{
	int parentheses = 0, brackets = 0, braces = 0;
	bool inString = false, escaped = false;
	size_t argumentBegin = begin;
	for(size_t i = begin; i < end; i++){
		char c = source[i];
		// Commas and brackets inside strings ("$25,000", "{33CC33}") are text.
		if(inString){
			if(escaped) escaped = false;
			else if(c == '\\') escaped = true;
			else if(c == '"') inString = false;
			continue;
		}
		if(c == '"') inString = true;
		else if(c == '(') parentheses++;
		else if(c == ')' && parentheses > 0) parentheses--;
		else if(c == '[') brackets++;
		else if(c == ']' && brackets > 0) brackets--;
		else if(c == '{') braces++;
		else if(c == '}' && braces > 0) braces--;
		else if(c == ',' && parentheses == 0 && brackets == 0 && braces == 0){
			args.push_back(pawnTrimString(source.substr(argumentBegin, i-argumentBegin)));
			argumentBegin = i + 1;
		}
	}
	args.push_back(pawnTrimString(source.substr(argumentBegin, end-argumentBegin)));
	return parentheses == 0 && brackets == 0 && braces == 0;
}

static bool
pawnParseCall(const std::string &source, size_t openParen, size_t *afterCall,
	std::vector<std::string> &args)
{
	int depth = 1;
	bool inString = false;
	bool escaped = false;
	int brackets = 0, braces = 0;
	for(size_t i = openParen + 1; i < source.size(); i++){
		char c = source[i];
		if(inString){
			if(escaped) escaped = false;
			else if(c == '\\') escaped = true;
			else if(c == '"') inString = false;
			continue;
		}
		if(c == '"') inString = true;
		else if(c == '(') depth++;
		else if(c == ')'){
			depth--;
			if(depth == 0){
				if(!pawnSplitArguments(source, openParen + 1, i, args))
					return false;
				if(afterCall) *afterCall = i + 1;
				return true;
			}
		}else if(c == '[') brackets++;
		else if(c == ']' && brackets > 0) brackets--;
		else if(c == '{') braces++;
		else if(c == '}' && braces > 0) braces--;
	}
	return false;
}

// Reads Haumea's metadata from the comment after an object call:
//   CreateObject(...); // model_name @group("Bridge") @note("text")
static void
pawnReadObjectTags(const std::string &rawSource, size_t afterCall, PawnImportEntry *entry)
{
	size_t end = rawSource.find('\n', afterCall);
	std::string line = rawSource.substr(afterCall,
		end == std::string::npos ? std::string::npos : end - afterCall);
	size_t comment = line.find("//");
	if(comment == std::string::npos)
		return;
	line = line.substr(comment + 2);
	size_t word = line.find_first_not_of(" \t");
	entry->radarArt = word != std::string::npos && line.compare(word, 9, "radar art") == 0;
	struct Tag { const char *name; char *out; size_t size; };
	Tag tags[] = {
		{ "@group(", entry->group, sizeof(entry->group) },
		{ "@note(", entry->note, sizeof(entry->note) },
		{ "@race(", entry->element.cpRace, sizeof(entry->element.cpRace) },
	};
	for(size_t i = 0; i < sizeof(tags)/sizeof(tags[0]); i++){
		size_t at = line.find(tags[i].name);
		if(at == std::string::npos)
			continue;
		size_t open = at + strlen(tags[i].name);
		if(open >= line.size() || line[open] != '"')
			continue;
		size_t close = open + 1;
		while(close < line.size() && line[close] != '"')
			close += line[close] == '\\' ? 2 : 1;
		if(close >= line.size())
			continue;
		pawnParseString(line.substr(open, close - open + 1), tags[i].out, tags[i].size);
	}
}

// Stream/draw distances, accepting the streamer's default constants.
static bool
pawnParseDistance(const std::string &value, float *out)
{
	std::string token = pawnStripTag(value);
	// STREAMER_OBJECT_SD, STREAMER_PICKUP_SD, STREAMER_3D_TEXT_LABEL_SD, ...
	if(token.compare(0, 9, "STREAMER_") == 0 && token.size() > 12 &&
	   (token.compare(token.size()-3, 3, "_SD") == 0 || token.compare(token.size()-3, 3, "_DD") == 0)){
		*out = 0.0f;	// the editor's "default"
		return true;
	}
	return pawnParseFloat(token, out);
}

// Integers in element calls: numbers, true/false and the a_samp constants
// those calls take.
static bool
pawnParseElementInt(const std::string &value, int *out)
{
	if(pawnParseMaterialInt(value, out))
		return true;
	std::string token = pawnStripTag(value);
	static const struct { const char *name; int value; } constants[] = {
		{ "MAPICON_LOCAL", 0 }, { "MAPICON_GLOBAL", 1 },
		{ "MAPICON_LOCAL_CHECKPOINT", 2 }, { "MAPICON_GLOBAL_CHECKPOINT", 3 },
		{ "INVALID_PLAYER_ID", 0xFFFF }, { "INVALID_VEHICLE_ID", 0xFFFF },
		{ "BONE_SPINE", 1 }, { "BONE_HEAD", 2 },
		{ "BONE_LEFT_UPPER_ARM", 3 }, { "BONE_RIGHT_UPPER_ARM", 4 },
		{ "BONE_LEFT_HAND", 5 }, { "BONE_RIGHT_HAND", 6 },
		{ "BONE_LEFT_THIGH", 7 }, { "BONE_RIGHT_THIGH", 8 },
		{ "BONE_LEFT_FOOT", 9 }, { "BONE_RIGHT_FOOT", 10 },
		{ "BONE_RIGHT_CALF", 11 }, { "BONE_LEFT_CALF", 12 },
		{ "BONE_LEFT_FOREARM", 13 }, { "BONE_RIGHT_FOREARM", 14 },
		{ "BONE_LEFT_CLAVICLE", 15 }, { "BONE_RIGHT_CLAVICLE", 16 },
		{ "BONE_NECK", 17 }, { "BONE_JAW", 18 },
		{ "NO_TEAM", 255 },
	};
	for(size_t i = 0; i < sizeof(constants)/sizeof(constants[0]); i++)
		if(token == constants[i].name){
			*out = constants[i].value;
			return true;
		}
	for(size_t i = 0; i < sizeof(pawnWeaponNames)/sizeof(pawnWeaponNames[0]); i++)
		if(token == pawnWeaponNames[i].name){
			*out = pawnWeaponNames[i].value;
			return true;
		}
	return false;
}

// NPC_MOVE_TYPE_* by name or number; jog, the natives' default, otherwise.
static int
pawnParseMoveType(const std::string &value)
{
	static const char *moveTypes[] = {
		"NPC_MOVE_TYPE_NONE", "NPC_MOVE_TYPE_WALK", "NPC_MOVE_TYPE_JOG",
		"NPC_MOVE_TYPE_SPRINT", "NPC_MOVE_TYPE_DRIVE", "NPC_MOVE_TYPE_AUTO",
	};
	std::string token = pawnStripTag(value);
	for(int t = 0; t < 6; t++)
		if(token == moveTypes[t])
			return t;
	int moveType;
	return pawnParseElementInt(token, &moveType) ? moveType : 2;
}

// NPC_MOVE_SPEED_* by name or a speed; -1 (auto) otherwise.
static float
pawnParseMoveSpeed(const std::string &value)
{
	static const struct { const char *name; float speed; } speeds[] = {
		{ "NPC_MOVE_SPEED_AUTO", -1.0f }, { "NPC_MOVE_SPEED_WALK", 0.1552086f },
		{ "NPC_MOVE_SPEED_JOG", 0.56444f }, { "NPC_MOVE_SPEED_SPRINT", 0.926784f },
	};
	std::string token = pawnStripTag(value);
	for(size_t t = 0; t < sizeof(speeds)/sizeof(speeds[0]); t++)
		if(token == speeds[t].name)
			return speeds[t].speed;
	float speed;
	return pawnParseFloat(token, &speed) ? speed : -1.0f;
}

// Race checkpoints: Haumea tags each with @race("name") and writes a race
// in order. Hand-written ones are chained by their next points instead: a
// checkpoint no other one points at starts a race.
static void
pawnLinkRaceCheckpoints(std::vector<PawnImportEntry> &entries)
{
	std::map<std::string, int> counts;
	std::vector<int> untagged;
	for(size_t i = 0; i < entries.size(); i++){
		ObjectPawnProperties &el = entries[i].element;
		if(entries[i].elementKind != SAMP_ELEMENT_CHECKPOINT || el.cpKind != SAMP_CHECKPOINT_RACE)
			continue;
		if(el.cpRace[0] && IsValidPawnObjectName(el.cpRace))
			el.cpOrder = counts[el.cpRace]++;
		else
			untagged.push_back((int)i);
	}
	size_t n = untagged.size();
	std::vector<int> next(n, -1);
	std::vector<bool> pointedAt(n, false), done(n, false);
	for(size_t a = 0; a < n; a++){
		const PawnImportEntry &from = entries[untagged[a]];
		for(size_t b = 0; b < n; b++){
			const PawnImportEntry &to = entries[untagged[b]];
			if(a != b && fabsf(from.cpNext[0] - to.x) < 0.01f && fabsf(from.cpNext[1] - to.y) < 0.01f &&
			   fabsf(from.cpNext[2] - to.z) < 0.01f){
				next[a] = (int)b;
				pointedAt[b] = true;
				break;
			}
		}
	}
	int raceNumber = 1;
	// Starts first, then whatever is left (loops).
	for(int pass = 0; pass < 2; pass++)
		for(size_t a = 0; a < n; a++){
			if(done[a] || (pass == 0 && pointedAt[a]))
				continue;
			char name[PAWN_OBJECT_NAME_LEN];
			do
				snprintf(name, sizeof(name), "race%d", raceNumber++);
			while(counts.count(name));
			counts[name] = 0;
			for(int c = (int)a; c >= 0 && !done[c]; c = next[c]){
				done[c] = true;
				ObjectPawnProperties &el = entries[untagged[c]].element;
				snprintf(el.cpRace, sizeof(el.cpRace), "%s", name);
				el.cpOrder = counts[name]++;
			}
		}
}

static void
pawnParseSource(const std::string &rawSource, std::vector<PawnImportEntry> &entries,
	std::vector<PawnRemoveEntry> &removals,
	std::vector<PawnVehicleInteriorEntry> &vehicleInteriors,
	std::vector<PawnMaterialImportEntry> &materials,
	std::vector<PawnAttachImportEntry> &attachments,
	std::vector<PawnElementSetterEntry> &setters, int *numSkipped)
{
	const std::string source = pawnStripComments(rawSource);
	// Handles refer to whichever object was assigned to them last, so
	// Texture Studio's "tmpobjid = CreateObject(...)" chains resolve per object.
	std::map<std::string, int> handleEntries;
	std::map<std::string, int> npcPathPoints;	// points read so far, by path
	std::map<int, int> npcNodePoints;	// NPC_SetNodePoint so far, by node
	enum PawnCallKind {
		PAWN_CALL_CREATE,
		PAWN_CALL_CREATE_EX,
		PAWN_CALL_ATTACH,
		PAWN_CALL_ATTACH_VEHICLE,
		PAWN_CALL_PLAYER_ATTACHED,
		PAWN_CALL_VEHICLE_STATIC,
		PAWN_CALL_VEHICLE_STATIC_EX,
		PAWN_CALL_VEHICLE_DYNAMIC,
		PAWN_CALL_VEHICLE_INTERIOR,
		PAWN_CALL_VEHICLE_PAINTJOB,
		PAWN_CALL_VEHICLE_COMPONENT,
		PAWN_CALL_VEHICLE_PLATE,
		PAWN_CALL_REMOVE,
		PAWN_CALL_MATERIAL,
		PAWN_CALL_MATERIAL_TEXT,
		PAWN_CALL_PICKUP,
		PAWN_CALL_LABEL,
		PAWN_CALL_ACTOR,
		PAWN_CALL_ACTOR_WORLD,
		PAWN_CALL_ACTOR_INVULNERABLE,
		PAWN_CALL_ACTOR_HEALTH,
		PAWN_CALL_CLASS,
		PAWN_CALL_CLASS_EX,
		PAWN_CALL_GANGZONE,
		PAWN_CALL_GANGZONE_SHOW,
		PAWN_CALL_MAPICON,
		PAWN_CALL_CHECKPOINT,
		PAWN_CALL_RACE_CHECKPOINT,
		PAWN_CALL_AREA,	// CreateDynamicCircle .. CreateDynamicPolygon
		PAWN_CALL_NPC,
		PAWN_CALL_NPC_SPAWN,
		PAWN_CALL_NPC_SKIN,
		PAWN_CALL_NPC_POS,
		PAWN_CALL_NPC_ANGLE,
		PAWN_CALL_NPC_INTERIOR,
		PAWN_CALL_NPC_WORLD,
		PAWN_CALL_NPC_HEALTH,
		PAWN_CALL_NPC_ARMOUR,
		PAWN_CALL_NPC_WEAPON,
		PAWN_CALL_NPC_AMMO,
		PAWN_CALL_NPC_INVULNERABLE,
		PAWN_CALL_NPC_PATH,
		PAWN_CALL_NPC_PATH_POINT,
		PAWN_CALL_NPC_MOVE_BY_PATH,
		PAWN_CALL_NPC_START_PLAYBACK,
		PAWN_CALL_NPC_PUT_IN_VEHICLE,
		PAWN_CALL_NPC_OPEN_NODE,
		PAWN_CALL_NPC_SET_NODE_POINT,
		PAWN_CALL_NPC_PLAY_NODE,
		PAWN_CALL_ANIMATION,	// ApplyActorAnimation, ApplyDynamicActorAnimation, NPC_ApplyAnimation
		PAWN_CALL_AUDIO_STREAM,
		PAWN_CALL_ADD_SOUND,	// Haumea_<map>_AddSound, generated with the map
		PAWN_CALL_ADD_PLAYER_ZONE,	// Haumea_<map>_AddPlayerZone, generated with the map
		PAWN_CALL_ADD_STREAMED_ZONE,	// Haumea_<map>_AddStreamedZone, generated with the map
		PAWN_CALL_NPC_MOVE_REPEAT,	// Haumea_<map>_MoveByPathRepeat, generated with the map
	};
	struct PawnCallName { const char *name; int offset; bool streamer; PawnCallKind kind; };
	static const PawnCallName callNames[] = {
		{ "LinkVehicleToInterior", 0, false, PAWN_CALL_VEHICLE_INTERIOR },
		{ "ChangeVehiclePaintjob", 0, false, PAWN_CALL_VEHICLE_PAINTJOB },
		{ "AddVehicleComponent", 0, false, PAWN_CALL_VEHICLE_COMPONENT },
		{ "SetVehicleNumberPlate", 0, false, PAWN_CALL_VEHICLE_PLATE },
		{ "AddStaticVehicleEx", 0, false, PAWN_CALL_VEHICLE_STATIC_EX },
		{ "AddStaticVehicle", 0, false, PAWN_CALL_VEHICLE_STATIC },
		{ "CreateVehicle", 0, false, PAWN_CALL_VEHICLE_DYNAMIC },
		{ "SetPlayerObjectMaterialText", 1, false, PAWN_CALL_MATERIAL_TEXT },
		{ "SetDynamicObjectMaterialText", 0, true, PAWN_CALL_MATERIAL_TEXT },
		{ "SetObjectMaterialText", 0, false, PAWN_CALL_MATERIAL_TEXT },
		{ "SetPlayerObjectMaterial", 1, false, PAWN_CALL_MATERIAL },
		{ "SetDynamicObjectMaterial", 0, true, PAWN_CALL_MATERIAL },
		{ "SetObjectMaterial", 0, false, PAWN_CALL_MATERIAL },
		{ "RemoveBuildingForPlayer", 1, false, PAWN_CALL_REMOVE },
		{ "AttachDynamicObjectToVehicle", 0, true, PAWN_CALL_ATTACH_VEHICLE },
		{ "AttachPlayerObjectToVehicle", 1, false, PAWN_CALL_ATTACH_VEHICLE },
		{ "AttachObjectToVehicle", 0, false, PAWN_CALL_ATTACH_VEHICLE },
		{ "SetPlayerAttachedObject", 0, false, PAWN_CALL_PLAYER_ATTACHED },
		{ "AttachPlayerObjectToObject", 1, false, PAWN_CALL_ATTACH },
		{ "AttachDynamicObjectToObject", 0, true, PAWN_CALL_ATTACH },
		{ "AttachObjectToObject", 0, false, PAWN_CALL_ATTACH },
		{ "CreateDynamicObjectEx", 0, true, PAWN_CALL_CREATE_EX },
		{ "CreateDynamicObject", 0, true, PAWN_CALL_CREATE },
		{ "CreatePlayerObject", 1, false, PAWN_CALL_CREATE },
		{ "CreateObject", 0, false, PAWN_CALL_CREATE },
		{ "CreateDynamicPickup", 0, true, PAWN_CALL_PICKUP },
		{ "AddStaticPickup", 0, false, PAWN_CALL_PICKUP },
		{ "CreatePickup", 0, false, PAWN_CALL_PICKUP },
		{ "CreateDynamic3DTextLabel", 0, true, PAWN_CALL_LABEL },
		{ "Create3DTextLabel", 0, false, PAWN_CALL_LABEL },
		{ "CreateDynamicActor", 0, true, PAWN_CALL_ACTOR },
		{ "CreateActor", 0, false, PAWN_CALL_ACTOR },
		{ "SetActorVirtualWorld", 0, false, PAWN_CALL_ACTOR_WORLD },
		{ "SetActorInvulnerable", 0, false, PAWN_CALL_ACTOR_INVULNERABLE },
		{ "SetActorHealth", 0, false, PAWN_CALL_ACTOR_HEALTH },
		// Ex first: both start at the same position.
		{ "AddPlayerClassEx", 0, false, PAWN_CALL_CLASS_EX },
		{ "AddPlayerClass", 0, false, PAWN_CALL_CLASS },
		{ "GangZoneCreate", 0, false, PAWN_CALL_GANGZONE },
		{ "GangZoneShowForPlayer", 1, false, PAWN_CALL_GANGZONE_SHOW },
		{ "GangZoneShowForAll", 0, false, PAWN_CALL_GANGZONE_SHOW },
		{ "CreateDynamicMapIcon", 0, true, PAWN_CALL_MAPICON },
		{ "CreateDynamicRaceCP", 0, true, PAWN_CALL_RACE_CHECKPOINT },
		{ "CreateDynamicCircle", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicCylinder", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicSphere", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicRectangle", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicCuboid", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicCube", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicPolygon", 0, true, PAWN_CALL_AREA },
		{ "CreateDynamicCP", 0, true, PAWN_CALL_CHECKPOINT },
		{ "SetPlayerMapIcon", 2, false, PAWN_CALL_MAPICON },	// playerid, iconid
		// open.mp NPCs: NPC_Create gives the name, the setters the rest.
		// Before NPC_Create: both start at the same position.
		{ "NPC_CreatePath", 0, false, PAWN_CALL_NPC_PATH },
		{ "NPC_AddPointToPath", 0, false, PAWN_CALL_NPC_PATH_POINT },
		{ "NPC_MoveByPath", 0, false, PAWN_CALL_NPC_MOVE_BY_PATH },
		// Ex takes a loaded record ID, which a map cannot name; only this one.
		{ "NPC_StartPlayback", 0, false, PAWN_CALL_NPC_START_PLAYBACK },
		{ "NPC_PutInVehicle", 0, false, PAWN_CALL_NPC_PUT_IN_VEHICLE },
		{ "NPC_OpenNode", 0, false, PAWN_CALL_NPC_OPEN_NODE },
		{ "NPC_SetNodePoint", 0, false, PAWN_CALL_NPC_SET_NODE_POINT },
		{ "NPC_PlayNode", 0, false, PAWN_CALL_NPC_PLAY_NODE },
		{ "NPC_ApplyAnimation", 0, false, PAWN_CALL_ANIMATION },
		{ "ApplyDynamicActorAnimation", 0, true, PAWN_CALL_ANIMATION },
		{ "ApplyActorAnimation", 0, false, PAWN_CALL_ANIMATION },
		{ "NPC_Create", 0, false, PAWN_CALL_NPC },
		{ "NPC_Spawn", 0, false, PAWN_CALL_NPC_SPAWN },
		{ "NPC_SetSkin", 0, false, PAWN_CALL_NPC_SKIN },
		{ "NPC_SetPos", 0, false, PAWN_CALL_NPC_POS },
		{ "NPC_SetFacingAngle", 0, false, PAWN_CALL_NPC_ANGLE },
		{ "NPC_SetInterior", 0, false, PAWN_CALL_NPC_INTERIOR },
		{ "NPC_SetVirtualWorld", 0, false, PAWN_CALL_NPC_WORLD },
		{ "NPC_SetHealth", 0, false, PAWN_CALL_NPC_HEALTH },
		{ "NPC_SetArmour", 0, false, PAWN_CALL_NPC_ARMOUR },
		{ "NPC_SetWeapon", 0, false, PAWN_CALL_NPC_WEAPON },
		{ "NPC_SetAmmo", 0, false, PAWN_CALL_NPC_AMMO },
		{ "NPC_SetInvulnerable", 0, false, PAWN_CALL_NPC_INVULNERABLE },
		{ "PlayAudioStreamForPlayer", 1, false, PAWN_CALL_AUDIO_STREAM },	// playerid
		{ "_AddSound", 0, false, PAWN_CALL_ADD_SOUND },
		{ "_AddPlayerZone", 0, false, PAWN_CALL_ADD_PLAYER_ZONE },
		{ "_AddStreamedZone", 0, false, PAWN_CALL_ADD_STREAMED_ZONE },
		{ "_MoveByPathRepeat", 0, false, PAWN_CALL_NPC_MOVE_REPEAT },
	};

	// Each name's next occurrence, searched again only once passed: a name
	// the map never uses is looked for once, not once per call.
	const size_t numCallNames = sizeof(callNames)/sizeof(callNames[0]);
	std::vector<size_t> nextAt(numCallNames, 0);
	std::vector<bool> searched(numCallNames, false);
	PawnStringScanner strings;
	for(size_t pos = 0; pos < source.size();){
		size_t found = std::string::npos;
		const PawnCallName *call = nil;
		for(size_t n = 0; n < numCallNames; n++){
			if(!searched[n] || (nextAt[n] != std::string::npos && nextAt[n] < pos)){
				nextAt[n] = source.find(callNames[n].name, pos);
				searched[n] = true;
			}
			if(nextAt[n] < found){
				found = nextAt[n];
				call = &callNames[n];
			}
		}
		if(call == nil || found == std::string::npos)
			break;
		pos = found + strlen(call->name);
		if(strings.inStringAt(source, found))
			continue;
		if(call->kind == PAWN_CALL_ADD_SOUND || call->kind == PAWN_CALL_NPC_MOVE_REPEAT ||
		   call->kind == PAWN_CALL_ADD_PLAYER_ZONE || call->kind == PAWN_CALL_ADD_STREAMED_ZONE){
			// The end of a Haumea_ name, and a call rather than the stock itself.
			size_t start = found;
			while(start > 0 && pawnCharIsIdentifier(source[start-1]))
				start--;
			size_t before = start;
			while(before > 0 && isspace((unsigned char)source[before-1]))
				before--;
			if(source.compare(start, 7, "Haumea_") != 0 || start + 7 >= found ||
			   (pos < source.size() && pawnCharIsIdentifier(source[pos])) ||
			   (before >= 5 && source.compare(before - 5, 5, "stock") == 0))
				continue;
		}else if(!pawnNameEqualsAt(source, found, call->name))
			continue;
		size_t open = pos;
		while(open < source.size() && isspace((unsigned char)source[open])) open++;
		if(open >= source.size() || source[open] != '(')
			continue;

		std::vector<std::string> args;
		size_t afterCall = open + 1;
		if(!pawnParseCall(source, open, &afterCall, args)){
			if(numSkipped) (*numSkipped)++;
			continue;
		}
		pos = afterCall;

		const int offset = call->offset;
		if(call->kind == PAWN_CALL_REMOVE){
			if((int)args.size() < offset + 5){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			PawnRemoveEntry removal;
			bool valid = pawnParseInt(args[offset + 0], &removal.objectId) &&
				pawnParseFloat(args[offset + 1], &removal.x) &&
				pawnParseFloat(args[offset + 2], &removal.y) &&
				pawnParseFloat(args[offset + 3], &removal.z) &&
				pawnParseFloat(args[offset + 4], &removal.radius);
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			removals.push_back(removal);
			continue;
		}
		if(call->kind == PAWN_CALL_ATTACH){
			// AttachObjectToObject(obj, attachto, x, y, z, rx, ry, rz, sync = 1)
			char child[64], parent[64];
			PawnAttachImportEntry attach = {};
			attach.bone = -1;
			attach.scale = { 1.0f, 1.0f, 1.0f };
			int sync = 1;
			bool valid = (int)args.size() >= offset + 8 &&
				pawnParseToken(args[offset], child, sizeof(child)) &&
				pawnParseToken(args[offset + 1], parent, sizeof(parent)) &&
				pawnParseFloat(args[offset + 2], &attach.x) &&
				pawnParseFloat(args[offset + 3], &attach.y) &&
				pawnParseFloat(args[offset + 4], &attach.z) &&
				pawnParseFloat(args[offset + 5], &attach.rx) &&
				pawnParseFloat(args[offset + 6], &attach.ry) &&
				pawnParseFloat(args[offset + 7], &attach.rz) &&
				((int)args.size() <= offset + 8 || pawnParseMaterialInt(args[offset + 8], &sync)) &&
				handleEntries.find(child) != handleEntries.end() &&
				handleEntries.find(parent) != handleEntries.end();
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			attach.child = handleEntries[child];
			attach.parent = handleEntries[parent];
			attach.syncRotation = sync != 0;
			attachments.push_back(attach);
			continue;
		}
		if(call->kind == PAWN_CALL_ATTACH_VEHICLE){
			// AttachObjectToVehicle(obj, vehicle, x, y, z, rx, ry, rz)
			char child[64], parent[64];
			PawnAttachImportEntry attach = {};
			attach.bone = -1;
			attach.scale = { 1.0f, 1.0f, 1.0f };
			bool valid = args.size() >= (size_t)offset + 8 &&
				pawnParseToken(args[offset], child, sizeof(child)) &&
				pawnParseToken(args[offset + 1], parent, sizeof(parent)) &&
				pawnParseFloat(args[offset + 2], &attach.x) && pawnParseFloat(args[offset + 3], &attach.y) &&
				pawnParseFloat(args[offset + 4], &attach.z) && pawnParseFloat(args[offset + 5], &attach.rx) &&
				pawnParseFloat(args[offset + 6], &attach.ry) && pawnParseFloat(args[offset + 7], &attach.rz) &&
				handleEntries.find(child) != handleEntries.end() &&
				handleEntries.find(parent) != handleEntries.end() &&
				entries[handleEntries[parent]].isVehicle;
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			attach.child = handleEntries[child];
			attach.parent = handleEntries[parent];
			attach.syncRotation = true;
			attach.toVehicle = true;
			attachments.push_back(attach);
			continue;
		}
		if(call->kind == PAWN_CALL_PLAYER_ATTACHED){
			// SetPlayerAttachedObject(playerid, slot, model, bone, offset, rotation, scale, colours)
			PawnAttachImportEntry attach = {};
			attach.bone = -1;
			attach.scale = { 1.0f, 1.0f, 1.0f };
			int slot, model, bone;
			uint32 color1 = 0, color2 = 0;
			bool valid = args.size() >= 13 && pawnParseElementInt(args[1], &slot) &&
				pawnParseElementInt(args[2], &model) && pawnParseElementInt(args[3], &bone) &&
				pawnParseFloat(args[4], &attach.x) && pawnParseFloat(args[5], &attach.y) &&
				pawnParseFloat(args[6], &attach.z) && pawnParseFloat(args[7], &attach.rx) &&
				pawnParseFloat(args[8], &attach.ry) && pawnParseFloat(args[9], &attach.rz) &&
				pawnParseFloat(args[10], &attach.scale.x) && pawnParseFloat(args[11], &attach.scale.y) &&
				pawnParseFloat(args[12], &attach.scale.z);
			if(valid && args.size() >= 14)
				valid = pawnParseMaterialColor(args[13], &color1);
			if(valid && args.size() >= 15)
				valid = pawnParseMaterialColor(args[14], &color2);
			if(!valid || model < 0 || bone < 1 || bone > 18 || slot < 0 || slot > 9){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			PawnImportEntry entry = {};
			entry.objectId = EditorModelId(model);
			entry.interior = -1;
			entry.world = 0;
			entry.elementKind = SAMP_ELEMENT_OBJECT;
			InitObjectPawnProperties(&entry.element);
			attach.child = (int)entries.size();
			attach.parent = PAWN_ATTACH_PLAYER;
			attach.syncRotation = true;
			attach.bone = bone;
			attach.slot = slot;
			attach.color1 = color1;
			attach.color2 = color2;
			attach.toVehicle = false;
			char player[64];
			if(pawnParseToken(args[0], player, sizeof(player))){
				std::map<std::string, int>::const_iterator target = handleEntries.find(player);
				if(target != handleEntries.end() && entries[target->second].elementKind == SAMP_ELEMENT_NPC)
					attach.parent = target->second;
			}
			entries.push_back(entry);
			attachments.push_back(attach);
			continue;
		}
		if(call->kind == PAWN_CALL_VEHICLE_INTERIOR){
			PawnVehicleInteriorEntry entry = {};
			if(args.size() < 2 ||
			   !pawnParseToken(args[offset], entry.target, sizeof(entry.target)) ||
			   !pawnParseInt(args[offset + 1], &entry.interior) || entry.interior < 0 ||
			   handleEntries.find(entry.target) == handleEntries.end()){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			entry.entryIndex = handleEntries[entry.target];
			entry.field = PawnVehicleInteriorEntry::INTERIOR;
			vehicleInteriors.push_back(entry);
			continue;
		}
		if(call->kind == PAWN_CALL_VEHICLE_PAINTJOB || call->kind == PAWN_CALL_VEHICLE_COMPONENT ||
		   call->kind == PAWN_CALL_VEHICLE_PLATE){
			PawnVehicleInteriorEntry entry = {};
			bool ok = args.size() >= 2 &&
				pawnParseToken(args[offset], entry.target, sizeof(entry.target)) &&
				handleEntries.find(entry.target) != handleEntries.end();
			if(ok && call->kind == PAWN_CALL_VEHICLE_PLATE){
				entry.field = PawnVehicleInteriorEntry::PLATE;
				ok = pawnParseString(args[offset + 1], entry.plate, sizeof(entry.plate));
			}else if(ok){
				entry.field = call->kind == PAWN_CALL_VEHICLE_PAINTJOB ?
					PawnVehicleInteriorEntry::PAINTJOB : PawnVehicleInteriorEntry::COMPONENT;
				ok = pawnParseInt(args[offset + 1], &entry.interior) &&
					(call->kind == PAWN_CALL_VEHICLE_PAINTJOB ?
						entry.interior >= 0 && entry.interior <= 2 :
						GetVehicleComponentSlot(entry.interior) >= 0);
			}
			if(!ok){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			entry.entryIndex = handleEntries[entry.target];
			vehicleInteriors.push_back(entry);
			continue;
		}
		if(call->kind == PAWN_CALL_MATERIAL || call->kind == PAWN_CALL_MATERIAL_TEXT){
			// SetObjectMaterial(obj, index, model, txd[], texture[], color = 0)
			// SetObjectMaterialText(obj, text[], index = 0, size = 90, font[] = "Arial",
			//     fontsize = 24, bold = 1, fontcolor = -1, backcolor = 0, align = 0)
			// The streamer puts the index before the text:
			// SetDynamicObjectMaterialText(obj, index, text[], size, ...)
			bool isText = call->kind == PAWN_CALL_MATERIAL_TEXT;
			int minimumArgs = isText ? (call->streamer ? 3 : 2) : 5;
			if((int)args.size() < offset + minimumArgs){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			int indexArg = offset + 1, textArg = offset + 1;
			if(isText){
				if(call->streamer) textArg = offset + 2;
				else indexArg = offset + 2;
			}
			char target[64];
			int slot = 0;
			bool hasIndex = indexArg < (int)args.size();
			if(!pawnParseToken(args[offset], target, sizeof(target)) ||
			   (hasIndex && !pawnParseMaterialInt(args[indexArg], &slot)) ||
			   slot < 0 || slot >= MAX_PAWN_MATERIALS){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			std::map<std::string, int>::iterator handle = handleEntries.find(target);
			if(handle == handleEntries.end()){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			PawnMaterialImportEntry *entry = pawnFindMaterialImport(materials, handle->second, slot);
			if(!isText){
				int model;
				uint32 color = 0;
				char txd[MODELNAMELEN], texture[PAWN_TEXTURE_NAME_LEN];
				bool valid = pawnParseMaterialInt(args[offset + 2], &model) &&
					pawnParseString(args[offset + 3], txd, sizeof(txd)) &&
					pawnParseString(args[offset + 4], texture, sizeof(texture)) &&
					((int)args.size() <= offset + 5 ||
					 pawnParseMaterialColor(args[offset + 5], &color));
				if(!valid){
					if(numSkipped) (*numSkipped)++;
					continue;
				}
				entry->hasMaterial = true;
				entry->textIsLast = false;
				entry->material.active = true;
				// -1 (or 0xFFFF) keeps the texture, 0 is SA-MP's blank texture.
				if(model == 0xFFFF) model = -1;
				model = EditorModelId(model);	// textures of 0.3.DL custom models
				entry->material.hasTexture = model >= 0;
				entry->material.sourceModel = model;
				entry->material.txdSlot = FindTxdSlot(txd);
				strncpy(entry->material.txdName, txd, sizeof(entry->material.txdName)-1);
				entry->material.txdName[sizeof(entry->material.txdName)-1] = '\0';
				strncpy(entry->material.textureName, texture, sizeof(entry->material.textureName)-1);
				entry->material.textureName[sizeof(entry->material.textureName)-1] = '\0';
				entry->material.color = pawnColorFromInt(color);
			}else{
				ObjectMaterialOverride defaults;
				InitObjectMaterialOverride(&defaults, -1);
				uint32 textColor = pawnColorToInt(defaults.textColor);
				uint32 backgroundColor = pawnColorToInt(defaults.backgroundColor);
				char text[PAWN_MATERIAL_TEXT_LEN], font[PAWN_MATERIAL_FONT_LEN];
				strncpy(font, defaults.font, sizeof(font));
				int materialSize = defaults.materialSize, fontSize = defaults.fontSize;
				int bold = defaults.bold, alignment = defaults.alignment;
				int n = (int)args.size();
				bool valid = pawnParseString(args[textArg], text, sizeof(text)) &&
					(n <= offset + 3 || pawnParseMaterialInt(args[offset + 3], &materialSize)) &&
					(n <= offset + 4 || pawnParseString(args[offset + 4], font, sizeof(font))) &&
					(n <= offset + 5 || pawnParseMaterialInt(args[offset + 5], &fontSize)) &&
					(n <= offset + 6 || pawnParseMaterialInt(args[offset + 6], &bold)) &&
					(n <= offset + 7 || pawnParseMaterialColor(args[offset + 7], &textColor)) &&
					(n <= offset + 8 || pawnParseMaterialColor(args[offset + 8], &backgroundColor)) &&
					(n <= offset + 9 || pawnParseMaterialInt(args[offset + 9], &alignment));
				if(!valid){
					if(numSkipped) (*numSkipped)++;
					continue;
				}
				entry->hasText = true;
				entry->textIsLast = true;
				entry->material.textEnabled = true;
				strncpy(entry->material.text, text, sizeof(entry->material.text)-1);
				entry->material.text[sizeof(entry->material.text)-1] = '\0';
				strncpy(entry->material.font, font, sizeof(entry->material.font)-1);
				entry->material.font[sizeof(entry->material.font)-1] = '\0';
				entry->material.materialSize = materialSize;
				entry->material.fontSize = fontSize;
				entry->material.bold = bold != 0;
				entry->material.textColor = pawnColorFromInt(textColor);
				entry->material.backgroundColor = pawnColorFromInt(backgroundColor);
				entry->material.alignment = alignment;
			}
			continue;
		}
		if(call->kind == PAWN_CALL_ACTOR_WORLD || call->kind == PAWN_CALL_ACTOR_INVULNERABLE ||
		   call->kind == PAWN_CALL_ACTOR_HEALTH || call->kind == PAWN_CALL_GANGZONE_SHOW){
			// Setter(handle, value) / GangZoneShowFor*([playerid,] zone, color)
			char target[64];
			PawnElementSetterEntry setter = {};
			int intValue = 0;
			bool valid = (int)args.size() >= offset + 2 &&
				pawnParseToken(args[offset], target, sizeof(target)) &&
				handleEntries.find(target) != handleEntries.end();
			if(valid){
				switch(call->kind){
				case PAWN_CALL_ACTOR_WORLD:
					setter.field = PawnElementSetterEntry::WORLD;
					valid = pawnParseElementInt(args[offset + 1], &intValue);
					setter.value = (float)intValue;
					break;
				case PAWN_CALL_ACTOR_INVULNERABLE:
					setter.field = PawnElementSetterEntry::INVULNERABLE;
					valid = pawnParseElementInt(args[offset + 1], &intValue);
					setter.value = (float)intValue;
					break;
				case PAWN_CALL_ACTOR_HEALTH:
					setter.field = PawnElementSetterEntry::HEALTH;
					valid = pawnParseFloat(pawnStripTag(args[offset + 1]), &setter.value);
					break;
				default:
					setter.field = PawnElementSetterEntry::ZONE_COLOR;
					valid = pawnParseMaterialColor(args[offset + 1], &setter.color);
					break;
				}
			}
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			setter.entryIndex = handleEntries[target];
			setters.push_back(setter);
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_PATH){
			// new patrol = NPC_CreatePath(); the points name it.
			char handle[64];
			pawnCaptureAssignment(source, found, handle, sizeof(handle));
			if(handle[0])
				npcPathPoints[handle] = 0;
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_PATH_POINT){
			// NPC_AddPointToPath(path, x, y, z, stopRange)
			char path[64];
			PawnImportEntry entry = {};
			entry.interior = -1;
			entry.world = -1;
			InitObjectPawnProperties(&entry.element);
			entry.elementKind = SAMP_ELEMENT_PATHPOINT;
			entry.element.elementKind = SAMP_ELEMENT_PATHPOINT;
			entry.element.pathStopRange = 0.2f;
			entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_PATHPOINT);
			bool valid = (int)args.size() >= 4 &&
				pawnParseToken(args[0], path, sizeof(path)) &&
				IsValidPawnObjectName(path) &&
				pawnParseFloat(args[1], &entry.x) &&
				pawnParseFloat(args[2], &entry.y) &&
				pawnParseFloat(args[3], &entry.z) &&
				((int)args.size() < 5 ||
				 pawnParseFloat(pawnStripTag(args[4]), &entry.element.pathStopRange));
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			snprintf(entry.element.pathName, sizeof(entry.element.pathName), "%s", path);
			entry.element.pathOrder = npcPathPoints[path]++;
			pawnReadObjectTags(rawSource, afterCall, &entry);
			entries.push_back(entry);
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_MOVE_BY_PATH || call->kind == PAWN_CALL_NPC_MOVE_REPEAT){
			// NPC_MoveByPath(npc, path, moveType = JOG, moveSpeed = AUTO, reversed = false), or
			// Haumea_<map>_MoveByPathRepeat(npc, path, moveType, moveSpeed, reversed, mode)
			if(!args.empty() && pawnStripTag(args[0]) == std::string("npcid"))
				continue;	// the generated helpers' own calls
			char target[64], path[64];
			std::map<std::string, int>::iterator npc = handleEntries.end();
			int n = (int)args.size();
			bool valid = n >= 2 && pawnParseToken(args[0], target, sizeof(target)) &&
				pawnParseToken(args[1], path, sizeof(path)) && IsValidPawnObjectName(path);
			if(valid)
				npc = handleEntries.find(target);
			if(!valid || npc == handleEntries.end() ||
			   entries[npc->second].elementKind != SAMP_ELEMENT_NPC){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			ObjectPawnProperties &el = entries[npc->second].element;
			snprintf(el.pathName, sizeof(el.pathName), "%s", path);
			el.npcMoveType = 2;
			el.npcMoveSpeed = -1.0f;
			el.npcPathReversed = false;
			if(n >= 3)
				el.npcMoveType = pawnParseMoveType(args[2]);
			if(n >= 4)
				el.npcMoveSpeed = pawnParseMoveSpeed(args[3]);
			int reversed = 0;
			if(n >= 5 && pawnParseElementInt(args[4], &reversed))
				el.npcPathReversed = reversed != 0;
			el.npcPathRepeat = NPC_PATH_ONCE;
			if(call->kind == PAWN_CALL_NPC_MOVE_REPEAT && n >= 6){
				std::string mode = pawnStripTag(args[5]);
				int value = 0;
				if(mode == "NPC_PATH_LOOP") el.npcPathRepeat = NPC_PATH_LOOP;
				else if(mode == "NPC_PATH_BACK_AND_FORTH") el.npcPathRepeat = NPC_PATH_BACK_AND_FORTH;
				else if(pawnParseElementInt(args[5], &value)) el.npcPathRepeat = value;
			}
			continue;
		}
		if(call->kind == PAWN_CALL_ANIMATION){
			// (actor or npc, "lib", "name", delta, loop, lockX, lockY, freeze, time)
			char target[64];
			std::map<std::string, int>::iterator element = handleEntries.end();
			int n = (int)args.size();
			ObjectPawnProperties anim;
			InitObjectPawnProperties(&anim);
			bool valid = n >= 3 && pawnParseToken(pawnStripTag(args[0]), target, sizeof(target)) &&
				pawnParseString(args[1], anim.animLib, sizeof(anim.animLib)) &&
				pawnParseString(args[2], anim.animName, sizeof(anim.animName));
			if(valid)
				element = handleEntries.find(target);
			int kind = element != handleEntries.end() ? entries[element->second].elementKind : -1;
			if(!valid || (kind != SAMP_ELEMENT_ACTOR && kind != SAMP_ELEMENT_NPC)){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			ObjectPawnProperties &el = entries[element->second].element;
			snprintf(el.animLib, sizeof(el.animLib), "%s", anim.animLib);
			snprintf(el.animName, sizeof(el.animName), "%s", anim.animName);
			el.animDelta = 4.1f;
			if(n > 3)
				pawnParseFloat(pawnStripTag(args[3]), &el.animDelta);
			bool *flags[4] = { &el.animLoop, &el.animLockX, &el.animLockY, &el.animFreeze };
			for(int k = 0; k < 4; k++){
				int value = 0;
				*flags[k] = n > 4 + k && pawnParseElementInt(args[4 + k], &value) && value != 0;
			}
			el.animTime = 0;
			if(n > 8)
				pawnParseElementInt(args[8], &el.animTime);
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_OPEN_NODE)
			continue;	// every node an NPC plays is opened on save
		if(call->kind == PAWN_CALL_NPC_SET_NODE_POINT){
			// NPC_SetNodePoint(nodeid, pointid): where the next NPC_PlayNode starts.
			int node, point;
			if((int)args.size() >= 2 && pawnParseElementInt(args[0], &node) &&
			   pawnParseElementInt(args[1], &point))
				npcNodePoints[node] = point;
			else if(numSkipped)
				(*numSkipped)++;
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_PLAY_NODE){
			// NPC_PlayNode(npc, nodeid, moveType = JOG, speed = AUTO, radius = 0.0, setAngle = true)
			char target[64];
			std::map<std::string, int>::iterator npc = handleEntries.end();
			int n = (int)args.size();
			int node = 0;
			bool valid = n >= 2 && pawnParseToken(args[0], target, sizeof(target)) &&
				pawnParseElementInt(args[1], &node);
			if(valid)
				npc = handleEntries.find(target);
			if(!valid || npc == handleEntries.end() ||
			   entries[npc->second].elementKind != SAMP_ELEMENT_NPC){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			ObjectPawnProperties &el = entries[npc->second].element;
			el.npcPlaysNode = true;
			el.npcNode = node;
			el.npcNodePoint = npcNodePoints.count(node) ? npcNodePoints[node] : 0;
			el.npcMoveType = n >= 3 ? pawnParseMoveType(args[2]) : 2;
			el.npcMoveSpeed = n >= 4 ? pawnParseMoveSpeed(args[3]) : -1.0f;
			el.npcNodeRadius = 0.0f;
			if(n >= 5)
				pawnParseFloat(pawnStripTag(args[4]), &el.npcNodeRadius);
			int setAngle = 1;
			if(n >= 6)
				pawnParseElementInt(args[5], &setAngle);
			el.npcNodeSetAngle = setAngle != 0;
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_PUT_IN_VEHICLE){
			// NPC_PutInVehicle(npcid, vehicleid, seatid)
			char npcName[64], vehicleName[64];
			int seat = 0;
			bool valid = args.size() >= 3 && pawnParseToken(args[0], npcName, sizeof(npcName)) &&
				pawnParseToken(args[1], vehicleName, sizeof(vehicleName)) &&
				pawnParseElementInt(args[2], &seat);
			std::map<std::string, int>::iterator npc = valid ? handleEntries.find(npcName) : handleEntries.end();
			std::map<std::string, int>::iterator vehicle = valid ? handleEntries.find(vehicleName) : handleEntries.end();
			if(npc == handleEntries.end() || vehicle == handleEntries.end() ||
			   entries[npc->second].elementKind != SAMP_ELEMENT_NPC || !entries[vehicle->second].isVehicle){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			entries[npc->second].npcVehicleEntry = vehicle->second + 1;
			entries[npc->second].element.npcSeat = clamp(seat, 0, 3);
			continue;
		}
		if(call->kind == PAWN_CALL_NPC_START_PLAYBACK){
			// NPC_StartPlayback(npc, "name", autoUnload, x, y, z, rx, ry, rz)
			char target[64];
			std::map<std::string, int>::iterator npc = handleEntries.end();
			int n = (int)args.size();
			char recording[PAWN_NPC_RECORDING_LEN];
			bool valid = n >= 2 && pawnParseToken(args[0], target, sizeof(target)) &&
				pawnParseString(args[1], recording, sizeof(recording));
			if(valid)
				npc = handleEntries.find(target);
			if(!valid || npc == handleEntries.end() ||
			   entries[npc->second].elementKind != SAMP_ELEMENT_NPC){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			ObjectPawnProperties &el = entries[npc->second].element;
			snprintf(el.npcRecording, sizeof(el.npcRecording), "%s", recording);
			// Started away from where it was recorded: at the NPC, as saved.
			float start[3] = { 0.0f, 0.0f, 0.0f };
			for(int k = 0; k < 3 && n > 3 + k; k++)
				pawnParseFloat(pawnStripTag(args[3 + k]), &start[k]);
			el.npcRecordingAtNpc = start[0] != 0.0f || start[1] != 0.0f || start[2] != 0.0f;
			continue;
		}
		if(call->kind == PAWN_CALL_NPC){
			// new npc = NPC_Create("name"); it stands at 0,0,0 until NPC_SetPos.
			PawnImportEntry entry = {};
			entry.interior = -1;
			entry.world = 0;
			InitObjectPawnProperties(&entry.element);
			InitSampElementProperties(&entry.element, SAMP_ELEMENT_NPC);
			entry.elementKind = SAMP_ELEMENT_NPC;
			if((int)args.size() < 1 ||
			   !pawnParseString(args[0], entry.element.npcName, sizeof(entry.element.npcName))){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			entry.objectId = GetSampElementDisplayModel(SAMP_ELEMENT_NPC, entry.element.elementModel);
			pawnCaptureAssignment(source, found, entry.handle, sizeof(entry.handle));
			pawnReadObjectTags(rawSource, afterCall, &entry);
			entries.push_back(entry);
			if(entry.handle[0])
				handleEntries[entry.handle] = (int)entries.size() - 1;
			continue;
		}
		if(call->kind >= PAWN_CALL_NPC_SPAWN && call->kind <= PAWN_CALL_NPC_INVULNERABLE){
			// NPC_Set*(npc, ...) on an NPC created above. NPC_Spawn has nothing to keep.
			char target[64];
			std::map<std::string, int>::iterator handle = handleEntries.end();
			if((int)args.size() >= 1 && pawnParseToken(args[0], target, sizeof(target)))
				handle = handleEntries.find(target);
			if(handle == handleEntries.end() ||
			   entries[handle->second].elementKind != SAMP_ELEMENT_NPC){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			PawnImportEntry &npc = entries[handle->second];
			ObjectPawnProperties &el = npc.element;
			int n = (int)args.size();
			int intValue = 0;
			bool valid = true;
			switch(call->kind){
			case PAWN_CALL_NPC_SPAWN:
				break;
			case PAWN_CALL_NPC_SKIN:
				valid = n >= 2 && pawnParseElementInt(args[1], &el.elementModel);
				if(valid)
					npc.objectId = GetSampElementDisplayModel(SAMP_ELEMENT_NPC, el.elementModel);
				break;
			case PAWN_CALL_NPC_POS:
				valid = n >= 4 && pawnParseFloat(args[1], &npc.x) &&
					pawnParseFloat(args[2], &npc.y) && pawnParseFloat(args[3], &npc.z);
				break;
			case PAWN_CALL_NPC_ANGLE:
				valid = n >= 2 && pawnParseFloat(pawnStripTag(args[1]), &npc.rz);
				break;
			case PAWN_CALL_NPC_INTERIOR:
				valid = n >= 2 && pawnParseElementInt(args[1], &npc.interior);
				break;
			case PAWN_CALL_NPC_WORLD:
				valid = n >= 2 && pawnParseElementInt(args[1], &npc.world);
				break;
			case PAWN_CALL_NPC_HEALTH:
				valid = n >= 2 && pawnParseFloat(pawnStripTag(args[1]), &el.actorHealth);
				break;
			case PAWN_CALL_NPC_ARMOUR:
				valid = n >= 2 && pawnParseFloat(pawnStripTag(args[1]), &el.npcArmour);
				break;
			case PAWN_CALL_NPC_WEAPON:
				valid = n >= 2 && (pawnParseElementInt(args[1], &el.npcWeapon) ||
					pawnParseElementInt(pawnStripTag(args[1]), &el.npcWeapon));
				break;
			case PAWN_CALL_NPC_AMMO:
				valid = n >= 2 && pawnParseElementInt(args[1], &el.npcAmmo);
				break;
			case PAWN_CALL_NPC_INVULNERABLE:
				valid = n >= 2 && pawnParseElementInt(args[1], &intValue);
				if(valid)
					el.actorInvulnerable = intValue != 0;
				break;
			default:
				break;
			}
			if(!valid && numSkipped)
				(*numSkipped)++;
			continue;
		}
		if(call->kind == PAWN_CALL_PICKUP || call->kind == PAWN_CALL_LABEL ||
		   call->kind == PAWN_CALL_ACTOR || call->kind == PAWN_CALL_CLASS ||
		   call->kind == PAWN_CALL_CLASS_EX || call->kind == PAWN_CALL_GANGZONE ||
		   call->kind == PAWN_CALL_MAPICON || call->kind == PAWN_CALL_AUDIO_STREAM ||
		   call->kind == PAWN_CALL_ADD_SOUND || call->kind == PAWN_CALL_CHECKPOINT ||
		   call->kind == PAWN_CALL_ADD_PLAYER_ZONE || call->kind == PAWN_CALL_ADD_STREAMED_ZONE ||
		   call->kind == PAWN_CALL_RACE_CHECKPOINT || call->kind == PAWN_CALL_AREA){
			PawnImportEntry entry = {};
			entry.interior = -1;
			entry.world = -1;
			InitObjectPawnProperties(&entry.element);
			ObjectPawnProperties &el = entry.element;
			int n = (int)args.size();
			const int o = offset;
			auto arg = [&](int index) -> const std::string& { return args[o + index]; };
			auto optInt = [&](int index, int *out){
				int value;
				if(n > o + index && pawnParseElementInt(arg(index), &value))
					*out = value;
			};
			auto optFloat = [&](int index, float *out){
				float value;
				if(n > o + index && pawnParseFloat(pawnStripTag(arg(index)), &value))
					*out = value;
			};
			auto optDistance = [&](int index, float *out){
				float value;
				if(n > o + index && pawnParseDistance(arg(index), &value))
					*out = value;
			};
			bool valid = false;
			// A call with variables for its place, like the editor's own sound
			// helper's CreateDynamicSphere(x, y, z, radius...): code, not an element.
			bool notAnElement = false;
			switch(call->kind){
			case PAWN_CALL_PICKUP:
				// CreatePickup(model, type, x, y, z, virtualworld = 0)
				// CreateDynamicPickup(model, type, x, y, z, worldid, interiorid, playerid, streamdistance)
				entry.elementKind = SAMP_ELEMENT_PICKUP;
				valid = n >= o + 5 &&
					pawnParseElementInt(arg(0), &entry.objectId) &&
					pawnParseElementInt(arg(1), &el.pickupType) &&
					pawnParseFloat(arg(2), &entry.x) &&
					pawnParseFloat(arg(3), &entry.y) &&
					pawnParseFloat(arg(4), &entry.z);
				if(call->streamer){
					optInt(5, &entry.world);
					optInt(6, &entry.interior);
					optDistance(8, &entry.streamDistance);
				}else{
					entry.world = 0;
					optInt(5, &entry.world);
				}
				break;
			case PAWN_CALL_LABEL: {
				// Create3DTextLabel(text[], color, x, y, z, DrawDistance, virtualworld, testLOS = 0)
				// CreateDynamic3DTextLabel(text[], color, x, y, z, drawdistance, attachedplayer,
				//     attachedvehicle, testlos, worldid, interiorid, playerid, streamdistance)
				entry.elementKind = SAMP_ELEMENT_LABEL;
				entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_LABEL);
				int testLOS = 0;
				valid = n >= o + 6 &&
					pawnParseString(arg(0), el.labelText, sizeof(el.labelText)) &&
					pawnParseMaterialColor(arg(1), &el.labelColor) &&
					pawnParseFloat(arg(2), &entry.x) &&
					pawnParseFloat(arg(3), &entry.y) &&
					pawnParseFloat(arg(4), &entry.z) &&
					pawnParseFloat(pawnStripTag(arg(5)), &entry.drawDistance);
				if(call->streamer){
					int attachedPlayer = 0xFFFF, attachedVehicle = 0xFFFF;
					optInt(6, &attachedPlayer);
					optInt(7, &attachedVehicle);
					// Labels on players or vehicles have no place in a map.
					if(attachedPlayer != 0xFFFF || attachedVehicle != 0xFFFF)
						valid = false;
					optInt(8, &testLOS);
					optInt(9, &entry.world);
					optInt(10, &entry.interior);
					optDistance(12, &entry.streamDistance);
				}else{
					entry.world = 0;
					optInt(6, &entry.world);
					optInt(7, &testLOS);
				}
				el.labelTestLOS = testLOS != 0;
				break;
			}
			case PAWN_CALL_ACTOR: {
				// CreateActor(modelid, x, y, z, rotation)
				// CreateDynamicActor(modelid, x, y, z, r, invulnerable = true, health = 100.0,
				//     worldid, interiorid, playerid, streamdistance)
				entry.elementKind = SAMP_ELEMENT_ACTOR;
				int invulnerable = 1;
				el.actorHealth = 100.0f;
				valid = n >= o + 5 &&
					pawnParseElementInt(arg(0), &el.elementModel) &&
					pawnParseFloat(arg(1), &entry.x) &&
					pawnParseFloat(arg(2), &entry.y) &&
					pawnParseFloat(arg(3), &entry.z) &&
					pawnParseFloat(arg(4), &entry.rz);
				if(call->streamer){
					optInt(5, &invulnerable);
					optFloat(6, &el.actorHealth);
					optInt(7, &entry.world);
					optInt(8, &entry.interior);
					optDistance(10, &entry.streamDistance);
				}else
					entry.world = 0;
				el.actorInvulnerable = invulnerable != 0;
				break;
			}
			case PAWN_CALL_CLASS:
			case PAWN_CALL_CLASS_EX: {
				// AddPlayerClass(modelid, x, y, z, angle, weapon1, ammo1, weapon2, ammo2, weapon3, ammo3)
				// AddPlayerClassEx(teamid, modelid, ...)
				entry.elementKind = SAMP_ELEMENT_CLASS;
				int c = 0;
				el.classTeam = -1;
				valid = true;
				if(call->kind == PAWN_CALL_CLASS_EX){
					valid = n >= o + 1 && pawnParseElementInt(arg(0), &el.classTeam);
					c = 1;
				}
				valid = valid && n >= o + c + 5 &&
					pawnParseElementInt(arg(c + 0), &el.elementModel) &&
					pawnParseFloat(arg(c + 1), &entry.x) &&
					pawnParseFloat(arg(c + 2), &entry.y) &&
					pawnParseFloat(arg(c + 3), &entry.z) &&
					pawnParseFloat(arg(c + 4), &entry.rz);
				for(int w = 0; w < PAWN_CLASS_WEAPONS; w++){
					optInt(c + 5 + w*2, &el.classWeapons[w]);
					optInt(c + 6 + w*2, &el.classAmmo[w]);
				}
				entry.world = -1;
				break;
			}
			case PAWN_CALL_ADD_PLAYER_ZONE:
			case PAWN_CALL_ADD_STREAMED_ZONE:
			case PAWN_CALL_GANGZONE: {
				// GangZoneCreate(minx, miny, maxx, maxy), and
				// <prefix>_AddPlayerZone/_AddStreamedZone(minx, miny, maxx, maxy, colour)
				entry.elementKind = SAMP_ELEMENT_GANGZONE;
				entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_GANGZONE);
				float minX, minY, maxX, maxY;
				valid = n >= o + 4 &&
					pawnParseFloat(arg(0), &minX) && pawnParseFloat(arg(1), &minY) &&
					pawnParseFloat(arg(2), &maxX) && pawnParseFloat(arg(3), &maxY);
				if(valid){
					entry.x = (minX + maxX) * 0.5f;
					entry.y = (minY + maxY) * 0.5f;
					entry.z = -10000.0f;	// on the ground, found when placed
					el.zoneSizeX = fabsf(maxX - minX);
					el.zoneSizeY = fabsf(maxY - minY);
					el.zoneColor = 0xFF000088;
				}
				if(call->kind != PAWN_CALL_GANGZONE){
					el.zoneMode = call->kind == PAWN_CALL_ADD_PLAYER_ZONE ? SAMP_ZONE_PLAYER : SAMP_ZONE_STREAMED;
					valid = valid && n >= o + 5 && pawnParseMaterialColor(arg(4), &el.zoneColor);
				}
				break;
			}
			case PAWN_CALL_MAPICON:
				// SetPlayerMapIcon(playerid, iconid, x, y, z, markertype, color, style = MAPICON_LOCAL)
				// CreateDynamicMapIcon(x, y, z, type, color, worldid, interiorid, playerid,
				//     streamdistance, style = MAPICON_LOCAL)
				entry.elementKind = SAMP_ELEMENT_MAPICON;
				entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_MAPICON);
				valid = n >= o + 5 &&
					pawnParseFloat(arg(0), &entry.x) &&
					pawnParseFloat(arg(1), &entry.y) &&
					pawnParseFloat(arg(2), &entry.z) &&
					pawnParseElementInt(arg(3), &el.mapIconType) &&
					pawnParseMaterialColor(arg(4), &el.mapIconColor);
				if(call->streamer){
					optInt(5, &entry.world);
					optInt(6, &entry.interior);
					optDistance(8, &entry.streamDistance);
					optInt(9, &el.mapIconStyle);
				}else
					optInt(5, &el.mapIconStyle);
				break;
			case PAWN_CALL_AUDIO_STREAM: {
				// PlayAudioStreamForPlayer(playerid, url[], x = 0.0, y = 0.0, z = 0.0,
				//     distance = 50.0, usepos = false)
				entry.elementKind = SAMP_ELEMENT_SOUND;
				entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_SOUND);
				el.soundKind = SAMP_SOUND_STREAM;
				el.soundRadius = 50.0f;
				int usePos = 0;
				valid = n >= o + 1 && pawnParseString(arg(0), el.soundUrl, sizeof(el.soundUrl));
				optFloat(1, &entry.x);
				optFloat(2, &entry.y);
				optFloat(3, &entry.z);
				optFloat(4, &el.soundRadius);
				optInt(5, &usePos);
				el.soundUsePos = usePos != 0;
				break;
			}
			case PAWN_CALL_ADD_SOUND: {
				// <prefix>_AddSound(soundid, x, y, z, radius, stopOnLeave, worldid = -1, interiorid = -1)
				entry.elementKind = SAMP_ELEMENT_SOUND;
				entry.objectId = GetSampElementProxyModel(SAMP_ELEMENT_SOUND);
				el.soundKind = SAMP_SOUND_GAME;
				el.soundUsePos = true;
				int stop = 1;
				valid = n >= o + 5 &&
					pawnParseElementInt(arg(0), &el.soundId) &&
					pawnParseFloat(arg(1), &entry.x) &&
					pawnParseFloat(arg(2), &entry.y) &&
					pawnParseFloat(arg(3), &entry.z) &&
					pawnParseFloat(pawnStripTag(arg(4)), &el.soundRadius);
				optInt(5, &stop);
				optInt(6, &entry.world);
				optInt(7, &entry.interior);
				el.soundStopOnLeave = stop != 0;
				break;
			}
			case PAWN_CALL_CHECKPOINT:
				// CreateDynamicCP(x, y, z, size, worldid, interiorid, playerid, streamdistance)
				entry.elementKind = SAMP_ELEMENT_CHECKPOINT;
				el.cpKind = SAMP_CHECKPOINT_NORMAL;
				valid = n >= o + 4 &&
					pawnParseFloat(arg(0), &entry.x) &&
					pawnParseFloat(arg(1), &entry.y) &&
					pawnParseFloat(arg(2), &entry.z) &&
					pawnParseFloat(pawnStripTag(arg(3)), &el.cpSize);
				optInt(4, &entry.world);
				optInt(5, &entry.interior);
				optDistance(7, &entry.streamDistance);
				break;
			case PAWN_CALL_RACE_CHECKPOINT:
				// CreateDynamicRaceCP(type, x, y, z, nextx, nexty, nextz, size, worldid,
				//     interiorid, playerid, streamdistance)
				entry.elementKind = SAMP_ELEMENT_CHECKPOINT;
				el.cpKind = SAMP_CHECKPOINT_RACE;
				valid = n >= o + 8 &&
					pawnParseElementInt(arg(0), &el.cpRaceType) &&
					pawnParseFloat(arg(1), &entry.x) &&
					pawnParseFloat(arg(2), &entry.y) &&
					pawnParseFloat(arg(3), &entry.z) &&
					pawnParseFloat(arg(4), &entry.cpNext[0]) &&
					pawnParseFloat(arg(5), &entry.cpNext[1]) &&
					pawnParseFloat(arg(6), &entry.cpNext[2]) &&
					pawnParseFloat(pawnStripTag(arg(7)), &el.cpSize);
				optInt(8, &entry.world);
				optInt(9, &entry.interior);
				optDistance(11, &entry.streamDistance);
				break;
			case PAWN_CALL_AREA: {
				// CreateDynamicCircle(x, y, size, ...), Cylinder(x, y, minz, maxz, size, ...),
				// Sphere(x, y, z, size, ...), Rectangle(minx, miny, maxx, maxy, ...),
				// Cuboid/Cube(minx, miny, minz, maxx, maxy, maxz, ...),
				// Polygon(points[], minz, maxz, maxpoints, ...); then worldid, interiorid.
				entry.elementKind = SAMP_ELEMENT_AREA;
				std::string shape = call->name + strlen("CreateDynamic");
				float v[6] = {};
				float first;
				if(shape != "Polygon" && n > o && !pawnParseFloat(pawnStripTag(arg(0)), &first))
					notAnElement = true;
				auto floats = [&](int count){
					if(n < o + count)
						return false;
					for(int i = 0; i < count; i++)
						if(!pawnParseFloat(pawnStripTag(arg(i)), &v[i]))
							return false;
					return true;
				};
				int worldAt = -1;
				if(shape == "Circle" && (valid = floats(3))){
					el.areaShape = SAMP_AREA_CIRCLE;
					entry.x = v[0]; entry.y = v[1]; el.areaRadius = v[2];
					worldAt = 3;
				}else if(shape == "Cylinder" && (valid = floats(5))){
					el.areaShape = SAMP_AREA_CYLINDER;
					entry.x = v[0]; entry.y = v[1]; entry.z = v[2];
					el.areaHeight = v[3] - v[2]; el.areaRadius = v[4];
					worldAt = 5;
				}else if(shape == "Sphere" && (valid = floats(4))){
					el.areaShape = SAMP_AREA_SPHERE;
					entry.x = v[0]; entry.y = v[1]; entry.z = v[2]; el.areaRadius = v[3];
					worldAt = 4;
				}else if(shape == "Rectangle" && (valid = floats(4))){
					el.areaShape = SAMP_AREA_RECTANGLE;
					entry.x = (v[0] + v[2]) * 0.5f; entry.y = (v[1] + v[3]) * 0.5f;
					el.areaSize = { fabsf(v[2] - v[0]), fabsf(v[3] - v[1]), 0.0f };
					worldAt = 4;
				}else if((shape == "Cuboid" || shape == "Cube") && (valid = floats(6))){
					el.areaShape = shape == "Cube" ? SAMP_AREA_CUBE : SAMP_AREA_CUBOID;
					entry.x = (v[0] + v[3]) * 0.5f; entry.y = (v[1] + v[4]) * 0.5f;
					entry.z = v[2] < v[5] ? v[2] : v[5];
					el.areaSize = { fabsf(v[3] - v[0]), fabsf(v[4] - v[1]), fabsf(v[5] - v[2]) };
					worldAt = 6;
				}else if(shape == "Polygon" && n >= o + 1){
					std::vector<float> points;
					valid = pawnParsePolygonPoints(arg(0), source, found, points);
					if(valid){
						int count = std::min((int)points.size() / 2, (int)PAWN_AREA_POINTS);
						float minX = points[0], maxX = points[0], minY = points[1], maxY = points[1];
						for(int i = 1; i < count; i++){
							minX = std::min(minX, points[2*i]); maxX = std::max(maxX, points[2*i]);
							minY = std::min(minY, points[2*i+1]); maxY = std::max(maxY, points[2*i+1]);
						}
						el.areaShape = SAMP_AREA_POLYGON;
						entry.x = (minX + maxX) * 0.5f; entry.y = (minY + maxY) * 0.5f;
						el.areaPointCount = count;
						for(int i = 0; i < count; i++){
							el.areaPoints[i][0] = points[2*i] - entry.x;
							el.areaPoints[i][1] = points[2*i+1] - entry.y;
						}
						// minz, maxz: absent or +-FLOAT_INFINITY for every height.
						float minZ, maxZ;
						if(n > o + 2 && pawnParseFloat(pawnStripTag(arg(1)), &minZ) &&
						   pawnParseFloat(pawnStripTag(arg(2)), &maxZ)){
							entry.z = minZ;
							el.areaHeight = maxZ - minZ;
						}
						worldAt = 4;
					}
				}
				if(valid && worldAt >= 0){
					optInt(worldAt, &entry.world);
					optInt(worldAt + 1, &entry.interior);
				}
				break;
			}
			default:
				break;
			}
			if(!valid){
				if(numSkipped && !notAnElement) (*numSkipped)++;
				continue;
			}
			// The item's areaid, by where it is in each streamer call.
			if(call->streamer){
				int areaAt = call->kind == PAWN_CALL_PICKUP ? 9 : call->kind == PAWN_CALL_LABEL ? 13 :
					call->kind == PAWN_CALL_ACTOR ? 11 : call->kind == PAWN_CALL_CHECKPOINT ? 8 :
					call->kind == PAWN_CALL_RACE_CHECKPOINT ? 12 : call->kind == PAWN_CALL_MAPICON ? 10 : -1;
				if(areaAt >= 0)
					entry.areaEntry = pawnAreaEntryFor(args, o + areaAt, handleEntries, entries);
			}
			if(entry.elementKind == SAMP_ELEMENT_PICKUP)
				el.elementModel = entry.objectId;
			entry.objectId = GetSampElementDisplayModel(entry.elementKind, el.elementModel);
			pawnCaptureAssignment(source, found, entry.handle, sizeof(entry.handle));
			pawnReadObjectTags(rawSource, afterCall, &entry);
			entries.push_back(entry);
			if(entry.handle[0])
				handleEntries[entry.handle] = (int)entries.size() - 1;
			continue;
		}
		if(call->kind == PAWN_CALL_VEHICLE_STATIC ||
		   call->kind == PAWN_CALL_VEHICLE_STATIC_EX ||
		   call->kind == PAWN_CALL_VEHICLE_DYNAMIC){
			int minimumArgs = call->kind == PAWN_CALL_VEHICLE_STATIC ? 7 : 8;
			if((int)args.size() < offset + minimumArgs){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			PawnImportEntry entry = {};
			entry.interior = 0;
			entry.isVehicle = true;
			entry.vehicleStatic = call->kind != PAWN_CALL_VEHICLE_DYNAMIC;
			entry.vehicleRespawnDelay = call->kind == PAWN_CALL_VEHICLE_STATIC ? -1 : 100;
			bool valid = pawnParseInt(args[offset + 0], &entry.objectId) &&
				pawnParseFloat(args[offset + 1], &entry.x) &&
				pawnParseFloat(args[offset + 2], &entry.y) &&
				pawnParseFloat(args[offset + 3], &entry.z) &&
				pawnParseFloat(args[offset + 4], &entry.rz) &&
				pawnParseInt(args[offset + 5], &entry.vehicleColor1) &&
				pawnParseInt(args[offset + 6], &entry.vehicleColor2);
			if(call->kind != PAWN_CALL_VEHICLE_STATIC && valid)
				valid = pawnParseInt(args[offset + 7], &entry.vehicleRespawnDelay);
			if(!valid){
				if(numSkipped) (*numSkipped)++;
				continue;
			}
			if(IsTrainEngineModel(entry.objectId))
				entry.rz = TrainImportAngle(entry.rz);
			entry.rx = entry.ry = 0.0f;
			entry.world = -1;
			pawnCaptureAssignment(source, found, entry.handle, sizeof(entry.handle));
			entries.push_back(entry);
			if(entry.handle[0])
				handleEntries[entry.handle] = (int)entries.size() - 1;
			continue;
		}
		if((int)args.size() < offset + 7){
			if(numSkipped) (*numSkipped)++;
			continue;
		}
		PawnImportEntry entry;
		memset(&entry, 0, sizeof(entry));
		entry.interior = -1;
		entry.world = -1;
		bool valid = pawnParseInt(args[offset + 0], &entry.objectId) &&
			pawnParseFloat(args[offset + 1], &entry.x) &&
			pawnParseFloat(args[offset + 2], &entry.y) &&
			pawnParseFloat(args[offset + 3], &entry.z) &&
			pawnParseDouble(args[offset + 4], &entry.sourceDegrees[0]) &&
			pawnParseDouble(args[offset + 5], &entry.sourceDegrees[1]) &&
			pawnParseDouble(args[offset + 6], &entry.sourceDegrees[2]);
		entry.rx = (float)entry.sourceDegrees[0];
		entry.ry = (float)entry.sourceDegrees[1];
		entry.rz = (float)entry.sourceDegrees[2];
		entry.objectId = EditorModelId(entry.objectId);	// 0.3.DL custom models
		// Optional arguments are often script variables (Texture Studio writes
		// "object_world, object_int"); those keep the defaults rather than
		// dropping the object.
		int n = (int)args.size();
		auto optionalInt = [&](int index, int *out){
			int value;
			if(n > offset + index && pawnParseMaterialInt(args[offset + index], &value))
				*out = value;
		};
		auto optionalDistance = [&](int index, float *out){
			float value;
			if(n > offset + index && pawnParseDistance(args[offset + index], &value))
				*out = value;
		};
		if(valid && call->kind == PAWN_CALL_CREATE_EX){
			// CreateDynamicObjectEx(model, x, y, z, rx, ry, rz, streamdistance, drawdistance, ...)
			optionalDistance(7, &entry.streamDistance);
			optionalDistance(8, &entry.drawDistance);
		}else if(valid && call->streamer){
			// CreateDynamicObject(model, x, y, z, rx, ry, rz, worldid, interiorid,
			//     playerid, streamdistance, drawdistance, ...)
			optionalInt(7, &entry.world);
			optionalInt(8, &entry.interior);
			optionalDistance(10, &entry.streamDistance);
			optionalDistance(11, &entry.drawDistance);
		}else if(valid){
			// CreateObject / CreatePlayerObject(..., DrawDistance)
			optionalDistance(7, &entry.drawDistance);
		}
		if(!valid){
			if(numSkipped) (*numSkipped)++;
			continue;
		}
		if(entry.streamDistance == 300.0f)
			entry.streamDistance = 0.0f;	// STREAMER_OBJECT_SD
		if(call->streamer && call->kind == PAWN_CALL_CREATE)
			// ..., streamdistance, drawdistance, areaid, priority
			entry.areaEntry = pawnAreaEntryFor(args, offset + 12, handleEntries, entries);
		pawnCaptureAssignment(source, found, entry.handle, sizeof(entry.handle));
		pawnReadObjectTags(rawSource, afterCall, &entry);
		entries.push_back(entry);
		if(entry.handle[0])
			handleEntries[entry.handle] = (int)entries.size() - 1;
	}
}

static bool
pawnReadFile(const char *path, std::string &source)
{
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return false;
	char buffer[4096];
	while(true){
		size_t n = fread(buffer, 1, sizeof(buffer), f);
		if(n > 0) source.append(buffer, n);
		if(n < sizeof(buffer))
			break;
	}
	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

bool GetGroundPlacementSurface(rw::V3d pos, rw::V3d *hitPos, rw::V3d *hitNormal, bool ignoreSelection, bool forStanding);

static PawnImportReport gLastPawnImportReport;

static bool
txdChainContains(int txdSlot, int wantedSlot)
{
	for(int depth = 0; txdSlot >= 0 && depth < GetNumTxdSlots(); depth++){
		if(txdSlot == wantedSlot)
			return true;
		TxdDef *txd = GetTxdDef(txdSlot);
		if(txd == nil)
			break;
		txdSlot = txd->parentId;
	}
	return false;
}

// The messages are English for the agent and Map Check; the UI asks for them
// translated.
static const char *
materialTextureText(const char *english, bool translated)
{
	return translated ? Translate(english) : english;
}

int
CheckMaterialTexture(const ObjectMaterialOverride &material, char *buf, size_t size, bool translated)
{
	int model = material.hasTexture ? material.sourceModel : -1;
	if(model < 0){
		snprintf(buf, size, materialTextureText(N_("Model -1: keeps the original texture; only the color applies."), translated));
		return MATERIAL_TEXTURE_OK;
	}
	if(model == 0){
		snprintf(buf, size, materialTextureText(N_("Model 0: SA-MP's plain white \"blanktex\"; TXD and texture names are ignored."), translated));
		return MATERIAL_TEXTURE_OK;
	}
	ObjectDef *source = GetObjectDef(model);
	int pawnModel = PawnModelId(model);
	if(source == nil){
		snprintf(buf, size, materialTextureText(N_("Model %d does not exist, so SA-MP ignores the whole call (color too)."), translated), pawnModel);
		return MATERIAL_TEXTURE_ERROR;
	}
	if(IsCustomModel(model)){
		// A 0.3.DL / open.mp custom model's TXD has no stock name; its own is used.
		if(FindTextureInTxd(source->m_txdSlot, material.textureName) == nil){
			snprintf(buf, size, materialTextureText(N_("\"%s\" is not in custom model %d's TXD; the original texture stays."), translated),
				material.textureName, pawnModel);
			return MATERIAL_TEXTURE_WARNING;
		}
		snprintf(buf, size, materialTextureText(N_("Resolves in game: custom model %d's TXD has %s."), translated), pawnModel, material.textureName);
		return MATERIAL_TEXTURE_OK;
	}
	int txdSlot = FindTxdSlot(material.txdName);
	if(txdSlot < 0){
		snprintf(buf, size, materialTextureText(N_("TXD \"%s\" was not found; the original texture stays."), translated), material.txdName);
		return MATERIAL_TEXTURE_WARNING;
	}
	if(FindTextureInTxd(txdSlot, material.textureName) == nil){
		snprintf(buf, size, materialTextureText(N_("\"%s\" is not in TXD \"%s\" (or its parents); the original texture stays."), translated),
			material.textureName, material.txdName);
		return MATERIAL_TEXTURE_WARNING;
	}
	if(!txdChainContains(source->m_txdSlot, txdSlot)){
		snprintf(buf, size, materialTextureText(N_("Model %d (%s) does not use TXD \"%s\". SA-MP loads the model's TXD, "
			"then looks the texture up by TXD name, so this only works if \"%s\" is already loaded."), translated),
			pawnModel, source->m_name, material.txdName, material.txdName);
		return MATERIAL_TEXTURE_NOTE;
	}
	snprintf(buf, size, materialTextureText(N_("Resolves in game: model %d loads %s:%s."), translated), pawnModel,
		material.txdName, material.textureName);
	return MATERIAL_TEXTURE_OK;
}

void
AddPawnImportNote(const std::string &note)
{
	gLastPawnImportReport.notes.push_back(note);
}

const PawnImportReport&
GetLastPawnImportReport(void)
{
	return gLastPawnImportReport;
}

void
ClearLastPawnImportReport(void)
{
	gLastPawnImportReport = PawnImportReport();
}

int
ImportPawnCode(const char *path, int *numSkipped)
{
	gLastPawnImportReport = PawnImportReport();
	if(numSkipped)
		*numSkipped = 0;
	if(path == nil || path[0] == '\0')
		return -1;
	std::string source;
	if(!pawnReadFile(path, source)){
		log("ImportPawnCode: failed to read %s\n", path);
		return -1;
	}
	return ImportPawnSource(source, path, numSkipped, true);
}

// A map's objects for its start-screen thumbnail: its largest ones, as
// they would be placed. Other tools' maps get no thumbnail.
static bool
readMapThumbnailObjects(const char *path, std::vector<ThumbnailObject> &out)
{
	size_t n = strlen(path);
	bool pawn = n > 4 && (rw::strncmp_ci(path + n - 4, ".pwn", 4) == 0 || rw::strncmp_ci(path + n - 4, ".inc", 4) == 0);
	std::string source;
	if(!pawn || !pawnReadFile(path, source))
		return false;
	std::vector<PawnImportEntry> entries;
	std::vector<PawnRemoveEntry> removals;
	std::vector<PawnVehicleInteriorEntry> vehicleInteriors;
	std::vector<PawnMaterialImportEntry> materials;
	std::vector<PawnAttachImportEntry> attachments;
	std::vector<PawnElementSetterEntry> setters;
	int skipped = 0;
	pawnParseSource(DecodePawnFileText(source, nil), entries, removals, vehicleInteriors, materials,
		attachments, setters, &skipped);
	struct Sized { float radius; ThumbnailObject object; };
	std::vector<Sized> found;
	for(const PawnImportEntry &e : entries){
		ObjectDef *obj = GetObjectDef(e.objectId);
		if(e.isVehicle || e.elementKind != SAMP_ELEMENT_OBJECT || obj == nil || obj->m_isVehicle)
			continue;
		rw::Quat q = pawnMakeStoredQuaternion(e.rx, e.ry, e.rz);
		float radius = obj->m_colModel ? obj->m_colModel->boundingSphere.radius : 1.0f;
		found.push_back({ radius, { e.objectId, e.x, e.y, e.z, q.x, q.y, q.z, q.w, SAMP_ELEMENT_OBJECT } });
	}
	std::sort(found.begin(), found.end(), [](const Sized &a, const Sized &b){ return a.radius > b.radius; });
	if(found.size() > (size_t)MAX_THUMBNAIL_OBJECTS)
		found.resize(MAX_THUMBNAIL_OBJECTS);
	out.clear();
	for(const Sized &f : found)
		out.push_back(f.object);
	return !out.empty();
}

static PawnTextEncoding gLastImportTextEncoding = PAWN_TEXT_CP1252;

PawnTextEncoding
GetLastImportTextEncoding(void)
{
	return gLastImportTextEncoding;
}

// Pawn source from anywhere (a file, a converted Texture Studio map);
// path only names it in the log.
int
ImportPawnSource(const std::string &rawSource, const char *path, int *numSkipped, bool keepRotationNumbers)
{
	// pawno and Texture Studio maps are Windows-1252; the editor works in UTF-8.
	std::string source = DecodePawnFileText(rawSource, &gLastImportTextEncoding);
	EditorXML::Node document;std::string xmlError;
	int xmlState=path?EditorXML::ReadDocument(path,rawSource,"map",&document,&xmlError):0;
	if(xmlState>0&&!ValidateEditorDocument(document,&xmlError))xmlState=-1;
	if(xmlState==1){
		gLastImportTextEncoding=document.get("encoding")=="UTF-8"?PAWN_TEXT_UTF8:PAWN_TEXT_CP1252;
		source=gLastImportTextEncoding==PAWN_TEXT_UTF8?rawSource:Cp1252ToUtf8(rawSource);
	}
	PawnImportReport &report = gLastPawnImportReport;
	report = PawnImportReport();
	if(xmlState<0)report.notes.push_back("Could not load editor XML: "+xmlError+". The Pawn code was read; saving is blocked until the XML is repaired or moved.");
	if(xmlState==2)report.notes.push_back("Pawn changed outside the editor. Runtime values were read from Pawn; matching editor metadata was kept from XML.");
	gPawnStringDefines.clear();
	gPawnUnresolvedMacros.clear();
	pawnCollectStringDefines(source, gPawnStringDefines);
	if(numSkipped)
		*numSkipped = 0;
	if(!isSA()){
		log("ImportPawnCode: Pawn object code is only supported for GTA San Andreas\n");
		return -1;
	}
	std::vector<PawnImportEntry> entries;
	std::vector<PawnRemoveEntry> removals;
	std::vector<PawnVehicleInteriorEntry> vehicleInteriors;
	std::vector<PawnMaterialImportEntry> materials;
	std::vector<PawnAttachImportEntry> attachments;
	std::vector<PawnElementSetterEntry> setters;
	int skipped = 0;
	pawnParseSource(source, entries, removals, vehicleInteriors, materials, attachments,
		setters, &skipped);
	if(xmlState==1&&document.child("removals")){
		std::vector<PawnAreaRemoval> snapshot;
		if(ReadMapXMLRemovals(document,snapshot,&xmlError)){
			removals.erase(std::remove_if(removals.begin(),removals.end(),[](const PawnRemoveEntry &r){return r.objectId==-1;}),removals.end());
			for(const PawnAreaRemoval &r:snapshot)removals.push_back({-1,r.x,r.y,r.z,r.radius});
		}
	}
	pawnLinkRaceCheckpoints(entries);
	report.unreadableCalls = skipped;
	if(!gPawnUnresolvedMacros.empty()){
		std::string names;
		for(size_t i = 0; i < gPawnUnresolvedMacros.size() && i < 8; i++)
			names += (i ? ", " : "") + gPawnUnresolvedMacros[i];
		if(gPawnUnresolvedMacros.size() > 8)
			names += ", ...";
		char note[2048];
		// English: the agent reads the notes too.
		snprintf(note, sizeof(note), "Text uses %s, defined neither in this map nor in the workspace's "
			"gamemodes or includes, so it shows without them (often a colour such as {FF6347}). "
			"Saving writes the text without them too.", names.c_str());
		report.notes.push_back(note);
	}
	// Textdraws now have their own file; a map's stay read-only until moved there.
	size_t previousTextDraws=GetMapTextDraws().size();
	int numTextDraws = ImportTextDrawsFromSource(source, GetMapTextDraws());
	if(numTextDraws > 0)
		log("ImportPawnCode: %d textdraw(s) from %s, read-only in the map\n", numTextDraws, path);
	if(entries.empty() && removals.empty() && materials.empty() && xmlState<=0){
		if(numSkipped) *numSkipped = skipped;
		log("ImportPawnCode: no supported Pawn object calls found in %s\n", path);
		return 0;
	}

	int numRemoved = 0;
	if(!removals.empty()){
		std::vector<ObjectInst*> matches;
		std::vector<bool> removalMatched(removals.size(), false);
		for(size_t i = 0; i < removals.size(); i++){
			const PawnRemoveEntry &removal = removals[i];
			// Model -1: every building in the sphere, HD and LOD; kept as one
			// area removal, exported as written.
			bool area = removal.objectId == -1;
			if(area){
				bool known = false;
				for(const PawnAreaRemoval &r : gAreaRemovals)
					known |= r.x == removal.x && r.y == removal.y && r.z == removal.z && r.radius == removal.radius;
				if(!known)
					gAreaRemovals.push_back({ removal.x, removal.y, removal.z, removal.radius });
				removalMatched[i] = true;	// kept even with nothing inside it
			}
			for(CPtrNode *p = instances.first; p; p = p->next){
				ObjectInst *inst = (ObjectInst*)p->item;
				if(!IsPawnWorldObject(inst) || inst->m_isDeleted ||
				   (!area && inst->m_objectId != removal.objectId))
					continue;
				// As the client does: the building's position within the radius.
				if(!insideRemovalSphere(inst, removal.x, removal.y, removal.z, removal.radius))
					continue;
				bool alreadyMatched = false;
				for(size_t j = 0; j < matches.size(); j++)
					if(matches[j] == inst){ alreadyMatched = true; break; }
				if(!alreadyMatched){
					if(matches.size() < MAX_BATCH_OBJECTS)
						matches.push_back(inst);
					else
						skipped++;
				}
				removalMatched[i] = true;
			}
		}
		if(!matches.empty()){
			// RemoveBuildingForPlayer removes exactly the listed model, so no
			// LOD/HD cascade here.
			UndoRecordDelete(&matches[0], (int)matches.size());
			for(size_t i = 0; i < matches.size(); i++)
				setDeletedWithoutCascade(matches[i], true);
			numRemoved = (int)matches.size();
		}
		for(size_t i = 0; i < removalMatched.size(); i++)
			if(!removalMatched[i]){
				skipped++;
				report.unmatchedRemovals++;
			}
	}

	std::vector<ObjectInst*> created;
	std::vector<ObjectInst*> createdForEntry(entries.size(), nil);
	// Radar art's zones go back into layers, not into the map as elements;
	// zones grouped "radar art" by an older Haumea too.
	std::vector<RadarArtLayer> radarLayers;
	std::unordered_map<size_t, std::pair<size_t, size_t>> radarZoneForEntry;	// entry: layer, zone
	int radarZones = 0;
	if(!entries.empty()){
		SetCustomPlacementIpl(DEFAULT_CUSTOM_IPL_PATH, nil, false);
		GameFile *file = GetOrCreateCustomIplFile();
		int maxIdx = GetMaxIplIndexForFile(file);
		for(size_t i = 0; i < entries.size(); i++){
			PawnImportEntry &e = entries[i];
			// An older Haumea's "radar art" zones join a layer too, unless they
			// carry something a layer can't keep: a note or a hand-written name.
			bool generatedHandle = e.handle[0] == '\0' ||
				(strncmp(e.handle, "Haumea_", 7) == 0 && strstr(e.handle, "_zone_"));
			bool olderRadarArt = strcmp(e.group, "radar art") == 0 && e.note[0] == '\0' && generatedHandle;
			if(e.elementKind == SAMP_ELEMENT_GANGZONE && (e.radarArt || olderRadarArt)){
				size_t l = 0;
				while(l < radarLayers.size() && (strcmp(radarLayers[l].group, e.group) != 0 ||
				      radarLayers[l].zoneMode != e.element.zoneMode))
					l++;
				if(l == radarLayers.size()){
					radarLayers.emplace_back();
					snprintf(radarLayers[l].group, sizeof(radarLayers[l].group), "%s",
						e.group[0] ? e.group : "radar art");
					radarLayers[l].zoneMode = e.element.zoneMode;
				}
				float hx = e.element.zoneSizeX * 0.5f, hy = e.element.zoneSizeY * 0.5f;
				RadarArtZone z = { e.x - hx, e.y - hy, e.x + hx, e.y + hy, 0.0f, e.element.zoneColor };
				radarZoneForEntry[i] = { l, radarLayers[l].zones.size() };
				radarLayers[l].zones.push_back(z);
				radarZones++;
				continue;
			}
			ObjectDef *obj = GetObjectDef(e.objectId);
			if(obj == nil && RemapLegacyModel(e.objectId) >= 0){
				e.objectId = RemapLegacyModel(e.objectId);	// an old import, now a custom model
				obj = GetObjectDef(e.objectId);
			}
			if(obj == nil || obj->m_isVehicle != e.isVehicle){
				skipped++;
				report.unknownModelObjects++;
				if(std::find(report.unknownModels.begin(), report.unknownModels.end(), e.objectId) ==
				   report.unknownModels.end())
					report.unknownModels.push_back(e.objectId);
				log("ImportPawnCode: skipping unknown model ID %d\n", e.objectId);
				continue;
			}
			rw::V3d position = { e.x, e.y, e.z };
			if(e.elementKind == SAMP_ELEMENT_GANGZONE){
				// Gang zones are flat; stand them on the ground for editing.
				rw::V3d probe = { e.x, e.y, 0.0f }, hit, normal;
				position.z = GetGroundPlacementSurface(probe, &hit, &normal, true) ? hit.z : 0.0f;
			}
			rw::Quat rotation = pawnMakeStoredQuaternion(e.rx, e.ry, e.rz);
			ObjectInst *inst = createSpawnedInstance(e.objectId, position, file, ++maxIdx, &rotation);
			// Objects only: a class's, vehicle's or actor's heading is one
			// number, written as the export always wrote it (270 as -90).
			if(keepRotationNumbers && e.elementKind == SAMP_ELEMENT_OBJECT && !e.isVehicle){
				// -0.0 reads back as 0.0: never write it as -0.000000.
				for(int k = 0; k < 3; k++)
					inst->m_sourceDegrees[k] = e.sourceDegrees[k] + 0.0;
				inst->m_sourceRotation = inst->m_rotation;
				inst->m_hasSourceDegrees = true;
			}
			inst->m_area = e.interior >= 0 ? e.interior : 0;
			if(e.isVehicle){
				inst->m_vehicleStatic = e.vehicleStatic;
				inst->m_vehicleColor1 = e.vehicleColor1;
				inst->m_vehicleColor2 = e.vehicleColor2;
				inst->m_vehicleRespawnDelay = e.vehicleRespawnDelay;
				inst->RefreshVehicleColors();
				// A hand-written variable name stays; generated ones are made again.
				bool generated = strncmp(e.handle, "Haumea_", 7) == 0 && strstr(e.handle, "_vehicle_");
				if(!generated && IsValidPawnObjectName(e.handle) && FindObjectByPawnName(e.handle) == nil){
					ObjectPawnProperties properties;
					InitObjectPawnProperties(&properties);
					strncpy(properties.name, e.handle, sizeof(properties.name)-1);
					SetObjectPawnProperties(inst, properties, false);
				}
			}else{
				ObjectPawnProperties properties;
				InitObjectPawnProperties(&properties);
				if(e.elementKind != SAMP_ELEMENT_OBJECT){
					properties = e.element;
					properties.elementKind = e.elementKind;
				}
				// Only native calls and streamer calls without one leave it at -1.
				properties.interiorZeroOnly = e.interior == 0;
				// Keep hand-written variable names; generated and temporary
				// handles are recreated on export.
				bool generated = strcmp(e.handle, "tmpobjid") == 0 ||
					(strncmp(e.handle, "Haumea_", 7) == 0 &&
					 (strstr(e.handle, "_obj_") || strstr(e.handle, "_zone_") ||
					  strstr(e.handle, "_actor_") || strstr(e.handle, "_npc_") || strstr(e.handle, "_area_")));
				if(!generated && IsValidPawnObjectName(e.handle) &&
				   FindObjectByPawnName(e.handle) == nil)
					strncpy(properties.name, e.handle, sizeof(properties.name)-1);
				strncpy(properties.group, e.group, sizeof(properties.group)-1);
				strncpy(properties.comment, e.note, sizeof(properties.comment)-1);
				properties.drawDistance = e.drawDistance;
				properties.streamDistance = e.streamDistance;
				properties.virtualWorld = e.world;
				SetObjectPawnProperties(inst, properties, false);
			}
			createdForEntry[i] = inst;
			created.push_back(inst);
		}
	}
	// Dynamic areas: each keeps its variable name, or gets one from it when an
	// earlier area of the map took it (scripts reuse "areaid" in each block);
	// items limited to an area then name their own area.
	for(size_t i = 0; i < entries.size(); i++){
		ObjectInst *area = createdForEntry[i];
		if(area == nil || entries[i].elementKind != SAMP_ELEMENT_AREA || GetObjectPawnProperties(area)->name[0])
			continue;
		ObjectPawnProperties properties = *GetObjectPawnProperties(area);
		MakeUniqueAreaName(entries[i].handle, properties.name, sizeof(properties.name));
		SetObjectPawnProperties(area, properties, false);
	}
	for(size_t i = 0; i < entries.size(); i++){
		int areaEntry = entries[i].areaEntry - 1;
		ObjectInst *item = createdForEntry[i];
		if(areaEntry < 0 || item == nil || item->m_isVehicle || createdForEntry[areaEntry] == nil)
			continue;
		ObjectPawnProperties properties = *GetObjectPawnProperties(item);
		snprintf(properties.areaName, sizeof(properties.areaName), "%s",
			GetObjectPawnProperties(createdForEntry[areaEntry])->name);
		SetObjectPawnProperties(item, properties, false);
	}
	// NPCs in vehicles, once both exist.
	for(size_t i = 0; i < entries.size(); i++){
		int vehicleEntry = entries[i].npcVehicleEntry - 1;
		ObjectInst *npc = createdForEntry[i];
		if(vehicleEntry < 0 || npc == nil || createdForEntry[vehicleEntry] == nil)
			continue;
		ObjectPawnProperties properties = *GetObjectPawnProperties(npc);
		properties.npcVehicle = createdForEntry[vehicleEntry]->m_id;
		properties.npcSeat = entries[i].element.npcSeat;
		SetObjectPawnProperties(npc, properties, false);
	}
	for(size_t i = 0; i < attachments.size(); i++){
		const PawnAttachImportEntry &attach = attachments[i];
		ObjectInst *child = createdForEntry[attach.child];
		ObjectInst *parent = attach.parent >= 0 ? createdForEntry[attach.parent] : nil;
		bool playerTarget = attach.parent == PAWN_ATTACH_PLAYER;
		bool validParent = attach.toVehicle ? parent && parent->m_isVehicle :
			playerTarget || (attach.bone >= 1 ? IsPlayerAttachmentTarget(parent) :
				parent && !parent->m_isVehicle && GetSampElementKind(parent) == SAMP_ELEMENT_OBJECT);
		if(child == nil || child->m_isVehicle || !validParent || parent == child){
			skipped++;
			report.unresolvedLinks++;
			continue;
		}
		ObjectPawnProperties properties = *GetObjectPawnProperties(child);
		properties.attachParent = playerTarget ? PAWN_ATTACH_PLAYER : parent->m_id;
		properties.attachOffset = { attach.x, attach.y, attach.z };
		properties.attachRotation = { attach.rx, attach.ry, attach.rz };
		properties.attachSyncRotation = attach.toVehicle || attach.bone >= 1 ? true : attach.syncRotation;
		properties.attachBone = attach.bone;
		properties.attachSlot = attach.slot;
		properties.attachScale = attach.scale;
		properties.attachColor1 = attach.color1;
		properties.attachColor2 = attach.color2;
		properties.attachSkin = playerTarget ? 1 :
			parent && GetSampElementKind(parent) == SAMP_ELEMENT_CLASS ?
				GetObjectPawnProperties(parent)->elementModel : 0;
		SetObjectPawnProperties(child, properties, false);
	}
	for(size_t i = 0; i < setters.size(); i++){
		const PawnElementSetterEntry &setter = setters[i];
		auto radarZone = radarZoneForEntry.find(setter.entryIndex);
		if(radarZone != radarZoneForEntry.end()){
			if(setter.field == PawnElementSetterEntry::ZONE_COLOR)
				radarLayers[radarZone->second.first].zones[radarZone->second.second].colour = setter.color;
			continue;
		}
		ObjectInst *target = createdForEntry[setter.entryIndex];
		int kind = GetSampElementKind(target);
		bool zoneSetter = setter.field == PawnElementSetterEntry::ZONE_COLOR;
		if(target == nil || kind != (zoneSetter ? SAMP_ELEMENT_GANGZONE : SAMP_ELEMENT_ACTOR)){
			skipped++;
			report.unresolvedLinks++;
			continue;
		}
		ObjectPawnProperties properties = *GetObjectPawnProperties(target);
		switch(setter.field){
		case PawnElementSetterEntry::WORLD: properties.virtualWorld = (int)setter.value; break;
		case PawnElementSetterEntry::INVULNERABLE: properties.actorInvulnerable = setter.value != 0.0f; break;
		case PawnElementSetterEntry::HEALTH: properties.actorHealth = setter.value; break;
		case PawnElementSetterEntry::ZONE_COLOR: properties.zoneColor = setter.color; break;
		}
		SetObjectPawnProperties(target, properties, false);
	}
	for(size_t i = 0; i < vehicleInteriors.size(); i++){
		PawnVehicleInteriorEntry &link = vehicleInteriors[i];
		ObjectInst *target = createdForEntry[link.entryIndex];
		if(target && target->m_isVehicle && link.field == PawnVehicleInteriorEntry::COMPONENT &&
		   !IsVehicleComponentValid(target->m_objectId, link.interior)){
			skipped++;	// SA-MP refuses parts that don't fit the model
			continue;
		}
		if(target && target->m_isVehicle){
			switch(link.field){
			case PawnVehicleInteriorEntry::INTERIOR: target->m_area = link.interior; break;
			case PawnVehicleInteriorEntry::PAINTJOB: target->m_vehiclePaintjob = link.interior + 1; break;
			case PawnVehicleInteriorEntry::COMPONENT:
				target->m_vehicleMods[GetVehicleComponentSlot(link.interior)] = (uint16)link.interior;
				break;
			case PawnVehicleInteriorEntry::PLATE:
				snprintf(target->m_vehiclePlate, sizeof(target->m_vehiclePlate), "%s", link.plate);
				break;
			}
			if(link.field != PawnVehicleInteriorEntry::INTERIOR)
				target->RefreshVehicleUpgrades();
		}else{
			skipped++;
			report.unresolvedLinks++;
		}
	}

	// Material calls refer to the handles emitted by the exporter (or handles
	// used by a hand-written Pawn file). Numeric runtime IDs are intentionally
	// not guessed because they are not stable across server runs.
	for(size_t i = 0; i < materials.size(); i++){
		PawnMaterialImportEntry &materialEntry = materials[i];
		ObjectInst *target = createdForEntry[materialEntry.entryIndex];
		if(target == nil || target->m_isVehicle || (!materialEntry.hasMaterial && !materialEntry.hasText)){
			skipped++;
			report.unresolvedLinks++;
			continue;
		}
		// The entry already holds the parsed fields of both calls; only the
		// later one is in effect in game, the other is kept for editing.
		ObjectMaterialOverride material = materialEntry.material;
		material.active = materialEntry.hasMaterial;
		material.textEnabled = materialEntry.hasText && materialEntry.textIsLast;
		SetObjectMaterialOverride(target, materialEntry.slot, material, false);
	}
	if(xmlState>0){
		std::unordered_map<std::string,ObjectInst*> xmlHandles;
		for(size_t i=0;i<entries.size();i++)if(createdForEntry[i]&&entries[i].handle[0])xmlHandles[entries[i].handle]=createdForEntry[i];
		if(!ApplyMapXML(document,xmlState==1,created,radarLayers,previousTextDraws,xmlHandles,&xmlError))
			report.notes.push_back("Could not restore editor XML: "+xmlError);
	}
	// Place attached objects on their parents now, as the game does.
	UpdateObjectAttachments();

	ClearSelection();
	for(size_t i = 0; i < created.size(); i++)
		created[i]->Select();
	UndoBeginGroup();
	if(!created.empty()){
		UndoRecordPaste(&created[0], (int)created.size());
	}
	if(!radarLayers.empty()){
		// Added to the map's layers: a merge's zones join a layer of the same group.
		std::vector<RadarArtLayer> layers = GetRadarArtLayers();
		for(const RadarArtLayer &in : radarLayers){
			size_t l = 0;
			while(l < layers.size() && (strcmp(layers[l].group, in.group) != 0 || layers[l].zoneMode != in.zoneMode))
				l++;
			if(l == layers.size())
				layers.push_back(in);
			else
				layers[l].zones.insert(layers[l].zones.end(), in.zones.begin(), in.zones.end());
		}
		SetRadarArtLayers(layers, true);
	}
	UndoEndGroup();
	if(numSkipped) *numSkipped = skipped;
	if(created.size() > MAX_BATCH_OBJECTS)
		Toast(TOAST_SPAWN, _("Undo limited to first %d of %d imported objects"), MAX_BATCH_OBJECTS, (int)created.size());
	report.placed = (int)created.size();
	report.removed = numRemoved;
	std::sort(report.unknownModels.begin(), report.unknownModels.end());
	log("ImportPawnCode: placed %d object(s), %d radar art zone(s), removed %d world object(s), skipped %d from %s\n",
		(int)created.size(), radarZones, numRemoved, skipped, path);
	if(created.empty() && numRemoved == 0 && radarZones == 0)
		return 0;
	return (int)created.size() + numRemoved + radarZones;
}

static bool
prefabNextToken(const char **cursor, char *out, size_t outSize)
{
	if(cursor == nil || *cursor == nil)
		return false;
	const char *p = *cursor;
	while(*p && isspace((unsigned char)*p)) p++;
	if(*p == '\0'){
		*cursor = p;
		return false;
	}
	size_t written = 0;
	if(*p == '"'){
		p++;
		while(*p && *p != '"'){
			char c = *p++;
			if(c == '\\' && *p){
				char escaped = *p++;
				switch(escaped){
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case '\\': c = '\\'; break;
				case '"': c = '"'; break;
				default: c = escaped; break;
				}
			}
			if(out && outSize > 1 && written + 1 < outSize)
				out[written++] = c;
		}
		if(*p == '"') p++;
	}else{
		while(*p && !isspace((unsigned char)*p)){
			if(out && outSize > 1 && written + 1 < outSize)
				out[written++] = *p;
			p++;
		}
	}
	if(out && outSize > 0)
		out[written < outSize ? written : outSize-1] = '\0';
	*cursor = p;
	return true;
}

static bool
prefabParseIntToken(const char *token, int *out)
{
	if(token == nil || token[0] == '\0')
		return false;
	char *end = nil;
	long value = strtol(token, &end, 0);
	if(end == token || *end != '\0' || value < INT_MIN || value > INT_MAX)
		return false;
	if(out) *out = (int)value;
	return true;
}

static bool
prefabParseColorToken(const char *token, rw::RGBA *out)
{
	if(token == nil || token[0] == '\0')
		return false;
	char *end = nil;
	unsigned long value = strtoul(token, &end, 0);
	if(end == token || *end != '\0')
		return false;
	if(out){
		uint32 color = (uint32)value;
		*out = {
			(uint8)((color >> 16) & 0xFF),
			(uint8)((color >> 8) & 0xFF),
			(uint8)(color & 0xFF),
			(uint8)((color >> 24) & 0xFF)
		};
	}
	return true;
}

int
ImportPrefabAt(const char *path, rw::V3d spawnPos)
{
	return ImportPrefabAtEx(path, spawnPos, 0.0f, nil);
}

// A prefab entry turned by heading degrees about Z around the spawn point.
static void
prefabEntryPlace(const PrefabEntry *e, const rw::V3d &spawnPos, float heading, rw::V3d *pos, rw::Quat *rot)
{
	float r = heading * (float)M_PI / 180.0f, c = cosf(r), s = sinf(r);
	pos->x = spawnPos.x + e->relX * c - e->relY * s;
	pos->y = spawnPos.y + e->relX * s + e->relY * c;
	pos->z = spawnPos.z + e->relZ;
	rw::Quat own = { e->rotX, e->rotY, e->rotZ, e->rotW };
	*rot = heading == 0.0f ? own : ApplyObjectRotationDelta(own, { 0.0f, 0.0f, heading }, true);
}

int
ImportPrefabAtEx(const char *path, rw::V3d spawnPos, float heading, const char *group)
{
	// On the heap: 256 entries are about 2 MB, past the 1 MB a Windows
	// thread's stack has (placing any prefab crashed there with a stack overflow).
	std::vector<PrefabEntry> entryStore(256);
	PrefabEntry *entries = entryStore.data();
	int numEntries = 0;
	if(!ReadPrefabEntries(path, entries, 256, &numEntries, true))
		return 0;

	GameFile *file = GetOrCreateCurrentPlacementIplFile();
	int maxIdx = GetMaxIplIndexForFile(file);

	// Pass 1: create all instances
	ObjectInst *created[256];
	ObjectInst *pasted[256];
	for(int i = 0; i < 256; i++)
		created[i] = nil;
	int numPasted = 0;

	for(int i = 0; i < numEntries; i++){
		PrefabEntry *e = &entries[i];

		ObjectDef *obj = GetObjectDef(e->objectId);
		if(obj == nil && RemapLegacyModel(e->objectId) >= 0){
			e->objectId = RemapLegacyModel(e->objectId);	// an old import, now a custom model
			obj = GetObjectDef(e->objectId);
		}
		if(obj == nil){
			std::vector<int> unknown(1, e->objectId), legacy;
			if(FindLegacyImports(unknown, legacy) > 0)
				Toast(TOAST_SPAWN, _("Model %d was a modloader/Haumea import: open a map that uses it to move it to the server"), e->objectId);
			log("ImportPrefab: skipping unknown objectId %d (%s)\n", e->objectId, e->modelName);
			created[i] = nil;
			continue;
		}

		ObjectInst *inst = AddInstance();
		inst->m_objectId = e->objectId;
		inst->m_isVehicle = obj->m_isVehicle;
		inst->m_vehicleStatic = true;
		inst->m_vehicleColor1 = -1;
		inst->m_vehicleColor2 = -1;
		inst->m_vehicleRespawnDelay = 100;
		if(e->hasVehicle && inst->m_isVehicle){
			inst->m_vehicleStatic = e->vehicleStatic;
			inst->m_vehicleColor1 = e->vehicleColor1;
			inst->m_vehicleColor2 = e->vehicleColor2;
			inst->m_vehicleRespawnDelay = e->vehicleRespawnDelay;
			inst->m_vehiclePaintjob = e->vehiclePaintjob;
			memcpy(inst->m_vehicleMods, e->vehicleMods, sizeof(inst->m_vehicleMods));
			memcpy(inst->m_vehiclePlate, e->vehiclePlate, sizeof(inst->m_vehiclePlate));
		}
		inst->m_area = e->area & 0xFF;
		if(isSA()){
			if(e->area & 0x100) inst->m_isUnimportant = true;
			if(e->area & 0x400) inst->m_isUnderWater = true;
			if(e->area & 0x800) inst->m_isTunnel = true;
			if(e->area & 0x1000) inst->m_isTunnelTransition = true;
		}
		prefabEntryPlace(e, spawnPos, heading, &inst->m_translation, &inst->m_rotation);
		inst->m_lodId = -1;
		inst->m_lod = nil;
		inst->m_numChildren = 0;
		inst->m_file = file;
		inst->m_imageIndex = -1;
		inst->m_binInstIndex = -1;
		inst->m_iplIndex = ++maxIdx;
		SetInstIplFilterKey(inst, file ? file->name : nil);
		inst->m_isAdded = true;
		inst->m_iplRotation = IsIplMapDocumentOpen();
		inst->m_isDirty = true;
		inst->m_savedStateValid = false;
		inst->m_wasSavedDeleted = false;
		inst->m_gameEntityExists = false;
		bool hasMaterials = false;
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++)
			if(materialOverrideIsActive(e->materials[slot])){
				hasMaterials = true;
				break;
			}
		if(hasMaterials){
			ObjectMaterialOverride *overrides = ensureObjectMaterialOverrides(inst);
			memcpy(overrides, e->materials,
			       sizeof(ObjectMaterialOverride) * MAX_PAWN_MATERIALS);
		}
		inst->m_materialGeometryApplied = false;
		inst->m_materialBaseGeometry = nil;
		inst->m_materialCustomGeometry = nil;
		StampChangeSeq(inst);

		if(obj->m_isBigBuilding)
			inst->SetupBigBuilding();

		inst->UpdateMatrix();

		if(!obj->IsLoaded()){
			RequestObject(e->objectId);
			LoadAllRequestedObjects();
		}

		inst->CreateRwObject();

		if(obj->m_colModel)
			InsertInstIntoSectors(inst);
		else{
			CPtrList *list = inst->m_isBigBuilding
				? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
			list->InsertItem(inst);
		}

		created[i] = inst;
		if(numPasted < 256)
			pasted[numPasted++] = inst;
	}

	// A dynamic area whose name the map already uses gets a new one, and the
	// prefab's own items follow it there.
	std::map<std::string, std::string> renamedAreas;
	for(int i = 0; i < numEntries; i++){
		ObjectPawnProperties &properties = entries[i].properties;
		if(created[i] == nil || properties.elementKind != SAMP_ELEMENT_AREA || !properties.name[0] ||
		   !FindObjectByPawnName(properties.name, created[i]))
			continue;
		char renamed[PAWN_OBJECT_NAME_LEN];
		MakeUniqueAreaName(properties.name, renamed, sizeof(renamed));
		renamedAreas[properties.name] = renamed;
		snprintf(properties.name, sizeof(properties.name), "%s", renamed);
	}
	for(int i = 0; i < numEntries && !renamedAreas.empty(); i++){
		std::map<std::string, std::string>::iterator it = renamedAreas.find(entries[i].properties.areaName);
		if(it != renamedAreas.end())
			snprintf(entries[i].properties.areaName, sizeof(entries[i].properties.areaName), "%s", it->second.c_str());
	}
	// SA-MP properties and attachments within the prefab. Names stay unique,
	// so a second copy of a named prefab gets automatic handles instead.
	for(int i = 0; i < numEntries; i++){
		if(created[i] == nil || created[i]->m_isVehicle)
			continue;
		ObjectPawnProperties properties = entries[i].properties;
		if(properties.name[0] && FindObjectByPawnName(properties.name, created[i]))
			properties.name[0] = '\0';
		if(properties.elementKind == SAMP_ELEMENT_NPC &&
		   FindNpcByName(properties.npcName, created[i]))
			MakeUniqueNpcName(properties.npcName, properties.npcName, sizeof(properties.npcName));
		int parentIndex = entries[i].attachParentIndex;
		if(parentIndex == PAWN_ATTACH_PLAYER)
			properties.attachParent = PAWN_ATTACH_PLAYER;
		else if(parentIndex >= 0 && parentIndex < numEntries && created[parentIndex])
			properties.attachParent = created[parentIndex]->m_id;
		else
			properties.attachParent = -1;
		int vehicleIndex = entries[i].npcVehicleIndex;
		if(vehicleIndex >= 0 && created[vehicleIndex] && created[vehicleIndex]->m_isVehicle)
			properties.npcVehicle = created[vehicleIndex]->m_id;
		else
			properties.npcVehicle = -1;
		if(group)
			snprintf(properties.group, sizeof(properties.group), "%s", group);
		SetObjectPawnProperties(created[i], properties, false);
	}
	// Vehicles keep their own settings; only the group is shared.
	for(int i = 0; group && i < numEntries; i++)
		if(created[i] && created[i]->m_isVehicle){
			ObjectPawnProperties properties = *GetObjectPawnProperties(created[i]);
			snprintf(properties.group, sizeof(properties.group), "%s", group);
			SetObjectPawnProperties(created[i], properties, false);
		}
	UpdateObjectAttachments();

	// Pass 2: link LODs
	for(int i = 0; i < numEntries; i++){
		if(created[i] == nil)
			continue;
		int lr = entries[i].lodRef;
		if(lr >= 0 && lr < numEntries && created[lr] != nil)
			finalizeLinkedLod(created[i], created[lr]);
	}

	// Select all spawned instances
	ClearSelection();
	for(int i = 0; i < numEntries; i++){
		if(created[i] != nil)
			created[i]->Select();
	}

	if(numPasted > 0){
		if(numPasted > 64)
			Toast(TOAST_SPAWN, _("Undo limited to first 64 of %d instances"), numPasted);
		UndoRecordPaste(pasted, numPasted > 64 ? 64 : numPasted);
	}

	log("ImportPrefab: placed %d instance(s) from %s\n", numPasted, path);
	return numPasted;
}

int
ImportPrefab(const char *path)
{
	rw::V3d spawnPos = add(TheCamera.m_position, scale(TheCamera.m_at, 50.0f));
	return ImportPrefabAt(path, spawnPos);
}

// The entries of the prefab being placed, read once per file version.
static bool
cachedPrefabEntries(const char *path, PrefabEntry **entries, int *numEntries)
{
	static std::string cachedPath;
	static time_t cachedTime;
	static std::vector<PrefabEntry> cached;
	static int checkedFrame;
	int frame = ImGui::GetFrameCount();
	if(cachedPath != path || frame - checkedFrame > 30){
		checkedFrame = frame;
		struct stat st;
		time_t mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
		if(cachedPath != path || mtime != cachedTime){
			cachedPath = path;
			cachedTime = mtime;
			cached.assign(256, PrefabEntry());
			int n = 0;
			if(!ReadPrefabEntries(path, cached.data(), 256, &n, false))
				n = 0;
			cached.resize(n);
		}
	}
	*entries = cached.data();
	*numEntries = (int)cached.size();
	return *numEntries > 0;
}

void
RenderPrefabPlacementGhost(const char *path, rw::V3d spawnPos, float heading)
{
	PrefabEntry *entries;
	int numEntries = 0;
	if(path == nil || path[0] == '\0' || !cachedPrefabEntries(path, &entries, &numEntries))
		return;
	rw::RGBA col = { 80, 180, 255, 95 };
	for(int i = 0; i < numEntries; i++){
		rw::V3d pos;
		rw::Quat rot;
		prefabEntryPlace(&entries[i], spawnPos, heading, &pos, &rot);
		if(ElementKindUsesIcon(entries[i].properties.elementKind))
			RenderElementIconGhost(entries[i].properties.elementKind, pos);
		else
			RenderPlacementGhost(entries[i].objectId, pos, rot, col);
	}
}

// What the library shows about a prefab file: its tags and how many
// instances and SA-MP elements it holds, from the header and element lines.
bool
ReadPrefabInfo(const char *path, PrefabInfo *info)
{
	*info = PrefabInfo();
	FILE *f = fopen(path, "r");
	if(f == nil)
		return false;
	char line[4096];
	bool header = false;
	while(fgets(line, sizeof(line), f)){
		if(strncmp(line, "HAUMEA_PREFAB", 13) == 0)
			header = true;
		else if(strncmp(line, "tags ", 5) == 0){
			std::string tags = line + 5;
			while(!tags.empty() && (tags.back() == '\n' || tags.back() == '\r'))
				tags.pop_back();
			info->tags = tags;
		}else if(strncmp(line, "count ", 6) == 0)
			sscanf(line, "count %d", &info->instances);
		else if(strncmp(line, "element ", 8) == 0)
			info->elements++;
		else if(strncmp(line, "vehicle ", 8) == 0)
			info->vehicles++;
	}
	fclose(f);
	struct stat st;
	info->mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
	return header;
}

int numSectorsX, numSectorsY;
CRect worldBounds;
Sector *sectors;
Sector outOfBoundsSector;

void
InitSectors(void)
{
	switch(params.map){
	case GAME_III:
		numSectorsX = 100;
		numSectorsY = 100;
		worldBounds.left = -2000.0f;
		worldBounds.bottom = -2000.0f;
		worldBounds.right = 2000.0f;
		worldBounds.top = 2000.0f;
		break;
	case GAME_VC:
		numSectorsX = 80;
		numSectorsY = 80;
		worldBounds.left = -2400.0f;
		worldBounds.bottom = -2000.0f;
		worldBounds.right = 1600.0f;
		worldBounds.top = 2000.0f;
		break;
	case GAME_SA:
		// Match SA-MP's default world bounds so streamed Pawn maps do not
		// unexpectedly fall into the editor's out-of-bounds bucket.
		numSectorsX = 240;
		numSectorsY = 240;
		worldBounds.left = (float)-SA_MP_WORLD_BOUND;
		worldBounds.bottom = (float)-SA_MP_WORLD_BOUND;
		worldBounds.right = (float)SA_MP_WORLD_BOUND;
		worldBounds.top = (float)SA_MP_WORLD_BOUND;
		break;
	}
	sectors = new Sector[numSectorsX*numSectorsY];
}

Sector*
GetSector(int ix, int iy)
{
	return &sectors[ix*numSectorsY + iy];
}

//Sector*
//GetSector(float x, float y)
//{
//	int i, j;
//	assert(x > worldBounds.left && x < worldBounds.right);
//	assert(y > worldBounds.bottom && y < worldBounds.top);
//	i = (x + worldBounds.right - worldBounds.left)/numSectorsX;
//	j = (y + worldBounds.top - worldBounds.bottom)/numSectorsY;
//	return GetSector(i, j);
//}

int
GetSectorIndexX(float x)
{
	if(x < worldBounds.left) x = worldBounds.left;
	if(x >= worldBounds.right) x = worldBounds.right - 1.0f;
	return (x - worldBounds.left) / ((worldBounds.right - worldBounds.left) / numSectorsX);
}

int
GetSectorIndexY(float y)
{
	if(y < worldBounds.bottom) y = worldBounds.bottom;
	if(y >= worldBounds.top) y = worldBounds.top - 1.0f;
	return (y - worldBounds.bottom) / ((worldBounds.top - worldBounds.bottom) / numSectorsY);
}

bool
IsInstInBounds(ObjectInst *inst)
{
	ObjectDef *obj;
	// Some objects don't have collision data
	// TODO: figure out what to do with them
	obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return false;
	CRect bounds = inst->GetBoundRect();
	return bounds.left >= worldBounds.left &&
		bounds.right < worldBounds.right &&
		bounds.bottom >= worldBounds.bottom &&
		bounds.top < worldBounds.top;
}

static void
removeFromList(CPtrList *list, ObjectInst *inst)
{
	CPtrNode *p, *next;
	for(p = list->first; p; p = next){
		next = p->next;
		if(p->item == inst)
			list->DeleteNode(p);
	}
}

void
RemoveInstFromSectors(ObjectInst *inst)
{
	int x, y;

	// Remove from out of bounds sector
	removeFromList(&outOfBoundsSector.buildings, inst);
	removeFromList(&outOfBoundsSector.buildings_overlap, inst);
	removeFromList(&outOfBoundsSector.bigbuildings, inst);
	removeFromList(&outOfBoundsSector.bigbuildings_overlap, inst);

	// Remove from all sectors
	for(y = 0; y < numSectorsY; y++)
		for(x = 0; x < numSectorsX; x++){
			Sector *s = GetSector(x, y);
			removeFromList(&s->buildings, inst);
			removeFromList(&s->buildings_overlap, inst);
			removeFromList(&s->bigbuildings, inst);
			removeFromList(&s->bigbuildings_overlap, inst);
		}
}

void
InsertInstIntoSectors(ObjectInst *inst)
{
	Sector *s;
	CPtrList *list;
	int x, xstart, xmid, xend;
	int y, ystart, ymid, yend;

	if(!IsInstInBounds(inst)){
		list = inst->m_isBigBuilding ? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
		list->InsertItem(inst);
		return;
	}

	CRect bounds = inst->GetBoundRect();

	xstart = GetSectorIndexX(bounds.left);
	xend   = GetSectorIndexX(bounds.right);
	xmid   = GetSectorIndexX((bounds.left + bounds.right)/2.0f);
	ystart = GetSectorIndexY(bounds.bottom);
	yend   = GetSectorIndexY(bounds.top);
	ymid   = GetSectorIndexY((bounds.bottom + bounds.top)/2.0f);

	for(y = ystart; y <= yend; y++)
		for(x = xstart; x <= xend; x++){
			s = GetSector(x, y);
			if(x == xmid && y == ymid)
				list = inst->m_isBigBuilding ? &s->bigbuildings : &s->buildings;
			else
				list = inst->m_isBigBuilding ? &s->bigbuildings_overlap : &s->buildings_overlap;
			list->InsertItem(inst);
		}
}
