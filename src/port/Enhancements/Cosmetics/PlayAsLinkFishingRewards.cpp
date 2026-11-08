#include "PlayAsLinkFishingRewards.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <ultra64.h>

#include "port/Enhancements/Retention/Retention.h"
#include "port/Rando/Rando.h"
#include "port/Romhack/RomhackConfig.h"

extern "C" {
#include "functions.h"
#include "enums.h"

f32* bundle_getVelocity(Actor* actor);
}

namespace {

constexpr float kPi = 3.14159265358979f;

constexpr int kMaxEggs = 10;
// Eggs that stand in for notes there was no room for
constexpr int kMaxEggsWithSubstitutes = 15;

constexpr int kTrophyPounds = 10;
constexpr int kBigTrophyPounds = 15;
constexpr int kTrophyNotes = 2;
constexpr int kBigTrophyNotes = 5;
constexpr int kLoachNotes = 5;

// Where they leave Link, and how: up and out, like the eggs a Yum-Yum is made to drop
constexpr float kPopHeight = 95.0f;
constexpr float kPopAhead = 20.0f;
constexpr float kMinSpeed = 130.0f;
constexpr float kMaxSpeed = 330.0f;
constexpr float kMinLift = 560.0f;
constexpr float kMaxLift = 820.0f;
// They scatter over this many degrees to either side of straight behind Link
constexpr float kFan = 85.0f;

float Random(float low, float high) {
    return low + (high - low) * ((float)rand() / (float)RAND_MAX);
}

// The `index`th of `count` pickups: a velocity fanned out behind Link, a little differently every time
void Velocity(int index, int count, float yaw, float out[3]) {
    const float share = count > 1 ? (float)index / (float)(count - 1) : 0.5f;
    const float angle = (yaw + 180.0f + (share * 2.0f - 1.0f) * kFan + Random(-12.0f, 12.0f)) * kPi / 180.0f;
    const float speed = Random(kMinSpeed, kMaxSpeed);
    out[0] = std::sin(angle) * speed;
    out[1] = Random(kMinLift, kMaxLift);
    out[2] = std::cos(angle) * speed;
}

} // namespace

namespace PlayAsLinkFishingRewards {

int EggsFor(int pounds) {
    return std::clamp((pounds + 1) / 2, 1, kMaxEggs);
}

int NotesFor(int pounds, bool loach) {
    if (IS_RANDO) {
        return 0;
    }
    int notes = 0;
    if (pounds >= kBigTrophyPounds) {
        notes = kBigTrophyNotes;
    } else if (pounds >= kTrophyPounds) {
        notes = kTrophyNotes;
    }
    return loach ? std::max(notes, kLoachNotes) : notes;
}

Given Give(int pounds, bool loach, const float link[3], float yaw) {
    Given given;
    int wantedNotes = NotesFor(pounds, loach);
    // Never more than the level has room for, even if its counter isn't the usual 100
    int notesMax = port_getRomhackNotesMax();
    if (notesMax < 0) {
        notesMax = 100;
    }
    wantedNotes = std::min(wantedNotes, std::max(0, notesMax - item_getCount(ITEM_C_NOTE)));

    const float forward = yaw * kPi / 180.0f;
    const float origin[3] = { link[0] + std::sin(forward) * kPopAhead, link[1] + kPopHeight,
                              link[2] + std::cos(forward) * kPopAhead };

    const int total = wantedNotes + EggsFor(pounds);
    int index = 0;
    for (int i = 0; i < wantedNotes; i++) {
        float velocity[3];
        Velocity(index, total, yaw, velocity);
        if (port_noteRetention_throwLiveNote(origin, velocity)) {
            given.notes++;
            index++;
        }
    }

    int eggs = EggsFor(pounds) + (wantedNotes - given.notes);
    eggs = std::min(eggs, kMaxEggsWithSubstitutes);
    for (int i = 0; i < eggs; i++) {
        float velocity[3];
        Velocity(index + i, given.notes + eggs, yaw, velocity);
        float position[3] = { origin[0], origin[1], origin[2] };
        bundle_setYaw(0.0f);
        Actor* egg = bundle_spawn_f32(BUNDLE_E_YUMYUM_BLUE_EGG, position);
        if (egg == nullptr) {
            continue;
        }
        f32* thrown = bundle_getVelocity(egg);
        thrown[0] = velocity[0];
        thrown[1] = velocity[1];
        thrown[2] = velocity[2];
        given.eggs++;
    }
    return given;
}

} // namespace PlayAsLinkFishingRewards
