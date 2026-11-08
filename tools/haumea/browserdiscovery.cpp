// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "browserdiscovery.h"
#include "modelquery.h"
#include "modelmeta.h"
#include "i18n.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {
enum Mode { CLOSED, INSTANCES, SIMILAR };
Mode mode;
char query[512];
std::string parsedQuery;
bool validQuery, highlight = true;
int scope, reference = -1, matchKind, chosenModel = -1, currentInstance = -1;
float tolerance = 20;
struct Group { int model; std::vector<ObjectInst*> members; };
std::vector<Group> groups;
std::unordered_map<int, size_t> groupIndex;
std::unordered_set<int> wanted;

bool inScope(const ObjectInst *inst)
{
	if(scope == 0) return true;
	bool mine = PawnObjectNeedsExport(const_cast<ObjectInst*>(inst));
	return scope == 0 || (scope == 1 ? mine : !mine);
}

void parseQuery()
{
	if(parsedQuery == query) return;
	parsedQuery = query;
	groups.clear(); groupIndex.clear(); wanted.clear();
	std::vector<std::pair<int, int>> ranges;
	validQuery = ParseModelIdQuery(query, &ranges);
	if(!validQuery) return;
	// Scan defined models instead of expanding ranges: even a huge numeric
	// range remains bounded by the editor's model catalog.
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		if(!GetObjectDef(id)) continue;
		int pawn = PawnModelId(id);
		for(auto range : ranges)
			if(pawn >= range.first && pawn <= range.second){ wanted.insert(id); break; }
	}
	for(auto range : ranges)
		if(range.first == range.second && range.first >= -30000 && range.first < NUMOBJECTDEFS)
			wanted.insert(EditorModelId(range.first));
	std::vector<int> ids(wanted.begin(), wanted.end());
	std::sort(ids.begin(), ids.end(), [](int a, int b){ return PawnModelId(a) < PawnModelId(b); });
	for(int id : ids){ groupIndex[id] = groups.size(); groups.push_back({id, {}}); }
	chosenModel = ids.empty() ? -1 : ids.front();
	currentInstance = -1;
}

void collect()
{
	parseQuery();
	for(Group &group : groups) group.members.clear();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || !inScope(inst)) continue;
		auto group = groupIndex.find(inst->m_objectId);
		if(group != groupIndex.end()) groups[group->second].members.push_back(inst);
	}
}

void jump(ObjectInst *inst)
{
	if(!inst || inst->m_isDeleted) return;
	currentInstance = inst->m_id;
	currentArea = inst->m_area;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj && obj->m_colModel) inst->JumpTo();
	else { TheCamera.setTarget(inst->m_translation); TheCamera.setDistanceToTarget(15.0f); }
}

void instancesPanel()
{
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##instance_ids", _("Model IDs: 1337, 19353-19355"), query, sizeof(query));
	ImGui::SetItemTooltip(_("Search several IDs or ranges. Custom models use their negative Pawn IDs."));
	const char *scopes[] = { _("Both"), _("My map"), _("GTA world") };
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::Combo(_("Scope"), &scope, scopes, 3);
	ImGui::Checkbox(_("Highlight matches"), &highlight);
	ImGui::SetItemTooltip(_("Yellow marks the search results. Your editing selection stays independent."));
	collect();
	if(!validQuery){ ImGui::TextWrapped("%s", _("Enter model IDs separated by commas, spaces or ranges.")); return; }
	int total = 0;
	for(const Group &group : groups) total += (int)group.members.size();
	ImGui::TextDisabled(_("%d models | %d instances"), (int)groups.size(), total);
	float height = std::min(5.0f, std::max(1.0f, (float)groups.size())) * ImGui::GetTextLineHeightWithSpacing() + 8.0f;
	ImGui::BeginChild("##instance_model_counts", ImVec2(0, height), ImGuiChildFlags_Borders);
	ImGuiListClipper clipper;
	clipper.Begin((int)groups.size());
	while(clipper.Step()) for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
		const Group &group = groups[i];
		ObjectDef *obj = GetObjectDef(group.model);
		char label[256];
		snprintf(label, sizeof(label), _("%d  %s  |  %d instances"), PawnModelId(group.model),
			obj ? obj->m_name : _("Unknown model"), (int)group.members.size());
		ImGui::PushID(group.model);
		if(ImGui::Selectable(label, chosenModel == group.model)){ chosenModel = group.model; currentInstance = -1; }
		ImGui::PopID();
	}
	ImGui::EndChild();
	auto index = groupIndex.find(chosenModel);
	if(index == groupIndex.end()) return;
	const auto &members = groups[index->second].members;
	ImGui::BeginDisabled(members.empty());
	auto next = [&](int step){
		if(members.empty()) return;
		int at = -1;
		for(size_t i = 0; i < members.size(); i++) if(members[i]->m_id == currentInstance) at = (int)i;
		int n = (int)members.size();
		jump(members[(at < 0 ? (step > 0 ? 0 : n - 1) : (at + step + n) % n)]);
	};
	if(ImGui::Button(_("Previous"))) next(-1);
	ImGui::SameLine();
	if(ImGui::Button(_("Next"))) next(1);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(_("Jump the camera between this model's instances, including other interiors."));
	ImGui::BeginDisabled(members.empty());
	if(ImGui::Button(_("Select all"))){
		ClearSelection();
		for(ObjectInst *inst : members) inst->Select();
		OpenObjectInspector();
	}
	ImGui::SetItemTooltip(_("Select all instances of this model in the current scope."));
	ImGui::EndDisabled();
	ImGui::BeginChild("##instance_locations", UiFillRest(UiListMinHeight()), ImGuiChildFlags_Borders);
	clipper.Begin((int)members.size());
	while(clipper.Step()) for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
		ObjectInst *inst = members[i];
		ImGui::PushID(inst->m_id);
		if(ImGui::SmallButton(_("Jump to"))) jump(inst);
		ImGui::SameLine();
		char label[256];
		snprintf(label, sizeof(label), "%s | %.2f, %.2f, %.2f | %d", PawnObjectNeedsExport(inst) ? _("My map") : _("GTA world"),
			inst->m_translation.x, inst->m_translation.y, inst->m_translation.z, inst->m_area);
		if(ImGui::Selectable(label, currentInstance == inst->m_id, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) jump(inst);
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void similarPanel(void (*drawModels)(int *, int, int), int selectedModel)
{
	int pawn = PawnModelId(reference);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	if(ImGui::InputInt(_("Reference ID"), &pawn)) reference = EditorModelId(pawn);
	const char *kinds[] = { _("Dimensions"), _("Proportions") };
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::Combo(_("Match"), &matchKind, kinds, 2);
	ImGui::SliderFloat(_("Tolerance"), &tolerance, 5.0f, 100.0f, "%.0f%%");
	ImGui::SetItemTooltip(_("Dimensions compare metres. Proportions compare shape ratios regardless of overall size."));
	const ModelMeta *base = GetModelMeta(reference);
	if(!base || !base->hasCollision || std::max(base->width, std::max(base->depth, base->height)) <= 0){
		ImGui::TextWrapped("%s", _("This model has no collision dimensions available to compare.")); return;
	}
	ImGui::TextDisabled(_("Reference: %.2f x %.2f x %.2f units"), base->width, base->depth, base->height);
	ImGui::SetItemTooltip(_("Sizes come from collision bounds. Use the previews to compare the actual model shapes."));
	struct Match { int id; float score; };
	std::vector<Match> matches;
	std::array<float, 3> size = {base->width, base->depth, base->height};
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		if(id == reference) continue;
		const ModelMeta *meta = GetModelMeta(id);
		if(!meta || !meta->hasCollision || meta->vehicle != base->vehicle || meta->skin != base->skin || meta->isLod != base->isLod) continue;
		float score = ModelDimensionDifference(size, {meta->width, meta->depth, meta->height}, matchKind == 1);
		if(score <= tolerance * 0.01f) matches.push_back({id, score});
	}
	std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b){ return a.score == b.score ? a.id < b.id : a.score < b.score; });
	std::vector<int> ids;
	for(const Match &match : matches) ids.push_back(match.id);
	ImGui::TextDisabled(_("%d similar models"), (int)ids.size());
	if(mode == SIMILAR) drawModels(ids.data(), (int)ids.size(), selectedModel);
}
}

bool BrowserDiscoveryActive(void) { return mode != CLOSED; }
bool BrowserDiscoveryShowsModels(void) { return mode == SIMILAR; }
void BrowserFindInstances(int model)
{
	snprintf(query, sizeof(query), "%d", PawnModelId(model));
	mode = INSTANCES; highlight = true;
}
void BrowserFindInstanceQuery(const char *ids)
{
	snprintf(query, sizeof(query), "%s", ids ? ids : "");
	mode = INSTANCES; highlight = true;
}
void BrowserFindSimilar(int model) { reference = model; mode = SIMILAR; }
void BrowserDiscoveryDraw(void (*drawModels)(int *, int, int), int selectedModel)
{
	if(ImGui::Button(_("Back to models"))){ mode = CLOSED; return; }
	ImGui::SameLine();
	ImGui::TextUnformatted(mode == INSTANCES ? _("Instances") : _("Similar models"));
	ImGui::Separator();
	if(mode == INSTANCES) instancesPanel();
	else similarPanel(drawModels, selectedModel);
}
bool BrowserDiscoveryIsMatch(const ObjectInst *inst)
{
	return mode == INSTANCES && highlight && inst && !inst->m_isDeleted && wanted.count(inst->m_objectId) && inScope(inst);
}
void BrowserDiscoveryHighlight(bool enabled)
{
	if(!enabled || mode != INSTANCES || !highlight) return;
	parseQuery();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(BrowserDiscoveryIsMatch(inst)){
			inst->m_highlight = HIGHLIGHT_MATCH;
			if(inst->m_lod) inst->m_lod->m_highlight = HIGHLIGHT_MATCH;
		}
	}
}
int BrowserDiscoveryCount(int pawnModel)
{
	collect();
	auto group = groupIndex.find(EditorModelId(pawnModel));
	return group == groupIndex.end() ? 0 : (int)groups[group->second].members.size();
}
