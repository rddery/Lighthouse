#include "MiscBehavior.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Rando/Logic/EntranceShuffle.h"

#include "save.h"

#include <array>

extern "C" {
typedef void (*WarpFunction)(NodeProp*, ActorMarker*);
extern WarpFunction sWarpFunctions[];

// The Lair doors' warps (src/core2/map/cutscene_skip.c)
void warp_mmEnterLevel(NodeProp*, ActorMarker*);
void warp_ttcEnterLevel(NodeProp*, ActorMarker*);
void warp_ccEnterLevel(NodeProp*, ActorMarker*);
void warp_bgsEnterLevel(NodeProp*, ActorMarker*);
void warp_fpEnterLevel(NodeProp*, ActorMarker*);
void warp_gvEnterLevel(NodeProp*, ActorMarker*);
void warp_mmmEnterLevel(NodeProp*, ActorMarker*);
void warp_rbbEnterLevel(NodeProp*, ActorMarker*);
void warp_ccwEnterLevel(NodeProp*, ActorMarker*);
}

// In the layout's door order (see EntranceShuffle.h)
static const std::array<WarpFunction, RANDO_WORLD_ENTRANCE_COUNT> kDoorWarps = {
    warp_mmEnterLevel, warp_ttcEnterLevel, warp_ccEnterLevel,  warp_bgsEnterLevel, warp_fpEnterLevel,
    warp_gvEnterLevel, warp_mmmEnterLevel, warp_rbbEnterLevel, warp_ccwEnterLevel,
};

// The door whose warp is taking the player into a world, until the map change it asks for
static int32_t sEnteringDoor = -1;

#define ENTRANCES_SHUFFLED (IS_RANDO && RANDO_SAVE_OPTIONS[RO_SHUFFLE_WORLD_ENTRANCES].optionValue == RO_GENERIC_ON)

void Rando::MiscBehavior::InitEntranceBehavior() {
    REGISTER_LISTENER(OnWarpDispatch, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        OnWarpDispatch* ev = (OnWarpDispatch*)event;
        sEnteringDoor = -1;
        if (!ENTRANCES_SHUFFLED || ev->warpDest >= 0) {
            return;
        }
        for (int32_t door = 0; door < RANDO_WORLD_ENTRANCE_COUNT; door++) {
            if (sWarpFunctions[ev->warpId] == kDoorWarps[door]) {
                sEnteringDoor = door;
            }
        }
    });

    // Entering a world through its Lair door, and leaving it (by its exit or the pause menu), follow the layout
    REGISTER_VB_SHOULD(VB_MAP_CHANGE_REQUEST, EVENT_PRIORITY_NORMAL, {
        int32_t* map = va_arg(args, int32_t*);
        int32_t* exit = va_arg(args, int32_t*);
        if (ENTRANCES_SHUFFLED) {
            Rando::EntranceShuffle::RemapMapChange(map, exit, sEnteringDoor);
        }
        sEnteringDoor = -1;
        (void)should;
    });

    // The layout belongs to the loaded file: a rando file brings its own (GeneratePoolFromSaveData, or a new
    // seed's generation), anything else uses the vanilla one
    REGISTER_LISTENER(OnSaveLoad, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        OnSaveLoad* ev = (OnSaveLoad*)event;
        SaveData* saveData = (SaveData*)ev->saveData;
        if (saveData->magic != 0 && saveData->shipSaveData.fileType != FILE_TYPE_SAVE_RANDO) {
            Rando::EntranceShuffle::ResetLayout();
        }
    });

    REGISTER_LISTENER(OnLoadFileSelect, EVENT_PRIORITY_NORMAL,
                      [](IEvent* event) { Rando::EntranceShuffle::ResetLayout(); });
}
