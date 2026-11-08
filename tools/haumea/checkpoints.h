#pragma once

// Checkpoints as the SA-MP client shows them (checkpoints.cpp): the red
// cylinder, race types 0-8 with the arrow to the next one, the finish flag,
// and the moving air hoops.

struct ObjectInst;

// One checkpoint at time seconds. pickColour: drawn solid in that colour for
// the picking pass (no arrow or flag); nil draws it as in game.
void DrawCheckpointMarker(const ObjectInst *inst, float time, const rw::RGBA *pickColour);
// Every checkpoint in the map, in the transparent pass (renderer.cpp), and
// in the colour-coded pick pass in their id colours.
void RenderCheckpointMarkers(float time);
void RenderCheckpointMarkersColourCoded(void);

// Driving a race in the editor, as a player would see it (haumea_races.inc's
// order): only the checkpoint to reach is shown, Finish on the last one of
// the last lap. Advance moves to the next; the preview ends past the finish.
bool StartRacePreview(const char *raceName);
void StopRacePreview(void);
void AdvanceRacePreview(void);
bool RacePreviewActive(void);
const char *RacePreviewRace(void);
// The checkpoint to reach (index in GetRaceCheckpoints), its lap, the laps
// and how many checkpoints the race has.
int RacePreviewTarget(void);
int RacePreviewLap(void);
int RacePreviewLaps(void);
int RacePreviewCount(void);
