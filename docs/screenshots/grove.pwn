// The scene docs/images' screenshots are taken in (tools/release/screenshots.py).
// Grove Street with one of most elements, in Haumea's own export format.
#include <open.mp>
#include <streamer>

static Haumea_grove_zone_1 = -1;
static Haumea_grove_soundId[1], Float:Haumea_grove_soundAt[1][4], bool:Haumea_grove_soundStop[1], Haumea_grove_numSounds;
static Haumea_grove_soundArea[1];

stock Haumea_grove_AddSound(soundid, Float:x, Float:y, Float:z, Float:radius, bool:stopOnLeave,
	worldid = -1, interiorid = -1)
{
	if(Haumea_grove_numSounds == sizeof Haumea_grove_soundId)
		return;
	new i = Haumea_grove_numSounds++;
	Haumea_grove_soundId[i] = soundid;
	Haumea_grove_soundAt[i][0] = x; Haumea_grove_soundAt[i][1] = y; Haumea_grove_soundAt[i][2] = z; Haumea_grove_soundAt[i][3] = radius;
	Haumea_grove_soundStop[i] = stopOnLeave;
	Haumea_grove_soundArea[i] = _:CreateDynamicSphere(x, y, z, radius, worldid, interiorid);
}

stock Haumea_grove_SoundArea(playerid, {DynamicArea, _}:areaid, bool:entered)
{
	for(new i = 0; i < Haumea_grove_numSounds; i++){
		if(Haumea_grove_soundArea[i] != _:areaid)
			continue;
		if(entered)
			PlayerPlaySound(playerid, Haumea_grove_soundId[i], Haumea_grove_soundAt[i][0], Haumea_grove_soundAt[i][1], Haumea_grove_soundAt[i][2]);
		else if(Haumea_grove_soundStop[i])
			PlayerPlaySound(playerid, 0, 0.0, 0.0, 0.0);
	}
}

stock Haumea_grove_CreateObjects()
{
	// ---- NPC paths (open.mp) ----
	new grove_patrol = NPC_CreatePath();
	NPC_AddPointToPath(grove_patrol, 2486.0, -1671.0, 13.4, 0.2); // path point
	NPC_AddPointToPath(grove_patrol, 2470.0, -1668.0, 13.4, 0.2); // path point
	NPC_AddPointToPath(grove_patrol, 2470.0, -1655.0, 13.4, 0.2); // path point
	NPC_AddPointToPath(grove_patrol, 2490.0, -1655.0, 13.4, 0.2); // path point
	CreateDynamicObject(1280, 2496.0, -1660.0, 13.75, 0.0, 0.0, 120.0, -1, -1, -1); // parkbench1
	CreateDynamicObject(1481, 2479.0, -1668.0, 13.95, 0.0, 0.0, 90.0, -1, -1, -1); // DYN_BAR_B_Q
	CreateDynamicObject(1342, 2468.0, -1662.0, 13.9, 0.0, 0.0, 30.0, -1, -1, -1); // noodlecart_prop
	CreateDynamicObject(2232, 2502.0, -1664.0, 13.95, 0.0, 0.0, -110.0, -1, -1, -1); // MED_SPEAKER_4
	CreateDynamicObject(1337, 2505.0, -1670.0, 13.95, 0.0, 0.0, 0.0, -1, -1, -1); // BinNt07_LA
	AddStaticVehicleEx(492, 2508.0, -1676.0, 13.2, 45.0, 59, 59, 300); // greenwoo
	CreateDynamicPickup(1239, 2, 2491.0, -1675.0, 13.4, -1, -1, -1, STREAMER_PICKUP_SD); // info [pickup]
	CreateDynamic3DTextLabel("{33AA33}Grove Street", 0xFFFFFFFF, 2492.0, -1690.0, 15.0, 30.0, INVALID_PLAYER_ID, INVALID_VEHICLE_ID, false, -1, -1, -1, STREAMER_3D_TEXT_LABEL_SD); // home [3D text label]
	Haumea_grove_zone_1 = GangZoneCreate(2450.0, -1700.0, 2520.0, -1645.0); // grove [gang zone]
	CreateDynamicMapIcon(2495.0, -1668.0, 13.3, 62, 0xFFFFFFFF, -1, -1, -1, STREAMER_MAP_ICON_SD, MAPICON_LOCAL); // gang [map icon]
	Haumea_grove_AddSound(1097, 2502.0, -1664.0, 13.6, 20.0, true, -1, -1); // Background music
	new Haumea_grove_actor_0 = CreateDynamicActor(105, 2480.0, -1666.0, 13.4, 200.0, 1, 100.0, -1, -1, -1, STREAMER_ACTOR_SD); // FAM1 [actor]
	ApplyDynamicActorAnimation(Haumea_grove_actor_0, "GANGS", "prtial_gngtlkA", 4.1, 1, 0, 0, 0, 0);
	new Haumea_grove_actor_1 = CreateDynamicActor(106, 2482.0, -1667.5, 13.4, 30.0, 1, 100.0, -1, -1, -1, STREAMER_ACTOR_SD); // FAM2 [actor]
	ApplyDynamicActorAnimation(Haumea_grove_actor_1, "GANGS", "prtial_gngtlkB", 4.1, 1, 0, 0, 0, 0);
	new Haumea_grove_actor_2 = CreateDynamicActor(107, 2495.5, -1661.0, 13.4, 120.0, 1, 100.0, -1, -1, -1, STREAMER_ACTOR_SD); // FAM3 [actor]
	ApplyDynamicActorAnimation(Haumea_grove_actor_2, "PED", "SEAT_idle", 4.1, 1, 0, 0, 0, 0);
	new Haumea_grove_npc_3 = NPC_Create("Sweet"); // FAM1 [NPC]
	NPC_Spawn(Haumea_grove_npc_3);
	NPC_SetSkin(Haumea_grove_npc_3, 105);
	NPC_SetPos(Haumea_grove_npc_3, 2486.0, -1671.0, 13.4);
	NPC_SetFacingAngle(Haumea_grove_npc_3, -60.0);
	AddPlayerClass(0, 2495.0, -1685.0, 13.5, 0.0, 0, 0, 0, 0, 0, 0); // CJ [player class]
	// ---- NPC movement ----
	NPC_MoveByPath(Haumea_grove_npc_3, grove_patrol, NPC_MOVE_TYPE_WALK, NPC_MOVE_SPEED_AUTO, false);
}

stock Haumea_grove_ShowForPlayer(playerid)
{
	GangZoneShowForPlayer(playerid, Haumea_grove_zone_1, 0x33AA3366); // grove
}
