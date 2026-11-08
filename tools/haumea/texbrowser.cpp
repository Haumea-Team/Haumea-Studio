// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "workspace.h"
#include "icons.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

// The Texture Browser: every texture of the game's TXDs, SA-MP's and the
// workspace's custom models, searchable by name. Each one carries the model
// whose TXD it is, the pair SA-MP's client resolves (CheckMaterialTexture),
// and applies to the selected objects' material slot in one click.

// ---- Index ----

struct TexEntry {
	int txdSlot;
	char name[32];
	uint8 tiles;	// TILES_*: whether it repeats without a seam (textureTilesFromData)
};
enum { TILES_ACROSS = 1, TILES_DOWN = 2 };

static std::vector<TexEntry> gIndex;
static int gNextSlot;	// TXD slots indexed so far
static std::vector<int> gOwner;	// txd slot -> model whose TXD it is, or -1
static std::vector<uint8> gKind;	// txd slot -> KIND_*
enum { KIND_GAME, KIND_SAMP, KIND_CUSTOM };

// Whether a texture tiles: decoded at its first mip level of 64 pixels or
// less, the colour difference between its left and right columns (and its
// top and bottom rows) against the usual difference between neighbouring
// columns (rows). Where an edge meets the opposite one no worse than
// neighbours meet inside, the texture repeats without a seam; a grassy
// texture's edges differ as much as its insides do, a sign's or a car's far
// more. The method is tuned on the game's gta3.img.
// s: the TextureNative's struct data, end: the end of the chunk.
static uint8
textureTilesFromData(const uint8 *s, const uint8 *end)
{
	if(end - s < 88)
		return 0;
	uint32 platform = *(const uint32*)s;
	uint32 format = *(const uint32*)(s + 72), d3dformat = *(const uint32*)(s + 76);
	int w = *(const uint16*)(s + 80), h = *(const uint16*)(s + 82);
	int levels = s[85], compression = s[87];
	const uint8 *q = s + 88;
	const uint8 *palette = nil;
	if(format & 0x2000){ palette = q; q += 1024; }	// PAL8
	else if(format & 0x4000)
		return 0;	// PAL4: none in the game's textures
	int dxt = 0;
	if(platform == 8 && (compression == 1 || compression == 3 || compression == 5))
		dxt = compression;
	else if(platform == 9 && (compression & 8))
		dxt = d3dformat == 0x31545844 ? 1 : d3dformat == 0x33545844 ? 3 : d3dformat == 0x35545844 ? 5 : -1;
	else if(platform != 8 && platform != 9)
		return 0;
	if(dxt < 0)
		return 0;
	// The level to look at.
	const uint8 *pixels = nil;
	uint32 size = 0;
	for(int lv = 0; lv < levels; lv++){
		if(end - q < 4)
			return 0;
		size = *(const uint32*)q;
		q += 4;
		if(size > (uint32)(end - q))
			return 0;
		if(std::max(w, h) <= 64 || lv == levels - 1){
			pixels = q;
			break;
		}
		q += size;
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	if(pixels == nil || w < 4 || h < 4)
		return 0;
	std::vector<uint8> rgb(w * h * 3);
	if(dxt){
		if(size < (uint32)((w / 4) * (h / 4) * (dxt == 1 ? 8 : 16)) || w % 4 || h % 4)
			return 0;
		rw::Image *img = rw::Image::create(w, h, 32);
		img->allocate();
		img->setPixelsDXT(dxt, (uint8*)pixels);
		for(int i = 0; i < w * h; i++)
			memcpy(&rgb[i * 3], &img->pixels[i * 4], 3);
		img->destroy();
	}else{
		int bpp = palette ? 1 : (format & 0xF00) == 0x500 || (format & 0xF00) == 0x600 ? 4 : 2;
		if(size < (uint32)(w * h * bpp) || (palette && end - palette < 1024))
			return 0;
		for(int i = 0; i < w * h; i++){
			uint8 *o = &rgb[i * 3];
			if(palette){
				memcpy(o, palette + 4 * pixels[i], 3);
				continue;
			}
			uint32 c = bpp == 4 ? *(const uint32*)(pixels + 4 * i) : *(const uint16*)(pixels + 2 * i);
			switch(format & 0xF00){
			case 0x500: case 0x600:	// B, G, R, A in memory
				o[0] = c >> 16 & 0xFF; o[1] = c >> 8 & 0xFF; o[2] = c & 0xFF; break;
			case 0x200:	// 565
				o[0] = (c >> 11 & 31) * 255 / 31; o[1] = (c >> 5 & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; break;
			case 0x100:	// 1555
				o[0] = (c >> 10 & 31) * 255 / 31; o[1] = (c >> 5 & 31) * 255 / 31; o[2] = (c & 31) * 255 / 31; break;
			case 0x300:	// 4444
				o[0] = (c >> 8 & 15) * 17; o[1] = (c >> 4 & 15) * 17; o[2] = (c & 15) * 17; break;
			default:
				return 0;
			}
		}
	}
	auto diff = [&](int x0, int y0, int x1, int y1){
		const uint8 *a = &rgb[(y0 * w + x0) * 3], *b = &rgb[(y1 * w + x1) * 3];
		return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
	};
	double edgeX = 0, insideX = 0, edgeY = 0, insideY = 0;
	for(int y = 0; y < h; y++){
		edgeX += diff(0, y, w - 1, y);
		for(int x = 0; x + 1 < w; x++)
			insideX += diff(x, y, x + 1, y);
	}
	for(int x = 0; x < w; x++){
		edgeY += diff(x, 0, x, h - 1);
		for(int y = 0; y + 1 < h; y++)
			insideY += diff(x, y, x, y + 1);
	}
	edgeX /= h; insideX /= h * (w - 1);
	edgeY /= w; insideY /= w * (h - 1);
	// Sums of three channels (0 to 765): no worse than 1.2 times the inside, give or take 4.
	return (edgeX <= 1.2 * insideX + 4.0 ? TILES_ACROSS : 0) | (edgeY <= 1.2 * insideY + 4.0 ? TILES_DOWN : 0);
}

// Texture names straight from a TXD's chunks, and whether each tiles.
static void
readTxdNames(const uint8 *data, int size, int slot)
{
	if(data == nil || size < 12 || *(const uint32*)data != 0x16)
		return;
	int end = std::min(size, 12 + (int)*(const uint32*)(data + 4));
	int p = 12;
	while(p + 12 <= end){
		uint32 type = *(const uint32*)(data + p);
		uint32 len = *(const uint32*)(data + p + 4);
		if(type == 0x15 && p + 24 + 8 + 32 <= end && *(const uint32*)(data + p + 12) == 1){
			// TextureNative: struct chunk, then platform, filter flags, name[32].
			const char *name = (const char*)(data + p + 24 + 8);
			TexEntry e;
			e.txdSlot = slot;
			memcpy(e.name, name, 31);
			e.name[31] = '\0';
			e.tiles = textureTilesFromData(data + p + 24, data + std::min(end, (int)(p + 12 + std::min(len, (uint32)(end - p)))));
			if(e.name[0])
				gIndex.push_back(e);
		}
		if(len > (uint32)(end - p))
			break;
		p += 12 + len;
	}
}

static uint8*
readLoose(const char *path, int *size)
{
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return nil;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8 *buf = (uint8*)malloc(n > 0 ? n : 1);
	*size = (int)fread(buf, 1, n, f);
	fclose(f);
	return buf;
}

static bool
containsCi(const char *haystack, const char *needle)
{
	for(const char *h = haystack; *h; h++){
		const char *a = h, *b = needle;
		while(*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)){ a++; b++; }
		if(*b == '\0')
			return true;
	}
	return false;
}

// The TXD name Pawn uses: a custom TXD's file name, else the slot's.
static const char*
pawnTxdName(int slot)
{
	const char *custom = GetCustomModelTxdName(slot);
	if(custom)
		return custom;
	TxdDef *txd = GetTxdDef(slot);
	return txd ? txd->name : "";
}

static void
indexSlot(int slot)
{
	TxdDef *txd = GetTxdDef(slot);
	if(txd == nil || txd->name[0] == '\0' || txd->name[0] == '~' && strncmp(txd->name, "~custom", 7) != 0)
		return;	// the editor's own TXDs (textdraw fonts and sprites)
	const char *customPath = GetCustomModelTxdPath(slot);
	if(customPath){
		gKind[slot] = KIND_CUSTOM;
		int size = 0;
		uint8 *data = readLoose(customPath, &size);
		readTxdNames(data, size, slot);
		free(data);
	}else if(txd->imageIndex >= 0){
		char source[1024];
		if(GetCdImageEntrySourcePath(txd->imageIndex, source, sizeof(source)) && containsCi(source, "samp"))
			gKind[slot] = KIND_SAMP;
		int size = 0;
		uint8 *data = ReadFileFromImage(txd->imageIndex, &size);	// the shared streaming buffer
		readTxdNames(data, size, slot);
	}else if(txd->txd){
		// Loaded from a loose file (generic vehicle.txd and the like).
		for(int i = 0; i < GetTxdTextureCount(slot); i++){
			rw::Texture *t = GetTxdTexture(slot, i);
			if(t == nil || t->name[0] == '\0')
				continue;
			TexEntry e;
			e.txdSlot = slot;
			strncpy(e.name, t->name, 31);
			e.name[31] = '\0';
			e.tiles = 0;	// already turned into the renderer's rasters
			gIndex.push_back(e);
		}
	}
}

static void
buildOwners(void)
{
	int n = GetNumTxdSlots();
	gOwner.assign(n, -1);
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		ObjectDef *obj = GetObjectDef(id);
		if(obj && obj->m_txdSlot >= 0 && obj->m_txdSlot < n && gOwner[obj->m_txdSlot] < 0)
			gOwner[obj->m_txdSlot] = id;
	}
	// A parent TXD (like the vehicles' shared one) belongs to a model that inherits it.
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		ObjectDef *obj = GetObjectDef(id);
		if(obj == nil)
			continue;
		for(int slot = obj->m_txdSlot, depth = 0; slot >= 0 && slot < n && depth < 8; depth++){
			if(gOwner[slot] < 0)
				gOwner[slot] = id;
			TxdDef *txd = GetTxdDef(slot);
			slot = txd ? txd->parentId : -1;
		}
	}
}

// Indexes up to budget TXDs; true when everything is indexed.
static bool
indexStep(int budget)
{
	int n = GetNumTxdSlots();
	if((int)gKind.size() < n)
		gKind.resize(n, KIND_GAME);
	if(gNextSlot == 0)
		gIndex.clear();
	for(; gNextSlot < n && budget > 0; budget--)
		indexSlot(gNextSlot++);
	if(gNextSlot >= n && (int)gOwner.size() != n)
		buildOwners();
	return gNextSlot >= n;
}

static int
ownerOf(int slot)
{
	return slot >= 0 && slot < (int)gOwner.size() ? gOwner[slot] : -1;
}

static int
tstudioIndexOf(const char *txd, const char *texture)
{
	static std::vector<std::pair<std::string, int>> sorted;
	static std::string sortedFrom;
	int n;
	std::string from;
	const TStudioTexture *tstudioTextures = TStudioTextureList(&n, &from);
	if(sorted.empty() || from != sortedFrom){
		sorted.clear();
		sortedFrom = from;
		for(int i = 1; i < n; i++){
			std::string key = std::string(tstudioTextures[i].txd) + ":" + tstudioTextures[i].texture;
			std::transform(key.begin(), key.end(), key.begin(), ::tolower);
			sorted.push_back(std::make_pair(key, i));
		}
		std::sort(sorted.begin(), sorted.end());
	}
	std::string key = std::string(txd) + ":" + texture;
	std::transform(key.begin(), key.end(), key.begin(), ::tolower);
	auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(key, 0));
	return it != sorted.end() && it->first == key ? it->second : 0;
}

int
SearchTextures(const char *query, int limit, std::vector<TextureSearchResult> &out)
{
	while(!indexStep(1 << 20));
	out.clear();
	for(size_t i = 0; i < gIndex.size() && (int)out.size() < limit; i++){
		const TexEntry &e = gIndex[i];
		TxdDef *txd = GetTxdDef(e.txdSlot);
		if(txd == nil || (query && query[0] && !containsCi(e.name, query) &&
		   !containsCi(pawnTxdName(e.txdSlot), query)))
			continue;
		TextureSearchResult r;
		r.model = PawnModelId(ownerOf(e.txdSlot));
		r.txd = pawnTxdName(e.txdSlot);
		r.texture = e.name;
		r.samp = gKind[e.txdSlot] == KIND_SAMP;
		r.custom = gKind[e.txdSlot] == KIND_CUSTOM;
		r.tstudioIndex = tstudioIndexOf(txd->name, e.name);
		r.tiles = e.tiles;
		out.push_back(r);
	}
	return (int)out.size();
}

// ---- Recent and favourites (in the config folder) ----

static std::vector<std::string> gRecent;
static std::set<std::string> gFavourites;
static bool gListsLoaded;

static std::string
entryKey(int slot, const char *texture)
{
	TxdDef *txd = GetTxdDef(slot);
	std::string key = std::string(txd ? txd->name : "") + ":" + texture;
	std::transform(key.begin(), key.end(), key.begin(), ::tolower);
	return key;
}

static void
loadLists(void)
{
	if(gListsLoaded)
		return;
	gListsLoaded = true;
	char line[256];
	FILE *f = fopen((std::string(GetConfigDir()) + "/texture_recent.txt").c_str(), "r");
	while(f && fgets(line, sizeof(line), f)){
		line[strcspn(line, "\r\n")] = '\0';
		if(line[0]) gRecent.push_back(line);
	}
	if(f) fclose(f);
	f = fopen((std::string(GetConfigDir()) + "/texture_favourites.txt").c_str(), "r");
	while(f && fgets(line, sizeof(line), f)){
		line[strcspn(line, "\r\n")] = '\0';
		if(line[0]) gFavourites.insert(line);
	}
	if(f) fclose(f);
}

static void
saveList(const char *file, const std::vector<std::string> &items)
{
	FILE *f = fopen((std::string(GetConfigDir()) + "/" + file).c_str(), "w");
	if(f == nil)
		return;
	for(size_t i = 0; i < items.size(); i++)
		fprintf(f, "%s\n", items[i].c_str());
	fclose(f);
}

static void
noteRecent(const std::string &key)
{
	gRecent.erase(std::remove(gRecent.begin(), gRecent.end(), key), gRecent.end());
	gRecent.insert(gRecent.begin(), key);
	if(gRecent.size() > 64)
		gRecent.resize(64);
	saveList("texture_recent.txt", gRecent);
}

static void
toggleFavourite(const std::string &key)
{
	if(gFavourites.count(key))
		gFavourites.erase(key);
	else
		gFavourites.insert(key);
	saveList("texture_favourites.txt", std::vector<std::string>(gFavourites.begin(), gFavourites.end()));
}

// ---- Applying ----

struct Pick {
	int model;	// editor model ID
	std::string txd, texture;
};

static int gSlot;

static void
selectedTargets(std::vector<ObjectInst*> &out)
{
	out.clear();
	// Object Info's "Edit all selected": every selected object that has the
	// slot, or only the one Object Info shows.
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst && !inst->m_isDeleted && !inst->m_isVehicle && IsPawnObjectEditable(inst) &&
		   GetSampElementKind(inst) == SAMP_ELEMENT_OBJECT && gSlot < GetInstanceMaterialCount(inst))
			out.push_back(inst);
		if(!GetBatchEditSelection() && !out.empty())
			break;
	}
}

static ObjectMaterialOverride
materialFor(ObjectInst *inst, const Pick &pick)
{
	ObjectMaterialOverride m;
	const ObjectMaterialOverride *current = GetObjectMaterialOverride(inst, gSlot);
	if(current && (current->active || current->textEnabled))
		m = *current;
	else
		InitObjectMaterialOverride(&m, inst->m_objectId);
	m.active = true;
	m.textEnabled = false;
	m.hasTexture = true;
	m.sourceModel = pick.model;
	snprintf(m.txdName, sizeof(m.txdName), "%s", pick.txd.c_str());
	snprintf(m.textureName, sizeof(m.textureName), "%s", pick.texture.c_str());
	return m;
}

// The hover preview: what was in the slot before, to put back.
static struct {
	bool active;
	std::string key;
	int slot;
	std::vector<ObjectInst*> insts;
	std::vector<ObjectMaterialOverride> before;
} gPreview;

static void
endPreview(void)
{
	if(!gPreview.active)
		return;
	for(size_t i = 0; i < gPreview.insts.size(); i++)
		PreviewObjectMaterialOverride(gPreview.insts[i], gPreview.slot, gPreview.before[i]);
	gPreview = decltype(gPreview)();
}

static void
preview(const Pick &pick, const std::string &key)
{
	if(gPreview.active && gPreview.key == key && gPreview.slot == gSlot)
		return;
	endPreview();
	std::vector<ObjectInst*> targets;
	selectedTargets(targets);
	if(targets.empty())
		return;
	gPreview.active = true;
	gPreview.key = key;
	gPreview.slot = gSlot;
	for(size_t i = 0; i < targets.size(); i++){
		ObjectMaterialOverride before;
		const ObjectMaterialOverride *current = GetObjectMaterialOverride(targets[i], gSlot);
		if(current)
			before = *current;
		else
			InitObjectMaterialOverride(&before, targets[i]->m_objectId);
		gPreview.insts.push_back(targets[i]);
		gPreview.before.push_back(before);
		PreviewObjectMaterialOverride(targets[i], gSlot, materialFor(targets[i], pick));
	}
}

static int
applyTextureSelection(const Pick &pick, const std::string &key)
{
	endPreview();
	if(pick.model < 0 || GetObjectDef(pick.model) == nil){
		Toast(TOAST_SELECTION, _("No model loads TXD %s, so SA-MP can't use this texture"), pick.txd.c_str());
		return -1;
	}
	std::vector<ObjectInst*> targets;
	selectedTargets(targets);
	UndoBeginGroup();
	for(size_t i = 0; i < targets.size(); i++)
		SetObjectMaterialOverride(targets[i], gSlot, materialFor(targets[i], pick));
	UndoEndGroup();
	if(!targets.empty())
		noteRecent(key);
	return (int)targets.size();
}

// ---- Window ----

enum { SHOW_ALL, SHOW_SAMP, SHOW_CUSTOM, SHOW_RECENT, SHOW_FAVOURITES };
static ImGuiTextFilter gSearch, gTxdFilter, gTsSearch;
static int gShow;
// 0 any; else textures that tile (TexEntry::tiles): 1 left to right,
// 2 top to bottom, 3 both ways.
static int gTiles;

static float gTile = 5.0f;

static bool
passes(const TexEntry &e, const std::string &key)
{
	TxdDef *txd = GetTxdDef(e.txdSlot);
	if(txd == nil)
		return false;
	switch(gShow){
	case SHOW_SAMP: if(gKind[e.txdSlot] != KIND_SAMP) return false; break;
	case SHOW_CUSTOM: if(gKind[e.txdSlot] != KIND_CUSTOM) return false; break;
	case SHOW_FAVOURITES: if(!gFavourites.count(key)) return false; break;
	case SHOW_RECENT: if(std::find(gRecent.begin(), gRecent.end(), key) == gRecent.end()) return false; break;
	}
	if(gTiles && (e.tiles & gTiles) != gTiles)
		return false;
	return gSearch.PassFilter(e.name) && gTxdFilter.PassFilter(pawnTxdName(e.txdSlot));
}

static void
targetBar(void)
{
	std::vector<ObjectInst*> targets;
	selectedTargets(targets);
	ObjectInst *first = targets.empty() ? nil : targets[0];
	ObjectDef *obj = first ? GetObjectDef(first->m_objectId) : nil;
	rw::Atomic *atomic = obj && obj->m_type == ObjectDef::ATOMIC ? obj->m_atomics[0] : nil;
	int count = atomic && atomic->geometry ? atomic->geometry->matList.numMaterials : 0;
	if(first == nil){
		ImGui::TextDisabled("%s", _("Select objects to apply textures to them."));
		return;
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
	if(ImGui::InputInt(L("Slot"), &gSlot))
		gSlot = std::max(0, std::min(gSlot, std::max(0, count - 1)));
	ImGui::SetItemTooltip(_("The material slot (SetObjectMaterial's index) the texture goes to.\n%s has %d."),
		obj->m_name, count);
	ImGui::SameLine();
	bool all = GetBatchEditSelection();
	if(ImGui::Checkbox(L("Edit all selected"), &all))
		SetBatchEditSelection(all);
	ImGui::SetItemTooltip("%s", _("As in Object Info: every selected object that has this slot, as one undo step."));
	std::vector<ObjectInst*> applied;
	selectedTargets(applied);
	if(applied.size() > 1){
		ImGui::SameLine();
		ImGui::TextDisabled(_("Applies to %d selected"), (int)applied.size());
	}
}

static void
tileGrid(const std::vector<int> &items, bool tstudio)
{
	float font = ImGui::GetFontSize();
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	// Room for the scrollbar whether it shows or not, or the grid can flicker
	// between two sizes as the scrollbar comes and goes.
	float avail = ImGui::GetContentRegionAvail().x;
	if(!ImGui::GetCurrentWindow()->ScrollbarY)
		avail -= ImGui::GetStyle().ScrollbarSize;
	int columns = std::max(1, (int)((avail + spacing) / (font * gTile + spacing)));
	float tileW = floorf((avail - spacing * (columns - 1)) / columns);
	float tileH = tileW + ImGui::GetTextLineHeightWithSpacing();
	int rows = ((int)items.size() + columns - 1) / columns;
	bool hoveredAny = false;
	ImGuiListClipper clipper;
	clipper.Begin(rows, tileH + ImGui::GetStyle().ItemSpacing.y);
	while(clipper.Step()){
		for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++){
			for(int col = 0; col < columns; col++){
				int k = row * columns + col;
				if(k >= (int)items.size())
					break;
				if(col)
					ImGui::SameLine();
				Pick pick;
				int slot;
				char title[64];
				if(tstudio){
					int numTs;
					const TStudioTexture *tstudioTextures = TStudioTextureList(&numTs);
					int i = items[k];
					pick.model = EditorModelId(tstudioTextures[i].model);
					pick.txd = tstudioTextures[i].txd;
					pick.texture = tstudioTextures[i].texture;
					slot = FindTxdSlot(pick.txd.c_str());
					snprintf(title, sizeof(title), "%d %s", i, pick.texture.c_str());
				}else{
					const TexEntry &e = gIndex[items[k]];
					slot = e.txdSlot;
					pick.model = ownerOf(slot);
					pick.txd = pawnTxdName(slot);
					pick.texture = e.name;
					snprintf(title, sizeof(title), "%s", e.name);
				}
				std::string key = slot >= 0 ? entryKey(slot, pick.texture.c_str()) : pick.txd + ":" + pick.texture;
				ImGui::PushID(k);
				ImGui::BeginGroup();
				ImVec2 pos = ImGui::GetCursorScreenPos();
				// "##tex <name>" lets UI scripts find a tile by texture name.
				bool clicked = ImGui::InvisibleButton(("##tex " + pick.texture).c_str(), ImVec2(tileW, tileH));
				bool hovered = ImGui::IsItemHovered();
				ImDrawList *dl = ImGui::GetWindowDrawList();
				rw::Texture *tex = slot >= 0 ? FindTextureInTxd(slot, pick.texture.c_str()) : nil;
				dl->AddRectFilled(pos, ImVec2(pos.x + tileW, pos.y + tileW), IM_COL32(60, 60, 66, 255));
				if(tex)
					dl->AddImage((ImTextureID)tex, pos, ImVec2(pos.x + tileW, pos.y + tileW));
				if(gFavourites.count(key))
					dl->AddText(ImVec2(pos.x + 3, pos.y + 2), IM_COL32(255, 210, 60, 255), ICON_FA_STAR);
				if(hovered)
					dl->AddRect(pos, ImVec2(pos.x + tileW, pos.y + tileH), IM_COL32(255, 220, 60, 255), 0, 0, 2.0f);
				ImGui::PushClipRect(pos, ImVec2(pos.x + tileW, pos.y + tileH), true);
				dl->AddText(ImVec2(pos.x + 2, pos.y + tileW + 1), ImGui::GetColorU32(ImGuiCol_Text), title);
				ImGui::PopClipRect();
				ImGui::EndGroup();
				if(hovered){
					hoveredAny = true;
					preview(pick, key);
					ImGui::SetTooltip(pick.model < 0 ?
						_("%s\nTXD %s, model %d (no model loads this TXD)\nClick applies it; right-click keeps it in Favourites.") :
						_("%s\nTXD %s, model %d\nClick applies it; right-click keeps it in Favourites."),
						pick.texture.c_str(), pick.txd.c_str(), PawnModelId(pick.model));
				}
				if(clicked){
					int n = applyTextureSelection(pick, key);
					if(n == 0)
						Toast(TOAST_SELECTION, "%s", _("Select objects to apply the texture to"));
				}
				if(ImGui::IsItemClicked(ImGuiMouseButton_Right))
					toggleFavourite(key);
				ImGui::PopID();
			}
		}
	}
	if(!hoveredAny)
		endPreview();
}

void
DrawTextureBrowserWindow(bool *open)
{
	if(open == nil || !*open){
		endPreview();
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 36.0f), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(LI(ICON_FA_PAINTBRUSH, "Texture Browser"), open)){
		endPreview();
		ImGui::End();
		return;
	}
	loadLists();
	bool indexed = indexStep(64);
	targetBar();
	if(ImGui::BeginTabBar("##texbrowser")){
		if(ImGui::BeginTabItem(L("Textures"))){
			gSearch.Draw("##search", -FLT_MIN);
			if(!gSearch.IsActive()){
				ImVec2 p = ImGui::GetItemRectMin();
				ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 6, p.y + ImGui::GetStyle().FramePadding.y),
					ImGui::GetColorU32(ImGuiCol_TextDisabled), TI(ICON_FA_MAGNIFYING_GLASS, "Search texture names"));
			}
			static const char *shows[] = { N_("All textures"), N_("SA-MP textures only"), N_("Custom models"), N_("Recently used"), N_("Favourites") };
			const char *showNames[IM_ARRAYSIZE(shows)];
			for(int i = 0; i < IM_ARRAYSIZE(shows); i++)
				showNames[i] = _(shows[i]);
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
			ImGui::Combo("##show", &gShow, showNames, IM_ARRAYSIZE(shows));
			ImGui::SameLine();
			gTxdFilter.Draw("TXD", ImGui::GetFontSize() * 8.0f);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
			ImGui::SliderFloat("##tile", &gTile, 3.0f, 12.0f, _("Size"));
			static const char *tiles[] = { N_("Any tiling"), N_("Tiles left to right"), N_("Tiles top to bottom"),
				N_("Tiles both ways") };
			const char *tileNames[IM_ARRAYSIZE(tiles)];
			for(int i = 0; i < IM_ARRAYSIZE(tiles); i++)
				tileNames[i] = _(tiles[i]);
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
			ImGui::Combo("##tiles", &gTiles, tileNames, IM_ARRAYSIZE(tiles));
			ImGui::SetItemTooltip(_("Textures that repeat without a seam, for floors, walls and long surfaces,\n"
				"judged from their edges; a guess, so check how it looks."));
			std::vector<int> items;
			if(gShow == SHOW_RECENT || gShow == SHOW_FAVOURITES){
				// Looked up by "txd:texture", recent ones newest first.
				static std::map<std::string, int> byKey;
				if(byKey.size() != gIndex.size() && indexed){
					byKey.clear();
					for(size_t i = 0; i < gIndex.size(); i++)
						byKey[entryKey(gIndex[i].txdSlot, gIndex[i].name)] = (int)i;
				}
				std::vector<std::string> keys = gShow == SHOW_RECENT ? gRecent :
					std::vector<std::string>(gFavourites.begin(), gFavourites.end());
				for(size_t r = 0; r < keys.size(); r++){
					std::map<std::string, int>::iterator it = byKey.find(keys[r]);
					if(it != byKey.end() && passes(gIndex[it->second], keys[r]))
						items.push_back(it->second);
				}
			}else
				for(size_t i = 0; i < gIndex.size(); i++)
					if(passes(gIndex[i], std::string()))
						items.push_back((int)i);
			if(!indexed)
				ImGui::TextDisabled(_("Indexing textures... %d of %d TXDs"), gNextSlot, GetNumTxdSlots());
			else
				ImGui::TextDisabled(_("Textures: %d"), (int)items.size());
			ImGui::BeginChild("##grid");
			tileGrid(items, false);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if(ImGui::BeginTabItem(L("Texture Studio list"))){
			gTsSearch.Draw("##tssearch", -FLT_MIN);
			std::string from;
			int n;
			const TStudioTexture *tstudioTextures = TStudioTextureList(&n, &from);
			ImGui::SetItemTooltip(_("Texture Studio's texture index (alltextures.pwn): search by number, TXD or name.\n"
				"From %s."), from == "bundled" ? _("the copy that comes with Haumea") : from.c_str());
			std::vector<int> items;
			for(int i = 1; i < n; i++){
				char line[128];
				snprintf(line, sizeof(line), "%d %s %s", i, tstudioTextures[i].txd, tstudioTextures[i].texture);
				if(gTsSearch.PassFilter(line))
					items.push_back(i);
			}
			ImGui::TextDisabled(_("%d of %d"), (int)items.size(), n - 1);
			ImGui::BeginChild("##tsgrid");
			tileGrid(items, true);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}
