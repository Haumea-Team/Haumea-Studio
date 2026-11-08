#include "haumea.h"
#include "objfilters.h"
#include "modelmeta.h"
#include "i18n.h"
#include "icons.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string.h>
#include <string>
#include <vector>

namespace {

// A range in the model's own unit; 0 on a side means no limit there.
struct Range
{
	float lo, hi;
	bool on(void) const { return lo > 0.0f || hi > 0.0f; }
	bool pass(float v) const { return (lo <= 0.0f || v >= lo) && (hi <= 0.0f || v <= hi); }
};

enum { TYPE_OBJECT, TYPE_BUILDING, TYPE_DYNAMIC, TYPE_BREAKABLE, TYPE_TIMED, TYPE_ANIMATED, TYPE_UPGRADE,
	TYPE_INTERIOR, TYPE_EXTERIOR, TYPE_ALPHA, TYPE_EXPLOSIVE, NUM_TYPES };
const char *const kTypeKeys[NUM_TYPES] = { "object", "building", "dynamic", "breakable", "timed", "animated",
	"upgrade", "interior", "exterior", "alpha", "explosive" };
const char *const kTypeNames[NUM_TYPES] = { N_("Object"), N_("Building"), N_("Moves when hit"), N_("Breakable"), N_("Timed (day or night)"),
	N_("Animated"), N_("Vehicle upgrade part"), N_("Interior model"), N_("Exterior model"), N_("See-through (alpha)"),
	N_("Explodes") };
const char *const kSourceKeys[NUM_MODEL_SOURCES] = { "gta", "samp", "custom" };
const char *const kCityKeys[NUM_MODEL_CITIES] = { "los_santos", "san_fierro", "las_venturas", "countryside",
	"interiors" };
enum { MINE_FAVOURITES, MINE_RECENT, MINE_OPEN_MAP, MINE_WORKSPACE, NUM_MINE };
const char *const kMineKeys[NUM_MINE] = { "favourites", "recent", "open_map", "workspace" };
const char *const kMineNames[NUM_MINE] = { N_("Favourites"), N_("Recently placed"), N_("In the open map"), N_("In the workspace's maps") };
const char *const kSortKeys[NUM_OBJECT_SORTS] = { "id", "name", "size", "polygons", "draw_distance", "recent" };
const char *const kSortNames[NUM_OBJECT_SORTS] = { N_("ID"), N_("Name"), N_("Size"), N_("Polygons"), N_("Draw distance"), N_("Recently used") };

struct Filters
{
	Range size, width, depth, height, volume, footprint, drawDistance, polygons, materials;
	unsigned sources = 0;	// 1 << ModelSource; 0 = any
	std::string ide;
	int collision = 0;	// 1 with, 2 without
	bool twoSided = false;
	int lod = 0;	// 1 full detail only, 2 far versions only
	int hasLod = 0;	// 1 has one, 2 has none
	unsigned types = 0;	// any of these
	int hour = -1;	// timed models shown at this hour
	std::string txd, texture;
	unsigned cities = 0;	// placed in any of these
	bool notPlaced = false;
	std::set<int> zones;	// placed in any of these
	unsigned mine = 0;	// any of these
	int sort = SORT_ID;
	bool descending = false;
};

Filters gFilters;

struct Preset { std::string name, text; };
std::vector<Preset> gPresets;

bool
needsDetails(const Filters &f)
{
	return f.polygons.on() || f.materials.on() || !f.texture.empty() || f.sort == SORT_POLYGONS;
}

bool
hasText(const char *haystack, const std::string &needle)
{
	if(needle.empty())
		return true;
	if(haystack == nil)
		return false;
	std::string h(haystack), n(needle);
	for(char &c : h) c = (char)tolower((unsigned char)c);
	for(char &c : n) c = (char)tolower((unsigned char)c);
	return h.find(n) != std::string::npos;
}

bool
isType(const ModelMeta &m, int type)
{
	switch(type){
	case TYPE_OBJECT: return !m.building && !m.vehicle && !m.skin;
	case TYPE_BUILDING: return m.building;
	case TYPE_DYNAMIC: return m.dynamic;
	case TYPE_BREAKABLE: return m.breakable;
	case TYPE_TIMED: return m.timed;
	case TYPE_ANIMATED: return m.animated;
	case TYPE_UPGRADE: return m.upgrade;
	case TYPE_INTERIOR: return m.interior;
	case TYPE_EXTERIOR: return !m.interior;
	case TYPE_ALPHA: return m.alpha;
	case TYPE_EXPLOSIVE: return m.explosive;
	}
	return false;
}

bool
recentIndex(int id, int *index)
{
	const int *recent;
	int n = GetRecentObjects(&recent);
	for(int i = 0; i < n; i++)
		if(recent[i] == id){
			*index = i;
			return true;
		}
	return false;
}

bool
mine(int id, int which)
{
	int index;
	switch(which){
	case MINE_FAVOURITES: return IsFavourite(id);
	case MINE_RECENT: return recentIndex(id, &index);
	case MINE_OPEN_MAP: return ModelInOpenMap(id);
	case MINE_WORKSPACE: return ModelInWorkspace(id);
	}
	return false;
}

bool
timedAt(const ModelMeta &m, int hour)
{
	if(!m.timed)
		return false;
	// On from timeOn up to timeOff, across midnight when it wraps.
	return m.timeOn <= m.timeOff ? hour >= m.timeOn && hour < m.timeOff : hour >= m.timeOn || hour < m.timeOff;
}

bool
passes(const Filters &f, int id)
{
	const ModelMeta *meta = GetModelMeta(id);
	if(meta == nil)
		return false;
	const ModelMeta &m = *meta;
	float longest = std::max(m.width, std::max(m.depth, m.height));
	if(f.size.on() && !f.size.pass(longest)) return false;
	if(f.width.on() && !f.width.pass(m.width)) return false;
	if(f.depth.on() && !f.depth.pass(m.depth)) return false;
	if(f.height.on() && !f.height.pass(m.height)) return false;
	if(f.volume.on() && !f.volume.pass(m.width * m.depth * m.height)) return false;
	if(f.footprint.on() && !f.footprint.pass(m.width * m.depth)) return false;
	if(f.sources && !(f.sources & 1u << m.source)) return false;
	if(!f.ide.empty() && !hasText(m.ideFile, f.ide)) return false;
	if(f.collision == 1 && !m.hasCollision) return false;
	if(f.collision == 2 && m.hasCollision) return false;
	if(f.twoSided && !m.twoSided) return false;
	if(f.lod == 1 && m.isLod) return false;
	if(f.lod == 2 && !m.isLod) return false;
	if(f.hasLod == 1 && !m.hasLod) return false;
	if(f.hasLod == 2 && m.hasLod) return false;
	if(f.drawDistance.on() && !f.drawDistance.pass(m.drawDistance)) return false;
	if(f.types){
		bool any = false;
		for(int t = 0; t < NUM_TYPES && !any; t++)
			any = (f.types & 1u << t) && isType(m, t);
		if(!any) return false;
	}
	if(f.hour >= 0 && !timedAt(m, f.hour)) return false;
	if(!f.txd.empty() && !hasText(m.txdName, f.txd)) return false;
	// Not read yet: left out until it is.
	if(f.polygons.on() && (m.polygons < 0 || !f.polygons.pass((float)m.polygons))) return false;
	if(f.materials.on() && (m.materials < 0 || !f.materials.pass((float)m.materials))) return false;
	if(!f.texture.empty() && (m.polygons < 0 || !ModelHasTextureContaining(id, f.texture.c_str()))) return false;
	if(f.cities || f.notPlaced || !f.zones.empty()){
		bool any = (f.cities & m.cities) != 0 || (f.notPlaced && m.placements == 0);
		for(int z : m.zones)
			if(f.zones.count(z)) any = true;
		if(!any) return false;
	}
	if(f.mine){
		bool any = false;
		for(int w = 0; w < NUM_MINE && !any; w++)
			any = (f.mine & 1u << w) && mine(id, w);
		if(!any) return false;
	}
	return true;
}

// ---- Text form: key=value;key=value ----

std::string
rangeText(const Range &r)
{
	char text[64];
	snprintf(text, sizeof(text), "%s-%s", r.lo > 0.0f ? std::to_string(r.lo).c_str() : "",
		r.hi > 0.0f ? std::to_string(r.hi).c_str() : "");
	// Trim the trailing zeros std::to_string leaves.
	std::string s = text, out;
	for(size_t i = 0; i < s.size(); i++){
		out += s[i];
		if(s[i] == '.'){
			size_t end = s.find('-', i);
			if(end == std::string::npos) end = s.size();
			std::string frac = s.substr(i + 1, end - i - 1);
			while(!frac.empty() && frac.back() == '0') frac.pop_back();
			if(frac.empty()) out.pop_back();
			else out += frac;
			i = end - 1;
		}
	}
	return out;
}

bool
parseRange(const std::string &text, Range *r)
{
	size_t dash = text.find('-');
	if(dash == std::string::npos)
		return false;
	std::string lo = text.substr(0, dash), hi = text.substr(dash + 1);
	char *end;
	r->lo = lo.empty() ? 0.0f : strtof(lo.c_str(), &end);
	if(!lo.empty() && *end) return false;
	r->hi = hi.empty() ? 0.0f : strtof(hi.c_str(), &end);
	if(!hi.empty() && *end) return false;
	return r->lo >= 0.0f && r->hi >= 0.0f;
}

struct RangeKey { const char *key; Range Filters::*field; };
const RangeKey kRanges[] = {
	{ "size", &Filters::size }, { "width", &Filters::width }, { "depth", &Filters::depth },
	{ "height", &Filters::height }, { "volume", &Filters::volume }, { "footprint", &Filters::footprint },
	{ "draw_distance", &Filters::drawDistance }, { "polygons", &Filters::polygons },
	{ "materials", &Filters::materials },
};

std::string
listText(unsigned bits, const char *const *keys, int n)
{
	std::string out;
	for(int i = 0; i < n; i++)
		if(bits & 1u << i)
			out += (out.empty() ? "" : ",") + std::string(keys[i]);
	return out;
}

bool
parseList(const std::string &text, const char *const *keys, int n, unsigned *bits)
{
	*bits = 0;
	size_t at = 0;
	while(at <= text.size()){
		size_t comma = text.find(',', at);
		std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
		if(!item.empty()){
			int found = -1;
			for(int i = 0; i < n; i++)
				if(item == keys[i]) found = i;
			if(found < 0) return false;
			*bits |= 1u << found;
		}
		if(comma == std::string::npos) break;
		at = comma + 1;
	}
	return true;
}

std::string
filtersText(const Filters &f)
{
	std::string out;
	auto add = [&](const char *key, const std::string &value){
		out += (out.empty() ? "" : ";") + std::string(key) + "=" + value;
	};
	for(const RangeKey &r : kRanges)
		if((f.*(r.field)).on()) add(r.key, rangeText(f.*(r.field)));
	if(f.sources) add("source", listText(f.sources, kSourceKeys, NUM_MODEL_SOURCES));
	if(!f.ide.empty()) add("ide", f.ide);
	if(f.collision) add("collision", f.collision == 1 ? "with" : "without");
	if(f.twoSided) add("two_sided", "1");
	if(f.lod) add("lod", f.lod == 1 ? "hd" : "lod");
	if(f.hasLod) add("has_lod", f.hasLod == 1 ? "1" : "0");
	if(f.types) add("type", listText(f.types, kTypeKeys, NUM_TYPES));
	if(f.hour >= 0) add("hour", std::to_string(f.hour));
	if(!f.txd.empty()) add("txd", f.txd);
	if(!f.texture.empty()) add("texture", f.texture);
	if(f.cities || f.notPlaced){
		std::string cities = listText(f.cities, kCityKeys, NUM_MODEL_CITIES);
		if(f.notPlaced) cities += (cities.empty() ? "" : ",") + std::string("not_placed");
		add("city", cities);
	}
	if(!f.zones.empty()){
		std::string zones;
		for(int z : f.zones) zones += (zones.empty() ? "" : ",") + std::string(GetMetaZoneName(z));
		add("zone", zones);
	}
	if(f.mine) add("mine", listText(f.mine, kMineKeys, NUM_MINE));
	if(f.sort != SORT_ID) add("sort", kSortKeys[f.sort]);
	if(f.descending) add("descending", "1");
	return out;
}

bool
parseFilters(const char *text, Filters *f, std::string *error)
{
	*f = Filters();
	std::string all = text ? text : "";
	size_t at = 0;
	while(at < all.size()){
		size_t semi = all.find(';', at);
		std::string item = all.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
		at = semi == std::string::npos ? all.size() : semi + 1;
		if(item.empty())
			continue;
		size_t eq = item.find('=');
		if(eq == std::string::npos){
			*error = "expected key=value: " + item;
			return false;
		}
		std::string key = item.substr(0, eq), value = item.substr(eq + 1);
		bool ok = true, known = false;
		for(const RangeKey &r : kRanges)
			if(key == r.key){
				known = true;
				ok = parseRange(value, &(f->*(r.field)));
			}
		if(known){
		}else if(key == "source"){
			ok = parseList(value, kSourceKeys, NUM_MODEL_SOURCES, &f->sources);
		}else if(key == "ide") f->ide = value;
		else if(key == "collision"){
			f->collision = value == "with" ? 1 : value == "without" ? 2 : 0;
			ok = f->collision != 0;
		}else if(key == "two_sided") f->twoSided = value == "1" || value == "true";
		else if(key == "lod"){
			f->lod = value == "hd" ? 1 : value == "lod" ? 2 : 0;
			ok = f->lod != 0;
		}else if(key == "has_lod") f->hasLod = value == "1" || value == "true" ? 1 : 2;
		else if(key == "type") ok = parseList(value, kTypeKeys, NUM_TYPES, &f->types);
		else if(key == "hour"){
			f->hour = atoi(value.c_str());
			ok = f->hour >= 0 && f->hour < 24;
		}else if(key == "txd") f->txd = value;
		else if(key == "texture") f->texture = value;
		else if(key == "city"){
			std::string rest = value;
			size_t np = rest.find("not_placed");
			if(np != std::string::npos){
				f->notPlaced = true;
				rest.erase(np, strlen("not_placed"));
			}
			ok = parseList(rest, kCityKeys, NUM_MODEL_CITIES, &f->cities);
		}else if(key == "zone"){
			size_t z = 0;
			while(z <= value.size()){
				size_t comma = value.find(',', z);
				std::string name = value.substr(z, comma == std::string::npos ? std::string::npos : comma - z);
				if(!name.empty()){
					int found = -1;
					for(int i = 0; i < NumMetaZones(); i++)
						if(name == GetMetaZoneName(i)) found = i;
					if(found < 0){
						*error = "no zone named " + name;
						return false;
					}
					f->zones.insert(found);
				}
				if(comma == std::string::npos) break;
				z = comma + 1;
			}
		}else if(key == "mine") ok = parseList(value, kMineKeys, NUM_MINE, &f->mine);
		else if(key == "sort"){
			f->sort = -1;
			for(int s = 0; s < NUM_OBJECT_SORTS; s++)
				if(value == kSortKeys[s]) f->sort = s;
			ok = f->sort >= 0;
			if(!ok) f->sort = SORT_ID;
		}else if(key == "descending") f->descending = value == "1" || value == "true";
		else{
			*error = "unknown filter " + key;
			return false;
		}
		if(!ok){
			*error = "bad value for " + key + ": " + value;
			return false;
		}
	}
	return true;
}

// ---- The panel ----

// The removable chips for what is on, each with what clears it.
struct Chip { std::string label; std::function<void(Filters&)> clear; };

std::string
rangeLabel(const char *what, const Range &r, const char *unit)
{
	char text[96];
	if(r.lo > 0.0f && r.hi > 0.0f) snprintf(text, sizeof(text), "%s %g-%g%s", what, r.lo, r.hi, unit);
	else if(r.lo > 0.0f) snprintf(text, sizeof(text), "%s %g%s or more", what, r.lo, unit);
	else snprintf(text, sizeof(text), "%s up to %g%s", what, r.hi, unit);
	return text;
}

std::vector<Chip>
activeChips(const Filters &f)
{
	std::vector<Chip> chips;
	struct RangeChip { const char *name; Range Filters::*field; const char *unit; };
	const RangeChip ranges[] = {
		{ _("Size"), &Filters::size, " m" }, { _("Width"), &Filters::width, " m" }, { _("Depth"), &Filters::depth, " m" },
		{ _("Height"), &Filters::height, " m" }, { _("Volume"), &Filters::volume, " m\xC2\xB3" },
		{ _("Footprint"), &Filters::footprint, " m\xC2\xB2" }, { _("Draw distance"), &Filters::drawDistance, " m" },
		{ _("Polygons"), &Filters::polygons, "" }, { _("Materials"), &Filters::materials, "" },
	};
	for(const RangeChip &r : ranges)
		if((f.*(r.field)).on()){
			Range Filters::*field = r.field;
			chips.push_back({ rangeLabel(r.name, f.*field, r.unit), [field](Filters &x){ x.*field = Range(); } });
		}
	for(int s = 0; s < NUM_MODEL_SOURCES; s++)
		if(f.sources & 1u << s)
			chips.push_back({ GetModelSourceName(s), [s](Filters &x){ x.sources &= ~(1u << s); } });
	if(!f.ide.empty()) chips.push_back({ std::string(_("IDE")) + " " + f.ide, [](Filters &x){ x.ide.clear(); } });
	if(f.collision) chips.push_back({ f.collision == 1 ? _("Has collision") : _("No collision"),
		[](Filters &x){ x.collision = 0; } });
	if(f.twoSided) chips.push_back({ _("Two-sided"), [](Filters &x){ x.twoSided = false; } });
	if(f.lod) chips.push_back({ f.lod == 1 ? _("Full detail only") : _("Far versions only"), [](Filters &x){ x.lod = 0; } });
	if(f.hasLod) chips.push_back({ f.hasLod == 1 ? _("Has a far version") : _("No far version"),
		[](Filters &x){ x.hasLod = 0; } });
	for(int t = 0; t < NUM_TYPES; t++)
		if(f.types & 1u << t)
			chips.push_back({ _(kTypeNames[t]), [t](Filters &x){ x.types &= ~(1u << t); } });
	if(f.hour >= 0){
		char text[48];
		snprintf(text, sizeof(text), "%s %02d:00", _("Shown at"), f.hour);
		chips.push_back({ text, [](Filters &x){ x.hour = -1; } });
	}
	if(!f.txd.empty()) chips.push_back({ std::string(_("TXD")) + " " + f.txd, [](Filters &x){ x.txd.clear(); } });
	if(!f.texture.empty()) chips.push_back({ std::string(_("Texture")) + " " + f.texture,
		[](Filters &x){ x.texture.clear(); } });
	for(int c = 0; c < NUM_MODEL_CITIES; c++)
		if(f.cities & 1u << c)
			chips.push_back({ _(GetModelCityName(c)), [c](Filters &x){ x.cities &= ~(1u << c); } });
	if(f.notPlaced) chips.push_back({ _("Not placed"), [](Filters &x){ x.notPlaced = false; } });
	for(int z : f.zones)
		chips.push_back({ GetMetaZoneName(z), [z](Filters &x){ x.zones.erase(z); } });
	for(int w = 0; w < NUM_MINE; w++)
		if(f.mine & 1u << w)
			chips.push_back({ _(kMineNames[w]), [w](Filters &x){ x.mine &= ~(1u << w); } });
	return chips;
}

// Wraps the next small button onto a new line when it would pass right, the
// edge of the line (the cursor's x plus the width available, taken before
// the first button).
void
flowSameLine(const char *label, bool first, float right)
{
	if(first)
		return;
	float width = ImGui::CalcTextSize(label, nil, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	if(ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
		ImGui::SameLine();
}

float
lineRight(void)
{
	return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
}

bool
rangeInputs(const char *label, Range &r, float step)
{
	ImGui::PushID(label);
	bool changed = false;
	float w = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label, nil, true).x) * 0.5f - ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetNextItemWidth(std::max(w, 40.0f));
	changed |= ImGui::InputFloat("##min", &r.lo, 0.0f, 0.0f, r.lo > 0.0f ? "%g" : _("any"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(std::max(w, 40.0f));
	changed |= ImGui::InputFloat("##max", &r.hi, 0.0f, 0.0f, r.hi > 0.0f ? "%g" : _("any"));
	ImGui::SameLine();
	ImGui::TextUnformatted(label, strstr(label, "##"));	// an L() label: without its ID
	(void)step;
	r.lo = std::max(r.lo, 0.0f);
	r.hi = std::max(r.hi, 0.0f);
	ImGui::PopID();
	return changed;
}

void
textInput(const char *label, std::string &value)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", value.c_str());
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
	if(ImGui::InputText(label, buf, sizeof(buf)))
		value = buf;
}

void
bitCheckbox(const char *label, unsigned &bits, int bit)
{
	bool on = (bits & 1u << bit) != 0;
	if(ImGui::Checkbox(label, &on))
		bits = on ? bits | 1u << bit : bits & ~(1u << bit);
}

// The quick chips: one click each, a second click turns it off again.
struct Quick { const char *label; std::function<bool(const Filters&)> on; std::function<void(Filters&, bool)> set; };

std::vector<Quick>
quickChips(void)
{
	auto sizeQuick = [](const char *label, float lo, float hi){
		return Quick{ label, [lo, hi](const Filters &f){ return f.size.lo == lo && f.size.hi == hi; },
			[lo, hi](Filters &f, bool on){ f.size = on ? Range{ lo, hi } : Range(); } };
	};
	return {
		sizeQuick(L("Small (under 2 m)"), 0.0f, 2.0f),
		sizeQuick(L("Medium (2-10 m)"), 2.0f, 10.0f),
		sizeQuick(L("Large (10-50 m)"), 10.0f, 50.0f),
		sizeQuick(L("Huge (over 50 m)"), 50.0f, 0.0f),
		{ L("Has collision"), [](const Filters &f){ return f.collision == 1; },
		  [](Filters &f, bool on){ f.collision = on ? 1 : 0; } },
		{ L("No collision"), [](const Filters &f){ return f.collision == 2; },
		  [](Filters &f, bool on){ f.collision = on ? 2 : 0; } },
		{ L("Full detail only"), [](const Filters &f){ return f.lod == 1; },
		  [](Filters &f, bool on){ f.lod = on ? 1 : 0; } },
		{ L("SA-MP"), [](const Filters &f){ return f.sources == 1u << MODEL_SOURCE_SAMP; },
		  [](Filters &f, bool on){ f.sources = on ? 1u << MODEL_SOURCE_SAMP : 0; } },
		{ L("Not placed"), [](const Filters &f){ return f.notPlaced; },
		  [](Filters &f, bool on){ f.notPlaced = on; } },
		{ L("Breakable"), [](const Filters &f){ return (f.types & 1u << TYPE_BREAKABLE) != 0; },
		  [](Filters &f, bool on){ f.types = on ? f.types | 1u << TYPE_BREAKABLE : f.types & ~(1u << TYPE_BREAKABLE); } },
		{ L("Night-time"), [](const Filters &f){ return f.hour == 0; },
		  [](Filters &f, bool on){ f.hour = on ? 0 : -1; } },
		{ L("Favourites"), [](const Filters &f){ return (f.mine & 1u << MINE_FAVOURITES) != 0; },
		  [](Filters &f, bool on){ f.mine = on ? f.mine | 1u << MINE_FAVOURITES : f.mine & ~(1u << MINE_FAVOURITES); } },
		{ L("In the open map"), [](const Filters &f){ return (f.mine & 1u << MINE_OPEN_MAP) != 0; },
		  [](Filters &f, bool on){ f.mine = on ? f.mine | 1u << MINE_OPEN_MAP : f.mine & ~(1u << MINE_OPEN_MAP); } },
	};
}

// How many models the world places in each city, and in each zone.
void
placeCounts(std::vector<int> &cities, std::vector<int> &zones)
{
	static int builtFor = -1;
	static std::vector<int> cityCount, zoneCount;
	if(builtFor != NumMetaZones()){
		builtFor = NumMetaZones();
		cityCount.assign(NUM_MODEL_CITIES + 1, 0);	// the last: not placed
		zoneCount.assign(NumMetaZones(), 0);
		for(int id = 0; id < NUMOBJECTDEFS; id++){
			const ModelMeta *m = GetModelMeta(id);
			if(m == nil || m->vehicle || m->skin)
				continue;
			for(int c = 0; c < NUM_MODEL_CITIES; c++)
				if(m->cities & 1u << c) cityCount[c]++;
			if(m->placements == 0) cityCount[NUM_MODEL_CITIES]++;
			for(int z : m->zones) zoneCount[z]++;
		}
	}
	cities = cityCount;
	zones = zoneCount;
}

void
uiWhereUsed(Filters &f)
{
	std::vector<int> cities, zones;
	placeCounts(cities, zones);
	for(int c = 0; c < NUM_MODEL_CITIES; c++){
		ImGui::PushID(c);
		char label[96];
		snprintf(label, sizeof(label), "%s (%d)", _(GetModelCityName(c)), cities[c]);
		bitCheckbox(label, f.cities, c);
		// The city's zones, by name, as a second level.
		std::vector<int> mine;
		for(int z = 0; z < NumMetaZones(); z++)
			if(GetMetaZoneCity(z) == c) mine.push_back(z);
		if(!mine.empty()){
			ImGui::SameLine();
			if(ImGui::TreeNode("##zones", "%s", _("Zones"))){
				std::sort(mine.begin(), mine.end(), [](int a, int b){ return strcmp(GetMetaZoneName(a), GetMetaZoneName(b)) < 0; });
				for(int z : mine){
					char zl[96];
					snprintf(zl, sizeof(zl), "%s (%d)##z%d", GetMetaZoneName(z), zones[z], z);
					bool on = f.zones.count(z) != 0;
					if(ImGui::Checkbox(zl, &on)){
						if(on) f.zones.insert(z);
						else f.zones.erase(z);
					}
				}
				ImGui::TreePop();
			}
		}
		ImGui::PopID();
	}
	char label[64];
	snprintf(label, sizeof(label), "%s (%d)", _("Not placed"), cities[NUM_MODEL_CITIES]);
	ImGui::Checkbox(label, &f.notPlaced);
}

void
uiPresets(void)
{
	static char name[48];
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
	if(ImGui::BeginCombo("##presets", _("Presets"))){
		for(size_t i = 0; i < gPresets.size(); i++){
			ImGui::PushID((int)i);
			if(ImGui::Selectable(gPresets[i].name.c_str())){
				std::string error;
				Filters loaded;
				if(parseFilters(gPresets[i].text.c_str(), &loaded, &error))
					gFilters = loaded;
				else
					Toast(TOAST_SPAWN, "%s: %s", gPresets[i].name.c_str(), error.c_str());
			}
			ImGui::PopID();
		}
		if(gPresets.empty())
			ImGui::TextDisabled("%s", _("No presets yet: name the filters on the right and save them."));
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
	ImGui::InputTextWithHint("##preset_name", _("Preset name"), name, sizeof(name));
	ImGui::SameLine();
	ImGui::BeginDisabled(name[0] == '\0' || strchr(name, '|') != nil);
	if(ImGui::SmallButton(ICON_FA_FLOPPY_DISK "##save_preset")){
		std::string text = filtersText(gFilters);
		bool replaced = false;
		for(Preset &p : gPresets)
			if(p.name == name){
				p.text = text;
				replaced = true;
			}
		if(!replaced)
			gPresets.push_back({ name, text });
		name[0] = '\0';
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("%s", _("Save the filters under this name (an existing preset is replaced)."));
	ImGui::SameLine();
	ImGui::BeginDisabled(name[0] == '\0');
	if(ImGui::SmallButton(ICON_FA_TRASH "##delete_preset"))
		gPresets.erase(std::remove_if(gPresets.begin(), gPresets.end(),
			[](const Preset &p){ return p.name == name; }), gPresets.end());
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("%s", _("Delete the preset with this name."));
}

}	// namespace

bool
ObjectFiltersActive(void)
{
	return !activeChips(gFilters).empty();
}

bool
ObjectPassesFilters(int id)
{
	return passes(gFilters, id);
}

void
SortObjectResults(int *ids, int n)
{
	const Filters &f = gFilters;
	if(f.sort == SORT_ID && !f.descending)
		return;	// already in ID order
	if(needsDetails(f))
		RequestModelMetaDetails();
	auto key = [&](int id) -> float {
		const ModelMeta *m = GetModelMeta(id);
		if(m == nil) return 0.0f;
		int index;
		switch(f.sort){
		case SORT_SIZE: return m->width * m->depth * m->height;
		case SORT_POLYGONS: return (float)m->polygons;
		case SORT_DRAW_DISTANCE: return m->drawDistance;
		case SORT_RECENT: return recentIndex(id, &index) ? (float)index : 1e9f;
		}
		return (float)id;
	};
	std::stable_sort(ids, ids + n, [&](int a, int b){
		bool less;
		if(f.sort == SORT_NAME){
			ObjectDef *oa = GetObjectDef(a), *ob = GetObjectDef(b);
			less = rw::strncmp_ci(oa ? oa->m_name : "", ob ? ob->m_name : "", MODELNAMELEN) < 0;
			if(f.descending) less = rw::strncmp_ci(ob ? ob->m_name : "", oa ? oa->m_name : "", MODELNAMELEN) < 0;
			return less;
		}
		float ka = key(a), kb = key(b);
		return f.descending ? ka > kb : ka < kb;
	});
}

void
uiObjectFiltersPanel(void)
{
	Filters &f = gFilters;
	if(needsDetails(f))
		RequestModelMetaDetails();
	std::vector<Chip> chips = activeChips(f);
	char header[96];
	if(chips.empty())
		snprintf(header, sizeof(header), ICON_FA_FILTER " %s###object_filters", _("Filters"));
	else
		snprintf(header, sizeof(header), ICON_FA_FILTER " %s (%d)###object_filters", _("Filters"), (int)chips.size());
	bool open = ImGui::CollapsingHeader(header);

	// What is on, always in view: click a chip to remove it.
	if(!chips.empty()){
		bool first = true;
		float right = lineRight();
		for(size_t i = 0; i < chips.size(); i++){
			std::string label = chips[i].label + " " ICON_FA_XMARK "##chip" + std::to_string(i);
			flowSameLine(label.c_str(), first, right);
			first = false;
			if(ImGui::SmallButton(label.c_str()))
				chips[i].clear(f);
		}
		flowSameLine(L("Clear all"), false, right);
		if(ImGui::SmallButton(L("Clear all")))
			ClearObjectFilters();
	}
	if(needsDetails(f) && ModelMetaDetailsProgress() < 1.0f)
		ImGui::TextDisabled("%s %d%%", _("Reading polygons and textures..."), (int)(ModelMetaDetailsProgress() * 100.0f));
	if(!open)
		return;

	ImGui::PushID("object_filters");
	// One click each.
	bool first = true;
	float right = lineRight();
	for(const Quick &q : quickChips()){
		bool on = q.on(f);
		flowSameLine(q.label, first, right);
		first = false;
		if(on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if(ImGui::SmallButton(q.label))
			q.set(f, !on);
		if(on) ImGui::PopStyleColor();
	}

	if(ImGui::TreeNode(L("Size"))){
		rangeInputs(L("Longest side (m)"), f.size, 1.0f);
		rangeInputs(L("Width X (m)"), f.width, 1.0f);
		rangeInputs(L("Depth Y (m)"), f.depth, 1.0f);
		rangeInputs(L("Height Z (m)"), f.height, 1.0f);
		rangeInputs(L("Volume (m\xC2\xB3)"), f.volume, 10.0f);
		rangeInputs(L("Footprint (m\xC2\xB2)"), f.footprint, 10.0f);
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Source"))){
		for(int s = 0; s < NUM_MODEL_SOURCES; s++)
			bitCheckbox(GetModelSourceName(s), f.sources, s);
		textInput(L("IDE file contains"), f.ide);
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Collision and detail"))){
		ImGui::RadioButton(L("Any collision"), &f.collision, 0);
		ImGui::SameLine();
		ImGui::RadioButton(L("Has collision"), &f.collision, 1);
		ImGui::SameLine();
		ImGui::RadioButton(L("None"), &f.collision, 2);
		ImGui::Checkbox(L("Two-sided (no backface culling)"), &f.twoSided);
		ImGui::RadioButton(L("Any detail"), &f.lod, 0);
		ImGui::SameLine();
		ImGui::RadioButton(L("Full detail"), &f.lod, 1);
		ImGui::SameLine();
		ImGui::RadioButton(L("Far versions (LOD)"), &f.lod, 2);
		ImGui::RadioButton(L("With or without a far version"), &f.hasLod, 0);
		ImGui::RadioButton(L("Has a far version"), &f.hasLod, 1);
		ImGui::SameLine();
		ImGui::RadioButton(L("Has none"), &f.hasLod, 2);
		rangeInputs(L("Draw distance (m)"), f.drawDistance, 10.0f);
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Type"))){
		for(int t = 0; t < NUM_TYPES; t++)
			bitCheckbox(L(kTypeNames[t]), f.types, t);
		bool atHour = f.hour >= 0;
		if(ImGui::Checkbox(L("Shown at"), &atHour))
			f.hour = atHour ? 0 : -1;
		if(atHour){
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
			ImGui::SliderInt("##hour", &f.hour, 0, 23, "%02d:00");
			ImGui::SetItemTooltip("%s", _("Timed models (day or night) that the game draws at this hour."));
		}
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Appearance"))){
		textInput(L("TXD contains"), f.txd);
		textInput(L("A texture contains"), f.texture);
		rangeInputs(L("Polygons"), f.polygons, 100.0f);
		rangeInputs(L("Materials"), f.materials, 1.0f);
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Where it's used"))){
		uiWhereUsed(f);
		ImGui::TreePop();
	}
	if(ImGui::TreeNode(L("Mine"))){
		for(int w = 0; w < NUM_MINE; w++)
			bitCheckbox(L(kMineNames[w]), f.mine, w);
		ImGui::TreePop();
	}

	// Sort, and presets.
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
	if(ImGui::BeginCombo(L("Sort by"), _(kSortNames[f.sort]))){
		for(int s = 0; s < NUM_OBJECT_SORTS; s++)
			if(ImGui::Selectable(L(kSortNames[s]), s == f.sort))
				f.sort = s;
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::Checkbox(L("Descending"), &f.descending);
	uiPresets();
	ImGui::PopID();
}

int NumObjectFilterPresets(void) { return (int)gPresets.size(); }

std::string
GetObjectFilterPresetLine(int i)
{
	return i >= 0 && i < (int)gPresets.size() ? gPresets[i].name + "|" + gPresets[i].text : "";
}

void
AddObjectFilterPresetLine(const char *line)
{
	const char *bar = strchr(line, '|');
	if(bar == nil || bar == line)
		return;
	std::string name(line, bar - line);
	for(Preset &p : gPresets)
		if(p.name == name){
			p.text = bar + 1;
			return;
		}
	gPresets.push_back({ name, bar + 1 });
}

std::string
GetObjectFiltersText(void)
{
	return filtersText(gFilters);
}

bool
SetObjectFiltersText(const char *text, std::string *error)
{
	Filters parsed;
	if(!parseFilters(text, &parsed, error))
		return false;
	gFilters = parsed;
	return true;
}

void
ClearObjectFilters(void)
{
	int sort = gFilters.sort;
	bool descending = gFilters.descending;
	gFilters = Filters();
	gFilters.sort = sort;	// the order is not a filter
	gFilters.descending = descending;
}

int
FilterModels(const char *text, const char *name, std::vector<int> &out, std::string *error)
{
	Filters f;
	if(!parseFilters(text, &f, error))
		return -1;
	if(needsDetails(f))
		ReadAllModelMetaDetails();	// the agent waits for the answer anyway
	out.clear();
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		ObjectDef *obj = GetObjectDef(id);
		if(obj == nil || (name && name[0] && !hasText(obj->m_name, name)))
			continue;
		if(passes(f, id))
			out.push_back(id);
	}
	Filters saved = gFilters;
	gFilters = f;	// the sort reads the chosen order from here
	SortObjectResults(out.data(), (int)out.size());
	gFilters = saved;
	return (int)out.size();
}
