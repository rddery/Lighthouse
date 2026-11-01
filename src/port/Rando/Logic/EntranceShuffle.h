#ifndef RANDO_ENTRANCE_SHUFFLE_H
#define RANDO_ENTRANCE_SHUFFLE_H

#include <cstdint>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "port/Rando/Types.h"
#include "functions.h"

// World entrance shuffle: each of the nine world doors in Gruntilda's Lair leads to a shuffled world, and leaving
// a world returns you to the door that leads to it. A door keeps its own requirements (its puzzle and the Lair
// around it); only the world behind it changes.
namespace Rando {

namespace EntranceShuffle {

// Index of a world in the layout: the door with this index leads to GetWorldBehindDoor(index).
// Order: MM, TTC, CC, BGS, FP, GV, MMM, RBB, CCW.
const char* GetWorldName(int32_t index);

// Identity layout (every door leads to its own world).
void ResetLayout();
// Copies a layout of RANDO_WORLD_ENTRANCE_COUNT entries; an invalid one (not a permutation) resets it.
void SetLayout(const int8_t* doorToWorld);
void GetLayout(int8_t* doorToWorld);
bool IsShuffled();
int32_t GetWorldBehindDoor(int32_t door);

void Shuffle(std::mt19937& rng);

// Logic: the region a connection actually leads to under the current layout.
RandoRegionId RemapConnection(RandoRegionId from, RandoRegionId to);

// Runtime: rewrites a map change into a world, or out of one back to the Lair, to follow the layout.
void RemapMapChange(int32_t* map, int32_t* exit);

// Simulates a playthrough with the given placements (vanilla items where nothing was placed): collects every
// reachable item, spends Jiggies on the puzzles it can reach, and reports whether Gruntilda can be beaten.
bool VerifyBeatable(const std::vector<RandoCheckId>& checkPool,
                    const std::vector<std::tuple<actor_e, int32_t, RandoCheckId>>& itemPool,
                    const std::vector<RandoCheckId>& abilityCheckPool,
                    const std::vector<std::tuple<actor_e, int32_t, RandoCheckId>>& abilityItemPool);

// A cheap check that rules out layouts no item placement can save: the same playthrough with every note, Jiggy,
// Mumbo Token and Jinjo already collected, and every ability too if molehills are shuffled.
bool CouldBeBeatable(bool abilitiesShuffled);

// During VerifyBeatable, the logic's world and Jinjo checks use the simulation's own bookkeeping.
bool IsVerifying();
bool VerifierWorldOpen(level_e levelId);
bool VerifierHasAllJinjos(level_e levelId);

} // namespace EntranceShuffle

} // namespace Rando

#endif
