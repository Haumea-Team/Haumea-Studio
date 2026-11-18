// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "radar.h"
#include "radarart.h"
#include "i18n.h"
#include "icons.h"
#include "startscreen.h"
#include "textdraws.h"
#include "imgui/imgui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace {

// CRadar's numbers (gta-reversed, Radar.cpp). The world map is 12 x 12
// tiles of 500 m, radar00 at the north-west corner (-3000, 3000).
const int TILES = 12;
const float TILE_SIZE = 500.0f;
const float MAP_EDGE = 3000.0f;
const float RANGE_ON_FOOT = 180.0f;	// RADAR_MIN_RANGE
const float RANGE_FAST = 350.0f;	// RADAR_MAX_RANGE
const ImU32 SEA = IM_COL32(111, 137, 170, 255);	// DrawRadarSection off the map
const int CIRCLE_SIDES = 48;

bool gShowRadar = false;
bool gShowMap = false;
float gRange = RANGE_ON_FOOT;

// The full map: its centre and pixels per metre.
float gMapX, gMapY, gMapScale = 0.25f;
bool gMapPlaced = false;
bool gMapZones = true;	// the gang zones and radar art over the tiles

struct V2 { float x, y; };

// ---- textures -------------------------------------------------------------

rw::Texture *
tileTexture(int x, int y)
{
	static int slots[TILES * TILES];
	static bool looked;
	if(!looked){
		looked = true;
		for(int i = 0; i < TILES * TILES; i++){
			char name[16];
			snprintf(name, sizeof(name), "radar%02d", i);
			slots[i] = FindTxdSlot(name);
		}
	}
	if(x < 0 || y < 0 || x >= TILES || y >= TILES)
		return nil;
	int slot = slots[y * TILES + x];
	if(slot < 0)
		return nil;
	if(!IsTxdLoaded(slot))
		LoadTxd(slot);
	return GetTxdTexture(slot, 0);
}

// models/hud.txd: the disc around the radar, the player's arrow and the N.
enum { HUD_DISC, HUD_CENTRE, HUD_NORTH, NUM_HUD };
rw::Texture *
hudTexture(int which)
{
	static rw::Texture *tex[NUM_HUD];
	static bool loaded;
	if(!loaded){
		loaded = true;
		int slot = AddTxdSlot("~radar_hud");
		LoadTxd(slot, "MODELS/HUD.TXD");
		tex[HUD_DISC] = FindTextureInTxd(slot, "radardisc");
		tex[HUD_CENTRE] = FindTextureInTxd(slot, "radar_centre");
		tex[HUD_NORTH] = FindTextureInTxd(slot, "radar_north");
	}
	return tex[which];
}

// ---- the map's gang zones ---------------------------------------------------

struct Zone { float minX, minY, maxX, maxY; ImU32 colour; };

// GangZoneCreate's rectangle and GangZoneShowForAll's 0xRRGGBBAA colour:
// the radar art layers', then the map's gang zones.
void
collectZones(std::vector<Zone> &out)
{
	out.clear();
	for(const RadarArtLayer &l : GetRadarArtLayers())
		for(const RadarArtZone &r : l.zones){
			uint32 c = r.colour;
			out.push_back({ r.minX, r.minY, r.maxX, r.maxY,
				IM_COL32(c >> 24 & 0xFF, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF) });
		}
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || GetSampElementKind(inst) != SAMP_ELEMENT_GANGZONE)
			continue;
		const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
		uint32 c = props->zoneColor;
		Zone z;
		z.minX = inst->m_translation.x - props->zoneSizeX * 0.5f;
		z.minY = inst->m_translation.y - props->zoneSizeY * 0.5f;
		z.maxX = inst->m_translation.x + props->zoneSizeX * 0.5f;
		z.maxY = inst->m_translation.y + props->zoneSizeY * 0.5f;
		z.colour = IM_COL32(c >> 24 & 0xFF, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF);
		out.push_back(z);
	}
}

// ---- drawing ------------------------------------------------------------------

// A convex polygon, as a fan (with a texture, uv per corner).
void
drawFan(ImDrawList *dl, rw::Texture *tex, const std::vector<ImVec2> &pos, const std::vector<ImVec2> &uv, ImU32 colour)
{
	int n = (int)pos.size();
	if(n < 3)
		return;
	dl->PushTexture((ImTextureID)tex);
	dl->PrimReserve((n - 2) * 3, n);
	ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	for(int i = 0; i < n; i++)
		dl->PrimWriteVtx(pos[i], uv[i], colour);
	for(int i = 1; i < n - 1; i++){
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx((ImDrawIdx)(base + i));
		dl->PrimWriteIdx((ImDrawIdx)(base + i + 1));
	}
	dl->PopTexture();
}

// ClipRadarPoly: a polygon in radar space (the unit circle is the radar's
// edge) clipped to the circle, as a polygon of CIRCLE_SIDES sides.
std::vector<V2>
clipToCircle(std::vector<V2> poly)
{
	const float edge = cosf((float)M_PI / CIRCLE_SIDES);
	for(int i = 0; i < CIRCLE_SIDES && !poly.empty(); i++){
		float a = (i + 0.5f) * 2.0f * (float)M_PI / CIRCLE_SIDES;
		V2 n = { cosf(a), sinf(a) };
		std::vector<V2> out;
		for(size_t k = 0; k < poly.size(); k++){
			V2 p = poly[k], q = poly[(k + 1) % poly.size()];
			float dp = p.x * n.x + p.y * n.y - edge, dq = q.x * n.x + q.y * n.y - edge;
			if(dp <= 0.0f)
				out.push_back(p);
			if((dp < 0.0f) != (dq < 0.0f) && dp != dq){
				float t = dp / (dp - dq);
				out.push_back({ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t });
			}
		}
		poly.swap(out);
	}
	return poly;
}

// The radar's frame: TransformRealWorldPointToRadarSpace and back, then
// TransformRadarPointToScreenSpace.
struct RadarFrameSpace {
	V2 origin;
	float range, c, s;	// the camera's heading, cached as CalculateCachedSinCos does
	ImVec2 centre;
	float rx, ry;	// an ellipse where the screen is stretched

	V2 toRadar(float wx, float wy) const {
		float dx = (wx - origin.x) / range, dy = (wy - origin.y) / range;
		return { dx * c + dy * s, -dx * s + dy * c };
	}
	V2 toWorld(V2 r) const {
		return { origin.x + (r.x * c - r.y * s) * range, origin.y + (r.x * s + r.y * c) * range };
	}
	ImVec2 toScreen(V2 r) const {
		return ImVec2(centre.x + r.x * rx, centre.y - r.y * ry);
	}
};

// Half a texel in from a tile's edges: DrawRadarMap clamps the tiles, ImGui
// wraps them, which shows the far edge as a line where two tiles meet.
float
texelInset(rw::Texture *tex)
{
	return tex && tex->raster && tex->raster->width > 0 ? 0.5f / tex->raster->width : 0.0f;
}

// A world rectangle on the radar, clipped; with a tile's texture its uv.
void
drawWorldRect(ImDrawList *dl, const RadarFrameSpace &f, float minX, float minY, float maxX, float maxY,
	rw::Texture *tex, ImU32 colour)
{
	std::vector<V2> poly = clipToCircle({ f.toRadar(minX, minY), f.toRadar(maxX, minY),
		f.toRadar(maxX, maxY), f.toRadar(minX, maxY) });
	if(poly.size() < 3)
		return;
	std::vector<ImVec2> pos, uv;
	float in = texelInset(tex);
	for(const V2 &r : poly){
		pos.push_back(f.toScreen(r));
		V2 w = f.toWorld(r);
		float u = (w.x - minX) / (maxX - minX), v = (maxY - w.y) / (maxY - minY);
		uv.push_back(ImVec2(in + u * (1.0f - 2.0f * in), in + v * (1.0f - 2.0f * in)));
	}
	if(tex)
		drawFan(dl, tex, pos, uv, colour);
	else
		dl->AddConvexPolyFilled(pos.data(), (int)pos.size(), colour);
}

// A HUD sprite centred on p, half-size h, turned by angle (radians, clockwise).
void
drawSprite(ImDrawList *dl, rw::Texture *tex, ImVec2 p, float h, float angle)
{
	float c = cosf(angle) * h, s = sinf(angle) * h;
	ImVec2 corner[4] = { ImVec2(-1, -1), ImVec2(1, -1), ImVec2(1, 1), ImVec2(-1, 1) };
	ImVec2 q[4];
	for(int i = 0; i < 4; i++)
		q[i] = ImVec2(p.x + corner[i].x * c - corner[i].y * s, p.y + corner[i].x * s + corner[i].y * c);
	dl->AddImageQuad((ImTextureID)tex, q[0], q[1], q[2], q[3]);
}

// The part of the main window the 3D view shows (as the pressed-keys overlay finds it).
void
viewRect(ImVec2 *pos, ImVec2 *size)
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	*pos = vp->WorkPos;
	*size = vp->WorkSize;
#ifdef IMGUI_HAS_DOCK
	ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaDockSpace"));
	if(central && central->Size.x > 32.0f && central->Size.y > 32.0f){
		*pos = central->Pos;
		*size = central->Size;
	}
#endif
}

void drawRadarDisc(ImDrawList *dl, const std::vector<Zone> &zones, ImVec2 centre, ImVec2 radius, float k);

// CHud::DrawRadar's place on a 640 x 448 screen: 40 from the left, the
// circle's centre 66 above the bottom, 38 across (the height's half).
void
drawRadar(const std::vector<Zone> &zones)
{
	ImVec2 vpos, vsize;
	viewRect(&vpos, &vsize);
	float k = vsize.y / 448.0f;
	float radius = 38.0f * k, ring = 4.0f * k;
	ImVec2 centre(vpos.x + 40.0f * k + radius, vpos.y + vsize.y - 66.0f * k);

	// A window under the circle, so clicks open the map instead of picking in 3D.
	ImGui::SetNextWindowPos(ImVec2(centre.x - radius - ring, centre.y - radius - ring));
	ImGui::SetNextWindowSize(ImVec2((radius + ring) * 2.0f, (radius + ring) * 2.0f));
#ifdef IMGUI_HAS_VIEWPORT
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
#endif
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
#ifdef IMGUI_HAS_DOCK
	flags |= ImGuiWindowFlags_NoDocking;
#endif
	bool open = ImGui::Begin("##radar", nil, flags);
	ImGui::PopStyleVar(2);
	if(!open){
		ImGui::End();
		return;
	}
	ImGui::InvisibleButton("##radar_button", ImGui::GetContentRegionAvail());
	ImVec2 mouse = ImGui::GetIO().MousePos;
	float mx = mouse.x - centre.x, my = mouse.y - centre.y;
	bool inside = mx * mx + my * my <= (radius + ring) * (radius + ring);
	if(ImGui::IsItemClicked(ImGuiMouseButton_Left) && inside)
		gShowMap = true;
	if(ImGui::IsItemHovered() && inside)
		ImGui::SetTooltip("%s", _("Click for the full map. Right-click for the radar's range."));
	if(ImGui::BeginPopupContextItem("##radar_menu")){
		if(ImGui::MenuItem(_("On foot (180 m)"), nil, gRange == RANGE_ON_FOOT))
			gRange = RANGE_ON_FOOT;
		if(ImGui::MenuItem(_("Driving fast (350 m)"), nil, gRange == RANGE_FAST))
			gRange = RANGE_FAST;
		ImGui::Separator();
		if(ImGui::MenuItem(_("Full Map")))
			gShowMap = true;
		if(ImGui::MenuItem(_("Hide Radar")))
			gShowRadar = false;
		ImGui::EndPopup();
	}

	drawRadarDisc(ImGui::GetWindowDrawList(), zones, centre, ImVec2(radius, radius), k);
	ImGui::End();
}

// The radar itself: the map around the camera in an ellipse of the radii,
// the disc around it, the player's arrow and the N. k: pixels per unit of
// the 448-high screen.
void
drawRadarDisc(ImDrawList *dl, const std::vector<Zone> &zones, ImVec2 centre, ImVec2 radius, float k)
{
	ImVec2 ring(4.0f * k * radius.x / radius.y, 4.0f * k);
	RadarFrameSpace f;
	f.origin = { TheCamera.m_position.x, TheCamera.m_position.y };
	f.range = gRange;
	float heading = TheCamera.getHeading();
	f.c = cosf(heading);
	f.s = sinf(heading);
	f.centre = centre;
	f.rx = radius.x;
	f.ry = radius.y;

	ImDrawListFlags oldFlags = dl->Flags;
	dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;	// no seams between tiles and zones
	// DrawRadarMap: the sea, the tiles around the player, then the gang zones.
	dl->AddEllipseFilled(centre, radius, SEA, 0.0f, CIRCLE_SIDES);
	float reach = gRange * 1.5f;
	int x0 = (int)floorf((f.origin.x - reach + MAP_EDGE) / TILE_SIZE), x1 = (int)floorf((f.origin.x + reach + MAP_EDGE) / TILE_SIZE);
	int y0 = (int)floorf((MAP_EDGE - f.origin.y - reach) / TILE_SIZE), y1 = (int)floorf((MAP_EDGE - f.origin.y + reach) / TILE_SIZE);
	for(int ty = std::max(y0, 0); ty <= std::min(y1, TILES - 1); ty++)
		for(int tx = std::max(x0, 0); tx <= std::min(x1, TILES - 1); tx++){
			rw::Texture *tex = tileTexture(tx, ty);
			if(tex == nil)
				continue;
			float left = tx * TILE_SIZE - MAP_EDGE, top = MAP_EDGE - ty * TILE_SIZE;
			drawWorldRect(dl, f, left, top - TILE_SIZE, left + TILE_SIZE, top, tex, IM_COL32_WHITE);
		}
	// Only the zones within reach of the circle: radar art has thousands.
	for(const Zone &z : zones)
		if(z.maxX > z.minX && z.maxY > z.minY && z.maxX > f.origin.x - reach && z.minX < f.origin.x + reach &&
		   z.maxY > f.origin.y - reach && z.minY < f.origin.y + reach)
			drawWorldRect(dl, f, z.minX, z.minY, z.maxX, z.maxY, nil, z.colour);
	dl->Flags = oldFlags;

	// CHud: the disc around it (a quarter, mirrored), the player and the N.
	if(rw::Texture *disc = hudTexture(HUD_DISC)){
		float rx = radius.x + ring.x, ry = radius.y + ring.y;
		ImTextureID id = (ImTextureID)disc;
		dl->AddImage(id, ImVec2(centre.x - rx, centre.y - ry), centre, ImVec2(0, 0), ImVec2(1, 1));
		dl->AddImage(id, ImVec2(centre.x, centre.y - ry), ImVec2(centre.x + rx, centre.y), ImVec2(1, 0), ImVec2(0, 1));
		dl->AddImage(id, ImVec2(centre.x - rx, centre.y), ImVec2(centre.x, centre.y + ry), ImVec2(0, 1), ImVec2(1, 0));
		dl->AddImage(id, centre, ImVec2(centre.x + rx, centre.y + ry), ImVec2(1, 1), ImVec2(0, 0));
	}else
		dl->AddEllipse(centre, ImVec2(radius.x + ring.x * 0.5f, radius.y + ring.y * 0.5f), IM_COL32(0, 0, 0, 255),
			0.0f, CIRCLE_SIDES, ring.y);
	// The camera looks up the radar, so the player's arrow points straight up.
	if(rw::Texture *arrow = hudTexture(HUD_CENTRE))
		drawSprite(dl, arrow, centre, 8.0f * k, 0.0f);
	else
		dl->AddTriangleFilled(ImVec2(centre.x, centre.y - 6.0f * k), ImVec2(centre.x + 4.0f * k, centre.y + 4.0f * k),
			ImVec2(centre.x - 4.0f * k, centre.y + 4.0f * k), IM_COL32_WHITE);
	ImVec2 north(centre.x + f.s * radius.x, centre.y - f.c * radius.y);
	if(rw::Texture *n = hudTexture(HUD_NORTH))
		drawSprite(dl, n, north, 8.0f * k, 0.0f);
	else
		dl->AddText(ImVec2(north.x - 4.0f, north.y - 7.0f), IM_COL32_WHITE, "N");
}

// ---- the full map ---------------------------------------------------------------

// See the whole map with its gang zones, find a place on it and fly there.
void
drawMap(const std::vector<Zone> &zones)
{
	float fs = ImGui::GetFontSize();
	const char *title = _("Full Map###radar_map");
	ImGuiWindowFlags flags = UiPlaceToolWindow(title, 36.0f, false);
	if(!ImGui::Begin(title, &gShowMap, flags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
		ImGui::End();
		return;
	}
	if(!gMapPlaced){
		gMapPlaced = true;
		gMapX = TheCamera.m_position.x;
		gMapY = TheCamera.m_position.y;
	}
	// The toolbar, one row: back to the camera, zoom, the zones layer, and
	// how to move about in a tooltip at its end.
	if(ImGui::Button(TI(ICON_FA_LOCATION_CROSSHAIRS, "Centre on Camera"))){
		gMapX = TheCamera.m_position.x;
		gMapY = TheCamera.m_position.y;
	}
	float h = ImGui::GetFrameHeight();
	ImGui::SameLine();
	if(ImGui::Button("-##radar_zoom_out", ImVec2(h, h)))
		gMapScale = std::max(gMapScale / 1.25f, 0.01f);
	ImGui::SetItemTooltip("%s", _("Zoom out"));
	ImGui::SameLine(0.0f, 0.0f);
	if(ImGui::Button("+##radar_zoom_in", ImVec2(h, h)))
		gMapScale = std::min(gMapScale * 1.25f, 16.0f);
	ImGui::SetItemTooltip("%s", _("Zoom in"));
	ImGui::SameLine();
	UiCheckbox(_("Gang zones"), &gMapZones);
	const char *help = ICON_FA_CIRCLE_INFO;
	float helpW = ImGui::CalcTextSize(help).x;
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - helpW));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", help);
	ImGui::SetItemTooltip("%s", _("Drag to move, wheel to zoom, double-click to fly there."));

	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImVec2 size = ImGui::GetContentRegionAvail();
	size.x = std::max(size.x, 64.0f);
	size.y = std::max(size.y, 64.0f);
	ImGui::InvisibleButton("##radar_map_canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	bool hovered = ImGui::IsItemHovered();
	ImGuiIO &io = ImGui::GetIO();
	ImVec2 mid(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);

	// Map coordinates: north up, as the pause menu's map.
	auto toScreen = [&](float wx, float wy) { return ImVec2(mid.x + (wx - gMapX) * gMapScale, mid.y - (wy - gMapY) * gMapScale); };
	auto toWorldX = [&](float sx) { return gMapX + (sx - mid.x) / gMapScale; };
	auto toWorldY = [&](float sy) { return gMapY - (sy - mid.y) / gMapScale; };

	if(ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))){
		gMapX -= io.MouseDelta.x / gMapScale;
		gMapY += io.MouseDelta.y / gMapScale;
	}
	if(hovered && io.MouseWheel != 0.0f){
		// Zoom about the point under the mouse.
		float wx = toWorldX(io.MousePos.x), wy = toWorldY(io.MousePos.y);
		gMapScale = std::min(std::max(gMapScale * powf(1.25f, io.MouseWheel), 0.01f), 16.0f);
		gMapX = wx - (io.MousePos.x - mid.x) / gMapScale;
		gMapY = wy + (io.MousePos.y - mid.y) / gMapScale;
	}
	if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		TheCamera.setTarget({ toWorldX(io.MousePos.x), toWorldY(io.MousePos.y), TheCamera.m_target.z });

	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p1(p0.x + size.x, p0.y + size.y);
	dl->PushClipRect(p0, p1, true);
	dl->AddRectFilled(p0, p1, SEA);
	for(int ty = 0; ty < TILES; ty++)
		for(int tx = 0; tx < TILES; tx++){
			float left = tx * TILE_SIZE - MAP_EDGE, top = MAP_EDGE - ty * TILE_SIZE;
			ImVec2 a = toScreen(left, top), b = toScreen(left + TILE_SIZE, top - TILE_SIZE);
			if(b.x < p0.x || a.x > p1.x || b.y < p0.y || a.y > p1.y)
				continue;
			if(rw::Texture *tex = tileTexture(tx, ty)){
				float in = texelInset(tex);
				dl->AddImage((ImTextureID)tex, a, b, ImVec2(in, in), ImVec2(1.0f - in, 1.0f - in));
			}
		}
	for(const Zone &z : zones){
		if(!gMapZones)
			break;
		ImVec2 a = toScreen(z.minX, z.maxY), b = toScreen(z.maxX, z.minY);
		if(b.x < p0.x || a.x > p1.x || b.y < p0.y || a.y > p1.y)
			continue;
		dl->AddRectFilled(a, b, z.colour);
	}
	// The camera, an arrow the way it looks.
	float heading = TheCamera.getHeading();
	ImVec2 cam = toScreen(TheCamera.m_position.x, TheCamera.m_position.y);
	if(rw::Texture *arrow = hudTexture(HUD_CENTRE))
		drawSprite(dl, arrow, cam, fs * 0.6f, -heading);
	else{
		float c = cosf(-heading), s = sinf(-heading), h = fs * 0.5f;
		auto at = [&](float x, float y) { return ImVec2(cam.x + x * c - y * s, cam.y + x * s + y * c); };
		dl->AddTriangleFilled(at(0, -h), at(h * 0.7f, h), at(-h * 0.7f, h), IM_COL32_WHITE);
	}
	if(hovered){
		char text[64];
		snprintf(text, sizeof(text), "%.1f, %.1f", toWorldX(io.MousePos.x), toWorldY(io.MousePos.y));
		ImVec2 at(p0.x + fs * 0.4f, p1.y - fs * 1.4f);
		dl->AddRectFilled(ImVec2(at.x - 3, at.y - 2), ImVec2(at.x + ImGui::CalcTextSize(text).x + 3, at.y + fs + 2),
			IM_COL32(0, 0, 0, 160));
		dl->AddText(at, IM_COL32_WHITE, text);
	}
	dl->PopClipRect();
	ImGui::End();
}

}	// namespace

void
RadarFrame(void)
{
	if(!gShowRadar && !gShowMap)
		return;
	if(StartScreenOpen() || InTextDrawMode())
		return;
	static std::vector<Zone> zones;
	collectZones(zones);
	if(gShowRadar)
		drawRadar(zones);
	if(gShowMap)
		drawMap(zones);
}

void
DrawRadarOnCanvas(ImDrawList *dl, ImVec2 centre, ImVec2 radius, float k)
{
	static std::vector<Zone> zones;
	collectZones(zones);
	drawRadarDisc(dl, zones, centre, radius, k);
}

bool
RadarEnabled(void)
{
	return gShowRadar;
}

void
RadarSetEnabled(bool on)
{
	gShowRadar = on;
}

bool
RadarMapOpen(void)
{
	return gShowMap;
}

bool
RadarLoadSetting(const char *key, const char *value)
{
	if(strcmp(key, "radar") == 0)
		gShowRadar = atoi(value) != 0;
	else if(strcmp(key, "radar_range") == 0)
		gRange = atof(value) >= RANGE_FAST ? RANGE_FAST : RANGE_ON_FOOT;
	else
		return false;
	return true;
}

void
RadarSaveSettings(FILE *f)
{
	fprintf(f, "radar %d\n", gShowRadar ? 1 : 0);
	fprintf(f, "radar_range %.0f\n", gRange);
}
