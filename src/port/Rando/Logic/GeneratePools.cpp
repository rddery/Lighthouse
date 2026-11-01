#include "Logic.h"
// #include "port/Rando/Spoiler/Spoiler.h"
#include "port/UI/Notification.h"
#include "port/UI/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <sstream>
#include <random>
#include <spdlog/spdlog.h>

#include "enums.h"

int32_t randoFinalSeed = 0;

namespace Rando {

namespace Logic {
std::vector<RandoCheckId> checkPool;
std::vector<std::tuple<actor_e, int32_t, RandoCheckId>> itemPool;

std::vector<RandoCheckId> abilityCheckPool;
std::vector<std::tuple<actor_e, int32_t, RandoCheckId>> abilityItemPool;

std::vector<RandoSaveCheck> shuffledPool;

uint32_t GetRandoSeed(const std::string& input) {
    if (CVarGetInteger(CVAR_RANDOMIZER_SETTING("ManualInput"), 0) && !input.empty()) {
        return Ship_Hash(input);
    }
    std::string randoHash = std::to_string(GetUnixTimestamp());
    return Ship_Hash(randoHash);
}

void ShuffleRandoItems(const std::string& input, std::vector<std::tuple<actor_e, int32_t, RandoCheckId>>& pool) {
    uint32_t seed = GetRandoSeed(input);

    std::mt19937 rando(seed);
    std::shuffle(pool.begin(), pool.end(), rando);

    randoFinalSeed = seed;
}

bool GenerateShufflePool(SaveData* saveData) {
    checkPool.clear();
    itemPool.clear();
    abilityCheckPool.clear();
    abilityItemPool.clear();
    shuffledPool.clear();

    for (auto& [randoCheckId, randoStaticCheck] : Rando::StaticData::Checks) {
        if (randoStaticCheck.randoCheckType == RCTYPE_EMPTY_HONEYCOMB &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_EMPTY_HONEYCOMBS].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_JIGGY &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_JIGGIES].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_JINJO &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_JINJOS].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_MOLEHILL) {
            if (CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_MOLEHILLS].cvar, 0) == RO_GENERIC_ON) {
                abilityCheckPool.push_back(randoCheckId);
                abilityItemPool.push_back(
                    { (actor_e)randoStaticCheck.actorId, randoStaticCheck.collectionId, randoCheckId });
            }
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_MUMBO_TOKEN &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_MUMBO_TOKENS].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_MUSIC_NOTE &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_MUSIC_NOTES].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        if (randoStaticCheck.randoCheckType == RCTYPE_STOP_N_SWOP &&
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_STOP_N_SWOP].cvar, 0) == RO_GENERIC_OFF) {
            continue;
        }

        checkPool.push_back(randoCheckId);
        itemPool.push_back({ (actor_e)randoStaticCheck.actorId, randoStaticCheck.collectionId, randoCheckId });
    }

    if (!itemPool.empty()) {
        Rando::Logic::ShuffleRandoItems(CVarGetString(CVAR_RANDOMIZER_SETTING("InputSeed"), ""), itemPool);
    }

    if (!abilityItemPool.empty()) {
        Rando::Logic::ShuffleRandoItems(CVarGetString(CVAR_RANDOMIZER_SETTING("InputSeed"), ""), abilityItemPool);
    }

    if (itemPool.empty() && abilityItemPool.empty()) {
        // Nothing to shuffle set the seed, but the entrances may still need one
        randoFinalSeed = GetRandoSeed(CVarGetString(CVAR_RANDOMIZER_SETTING("InputSeed"), ""));
    }

    const bool shuffleEntrances =
        CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_WORLD_ENTRANCES].cvar, 0) == RO_GENERIC_ON;
    // Its own stream, so the entrance layout doesn't change the item placement draws
    std::mt19937 entranceRng((uint32_t)randoFinalSeed ^ 0x454E5452u);
    Rando::EntranceShuffle::ResetLayout();

    if (RANDO_SAVE_OPTIONS[RO_LOGIC].optionValue == RO_LOGIC_GLITCHLESS) {
        // Placement can stall on a bad draw, so retry on the same RNG stream before giving up. With shuffled
        // entrances a layout can also leave the game unbeatable, which only a playthrough check catches (the
        // placement assumes vanilla progression for unshuffled items), so try further layouts too.
        // Most layouts can't work at all with vanilla abilities (Talon Trot for the first note door is only taught
        // in Mumbo's Mountain, for one), so a cheap check skips those before spending a placement on them.
        const bool abilitiesShuffled =
            CVarGetInteger(Rando::StaticData::Options[RO_SHUFFLE_MOLEHILLS].cvar, 0) == RO_GENERIC_ON;
        const int maxLayouts = shuffleEntrances ? 3000 : 1;
        const int maxPlacedLayouts = shuffleEntrances ? 40 : 1;
        const int maxAttempts = shuffleEntrances ? 3 : 10;
        int placedLayouts = 0;
        bool generated = false;
        Rando::Logic::SeedGlitchlessPlacement(randoFinalSeed);
        for (int layout = 1; layout <= maxLayouts && placedLayouts < maxPlacedLayouts && !generated; layout++) {
            if (shuffleEntrances) {
                Rando::EntranceShuffle::Shuffle(entranceRng);
                if (!Rando::EntranceShuffle::CouldBeBeatable(abilitiesShuffled)) {
                    continue;
                }
            }
            placedLayouts++;
            for (int attempt = 1; attempt <= maxAttempts && !generated; attempt++) {
                std::vector<RandoCheckId> attemptCheckPool = checkPool;
                std::vector<std::tuple<actor_e, int32_t, RandoCheckId>> attemptItemPool = itemPool;
                std::vector<RandoCheckId> attemptAbilityCheckPool = abilityCheckPool;
                std::vector<std::tuple<actor_e, int32_t, RandoCheckId>> attemptAbilityItemPool = abilityItemPool;

                generated = Rando::Logic::GenerateGlitchlessLogicPool(
                    attemptCheckPool, attemptItemPool, attemptAbilityCheckPool, attemptAbilityItemPool, saveData);
                if (generated && shuffleEntrances &&
                    !Rando::EntranceShuffle::VerifyBeatable(attemptCheckPool, attemptItemPool,
                                                            attemptAbilityCheckPool, attemptAbilityItemPool)) {
                    generated = false;
                }
                if (generated) {
                    checkPool = attemptCheckPool;
                    itemPool = attemptItemPool;
                    abilityCheckPool = attemptAbilityCheckPool;
                    abilityItemPool = attemptAbilityItemPool;
                } else {
                    SPDLOG_WARN("Glitchless generation attempt {} of {} failed (entrance layout {})", attempt,
                                maxAttempts, layout);
                }
            }
        }

        if (!generated) {
            Rando::EntranceShuffle::ResetLayout();
            return false;
        }
    } else if (RANDO_SAVE_OPTIONS[RO_LOGIC].optionValue == RO_LOGIC_NO_LOGIC) {
        if (shuffleEntrances) {
            Rando::EntranceShuffle::Shuffle(entranceRng);
        }
        Rando::Logic::GenerateNoLogicPool(itemPool, abilityItemPool);
    }

    Rando::EntranceShuffle::GetLayout(saveData->shipSaveData.randoSaveData.randoSaveEntrances);

    if (checkPool.size() != itemPool.size()) {
        return false;
    }

    for (int i = 0; i < checkPool.size(); i++) {
        RandoSaveCheck randoShuffleEntry = {
            .name = Rando::StaticData::Checks[checkPool[i]].name,
            .randoCheckId = checkPool[i],
            .randoItemId = Rando::StaticData::GetRandoItemByActorId(std::get<0>(itemPool[i])),
            .shuffledCheckId = std::get<2>(itemPool[i]),
            .randoCollectionId = std::get<1>(itemPool[i]),
            .isShuffled = checkPool[i] == RC_UNKNOWN ? false : true,
            .obtained = false,
            .skipped = false,
        };

        shuffledPool.push_back(randoShuffleEntry);

        RANDO_SAVE_CHECKS[checkPool[i]] = randoShuffleEntry;
    }

    if (!abilityCheckPool.empty()) {
        for (int a = 0; a < abilityCheckPool.size(); a++) {
            RandoSaveCheck randoShuffleEntry = {
                .name = Rando::StaticData::Checks[abilityCheckPool[a]].name,
                .randoCheckId = abilityCheckPool[a],
                .randoItemId = Rando::StaticData::GetRandoItemByActorId(std::get<0>(abilityItemPool[a])),
                .shuffledCheckId = std::get<2>(abilityItemPool[a]),
                .randoCollectionId = std::get<1>(abilityItemPool[a]),
                .isShuffled = true,
                .obtained = false,
                .skipped = false,
            };

            shuffledPool.push_back(randoShuffleEntry);
            RANDO_SAVE_CHECKS[abilityCheckPool[a]] = randoShuffleEntry;
        }
    }

    for (auto& [optionId, optionValue] : Rando::StaticData::Options) {
        RANDO_SAVE_OPTIONS[optionId].name = optionValue.name;
        RANDO_SAVE_OPTIONS[optionId].optionValue = CVarGetInteger(optionValue.cvar, optionValue.defaultValue);
    }

    saveData->shipSaveData.randoSaveData.seedId = randoFinalSeed;
    return true;
}

void GeneratePoolFromSaveData(SaveData* saveData) {
    Rando::EntranceShuffle::SetLayout(saveData->shipSaveData.randoSaveData.randoSaveEntrances);

    for (int i = RC_UNKNOWN; i < RC_MAX; i++) {
        RandoSaveCheck randoSaveCheck = saveData->shipSaveData.randoSaveData.randoSaveCheck[i];

        if (randoSaveCheck.isShuffled == false) {
            continue;
        }

        RandoSaveCheck shuffledObject = {
            .name = Rando::StaticData::Checks[(RandoCheckId)i].name,
            .randoCheckId = randoSaveCheck.randoCheckId,
            .randoItemId = randoSaveCheck.randoItemId,
            .shuffledCheckId = randoSaveCheck.shuffledCheckId,
            .randoCollectionId = randoSaveCheck.randoCollectionId,
            .isShuffled = randoSaveCheck.isShuffled,
            .obtained = randoSaveCheck.obtained,
            .skipped = randoSaveCheck.skipped,
        };

        Rando::Logic::shuffledPool.push_back(shuffledObject);
    }
}

} // namespace Logic

} // namespace Rando