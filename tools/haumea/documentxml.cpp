// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "documentxml.h"
#include "xmlfields.h"
#include "coordinatepin.h"
#include "layoutpaths.h"
#include "encoding.h"
#include "radarart.h"
#include "textdraws.h"
#include <algorithm>
#include <unordered_set>
#include <limits>

namespace
{
using EditorXML::Node;
using XMLFields::read;
using XMLFields::write;

void writeProperties(Node &node, const ObjectPawnProperties &value)
{
	write(node, "name", value.name);
	write(node, "group", value.group);
	write(node, "comment", value.comment);
	write(node, "drawDistance", value.drawDistance);
	write(node, "streamDistance", value.streamDistance);
	write(node, "virtualWorld", value.virtualWorld);
	write(node, "forceWorldReplacement", value.forceWorldReplacement);
	write(node, "attachOffset", value.attachOffset);
	write(node, "attachRotation", value.attachRotation);
	write(node, "attachSyncRotation", value.attachSyncRotation);
	write(node, "attachBone", value.attachBone);
	write(node, "attachSlot", value.attachSlot);
	write(node, "attachScale", value.attachScale);
	write(node, "attachColor1", value.attachColor1);
	write(node, "attachColor2", value.attachColor2);
	write(node, "attachSkin", value.attachSkin);
	write(node, "moveEnabled", value.moveEnabled);
	write(node, "moveOffset", value.moveOffset);
	write(node, "moveRotation", value.moveRotation);
	write(node, "moveRotates", value.moveRotates);
	write(node, "moveSpeed", value.moveSpeed);
	write(node, "elementKind", value.elementKind);
	write(node, "pickupType", value.pickupType);
	write(node, "labelText", value.labelText);
	write(node, "labelColor", value.labelColor);
	write(node, "labelTestLOS", value.labelTestLOS);
	write(node, "elementModel", value.elementModel);
	write(node, "actorInvulnerable", value.actorInvulnerable);
	write(node, "actorHealth", value.actorHealth);
	write(node, "classTeam", value.classTeam);
	write(node, "classWeapons", value.classWeapons);
	write(node, "classAmmo", value.classAmmo);
	write(node, "zoneSizeX", value.zoneSizeX);
	write(node, "zoneSizeY", value.zoneSizeY);
	write(node, "zoneColor", value.zoneColor);
	write(node, "zoneMode", value.zoneMode);
	write(node, "mapIconType", value.mapIconType);
	write(node, "mapIconColor", value.mapIconColor);
	write(node, "mapIconStyle", value.mapIconStyle);
	write(node, "npcName", value.npcName);
	write(node, "npcArmour", value.npcArmour);
	write(node, "npcWeapon", value.npcWeapon);
	write(node, "npcAmmo", value.npcAmmo);
	write(node, "pathName", value.pathName);
	write(node, "pathOrder", value.pathOrder);
	write(node, "pathStopRange", value.pathStopRange);
	write(node, "npcMoveType", value.npcMoveType);
	write(node, "npcMoveSpeed", value.npcMoveSpeed);
	write(node, "npcPathReversed", value.npcPathReversed);
	write(node, "npcPathRepeat", value.npcPathRepeat);
	write(node, "npcRecording", value.npcRecording);
	write(node, "npcRecordingAtNpc", value.npcRecordingAtNpc);
	write(node, "npcSeat", value.npcSeat);
	write(node, "npcPlaysNode", value.npcPlaysNode);
	write(node, "npcNode", value.npcNode);
	write(node, "npcNodePoint", value.npcNodePoint);
	write(node, "npcNodeRadius", value.npcNodeRadius);
	write(node, "npcNodeSetAngle", value.npcNodeSetAngle);
	write(node, "animLib", value.animLib);
	write(node, "animName", value.animName);
	write(node, "animDelta", value.animDelta);
	write(node, "animLoop", value.animLoop);
	write(node, "animLockX", value.animLockX);
	write(node, "animLockY", value.animLockY);
	write(node, "animFreeze", value.animFreeze);
	write(node, "animTime", value.animTime);
	write(node, "soundKind", value.soundKind);
	write(node, "soundUrl", value.soundUrl);
	write(node, "soundId", value.soundId);
	write(node, "soundRadius", value.soundRadius);
	write(node, "soundUsePos", value.soundUsePos);
	write(node, "soundStopOnLeave", value.soundStopOnLeave);
	write(node, "cpKind", value.cpKind);
	write(node, "cpSize", value.cpSize);
	write(node, "cpRaceType", value.cpRaceType);
	write(node, "cpRaceMode", value.cpRaceMode);
	write(node, "cpRaceLaps", value.cpRaceLaps);
	write(node, "cpRacePlayable", value.cpRacePlayable);
	write(node, "cpRace", value.cpRace);
	write(node, "cpOrder", value.cpOrder);
	write(node, "areaShape", value.areaShape);
	write(node, "areaRadius", value.areaRadius);
	write(node, "areaSize", value.areaSize);
	write(node, "areaHeight", value.areaHeight);
	write(node, "areaPointCount", value.areaPointCount);
	write(node, "areaPoints", value.areaPoints);
	write(node, "areaName", value.areaName);
	write(node, "interiorZeroOnly", value.interiorZeroOnly);
}
bool readProperties(const Node &node, ObjectPawnProperties &value, std::string *error)
{
	if (!read(node, "name", value.name, error))
		return false;
	if (!read(node, "group", value.group, error))
		return false;
	if (!read(node, "comment", value.comment, error))
		return false;
	if (!read(node, "drawDistance", value.drawDistance, error))
		return false;
	if (!read(node, "streamDistance", value.streamDistance, error))
		return false;
	if (!read(node, "virtualWorld", value.virtualWorld, error))
		return false;
	if (!read(node, "forceWorldReplacement", value.forceWorldReplacement, error))
		return false;
	if (!read(node, "attachOffset", value.attachOffset, error))
		return false;
	if (!read(node, "attachRotation", value.attachRotation, error))
		return false;
	if (!read(node, "attachSyncRotation", value.attachSyncRotation, error))
		return false;
	if (!read(node, "attachBone", value.attachBone, error))
		return false;
	if (!read(node, "attachSlot", value.attachSlot, error))
		return false;
	if (!read(node, "attachScale", value.attachScale, error))
		return false;
	if (!read(node, "attachColor1", value.attachColor1, error))
		return false;
	if (!read(node, "attachColor2", value.attachColor2, error))
		return false;
	if (!read(node, "attachSkin", value.attachSkin, error))
		return false;
	if (!read(node, "moveEnabled", value.moveEnabled, error) || !read(node, "moveOffset", value.moveOffset, error) ||
	    !read(node, "moveRotation", value.moveRotation, error) || !read(node, "moveRotates", value.moveRotates, error) ||
	    !read(node, "moveSpeed", value.moveSpeed, error))
		return false;
	if (!read(node, "elementKind", value.elementKind, error))
		return false;
	if (!read(node, "pickupType", value.pickupType, error))
		return false;
	if (!read(node, "labelText", value.labelText, error))
		return false;
	if (!read(node, "labelColor", value.labelColor, error))
		return false;
	if (!read(node, "labelTestLOS", value.labelTestLOS, error))
		return false;
	if (!read(node, "elementModel", value.elementModel, error))
		return false;
	if (!read(node, "actorInvulnerable", value.actorInvulnerable, error))
		return false;
	if (!read(node, "actorHealth", value.actorHealth, error))
		return false;
	if (!read(node, "classTeam", value.classTeam, error))
		return false;
	if (!read(node, "classWeapons", value.classWeapons, error))
		return false;
	if (!read(node, "classAmmo", value.classAmmo, error))
		return false;
	if (!read(node, "zoneSizeX", value.zoneSizeX, error))
		return false;
	if (!read(node, "zoneSizeY", value.zoneSizeY, error))
		return false;
	if (!read(node, "zoneColor", value.zoneColor, error))
		return false;
	if (!read(node, "zoneMode", value.zoneMode, error))
		return false;
	if (!read(node, "mapIconType", value.mapIconType, error))
		return false;
	if (!read(node, "mapIconColor", value.mapIconColor, error))
		return false;
	if (!read(node, "mapIconStyle", value.mapIconStyle, error))
		return false;
	if (!read(node, "npcName", value.npcName, error))
		return false;
	if (!read(node, "npcArmour", value.npcArmour, error))
		return false;
	if (!read(node, "npcWeapon", value.npcWeapon, error))
		return false;
	if (!read(node, "npcAmmo", value.npcAmmo, error))
		return false;
	if (!read(node, "pathName", value.pathName, error))
		return false;
	if (!read(node, "pathOrder", value.pathOrder, error))
		return false;
	if (!read(node, "pathStopRange", value.pathStopRange, error))
		return false;
	if (!read(node, "npcMoveType", value.npcMoveType, error))
		return false;
	if (!read(node, "npcMoveSpeed", value.npcMoveSpeed, error))
		return false;
	if (!read(node, "npcPathReversed", value.npcPathReversed, error))
		return false;
	if (!read(node, "npcPathRepeat", value.npcPathRepeat, error))
		return false;
	if (!read(node, "npcRecording", value.npcRecording, error))
		return false;
	if (!read(node, "npcRecordingAtNpc", value.npcRecordingAtNpc, error))
		return false;
	if (!read(node, "npcSeat", value.npcSeat, error))
		return false;
	if (!read(node, "npcPlaysNode", value.npcPlaysNode, error))
		return false;
	if (!read(node, "npcNode", value.npcNode, error))
		return false;
	if (!read(node, "npcNodePoint", value.npcNodePoint, error))
		return false;
	if (!read(node, "npcNodeRadius", value.npcNodeRadius, error))
		return false;
	if (!read(node, "npcNodeSetAngle", value.npcNodeSetAngle, error))
		return false;
	if (!read(node, "animLib", value.animLib, error))
		return false;
	if (!read(node, "animName", value.animName, error))
		return false;
	if (!read(node, "animDelta", value.animDelta, error))
		return false;
	if (!read(node, "animLoop", value.animLoop, error))
		return false;
	if (!read(node, "animLockX", value.animLockX, error))
		return false;
	if (!read(node, "animLockY", value.animLockY, error))
		return false;
	if (!read(node, "animFreeze", value.animFreeze, error))
		return false;
	if (!read(node, "animTime", value.animTime, error))
		return false;
	if (!read(node, "soundKind", value.soundKind, error))
		return false;
	if (!read(node, "soundUrl", value.soundUrl, error))
		return false;
	if (!read(node, "soundId", value.soundId, error))
		return false;
	if (!read(node, "soundRadius", value.soundRadius, error))
		return false;
	if (!read(node, "soundUsePos", value.soundUsePos, error))
		return false;
	if (!read(node, "soundStopOnLeave", value.soundStopOnLeave, error))
		return false;
	if (!read(node, "cpKind", value.cpKind, error))
		return false;
	if (!read(node, "cpSize", value.cpSize, error))
		return false;
	if (!read(node, "cpRaceType", value.cpRaceType, error))
		return false;
	if (!read(node, "cpRaceMode", value.cpRaceMode, error) || !read(node, "cpRaceLaps", value.cpRaceLaps, error) ||
	    !read(node, "cpRacePlayable", value.cpRacePlayable, error))
		return false;
	if (!read(node, "cpRace", value.cpRace, error))
		return false;
	if (!read(node, "cpOrder", value.cpOrder, error))
		return false;
	if (!read(node, "areaShape", value.areaShape, error))
		return false;
	if (!read(node, "areaRadius", value.areaRadius, error))
		return false;
	if (!read(node, "areaSize", value.areaSize, error))
		return false;
	if (!read(node, "areaHeight", value.areaHeight, error))
		return false;
	if (!read(node, "areaPointCount", value.areaPointCount, error))
		return false;
	if (!read(node, "areaPoints", value.areaPoints, error))
		return false;
	if (!read(node, "areaName", value.areaName, error))
		return false;
	if (!read(node, "interiorZeroOnly", value.interiorZeroOnly, error))
		return false;
	return true;
}

void writeMaterial(Node &node, const ObjectMaterialOverride &value)
{
	write(node, "active", value.active);
	write(node, "hasTexture", value.hasTexture);
	write(node, "sourceModel", value.sourceModel);
	write(node, "txdName", value.txdName);
	write(node, "textureName", value.textureName);
	write(node, "color", value.color);
	write(node, "textEnabled", value.textEnabled);
	write(node, "text", value.text);
	write(node, "font", value.font);
	write(node, "fontSize", value.fontSize);
	write(node, "bold", value.bold);
	write(node, "textColor", value.textColor);
	write(node, "backgroundColor", value.backgroundColor);
	write(node, "alignment", value.alignment);
	write(node, "materialSize", value.materialSize);
}
bool readMaterial(const Node &node, ObjectMaterialOverride &value, std::string *error)
{
	if (!read(node, "active", value.active, error))
		return false;
	if (!read(node, "hasTexture", value.hasTexture, error))
		return false;
	if (!read(node, "sourceModel", value.sourceModel, error))
		return false;
	if (!read(node, "txdName", value.txdName, error))
		return false;
	if (!read(node, "textureName", value.textureName, error))
		return false;
	if (!read(node, "color", value.color, error))
		return false;
	if (!read(node, "textEnabled", value.textEnabled, error))
		return false;
	if (!read(node, "text", value.text, error))
		return false;
	if (!read(node, "font", value.font, error))
		return false;
	if (!read(node, "fontSize", value.fontSize, error))
		return false;
	if (!read(node, "bold", value.bold, error))
		return false;
	if (!read(node, "textColor", value.textColor, error))
		return false;
	if (!read(node, "backgroundColor", value.backgroundColor, error))
		return false;
	if (!read(node, "alignment", value.alignment, error))
		return false;
	if (!read(node, "materialSize", value.materialSize, error))
		return false;
	return true;
}

void writeTextDraw(Node &node, const TextDraw &value)
{
	write(node, "name", value.name);
	write(node, "perPlayer", value.perPlayer);
	write(node, "x", value.x);
	write(node, "y", value.y);
	write(node, "text", value.text);
	write(node, "letterX", value.letterX);
	write(node, "letterY", value.letterY);
	write(node, "sizeX", value.sizeX);
	write(node, "sizeY", value.sizeY);
	write(node, "alignment", value.alignment);
	write(node, "colour", value.colour);
	write(node, "boxColour", value.boxColour);
	write(node, "backgroundColour", value.backgroundColour);
	write(node, "useBox", value.useBox);
	write(node, "shadow", value.shadow);
	write(node, "outline", value.outline);
	write(node, "font", value.font);
	write(node, "proportional", value.proportional);
	write(node, "selectable", value.selectable);
	write(node, "previewModel", value.previewModel);
	write(node, "previewRotX", value.previewRotX);
	write(node, "previewRotY", value.previewRotY);
	write(node, "previewRotZ", value.previewRotZ);
	write(node, "previewZoom", value.previewZoom);
	write(node, "previewColour1", value.previewColour1);
	write(node, "previewColour2", value.previewColour2);
	write(node, "locked", value.locked);
	write(node, "hidden", value.hidden);
	write(node, "group", value.group);
	write(node, "board", value.board);
	write(node, "shape", value.shape);
	if(!value.serverTxd.empty()) write(node, "serverTxd", value.serverTxd);
}
bool readTextDraw(const Node &node, TextDraw &value, std::string *error)
{
	if (!read(node, "name", value.name, error))
		return false;
	if (!read(node, "perPlayer", value.perPlayer, error))
		return false;
	if (!read(node, "x", value.x, error))
		return false;
	if (!read(node, "y", value.y, error))
		return false;
	if (!read(node, "text", value.text, error))
		return false;
	if (!read(node, "letterX", value.letterX, error))
		return false;
	if (!read(node, "letterY", value.letterY, error))
		return false;
	if (!read(node, "sizeX", value.sizeX, error))
		return false;
	if (!read(node, "sizeY", value.sizeY, error))
		return false;
	if (!read(node, "alignment", value.alignment, error))
		return false;
	if (!read(node, "colour", value.colour, error))
		return false;
	if (!read(node, "boxColour", value.boxColour, error))
		return false;
	if (!read(node, "backgroundColour", value.backgroundColour, error))
		return false;
	if (!read(node, "useBox", value.useBox, error))
		return false;
	if (!read(node, "shadow", value.shadow, error))
		return false;
	if (!read(node, "outline", value.outline, error))
		return false;
	if (!read(node, "font", value.font, error))
		return false;
	if (!read(node, "proportional", value.proportional, error))
		return false;
	if (!read(node, "selectable", value.selectable, error))
		return false;
	if (!read(node, "previewModel", value.previewModel, error))
		return false;
	if (!read(node, "previewRotX", value.previewRotX, error))
		return false;
	if (!read(node, "previewRotY", value.previewRotY, error))
		return false;
	if (!read(node, "previewRotZ", value.previewRotZ, error))
		return false;
	if (!read(node, "previewZoom", value.previewZoom, error))
		return false;
	if (!read(node, "previewColour1", value.previewColour1, error))
		return false;
	if (!read(node, "previewColour2", value.previewColour2, error))
		return false;
	if (!read(node, "locked", value.locked, error))
		return false;
	if (!read(node, "hidden", value.hidden, error))
		return false;
	if (!read(node, "group", value.group, error))
		return false;
	if (!read(node, "board", value.board, error))
		return false;
	if (!read(node, "shape", value.shape, error))
		return false;
	if (!read(node, "serverTxd", value.serverTxd, error))
		return false;
	return true;
}

struct Instance {
	unsigned id = 0, parent = 0, vehicle = 0, lod = 0;
	int model = 0, area = 0, kind = SAMP_ELEMENT_OBJECT;
	bool isVehicle = false, deleted = false, world = false, hidden = false, sourceDegrees = false, radar = false;
	bool playerTarget = false;
	rw::V3d position = {0, 0, 0}, original = {0, 0, 0};
	rw::Quat rotation = {0, 0, 0, 1}, sourceRotation = {0, 0, 0, 1}, originalRotation = {0, 0, 0, 1};
	double degrees[3] = {0, 0, 0};
	ObjectPawnProperties properties;
	bool vehicleStatic = false;
	int colour1 = -1, colour2 = -1, respawn = -1, paintjob = 0;
	char plate[VEHICLE_PLATE_LEN] = {0};
	uint16 mods[VEHICLE_MOD_SLOTS] = {0};
	std::vector<std::pair<int, ObjectMaterialOverride>> materials;
	std::string handle;
	Instance()
	{
		InitObjectPawnProperties(&properties);
	}
};
bool capturing;
Node captured;

Node writeTextDrawList(const std::vector<TextDraw> &textdraws, const std::vector<TextDrawGuide> &guides)
{
	Node list("textdraws");
	for (const TextDraw &td : textdraws)
		writeTextDraw(list.add("textdraw"), td);
	for (const TextDrawGuide &guide : guides) {
		Node &g = list.add("guide");
		g.set("board", guide.board);
		g.set("vertical", guide.vertical);
		g.set("at", guide.at);
	}
	return list;
}
bool readTextDrawList(const Node &list, std::vector<TextDraw> &textdraws, std::vector<TextDrawGuide> &guides,
		      std::string *error)
{
	for (const Node &node : list.children) {
		if (node.name == "textdraw") {
			TextDraw td;
			InitTextDraw(&td);
			if (!readTextDraw(node, td, error))
				return false;
			if (td.font < 0 || td.font > TD_FONT_REFERENCE || td.alignment < 1 || td.alignment > 3) {
				*error = "Invalid TextDraw in editor XML";
				return false;
			}
			textdraws.push_back(td);
		} else if (node.name == "guide") {
			TextDrawGuide guide;
			guide.vertical = false;
			guide.at = 0;
			if (!read(node, "board", guide.board, error) ||
			    !read(node, "vertical", guide.vertical, error) || !read(node, "at", guide.at, error))
				return false;
			guides.push_back(guide);
		}
	}
	return true;
}
unsigned documentId(ObjectInst *inst)
{
	return inst ? inst->m_editorDocumentId : 0;
}
Node writeInstance(ObjectInst *inst, const std::string &handle)
{
	Node node("object");
	const ObjectPawnProperties &p = *GetObjectPawnProperties(inst);
	node.set("id", documentId(inst));
	node.set("model", PawnModelId(inst->m_objectId));
	node.set("kind", GetSampElementKind(inst));
	node.set("runtime-handle", handle);
	node.set("vehicle", inst->m_isVehicle);
	node.set("world", IsPawnWorldObject(inst));
	node.set("deleted", inst->m_isDeleted);
	node.set("hidden", inst->m_editorHidden);
	node.set("radar", IsRadarArtStandIn(inst));
	node.set("area", inst->m_area);
	write(node, "position", inst->m_translation);
	write(node, "rotation", inst->m_rotation);
	write(node, "original", inst->m_origTranslation);
	write(node, "original-rotation", inst->m_origRotation);
	node.set("has-source-degrees", inst->m_hasSourceDegrees);
	if (inst->m_hasSourceDegrees) {
		write(node, "source-degrees", inst->m_sourceDegrees);
		write(node, "source-rotation", inst->m_sourceRotation);
	}
	if (p.attachParent == PAWN_ATTACH_PLAYER)
		node.set("parent-type", "player");
	else
		node.set("parent", documentId(GetInstanceByID(p.attachParent)));
	node.set("npc-vehicle", documentId(GetInstanceByID(p.npcVehicle)));
	node.set("lod", documentId(inst->m_lod));
	// Unchanged world geometry may only carry editor visibility. Its
	// default properties need no verbose copy of every element's fields.
	if (inst->m_pawnProperties || !IsPawnWorldObject(inst))
		writeProperties(node.add("properties"), p);
	if (inst->m_isVehicle) {
		Node &v = node.add("vehicle");
		v.set("static", inst->m_vehicleStatic);
		v.set("colour1", inst->m_vehicleColor1);
		v.set("colour2", inst->m_vehicleColor2);
		v.set("respawn", inst->m_vehicleRespawnDelay);
		v.set("paintjob", inst->m_vehiclePaintjob);
		v.set("plate", inst->m_vehiclePlate);
		write(v, "mods", inst->m_vehicleMods);
	}
	if (inst->m_materialOverrides)
		for (int slot = 0; slot < MAX_PAWN_MATERIALS; slot++) {
			Node m("material"), defaults("material");
			ObjectMaterialOverride def;
			InitObjectMaterialOverride(&def, inst->m_objectId);
			writeMaterial(defaults, def);
			writeMaterial(m, inst->m_materialOverrides[slot]);
			if (m.attributes == defaults.attributes)
				continue;
			m.set("slot", slot);
			node.children.push_back(std::move(m));
		}
	return node;
}
bool readInstance(const Node &node, Instance &out, std::string *error)
{
	if (!read(node, "id", out.id, error) || !read(node, "model", out.model, error) ||
	    !read(node, "kind", out.kind, error) || !read(node, "vehicle", out.isVehicle, error) ||
	    !read(node, "world", out.world, error) || !read(node, "deleted", out.deleted, error) ||
	    !read(node, "hidden", out.hidden, error) || !read(node, "radar", out.radar, error) ||
	    !read(node, "area", out.area, error) || !read(node, "position", out.position, error) ||
	    !read(node, "original", out.original, error) ||
	    !read(node, "original-rotation", out.originalRotation, error) ||
	    !read(node, "rotation", out.rotation, error) ||
	    !read(node, "has-source-degrees", out.sourceDegrees, error) ||
	    !read(node, "source-degrees", out.degrees, error) ||
	    !read(node, "source-rotation", out.sourceRotation, error) || !read(node, "parent", out.parent, error) ||
	    !read(node, "npc-vehicle", out.vehicle, error) || !read(node, "lod", out.lod, error))
		return false;
	out.playerTarget = node.get("parent-type") == "player";
	out.handle = node.get("runtime-handle");
	if (out.kind < SAMP_ELEMENT_OBJECT || out.kind > SAMP_ELEMENT_AREA || out.model < -30000 ||
	    out.model >= NUMOBJECTDEFS ||
	    (out.rotation.x * out.rotation.x + out.rotation.y * out.rotation.y + out.rotation.z * out.rotation.z +
	     out.rotation.w * out.rotation.w) < .000001f) {
		*error = "Invalid object in editor XML";
		return false;
	}
	if (const Node *p = node.child("properties"))
		if (!readProperties(*p, out.properties, error))
			return false;
	out.properties.elementKind = out.kind;
	if (const Node *v = node.child("vehicle")) {
		if (!read(*v, "static", out.vehicleStatic, error) || !read(*v, "colour1", out.colour1, error) ||
		    !read(*v, "colour2", out.colour2, error) || !read(*v, "respawn", out.respawn, error) ||
		    !read(*v, "paintjob", out.paintjob, error) || !read(*v, "plate", out.plate, error) ||
		    !read(*v, "mods", out.mods, error))
			return false;
	}
	std::unordered_set<int> slots;
	for (const Node &m : node.children)
		if (m.name == "material") {
			int slot = -1;
			ObjectMaterialOverride material;
			InitObjectMaterialOverride(&material, EditorModelId(out.model));
			if (!read(m, "slot", slot, error) || !readMaterial(m, material, error))
				return false;
			if (slot < 0 || slot >= MAX_PAWN_MATERIALS || !slots.insert(slot).second) {
				*error = "Invalid material slot in editor XML";
				return false;
			}
			out.materials.push_back({slot, material});
		}
	return true;
}
bool readRadar(const Node &root, std::vector<RadarArtLayer> &layers, std::string *error)
{
	const Node *list = root.child("radar");
	if (!list)
		return true;
	for (const Node &n : list->children)
		if (n.name == "layer") {
			RadarArtLayer layer = {};
			if (!read(n, "group", layer.group, error) || !read(n, "mode", layer.zoneMode, error))
				return false;
			for (const Node &z : n.children)
				if (z.name == "zone") {
					RadarArtZone zone = {};
					if (!read(z, "minX", zone.minX, error) || !read(z, "minY", zone.minY, error) ||
					    !read(z, "maxX", zone.maxX, error) || !read(z, "maxY", zone.maxY, error) ||
					    !read(z, "z", zone.z, error) || !read(z, "colour", zone.colour, error))
						return false;
					layer.zones.push_back(zone);
				}
			layers.push_back(layer);
		}
	return true;
}
bool decodeMap(const Node &root, std::vector<Instance> &items, std::vector<RadarArtLayer> &radar,
	       std::vector<TextDraw> &tds, std::vector<TextDrawGuide> &guides, std::string *error)
{
	const Node *scene = root.child("scene");
	if (!scene) {
		*error = "Editor XML has no scene";
		return false;
	}
	std::unordered_set<unsigned> ids;
	for (const Node &node : scene->children)
		if (node.name == "object") {
			Instance item;
			if (!readInstance(node, item, error))
				return false;
			if (!item.radar && (!item.id || !ids.insert(item.id).second)) {
				*error = "Duplicate or missing XML object identifier";
				return false;
			}
			items.push_back(item);
		}
	for (const Instance &item : items) {
		for (unsigned link : {item.parent, item.vehicle, item.lod})
			if (link && !ids.count(link)) {
				*error = "Unresolved XML object reference";
				return false;
			}
		if (item.parent == item.id && item.parent) {
			*error = "An XML object cannot attach to itself";
			return false;
		}
		if (item.properties.areaPointCount < 0 || item.properties.areaPointCount > PAWN_AREA_POINTS) {
			*error = "Invalid XML area point count";
			return false;
		}
	}
	std::vector<PawnAreaRemoval> removals;
	if (!ReadMapXMLRemovals(root, removals, error))
		return false;
	if (!readRadar(root, radar, error))
		return false;
	if (const Node *td = root.child("textdraws"))
		if (!readTextDrawList(*td, tds, guides, error))
			return false;
	return true;
}
} // namespace

void BeginMapXMLExport()
{
	capturing = true;
	captured = Node("haumea");
	captured.set("kind", "map");
}
void CaptureMapXMLExport(const std::vector<ObjectInst *> &objects,
			 const std::unordered_map<const ObjectInst *, std::string> &handles, int format,
			 bool selectedOnly)
{
	if (!capturing)
		return;
	captured.set("encoding", PawnTextEncodingName(GetPawnTextEncoding()));
	captured.set("format", format);
	captured.set("show-material-indexes", gShowMaterialIndexes);
	unsigned next = 0;
	std::unordered_set<unsigned> occupied;
	for (CPtrNode *p = instances.first; p; p = p->next) {
		unsigned id = ((ObjectInst *)p->item)->m_editorDocumentId;
		if (id) {
			occupied.insert(id);
			next = std::max(next, id);
		}
	}
	auto assign = [&](ObjectInst *inst) {
		if (IsRadarArtStandIn(inst) || inst->m_editorDocumentId)
			return;
		do {
			++next;
		} while (!next || occupied.count(next));
		inst->m_editorDocumentId = next;
		occupied.insert(next);
	};
	std::vector<ObjectInst *> ordered = objects;
	std::unordered_set<ObjectInst *> included(ordered.begin(), ordered.end());
	// Visibility may apply to the entire GTA world after Isolate; use a set
	// so collecting these overrides stays linear in the size of the scene.
	if (!selectedOnly) {
		for (CPtrNode *p = instances.first; p; p = p->next) {
			ObjectInst *inst = (ObjectInst *)p->item;
			if (IsPawnWorldObject(inst) && inst->m_editorHidden && included.insert(inst).second)
				ordered.push_back(inst);
		}
		// Keep references to unchanged world LODs without adding runtime calls.
		for (size_t i = 0; i < ordered.size(); i++) {
			ObjectInst *lod = ordered[i]->m_lod;
			if (lod && IsPawnWorldObject(lod) && included.insert(lod).second)
				ordered.push_back(lod);
		}
	}
	for (ObjectInst *inst : ordered)
		assign(inst);
	std::unordered_set<unsigned> exported;
	for (ObjectInst *inst : ordered)
		if (!IsRadarArtStandIn(inst))
			exported.insert(documentId(inst));
	std::stable_sort(ordered.begin(), ordered.end(),
			 [](ObjectInst *a, ObjectInst *b) { return documentId(a) < documentId(b); });
	Node &scene = captured.add("scene");
	for (ObjectInst *inst : ordered) {
		auto h = handles.find(inst);
		Node object = writeInstance(inst, h == handles.end() ? "" : h->second);
		for (const char *ref : {"parent", "npc-vehicle", "lod"}) {
			double id = 0;
			object.number(ref, &id);
			if (!exported.count((unsigned)id))
				object.set(ref, 0);
		}
		scene.children.push_back(std::move(object));
	}
	if (!selectedOnly) {
		Node &removals = captured.add("removals");
		for (const PawnAreaRemoval &r : GetPawnAreaRemovals()) {
			Node &n = removals.add("sphere");
			n.set("x", r.x);
			n.set("y", r.y);
			n.set("z", r.z);
			n.set("radius", r.radius);
		}
		Node &radar = captured.add("radar");
		for (const RadarArtLayer &layer : GetRadarArtLayers()) {
			Node &l = radar.add("layer");
			l.set("group", layer.group);
			l.set("mode", layer.zoneMode);
			for (const RadarArtZone &zone : layer.zones) {
				Node &z = l.add("zone");
				z.set("minX", zone.minX);
				z.set("minY", zone.minY);
				z.set("maxX", zone.maxX);
				z.set("maxY", zone.maxY);
				z.set("z", zone.z);
				z.set("colour", zone.colour);
			}
		}
		captured.children.push_back(writeTextDrawList(GetMapTextDraws(), {}));
		captured.children.push_back(WriteCoordinatePinXML());
		captured.children.push_back(WriteLayoutPathsXML());
		Node &camera = captured.add("camera");
		write(camera, "position", TheCamera.m_position);
		write(camera, "target", TheCamera.m_target);
		camera.set("fov", TheCamera.m_fov);
		camera.set("area", currentArea);
	}
}
EditorXML::Node FinishMapXMLExport()
{
	capturing = false;
	return std::move(captured);
}

EditorXML::Node BuildTextDrawXML(const std::vector<TextDraw> &tds, const std::vector<TextDrawGuide> &guides,
				 int encoding)
{
	Node root("haumea");
	root.set("kind", "textdraws");
	root.set("encoding", PawnTextEncodingName((PawnTextEncoding)encoding));
	root.children.push_back(writeTextDrawList(tds, guides));
	return root;
}
bool ReadTextDrawXML(const Node &document, bool sameRevision, std::vector<TextDraw> &tds,
		     std::vector<TextDrawGuide> &guides, std::string *error)
{
	const Node *list = document.child("textdraws");
	if (!list) {
		*error = "Editor XML has no textdraws";
		return false;
	}
	std::vector<TextDraw> snapshot;
	std::vector<TextDrawGuide> savedGuides;
	if (!readTextDrawList(*list, snapshot, savedGuides, error))
		return false;
	if (sameRevision)
		tds = std::move(snapshot);
	else {
		for (TextDraw &td : tds)
			for (const TextDraw &saved : snapshot)
				if (td.name[0] && strcmp(td.name, saved.name) == 0) {
					td.locked = saved.locked;
					td.hidden = saved.hidden;
					td.group = saved.group;
					td.board = saved.board;
					td.shape = saved.shape;
					if(td.text.substr(0, td.text.find(':')) == saved.text.substr(0, saved.text.find(':')))
						td.serverTxd = saved.serverTxd;
					break;
				}
		for (size_t i = 0; i < snapshot.size(); i++)
			if (snapshot[i].font == TD_FONT_REFERENCE)
				tds.insert(tds.begin() + std::min(i, tds.size()), snapshot[i]);
	}
	guides = std::move(savedGuides);
	return true;
}
bool ValidateEditorDocument(const Node &document, std::string *error)
{
	const std::string encoding = document.get("encoding", "Windows-1252");
	if (encoding != "Windows-1252" && encoding != "UTF-8") {
		*error = "Unsupported Pawn text encoding in XML";
		return false;
	}
	int format = 0;
	bool indexes = false;
	if (!read(document, "format", format, error) || !read(document, "show-material-indexes", indexes, error))
		return false;
	if (format != PAWN_CODE_NATIVE && format != PAWN_CODE_STREAMER) {
		*error = "Invalid Pawn export format in XML";
		return false;
	}
	if (document.get("kind") != "map" && document.get("kind") != "textdraws") {
		*error = "Unsupported XML document kind";
		return false;
	}
	if (document.get("kind") == "textdraws") {
		std::vector<TextDraw> tds;
		std::vector<TextDrawGuide> guides;
		return ReadTextDrawXML(document, true, tds, guides, error);
	}
	std::vector<Instance> objects;
	std::vector<RadarArtLayer> radar;
	std::vector<TextDraw> tds;
	std::vector<TextDrawGuide> guides;
	if (!decodeMap(document, objects, radar, tds, guides, error))
		return false;
	if (const Node *pins = document.child("alignment"))
		if (!ReadCoordinatePinXML(*pins, error, false))
			return false;
	if (const Node *paths = document.child("layout-paths"))
		if (!ReadLayoutPathsXML(*paths, error, false))
			return false;
	if (const Node *camera = document.child("camera")) {
		rw::V3d position = {}, target = {};
		float fov = 70;
		int area = 0;
		if (!read(*camera, "position", position, error) || !read(*camera, "target", target, error) ||
		    !read(*camera, "fov", fov, error) || !read(*camera, "area", area, error))
			return false;
	}
	return true;
}

bool CanSaveEditorDocument(const std::string &path, const char *kind, std::string *error)
{
	error->clear();
	std::string bytes;
	if (!EditorXML::ReadFile(EditorXML::Sidecar(path), &bytes, error))
		return error->empty();
	Node root;
	if (!EditorXML::Parse(bytes, &root, error))
		return false;
	if (root.name != "haumea" || root.get("version") != "1" || root.get("kind") != kind) {
		*error = "Unsupported editor XML: " + EditorXML::Sidecar(path);
		return false;
	}
	return ValidateEditorDocument(root, error);
}

bool ApplyMapXML(const Node &document, bool sameRevision, std::vector<ObjectInst *> &created,
		 std::vector<RadarArtLayer> &radarLayers, size_t previousTextDraws,
		 const std::unordered_map<std::string, ObjectInst *> &handles, std::string *error)
{
	std::vector<Instance> objects;
	std::vector<RadarArtLayer> radar;
	std::vector<TextDraw> tds;
	std::vector<TextDrawGuide> guides;
	if (!ValidateEditorDocument(document, error) || !decodeMap(document, objects, radar, tds, guides, error))
		return false;
	std::unordered_set<ObjectInst *> used;
	std::unordered_map<unsigned, ObjectInst *> restored;
	std::vector<ObjectInst *> imported = created;
	std::unordered_set<unsigned> occupied;
	unsigned next = 0;
	for (CPtrNode *p = instances.first; p; p = p->next) {
		ObjectInst *i = (ObjectInst *)p->item;
		if (i->m_editorDocumentId) {
			occupied.insert(i->m_editorDocumentId);
			next = std::max(next, i->m_editorDocumentId);
		}
	}
	auto compatible = [](const Instance &saved, ObjectInst *inst) {
		return inst && saved.model == PawnModelId(inst->m_objectId) && saved.kind == GetSampElementKind(inst) &&
		       saved.isVehicle == inst->m_isVehicle;
	};
	auto samePosition = [](const rw::V3d &a, const rw::V3d &b, bool flat) {
		return fabsf(a.x - b.x) < .002f && fabsf(a.y - b.y) < .002f && (flat || fabsf(a.z - b.z) < .002f);
	};
	auto spatialKey = [](int model, int kind, bool vehicle, const rw::V3d &position) {
		char key[200];
		snprintf(key, sizeof(key), "%d:%d:%d:%.2f:%.2f:%.2f", model, kind, (int)vehicle,
			 (double)(position.x == 0 ? 0 : position.x), (double)(position.y == 0 ? 0 : position.y),
			 (double)(position.z == 0 ? 0 : position.z));
		return std::string(key);
	};
	std::unordered_map<std::string, std::vector<ObjectInst *>> worldIndex;
	for (CPtrNode *p = instances.first; p; p = p->next) {
		ObjectInst *inst = (ObjectInst *)p->item;
		if (IsPawnWorldObject(inst))
			worldIndex[spatialKey(PawnModelId(inst->m_objectId), GetSampElementKind(inst), false,
					      inst->m_origTranslation)]
			    .push_back(inst);
	}
	std::unordered_map<int, std::vector<ObjectInst *>> byModel;
	for (auto i = imported.rbegin(); i != imported.rend(); ++i)
		byModel[PawnModelId((*i)->m_objectId)].push_back(*i);
	std::unordered_set<ObjectInst *> usedWorld;
	for (const Instance &saved : objects) {
		ObjectInst *target = nil;
		// A removed world building or unchanged reference LOD emits no create
		// call. It must not consume a newly placed object of the same model.
		bool activeMaterial = std::any_of(saved.materials.begin(), saved.materials.end(), [](const auto &m) {
			return m.second.active || m.second.textEnabled;
		});
		bool runtime =
		    !saved.deleted && (!saved.world || saved.position.x != saved.original.x ||
				       saved.position.y != saved.original.y || saved.position.z != saved.original.z ||
				       GetQuaternionSimilarity(saved.rotation, saved.originalRotation) < .9999f ||
				       !ObjectPawnPropertiesAreDefault(saved.properties) || activeMaterial);
		auto h = handles.find(saved.handle);
		if (runtime && !saved.handle.empty() && h != handles.end() && !used.count(h->second) &&
		    compatible(saved, h->second))
			target = h->second;
		if (runtime && !target)
			for (auto candidate = byModel[saved.model].rbegin(); candidate != byModel[saved.model].rend();
			     ++candidate) {
				ObjectInst *inst = *candidate;
				if (!used.count(inst) && compatible(saved, inst) &&
				    samePosition(saved.position, inst->m_translation,
						 saved.kind == SAMP_ELEMENT_GANGZONE)) {
					target = inst;
					break;
				}
			}
		// The matching Pawn hash makes this snapshot authoritative. A user may
		// have edited the XML itself, including a position that has no handle.
		if (runtime && !target && sameRevision && !saved.world)
			for (auto candidate = byModel[saved.model].rbegin(); candidate != byModel[saved.model].rend();
			     ++candidate) {
				ObjectInst *inst = *candidate;
				if (!used.count(inst) && compatible(saved, inst)) {
					target = inst;
					break;
				}
			}
		if (target) {
			used.insert(target);
			auto &candidates = byModel[saved.model];
			while (!candidates.empty() && used.count(candidates.back()))
				candidates.pop_back();
		}
		if (saved.radar && sameRevision) {
			if (target) {
				RemoveInstFromSectors(target);
				target->Deselect();
				target->m_isDeleted = true;
			}
			continue;
		}
		if (saved.world) {
			ObjectInst *world = nil;
			auto found = worldIndex.find(spatialKey(saved.model, saved.kind, false, saved.original));
			if (found != worldIndex.end())
				for (ObjectInst *inst : found->second) {
					if (!usedWorld.count(inst) && compatible(saved, inst) &&
					    samePosition(saved.original, inst->m_origTranslation, false) &&
					    GetQuaternionSimilarity(saved.originalRotation, inst->m_origRotation) >
						.9999f) {
						world = inst;
						usedWorld.insert(inst);
						break;
					}
				}
			if (world && sameRevision) {
				// The Pawn contains a removal plus a replacement; the XML can restore
				// the original world instance directly and avoids a second replacement.
				if (target) {
					RemoveInstFromSectors(target);
					target->Deselect();
					target->m_isDeleted = true;
				}
				target = world;
			} else if (!target)
				target = world;
		}
		if (!target && sameRevision && !saved.deleted) {
			target = SpawnDocumentInstance(EditorModelId(saved.model), saved.position, saved.rotation);
			if (target)
				created.push_back(target);
		}
		if (!target) {
			if (sameRevision)
				AddPawnImportNote("Could not restore XML model " + std::to_string(saved.model) +
						  ". Its game assets or original world instance are missing.");
			continue;
		}
		if (saved.id) {
			unsigned id = saved.id;
			if (occupied.count(id) && target->m_editorDocumentId != id) {
				do {
					++next;
				} while (!next || occupied.count(next));
				id = next;
			}
			target->m_editorDocumentId = id;
			occupied.insert(id);
			next = std::max(next, id);
			restored[saved.id] = target;
		}
		ObjectPawnProperties props = *GetObjectPawnProperties(target);
		if (sameRevision) {
			props = saved.properties;
			// References belong to this XML document, not to a previous process.
			props.attachParent = saved.playerTarget ? PAWN_ATTACH_PLAYER : saved.parent ? 0 : -1;
			props.npcVehicle = saved.vehicle ? 0 : -1;
			target->m_area = saved.area;
			RestoreDocumentTransform(target, saved.position, saved.rotation, saved.deleted);
			target->m_hasSourceDegrees = saved.sourceDegrees;
			target->m_sourceRotation = saved.sourceRotation;
			std::copy(saved.degrees, saved.degrees + 3, target->m_sourceDegrees);
			if (saved.isVehicle) {
				target->m_vehicleStatic = saved.vehicleStatic;
				target->m_vehicleColor1 = saved.colour1;
				target->m_vehicleColor2 = saved.colour2;
				target->m_vehicleRespawnDelay = saved.respawn;
				target->m_vehiclePaintjob = saved.paintjob;
				memcpy(target->m_vehiclePlate, saved.plate, sizeof(saved.plate));
				memcpy(target->m_vehicleMods, saved.mods, sizeof(saved.mods));
				target->RefreshVehicleColors();
				target->RefreshVehicleUpgrades();
			}
			for (const auto &m : saved.materials)
				SetObjectMaterialOverride(target, m.first, m.second, false);
		} else {
			memcpy(props.group, saved.properties.group, sizeof(props.group));
			memcpy(props.comment, saved.properties.comment, sizeof(props.comment));
		}
		SetInstanceHidden(target, saved.hidden);
		SetObjectPawnProperties(target, props, false);
	}
	if (sameRevision) {
		auto ref = [&](unsigned id) {
			auto p = restored.find(id);
			return p == restored.end() ? nil : p->second;
		};
		for (const Instance &saved : objects) {
			ObjectInst *inst = ref(saved.id);
			if (!inst)
				continue;
			ObjectPawnProperties props = saved.properties;
			ObjectInst *parent = ref(saved.parent), *vehicle = ref(saved.vehicle);
			props.attachParent = saved.playerTarget ? PAWN_ATTACH_PLAYER : parent && parent != inst ? parent->m_id : -1;
			props.npcVehicle = vehicle ? vehicle->m_id : -1;
			SetObjectPawnProperties(inst, props, false);
			ObjectInst *lod = ref(saved.lod);
			if ((lod || !IsPawnWorldObject(inst)) && inst->m_lod != lod) {
				if (inst->m_lod && inst->m_lod->m_numChildren > 0)
					--inst->m_lod->m_numChildren;
				inst->m_lod = lod;
				inst->m_lodId = lod ? lod->m_iplIndex : -1;
				if (lod) {
					++lod->m_numChildren;
					if (!lod->m_isBigBuilding)
						lod->SetupBigBuilding();
				}
			}
		}
		// Runtime-only shadows of world replacements and generated radar zones
		// are removed before the import's single paste undo record is made.
		for (ObjectInst *inst : imported)
			if (!used.count(inst)) {
				RemoveInstFromSectors(inst);
				inst->m_isDeleted = true;
				inst->Deselect();
			}
		radarLayers = std::move(radar);
		auto &mapTDs = GetMapTextDraws();
		mapTDs.resize(previousTextDraws);
		mapTDs.insert(mapTDs.end(), tds.begin(), tds.end());
		UpdateObjectAttachments();
	} else if (const Node *list = document.child("textdraws")) {
		std::vector<TextDraw> runtime(GetMapTextDraws().begin() + previousTextDraws, GetMapTextDraws().end());
		Node tdDocument("haumea");
		tdDocument.children.push_back(*list);
		if (!ReadTextDrawXML(tdDocument, false, runtime, guides, error))
			return false;
		auto &mapTDs = GetMapTextDraws();
		mapTDs.resize(previousTextDraws);
		mapTDs.insert(mapTDs.end(), runtime.begin(), runtime.end());
	}
	created.erase(
	    std::remove_if(created.begin(), created.end(), [](ObjectInst *inst) { return inst->m_isDeleted; }),
	    created.end());
	return true;
}

int RestoreMapXMLWorkspace(const char *path)
{
	std::string pawn, error;
	Node root;
	if (!EditorXML::ReadFile(path, &pawn, &error))
		return -1;
	int state = EditorXML::ReadDocument(path, pawn, "map", &root, &error);
	if (state <= 0 || !ValidateEditorDocument(root, &error))
		return -1;
	read(root, "show-material-indexes", gShowMaterialIndexes, &error);
	if (const Node *pins = root.child("alignment"))
		ReadCoordinatePinXML(*pins, &error);
	if (const Node *paths = root.child("layout-paths"))
		ReadLayoutPathsXML(*paths, &error);
	if (const Node *camera = root.child("camera")) {
		rw::V3d position = TheCamera.m_position, target = TheCamera.m_target;
		float fov = TheCamera.m_fov;
		int area = currentArea;
		if (read(*camera, "position", position, &error) && read(*camera, "target", target, &error) &&
		    read(*camera, "fov", fov, &error) && read(*camera, "area", area, &error)) {
			currentArea = clamp(area, 0, 255);
			TheCamera.m_position = position;
			TheCamera.m_target = target;
			TheCamera.m_fov = ClampCameraFov(fov);
			TheCamera.update();
		}
	}
	int format = -1;
	read(root, "format", format, &error);
	return state == 1 ? format : -1;
}

bool ReadMapXMLRemovals(const Node &document, std::vector<PawnAreaRemoval> &removals, std::string *error)
{
	std::vector<PawnAreaRemoval> loaded;
	if (const Node *list = document.child("removals"))
		for (const Node &n : list->children)
			if (n.name == "sphere") {
				PawnAreaRemoval r = {};
				if (!read(n, "x", r.x, error) || !read(n, "y", r.y, error) ||
				    !read(n, "z", r.z, error) || !read(n, "radius", r.radius, error))
					return false;
				if (r.radius < 0) {
					*error = "Invalid XML removal radius";
					return false;
				}
				loaded.push_back(r);
			}
	removals = std::move(loaded);
	return true;
}
