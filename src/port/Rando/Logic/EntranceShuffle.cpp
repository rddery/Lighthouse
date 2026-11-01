#include "EntranceShuffle.h"
#include "Logic.h"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <set>

#include <spdlog/spdlog.h>

extern "C" s32 D_80385F30[0x2C];

// GlitchlessLogic.cpp
void ResetSaveData();
void SetUnlockedAbility(ability_e abilityId);

namespace Rando {

namespace EntranceShuffle {

namespace {

struct World {
    const char* name;
    level_e level;
    RandoRegionId doorRegion;  // Lair region with the door; leaving the world returns here
    RandoRegionId worldRegion; // where entering the world puts you
    int32_t enterMap, enterExit;
    int32_t lairMap, lairExit;
};

// clang-format off
const std::array<World, RANDO_WORLD_ENTRANCE_COUNT> kWorlds = { {
    { "Mumbo's Mountain",       LEVEL_1_MUMBOS_MOUNTAIN,     RR_GRUNTILDAS_LAIR_LOBBY,                             RR_MUMBOS_MOUNTAIN_ENTRANCE,
      MAP_2_MM_MUMBOS_MOUNTAIN,       WARP_MM_5_WORLD_ENTRACE,     MAP_69_GL_MM_LOBBY,  WARP_GL_MM_LOBBY_2_MM_ENTRANCE },
    { "Treasure Trove Cove",    LEVEL_2_TREASURE_TROVE_COVE, RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_TREASURE_TROVE_COVE, RR_TREASURE_TROVE_COVE_ENTRANCE,
      MAP_7_TTC_TREASURE_TROVE_COVE,  WARP_TTC_4_WORLD_ENTRANCE,   MAP_6D_GL_TTC_LOBBY, WARP_GL_TTC_LOBBY_4_TTC_ENTRANCE },
    { "Clanker's Cavern",       LEVEL_3_CLANKERS_CAVERN,     RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_CLANKERS_CAVERN,     RR_CLANKERS_CAVERN_ENTRANCE,
      MAP_B_CC_CLANKERS_CAVERN,       WARP_CC_5_WORLD_ENTRANCE,    MAP_70_GL_CC_LOBBY,  WARP_GL_CC_LOBBY_2_CC_ENTRANCE },
    { "Bubblegloop Swamp",      LEVEL_4_BUBBLEGLOOP_SWAMP,   RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_BUBBLEGLOOP_SWAMP,   RR_BUBBLEGLOOP_SWAMP,
      MAP_D_BGS_BUBBLEGLOOP_SWAMP,    WARP_BGS_2_WORLD_ENTRANCE,   MAP_72_GL_BGS_LOBBY, WARP_GL_BGS_LOBBY_2_BGS_ENTRANCE },
    { "Freezeezy Peak",         LEVEL_5_FREEZEEZY_PEAK,      RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_FREEZEEZY_PEAK,      RR_FREEZEEZY_PEAK_ENTRANCE,
      MAP_27_FP_FREEZEEZY_PEAK,       WARP_FP_1_WORLD_ENTRANCE,    MAP_6F_GL_FP_LOBBY,  WARP_GL_FP_LOBBY_6_FP_ENTRANCE },
    { "Gobi's Valley",          LEVEL_7_GOBIS_VALLEY,        RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_GOBIS_VALLEY,        RR_GOBIS_VALLEY_ENTRANCE,
      MAP_12_GV_GOBIS_VALLEY,         WARP_GV_8_WORLD_ENTRANCE,    MAP_6E_GL_GV_LOBBY,  WARP_GL_GV_LOBBY_3_GV_ENTRANCE },
    { "Mad Monster Mansion",    LEVEL_A_MAD_MONSTER_MANSION, RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_MAD_MONSTER_MANSION, RR_MAD_MONSTER_MANSION_ENTRANCE,
      MAP_1B_MMM_MAD_MONSTER_MANSION, WARP_MMM_14_WORLD_ENTRANCE,  MAP_75_GL_MMM_LOBBY, WARP_GL_MMM_LOBBY_2_MMM_ENTRANCE },
    { "Rusty Bucket Bay",       LEVEL_9_RUSTY_BUCKET_BAY,    RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_RUSTY_BUCKET_BAY,    RR_RUSTY_BUCKET_BAY_ENTRANCE,
      MAP_31_RBB_RUSTY_BUCKET_BAY,    WARP_RBB_10_WORLD_ENTRANCE,  MAP_77_GL_RBB_LOBBY, WARP_GL_RBB_LOBBY_2_RBB_ENTRANCE },
    { "Click Clock Wood",       LEVEL_8_CLICK_CLOCK_WOOD,    RR_GRUNTILDAS_LAIR_NEAR_ENTRANCE_CLICK_CLOCK_WOOD,    RR_CLICK_CLOCK_WOOD_ENTRANCE,
      MAP_40_CCW_HUB,                 WARP_CCW_7_WORLD_ENTRANCE,   MAP_79_GL_CCW_LOBBY, WARP_GL_CCW_LOBBY_6_CCW_ENTRANCE },
} };

// The puzzles a playthrough spends Jiggies on: the nine world doors and the Door of Grunty, with their index in
// the game's picture table (src/lair/jigsawpicture.c PICTURE_INFO)
struct Puzzle {
    level_e level;
    int32_t costIndex; // _puzzleCost index
    RandoAccessId board;
};

const std::array<Puzzle, RANDO_WORLD_ENTRANCE_COUNT + 1> kPuzzles = { {
    { LEVEL_1_MUMBOS_MOUNTAIN,     0, RA_PUZZLE_BOARD_MUMBOS_MOUNTAIN },
    { LEVEL_2_TREASURE_TROVE_COVE, 1, RA_PUZZLE_BOARD_TREASURE_TROVE_COVE },
    { LEVEL_3_CLANKERS_CAVERN,     2, RA_PUZZLE_BOARD_CLANKERS_CAVERN },
    { LEVEL_4_BUBBLEGLOOP_SWAMP,   3, RA_PUZZLE_BOARD_BUBBLEGLOOP_SWAMP },
    { LEVEL_5_FREEZEEZY_PEAK,      4, RA_PUZZLE_BOARD_FREEZEEZY_PEAK },
    { LEVEL_7_GOBIS_VALLEY,        5, RA_PUZZLE_BOARD_GOBIS_VALLEY },
    { LEVEL_A_MAD_MONSTER_MANSION, 6, RA_PUZZLE_BOARD_MAD_MONSTER_MANSION },
    { LEVEL_9_RUSTY_BUCKET_BAY,    7, RA_PUZZLE_BOARD_RUSTY_BUCKET_BAY },
    { LEVEL_8_CLICK_CLOCK_WOOD,    8, RA_PUZZLE_BOARD_CLICK_CLOCK_WOOD },
    { LEVEL_6_LAIR,                9, RA_PUZZLE_BOARD_GRUNTILDA },
} };
// clang-format on

static_assert(RANDO_WORLD_ENTRANCE_COUNT == 9, "update the vanilla layouts below");
std::array<int8_t, RANDO_WORLD_ENTRANCE_COUNT> sDoorToWorld = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
std::array<int8_t, RANDO_WORLD_ENTRANCE_COUNT> sWorldToDoor = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

bool sVerifying = false;
std::set<level_e> sVerifierOpenWorlds;
std::map<int32_t, int32_t> sVerifierJinjos; // Jinjos collected, by the world they belong to

void UpdateInverse() {
    for (int32_t door = 0; door < RANDO_WORLD_ENTRANCE_COUNT; door++) {
        sWorldToDoor[sDoorToWorld[door]] = (int8_t)door;
    }
}

int32_t WorldIndexByLevel(int32_t level) {
    for (int32_t i = 0; i < RANDO_WORLD_ENTRANCE_COUNT; i++) {
        if (kWorlds[i].level == level) {
            return i;
        }
    }
    return -1;
}

} // namespace

const char* GetWorldName(int32_t index) {
    return index >= 0 && index < RANDO_WORLD_ENTRANCE_COUNT ? kWorlds[index].name : "";
}

void ResetLayout() {
    std::iota(sDoorToWorld.begin(), sDoorToWorld.end(), 0);
    UpdateInverse();
}

void SetLayout(const int8_t* doorToWorld) {
    std::array<bool, RANDO_WORLD_ENTRANCE_COUNT> used = {};
    for (int32_t door = 0; door < RANDO_WORLD_ENTRANCE_COUNT; door++) {
        int8_t world = doorToWorld[door];
        if (world < 0 || world >= RANDO_WORLD_ENTRANCE_COUNT || used[world]) {
            SPDLOG_WARN("[EntranceShuffle] Invalid world entrance layout, using the vanilla one");
            ResetLayout();
            return;
        }
        used[world] = true;
        sDoorToWorld[door] = world;
    }
    UpdateInverse();
}

void GetLayout(int8_t* doorToWorld) {
    std::copy(sDoorToWorld.begin(), sDoorToWorld.end(), doorToWorld);
}

bool IsShuffled() {
    for (int32_t door = 0; door < RANDO_WORLD_ENTRANCE_COUNT; door++) {
        if (sDoorToWorld[door] != door) {
            return true;
        }
    }
    return false;
}

int32_t GetWorldBehindDoor(int32_t door) {
    return door >= 0 && door < RANDO_WORLD_ENTRANCE_COUNT ? sDoorToWorld[door] : door;
}

void Shuffle(std::mt19937& rng) {
    std::iota(sDoorToWorld.begin(), sDoorToWorld.end(), 0);
    // Fisher-Yates with the generator's own draws: std::shuffle's draws differ between standard libraries
    for (int32_t i = RANDO_WORLD_ENTRANCE_COUNT - 1; i > 0; i--) {
        std::swap(sDoorToWorld[i], sDoorToWorld[rng() % (i + 1)]);
    }
    UpdateInverse();
}

RandoRegionId RemapConnection(RandoRegionId from, RandoRegionId to) {
    for (int32_t i = 0; i < RANDO_WORLD_ENTRANCE_COUNT; i++) {
        // Through a door into its world
        if (from == kWorlds[i].doorRegion && to == kWorlds[i].worldRegion) {
            return kWorlds[sDoorToWorld[i]].worldRegion;
        }
        // Out of a world back to the Lair
        if (from == kWorlds[i].worldRegion && to == kWorlds[i].doorRegion) {
            return kWorlds[sWorldToDoor[i]].doorRegion;
        }
    }
    return to;
}

void RemapMapChange(int32_t* map, int32_t* exit, int32_t enteringDoor) {
    const level_e currentLevel = map_getLevel((enum map_e)gsworld_getMap());

    // Entering through a Lair door
    if (enteringDoor >= 0 && enteringDoor < RANDO_WORLD_ENTRANCE_COUNT && *map == kWorlds[enteringDoor].enterMap &&
        *exit == kWorlds[enteringDoor].enterExit) {
        const World& world = kWorlds[sDoorToWorld[enteringDoor]];
        *map = world.enterMap;
        *exit = world.enterExit;
        return;
    }

    for (int32_t i = 0; i < RANDO_WORLD_ENTRANCE_COUNT; i++) {
        // Leaving world i, by its exit or the pause menu
        if (currentLevel == kWorlds[i].level && *map == kWorlds[i].lairMap && *exit == kWorlds[i].lairExit) {
            const World& door = kWorlds[sWorldToDoor[i]];
            *map = door.lairMap;
            *exit = door.lairExit;
            return;
        }
    }
}

bool IsVerifying() {
    return sVerifying;
}

bool VerifierWorldOpen(level_e levelId) {
    return sVerifierOpenWorlds.contains(levelId);
}

bool VerifierHasAllJinjos(level_e levelId) {
    auto it = sVerifierJinjos.find(levelId);
    return it != sVerifierJinjos.end() && it->second >= 5;
}

namespace {

// The regions and events reachable from Spiral Mountain, through the shuffled entrances
void ComputeReachable() {
    for (int32_t i = RR_UNKNOWN; i < RR_MAX; i++) {
        reachableRegions[i].canAccess = false;
    }
    for (int32_t i = RA_UNKNOWN; i < RA_MAX; i++) {
        reachableEvents[i].canAccess = false;
    }

    reachableRegions[RR_SPIRAL_MOUNTAIN_ENTRANCE].canAccess = true;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int32_t i = RR_UNKNOWN; i < RR_MAX; i++) {
            if (!reachableRegions[i].canAccess) {
                continue;
            }
            auto region = Rando::Logic::Regions.find((RandoRegionId)i);
            if (region == Rando::Logic::Regions.end()) {
                continue;
            }
            for (const auto& [eventId, condition] : region->second.events) {
                if (!reachableEvents[eventId].canAccess && condition()) {
                    reachableEvents[eventId].canAccess = true;
                    changed = true;
                }
            }
            for (const auto& [targetId, connection] : region->second.connections) {
                const RandoRegionId target = RemapConnection((RandoRegionId)i, targetId);
                if (!reachableRegions[target].canAccess && connection.first()) {
                    reachableRegions[target].canAccess = true;
                    changed = true;
                }
            }
        }
    }
}

} // namespace

namespace {

struct Item {
    int32_t actorId;
    int32_t collectionId;
    RandoCheckId home; // the check this item comes from, which decides the world a Jinjo belongs to
};

// Collects every reachable item, completing the cheapest affordable puzzle whenever nothing else is left to collect,
// and reports whether Gruntilda can be beaten. Optimistic: every note, Jiggy, Mumbo Token and Jinjo is already
// collected (and every ability, with allAbilities), so only the layout and the abilities can stop it.
bool Simulate(std::map<RandoCheckId, Item>& items, bool optimistic, bool allAbilities) {
    ResetSaveData();
    Rando::Logic::GrantStartingLoadout();
    sVerifying = true;
    sVerifierOpenWorlds.clear();
    sVerifierJinjos.clear();

    std::set<RandoCheckId> collected;
    int32_t notes = 0, jiggies = 0, tokens = 0, spentJiggies = 0;
    if (allAbilities) {
        for (int32_t ability = ABILITY_0_BARGE; ability <= ABILITY_13_1ST_NOTEDOOR; ability++) {
            SetUnlockedAbility((ability_e)ability);
        }
    }
    if (optimistic) {
        notes = 900;
        jiggies = 100;
        tokens = 116;
        for (const auto& world : kWorlds) {
            sVerifierJinjos[world.level] = 5;
        }
    }

    const auto updateCounts = [&]() {
        D_80385F30[ITEM_C_NOTE] = notes;
        D_80385F30[ITEM_E_JIGGY] = jiggies - spentJiggies;
        D_80385F30[ITEM_26_JIGGY_TOTAL] = jiggies;
        D_80385F30[ITEM_1C_MUMBO_TOKEN] = tokens;
        D_80385F30[ITEM_25_MUMBO_TOKEN_TOTAL] = tokens;
    };
    updateCounts();

    bool progress = true;
    while (progress) {
        progress = false;
        ComputeReachable();

        for (int32_t r = RR_UNKNOWN; r < RR_MAX; r++) {
            if (!reachableRegions[r].canAccess) {
                continue;
            }
            auto region = Rando::Logic::Regions.find((RandoRegionId)r);
            if (region == Rando::Logic::Regions.end()) {
                continue;
            }
            for (const auto& [checkId, check] : region->second.checks) {
                if (collected.contains(checkId) || !check.first()) {
                    continue;
                }
                collected.insert(checkId);
                progress = true;

                const Item& item = items[checkId];
                switch (item.actorId) {
                    case ACTOR_46_JIGGY:
                        jiggies++;
                        break;
                    case ACTOR_51_MUSIC_NOTE:
                        notes++;
                        break;
                    case ACTOR_2D_MUMBO_TOKEN:
                        tokens++;
                        break;
                    case ACTOR_12C_MOLEHILL:
                        SetUnlockedAbility((ability_e)item.collectionId);
                        break;
                    case ACTOR_5E_JINJO_YELLOW:
                    case ACTOR_5F_JINJO_ORANGE:
                    case ACTOR_60_JINJO_BLUE:
                    case ACTOR_61_JINJO_PINK:
                    case ACTOR_62_JINJO_GREEN:
                        sVerifierJinjos[Rando::StaticData::Checks[item.home].worldId]++;
                        break;
                    default:
                        break;
                }
                updateCounts();
            }
        }

        if (progress) {
            continue;
        }

        // Nothing new to collect: complete the cheapest puzzle we can reach and afford, then look again
        const Puzzle* cheapest = nullptr;
        int32_t cheapestCost = 0;
        for (const auto& puzzle : kPuzzles) {
            const int32_t cost = _puzzleCost(puzzle.costIndex);
            // World doors spend their Jiggies, since the layout decides which order they open in. The Door of
            // Grunty is checked like the rest of the logic checks it (enough Jiggies in total): its cost doesn't
            // depend on the layout, and the logic can't reach every vanilla Jiggy (some witch switch Jiggies
            // have no events yet), so the full 94 isn't reachable in it.
            const int32_t available = puzzle.level == LEVEL_6_LAIR ? jiggies : jiggies - spentJiggies;
            if (sVerifierOpenWorlds.contains(puzzle.level) || !reachableEvents[puzzle.board].canAccess ||
                available < cost) {
                continue;
            }
            if (cheapest == nullptr || cost < cheapestCost) {
                cheapest = &puzzle;
                cheapestCost = cost;
            }
        }
        if (cheapest != nullptr) {
            sVerifierOpenWorlds.insert(cheapest->level);
            spentJiggies += cheapest->level == LEVEL_6_LAIR ? 0 : cheapestCost;
            updateCounts();
            progress = true;
        }
    }

    const bool beatable = reachableEvents[RA_GAME_COMPLETE].canAccess;
    if (!optimistic) {
        SPDLOG_INFO("[EntranceShuffle] Playthrough check: {} ({} checks, {} jiggies, {} notes, {} tokens)",
                    beatable ? "beatable" : "not beatable", collected.size(), jiggies, notes, tokens);
    }

    sVerifying = false;
    sVerifierOpenWorlds.clear();
    sVerifierJinjos.clear();
    ResetSaveData();
    return beatable;
}

std::map<RandoCheckId, Item> VanillaItems() {
    std::map<RandoCheckId, Item> items;
    for (const auto& [checkId, check] : Rando::StaticData::Checks) {
        items[checkId] = { check.actorId, check.collectionId, checkId };
    }
    return items;
}

} // namespace

bool CouldBeBeatable(bool abilitiesShuffled) {
    std::map<RandoCheckId, Item> items = VanillaItems();
    return Simulate(items, true, abilitiesShuffled);
}

bool VerifyBeatable(const std::vector<RandoCheckId>& checkPool,
                    const std::vector<std::tuple<actor_e, int32_t, RandoCheckId>>& itemPool,
                    const std::vector<RandoCheckId>& abilityCheckPool,
                    const std::vector<std::tuple<actor_e, int32_t, RandoCheckId>>& abilityItemPool) {
    // What each location holds: the placed item where items were shuffled, its own item elsewhere
    std::map<RandoCheckId, Item> items = VanillaItems();
    for (size_t i = 0; i < checkPool.size() && i < itemPool.size(); i++) {
        items[checkPool[i]] = { std::get<0>(itemPool[i]), std::get<1>(itemPool[i]), std::get<2>(itemPool[i]) };
    }
    for (size_t i = 0; i < abilityCheckPool.size() && i < abilityItemPool.size(); i++) {
        items[abilityCheckPool[i]] = { std::get<0>(abilityItemPool[i]), std::get<1>(abilityItemPool[i]),
                                       std::get<2>(abilityItemPool[i]) };
    }
    return Simulate(items, false, false);
}

} // namespace EntranceShuffle

} // namespace Rando
