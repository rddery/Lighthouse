#include "MiscBehavior.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/Rando/Logic/EntranceShuffle.h"

#include "save.h"

void Rando::MiscBehavior::InitEntranceBehavior() {
    // Entering a world from its Lair door, and leaving it (by its exit or the pause menu), follow the layout
    REGISTER_VB_SHOULD(VB_MAP_CHANGE_REQUEST, EVENT_PRIORITY_NORMAL, {
        int32_t* map = va_arg(args, int32_t*);
        int32_t* exit = va_arg(args, int32_t*);
        if (IS_RANDO && RANDO_SAVE_OPTIONS[RO_SHUFFLE_WORLD_ENTRANCES].optionValue == RO_GENERIC_ON) {
            Rando::EntranceShuffle::RemapMapChange(map, exit);
        }
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
