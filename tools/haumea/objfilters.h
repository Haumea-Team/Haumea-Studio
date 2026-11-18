#pragma once

#include <string>
#include <vector>

// The Asset Browser's filters: size, source, collision, type, appearance,
// where the world uses a model, and the user's own (favourites, recent, the
// open map, the workspace), with a sort. They combine with the search and
// the category, update as they change and last for the editor session;
// named presets are saved with the settings. Facts come from modelmeta.

enum ObjectSort { SORT_ID, SORT_NAME, SORT_SIZE, SORT_POLYGONS, SORT_DRAW_DISTANCE, SORT_RECENT, NUM_OBJECT_SORTS };

// Whether a model passes every active filter.
bool ObjectPassesFilters(int id);
bool ObjectFiltersActive(void);
// Sorts the browser's results in place by the chosen order.
void SortObjectResults(int *ids, int n);
// The Filters button for the browser's toolbar (warning-tinted with a count
// while filters hide models), its natural width, and the panel it unfolds:
// the active chips, always, and the filters themselves while it is open.
void uiObjectFiltersButton(float width);
float ObjectFiltersButtonWidth(void);
bool ObjectFiltersOpen(void);
void uiObjectFiltersPanel(void);

// Presets, one settings line each ("name|key=value;...").
int NumObjectFilterPresets(void);
std::string GetObjectFilterPresetLine(int i);
void AddObjectFilterPresetLine(const char *line);

// For the agent and tests: the filters as key=value;... and back. Keys:
// width, depth, height, volume, footprint (min-max metres, either side
// empty), source (gta,samp,custom), ide, collision (with, without),
// two_sided, lod (hd, lod), has_lod, draw_distance, type (dynamic, breakable,
// timed, animated, upgrade, building, interior, exterior, alpha), hour,
// txd, texture, polygons, materials, city (los_santos, san_fierro,
// las_venturas, countryside, interiors, not_placed), zone, mine (favourites,
// recent, open_map, workspace), sort (id, name, size, polygons,
// draw_distance, recent), descending.
std::string GetObjectFiltersText(void);
bool SetObjectFiltersText(const char *text, std::string *error);
void ClearObjectFilters(void);
// Every model passing filters given as text (and whose name contains name,
// if given), in their sort order: the count, or -1 with error.
int FilterModels(const char *text, const char *name, std::vector<int> &out, std::string *error);
