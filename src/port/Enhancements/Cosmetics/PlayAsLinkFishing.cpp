#include "PlayAsLinkFishing.h"
#include "PlayAsLink.h"
#include "PlayAsLinkFish.h"
#include "PlayAsLinkFishingCamera.h"
#include "PlayAsLinkFishingCatch.h"
#include "PlayAsLinkFishingRewards.h"
#include "PlayAsLinkUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>

#include <libultraship/libultraship.h>
#include <fast/ucodehandlers.h>

#include "port/Interpolation/FrameInterpolation.h"
#include "port/UI/Notification.h"

extern "C" {
#include "functions.h"
#include "bk_time.h"
#include "core1/joy.h"
#include "core2/gc/transition.h"
#include "core2/yaw.h"

void core1_7F60_guMtxF2L(float mf[4][4], Mtx* m);
BKCollisionTriangle* mapModel_intersectLine(f32 start[3], f32 end[3], f32 normal[3], u32 filter);
}

using namespace PlayAsLinkUtil;

// The rod, lure and line are drawn the way OoT's Fishing actor draws them (z_fishing.c: Fishing_DrawRod,
// Fishing_DrawLureAndLine, Fishing_DrawEffects) and the cast, flight and reeling follow Fishing_UpdateLure, with the
// numbers turned into Banjo-Kazooie's units and the pond into whatever water is in front of the player.
namespace {

// Banjo-Kazooie units in one OoT world unit: Link's model is about 6000 tall, which is 156 here (kLinkScale in
// PlayAsLink.cpp) and about 60 world units there.
constexpr float kWorldScale = 2.6f;

constexpr float kPi = 3.14159265358979f;

// Animation frames (30 per second) in gPlayerAnim_link_fishing_throw: the rod is raised behind the head at the wind
// up, and the arm is swinging forward at the release
constexpr float kWindUpFrame = 16.0f;
constexpr float kReleaseFrame = 26.0f;
constexpr float kThrowLastFrame = 49.0f;
constexpr float kThrowSpeed = 2.5f; // the throw plays faster once B is let go: OoT waits almost a second to cast

// B pressed and let go within this time puts the rod away; holding it longer winds up the cast
constexpr float kTapSeconds = 0.2f;
constexpr float kFullChargeSeconds = 1.0f;

// The lure's flight. OoT throws it at 25 world units a tick with 15 up and a gravity of 1 (20 ticks a second), so
// the distance depends on the horizontal speed only. Here the speed follows how long B was held.
constexpr float kMinCastSpeed = 230.0f;
constexpr float kMaxCastSpeed = 850.0f;
constexpr float kCastUpSpeed = 780.0f;
constexpr float kGravity = 1040.0f;
constexpr float kMaxFlightSeconds = 6.0f;
// Holding A in the air slows the lure's travel (OoT multiplies its horizontal speed by 0.9 each tick)
constexpr float kAirReelDrag = 2.1f;

// Reeling in, units per second
constexpr float kReelStartSpeed = 120.0f;
constexpr float kReelMaxSpeed = 300.0f;
constexpr float kReelAcceleration = 300.0f;
constexpr float kDragSpeed = 220.0f;  // the stick's pull on the lure
constexpr float kLandedDelay = 0.35f; // before the lure can be reeled, like OoT's sLureMoveDelay
constexpr float kLureHomeDistance = 70.0f;
constexpr float kLureReturnSeconds = 0.25f;

constexpr float kLureHang = 27.0f;   // below the rod tip when it is reeled in
constexpr float kTurnSpeed = 150.0f; // degrees a second at full stick

// What counts as water in front of the player: a surface this far ahead, not higher than his head and not far below
constexpr float kProbeStart = 140.0f;
constexpr float kProbeEnd = 1000.0f;
constexpr float kProbeStep = 70.0f;

// mapModel_intersectLine skips triangles that have any of these flags: the first leaves only water surfaces (see
// mapModel_getWaterSurfaceY), the second only ground and walls (mapModel_getFloorY)
constexpr u32 kWaterFilter = 0xF800FF0F;
constexpr u32 kSolidFilter = 0x1E0000;

constexpr int kRippleCount = 16;
constexpr float kTickSeconds = 0.05f; // OoT updates 20 times a second

constexpr uint32_t kRodMaterial = 0x113D0;                          // gFishingRodMaterialDL
constexpr uint32_t kRodSegment = 0x11410;                           // gFishingRodSegmentDL
constexpr uint32_t kLureHook = 0x12160;                             // gFishingLureHookDL
constexpr uint32_t kLureFloat = 0x121F0;                            // gFishingLureFloatDL
constexpr uint32_t kLineModel = 0x3710;                             // gFishingLineModelDL
constexpr uint32_t kRippleMaterial = 0x8610;                        // gFishingRippleMaterialDL
constexpr uint32_t kRippleModel = 0x8678;                           // gFishingRippleModelDL
constexpr uint32_t kRodTextures[3] = { 0x11170, 0x11270, 0x11070 }; // black, white, strip (gFishingRodSegment*Tex)

// sRodScales and sRodBendRatios
constexpr float kRodScales[22] = {
    1.0f,        1.0f,        1.0f,        0.9625f,     0.925f, 0.8875f,     0.85f,       0.8125f,
    0.775f,      0.73749995f, 0.7f,        0.6625f,     0.625f, 0.5875f,     0.54999995f, 0.5125f,
    0.47499996f, 0.4375f,     0.39999998f, 0.36249995f, 0.325f, 0.28749996f,
};
constexpr float kRodBendRatios[22] = {
    0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.06f,   0.12f,   0.18f,   0.24f,   0.30f,   0.36f,
    0.42f, 0.48f, 0.54f, 0.60f, 0.60f, 0.5142f, 0.4285f, 0.3428f, 0.2571f, 0.1714f, 0.0857f,
};

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// Moves `value` toward `target` the way a smoothing filter with the given rate (per second) does, at any frame rate
void Approach(float& value, float target, float rate, float dt) {
    value += (target - value) * (1.0f - std::exp(-rate * dt));
}

// A matrix whose z axis runs from `from` to `to` (the line's segments are one unit of x wide and 1000 long in
// z), with its x axis kept level where it can be. `roll` turns it around that axis.
MtxF MtxAlong(const float from[3], const float to[3], float width, float roll90) {
    float z[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float length = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    MtxF m = MtxAt(from);
    if (length < 0.0001f) {
        return m;
    }
    for (float& c : z) {
        c /= length;
    }
    // x = up x z, y = z x x
    float x[3] = { z[2], 0.0f, -z[0] };
    float xLength = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xLength < 0.001f) {
        x[0] = 1.0f, x[1] = 0.0f, x[2] = 0.0f;
        xLength = 1.0f;
    }
    x[0] /= xLength, x[1] /= xLength, x[2] /= xLength;
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    // The second quad of a pair lies a quarter turn around the segment
    const float* xAxis = roll90 != 0.0f ? y : x;
    const float yAxis[3] = { roll90 != 0.0f ? -x[0] : y[0], roll90 != 0.0f ? -x[1] : y[1],
                             roll90 != 0.0f ? -x[2] : y[2] };
    for (int k = 0; k < 3; k++) {
        m.mf[0][k] = xAxis[k] * width;
        m.mf[1][k] = yAxis[k];
        m.mf[2][k] = z[k] * (length / 1000.0f);
    }
    return m;
}

// ---- The converted model ----

enum class Phase {
    Off,
    Ready,    // the lure hangs at the rod's tip
    Charging, // B is held for the cast
    Casting,  // B was let go; the throw plays, then the lure flies
    Flying,
    Landed,   // the lure rests on the water (or ground), and can be reeled
    Hooked,   // a fish has the lure: it bites, the hook is set, and it is fought and reeled in
    Catching, // the fish is in Link's hands, then goes back into the water
    Returning // reeled all the way: the lure goes up to the rod's tip
};

struct Ripple {
    bool active = false;
    float pos[3] = {};
    float scale = 0.0f;
    float scaleTarget = 0.0f;
    float scaleStep = 0.0f;
    float alpha = 0.0f;
    float alphaMax = 0.0f;
    bool fadingIn = false;
};

struct State {
    Phase phase = Phase::Off;
    // Controller, from the last frame
    bool aHeld = false;
    bool bHeld = false;
    bool downHeld = false;
    float bTime = 0.0f;
    bool bArmed = false; // B was pressed while the lure was in

    // Link
    float throwFrame = 0.0f;
    bool throwPlaying = false;
    float leanX = 0.0f; // the smoothed stick, from -1 to 1 (unk_858 and unk_85C of Player)
    float leanY = 0.0f;
    float leanXPrev = 0.0f;
    float leanYPrev = 0.0f;
    float bodyLean = 0.0f; // the way Link's body tips with the fish's pull, in 16-bit angles (shape.rot.x)
    float prevStickX = 0.0f;
    float prevStickY = 0.0f;

    // The rod
    float bendY = 0.0f;     // sRodBendRotY
    float bendX = 0.0f;     // D_80B7A6AC
    float flick = 0.0f;     // D_80B7A6B8: the whip just before the cast
    float rodTilt = 0.0f;   // D_80B7A6C0: the rod is raised against a fish's pull
    float fightBend = 0.0f; // D_80B7A6BC: and bent by it
    int hitTicks = 0;       // sRodHitTimer: the hard tug of setting the hook
    float rodTickTime = 0.0f;
    int rodTicks = 0;
    float idleTime = 0.0f;
    float tip[3] = {}; // the rod's tip in the world, from the last draw
    bool tipValid = false;

    // The lure
    float lure[3] = {};
    float velocity[3] = {};
    float charge = 0.0f; // seconds B was held past the tap
    float flightTime = 0.0f;
    bool onWater = false;
    float surfaceY = 0.0f;
    float landedTime = 0.0f;
    float reelSpeed = 0.0f;
    float lureYaw = 0.0f; // radians, around Y
    float lurePitch = -kPi / 2.0f;
    float bob = 0.0f;
    float sag = 0.0f;
    float returnTime = 0.0f;
    float returnFrom[3] = {};
    float wigglePos = 0.0f;   // sLureWigglePosY: the lure still jumping from a twitch
    float wiggleDelay = 0.0f; // D_80B7E150
    float wiggleSign = 1.0f;
    int wiggleLevel = 0; // 1 or 2 on the frame the lure is twitched, for the fish

    // The line's strain from a fish that pulls away, 0 to 1 (snaps at 1)
    float tension = 0.0f;
    float shakeTime = 0.0f;    // runs while the rod strains, for its shudder
    float creakTimer = 0.0f;   // until the line creaks again
    bool strainWarned = false; // the player was told the line is straining, for this stretch of high tension

    // A fish on the lure
    float catchTime = 0.0f;
    float releaseTime = 0.0f;   // when the fish was let go, in catchTime
    bool catchReleased = false; // the fish has been let go; Link's arms come down
    bool catchCounted = false;  // the records and the rewards have been dealt with
    bool catchTextShown = false;
    float catchBoxWait = 0.0f;      // how long the dialog box has been refused
    bool catchBoxFallback = false;  // it never opened: the catch was announced with notifications
    float catchFallbackTime = 0.0f; // when, in catchTime
    PlayAsLinkFishingCatch::Catch catchInfo;
    float catchLength = 0.0f;
    bool catchLoach = false;
    float mouth[3] = {}; // where the fish's mouth was last, in the world

    Ripple ripples[kRippleCount];
    float rippleTick = 0.0f;
    float idleRipple = 0.0f;
};

} // namespace

struct PlayAsLinkFishing::Fishing::Data {
    std::vector<uint8_t> fish;
    const uint8_t* keep = nullptr;
    size_t keepSize = 0;

    Gfx* rodMaterial = nullptr;
    Gfx* rodSegment = nullptr;
    Gfx* lureHook = nullptr;
    Gfx* lureFloat = nullptr;
    Gfx* lineModel = nullptr;
    Gfx* rippleMaterial = nullptr;
    Gfx* rippleModel = nullptr;

    State state;
    std::unique_ptr<PlayAsLinkFish::School> school; // null if the fish couldn't be read

    // One native array per (address, count) a list loads: loads can overlap and some arrays are only 8-byte
    // aligned
    std::deque<std::vector<Vtx>> vertexArrays;
    std::unordered_map<uint64_t, Vtx*> convertedVertices;
    std::deque<std::vector<Gfx>> listStorage;
    std::unordered_map<uint32_t, Gfx*> converted;

    const uint8_t* SegmentData(uint8_t segment, uint32_t offset, size_t& size) const;
    Vtx* ConvertVertices(uint32_t address, uint32_t count);
    Gfx* ConvertList(uint32_t address, int depth);
};

PlayAsLinkFishing::Fishing::Fishing() : data(std::make_unique<Data>()) {
}

PlayAsLinkFishing::Fishing::~Fishing() = default;

namespace PlayAsLinkFishing {

// Segment 6 is object_fish and segment 4 gameplay_keep, where the ripple's texture and vertices are
const uint8_t* Fishing::Data::SegmentData(uint8_t segment, uint32_t offset, size_t& size) const {
    if (segment == 6) {
        size = fish.size();
        return fish.data();
    }
    if (segment == 4) {
        size = keepSize;
        return keep;
    }
    size = 0;
    return nullptr;
}

// N64 vertices are big-endian with 16-bit positions; the renderer's are native with float positions.
Vtx* Fishing::Data::ConvertVertices(uint32_t address, uint32_t count) {
    size_t size;
    const uint8_t* data = SegmentData(address >> 24, address & 0xFFFFFF, size);
    const uint32_t offset = address & 0xFFFFFF;
    if (data == nullptr || count == 0 || offset + (size_t)count * 16 > size) {
        return nullptr;
    }
    const uint64_t key = ((uint64_t)address << 8) | count;
    if (auto it = convertedVertices.find(key); it != convertedVertices.end()) {
        return it->second;
    }
    std::vector<Vtx> out(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* src = &data[offset + i * 16];
        Vtx& vtx = out[i];
        std::memset(&vtx, 0, sizeof(vtx));
        vtx.v.ob[0] = (int16_t)Be16(src + 0);
        vtx.v.ob[1] = (int16_t)Be16(src + 2);
        vtx.v.ob[2] = (int16_t)Be16(src + 4);
        vtx.v.flag = Be16(src + 6);
        vtx.v.tc[0] = (int16_t)Be16(src + 8);
        vtx.v.tc[1] = (int16_t)Be16(src + 10);
        std::memcpy(vtx.v.cn, src + 12, 4);
    }
    vertexArrays.push_back(std::move(out));
    Vtx* converted = vertexArrays.back().data();
    convertedVertices[key] = converted;
    return converted;
}

// The lists stay F3DEX2 commands (they're drawn with that microcode), widened to native commands with their
// addresses resolved: object and gameplay_keep data become pointers.
Gfx* Fishing::Data::ConvertList(uint32_t address, int depth) {
    if (auto it = converted.find(address); it != converted.end()) {
        return it->second;
    }
    size_t size;
    const uint8_t* data = SegmentData(address >> 24, address & 0xFFFFFF, size);
    if (data == nullptr || (address & 7) != 0 || depth > 8) {
        return nullptr;
    }

    std::vector<Gfx> out;
    for (size_t offset = address & 0xFFFFFF; offset + 8 <= size; offset += 8) {
        const uint32_t w0 = Be32(&data[offset]);
        const uint32_t w1 = Be32(&data[offset + 4]);
        const uint8_t op = w0 >> 24;
        const uint8_t refSegment = w1 >> 24;
        uintptr_t nativeW1 = w1;
        bool end = false;

        switch (op) {
            case 0x01: // G_VTX
                nativeW1 = (uintptr_t)ConvertVertices(w1, (w0 >> 12) & 0xFF);
                if (nativeW1 == 0) {
                    return nullptr;
                }
                break;
            case 0xDA: // G_MTX
                nativeW1 = SegmentRef(w1);
                break;
            case 0xDE: // G_DL
                if (refSegment == 4 || refSegment == 6) {
                    nativeW1 = (uintptr_t)ConvertList(w1, depth + 1);
                    if (nativeW1 == 0) {
                        return nullptr;
                    }
                } else {
                    nativeW1 = SegmentRef(w1);
                }
                end = ((w0 >> 16) & 0xFF) == 1; // G_DL_NOPUSH: a branch
                break;
            case 0xFD: { // G_SETTIMG
                if (refSegment == 4 || refSegment == 6) {
                    size_t imageSize;
                    const uint8_t* image = SegmentData(refSegment, w1 & 0xFFFFFF, imageSize);
                    if (image == nullptr || (w1 & 0xFFFFFF) >= imageSize) {
                        return nullptr;
                    }
                    nativeW1 = (uintptr_t)(image + (w1 & 0xFFFFFF));
                } else {
                    nativeW1 = SegmentRef(w1);
                }
                break;
            }
            case 0xDB:          // G_MOVEWORD
            case 0xDC:          // G_MOVEMEM
                return nullptr; // these lists don't use them
            case 0xDF:          // G_ENDDL
                end = true;
                break;
            default:
                break;
        }
        out.push_back(MakeGfx(w0, nativeW1));
        if (end) {
            listStorage.push_back(std::move(out));
            Gfx* list = listStorage.back().data();
            converted[address] = list;
            return list;
        }
    }
    return nullptr;
}

std::unique_ptr<Fishing> Build(std::vector<uint8_t> objectFish, const uint8_t* gameplayKeep, size_t gameplayKeepSize,
                               std::string& error) {
    if (objectFish.size() != kFileSize) {
        error = "The fishing pond's data has an unexpected size";
        return nullptr;
    }
    auto fishing = std::make_unique<Fishing>();
    Fishing::Data& data = *fishing->data;
    data.fish = std::move(objectFish);
    data.keep = gameplayKeep;
    data.keepSize = gameplayKeepSize;

    // gFishingRodSegmentDL starts by loading its six vertices; a file from another release won't
    if (Be32(&data.fish[kRodSegment]) != 0x0100600C || Be32(&data.fish[kRodSegment + 4]) != 0x06011370) {
        error = "Only the USA 1.0 release of Ocarina of Time is supported";
        return nullptr;
    }
    const struct {
        Gfx** list;
        uint32_t offset;
    } lists[] = {
        { &data.rodMaterial, kRodMaterial }, { &data.rodSegment, kRodSegment },
        { &data.lureHook, kLureHook },       { &data.lureFloat, kLureFloat },
        { &data.lineModel, kLineModel },     { &data.rippleMaterial, kRippleMaterial },
        { &data.rippleModel, kRippleModel },
    };
    for (const auto& entry : lists) {
        *entry.list = data.ConvertList(0x06000000 | entry.offset, 0);
        if (*entry.list == nullptr) {
            error = "The fishing rod couldn't be converted";
            return nullptr;
        }
    }

    // The fish are optional: without them the rod still casts
    std::string fishError;
    data.school = PlayAsLinkFish::Build(
        data.fish.data(), data.fish.size(), [&data](uint32_t address) { return data.ConvertList(address, 0); },
        fishError);
    if (data.school == nullptr) {
        SPDLOG_WARN("Play as Link: no fish ({})", fishError);
    }
    return fishing;
}

} // namespace PlayAsLinkFishing

namespace {

// ---- Controller and Banjo ----

struct Input {
    bool a, b, down;
    bool aPressed, bPressed, downPressed;
    float stickX, stickY;
};

// The controller as it is, not as Banjo's own input sees it, which is switched off while the rod is out
Input ReadInput(State& state) {
    s32 face[6];
    s32 dpad[4];
    f32 stick[2];
    controller_copyFaceButtons(0, face);
    controller_copyDpadButtons(0, dpad);
    controller_copyJoystick(0, stick);

    Input in;
    in.a = face[FACE_BUTTON(BUTTON_A)] != 0;
    in.b = face[FACE_BUTTON(BUTTON_B)] != 0;
    in.down = dpad[DPAD_BUTTON(BUTTON_D_DOWN)] != 0;
    in.aPressed = in.a && !state.aHeld;
    in.bPressed = in.b && !state.bHeld;
    in.downPressed = in.down && !state.downHeld;
    in.stickX = std::clamp(stick[0], -1.0f, 1.0f);
    in.stickY = std::clamp(stick[1], -1.0f, 1.0f);
    state.aHeld = in.a;
    state.bHeld = in.b;
    state.downHeld = in.down;
    return in;
}

// Banjo's buttons and stick are what every move of his reads (bakey and bastick): switching them off, as the game
// does for cutscene-like moments (func_8028F784), leaves him standing where he is without touching his state
// machine.
void LockBanjo(bool locked) {
    bakey_disableAll(locked ? 1 : 0);
    bastick_lockAtzero(locked);
}

// ---- The world ----

// A line against the map's collision: the first surface that doesn't have any of the filter's flags
bool Cast(const float from[3], const float to[3], u32 filter, float hit[3], float normal[3]) {
    f32 start[3] = { from[0], from[1], from[2] };
    f32 end[3] = { to[0], to[1], to[2] };
    f32 n[3] = { 0.0f, 1.0f, 0.0f };
    if (mapModel_intersectLine(start, end, n, filter) == nullptr) {
        return false;
    }
    if (hit != nullptr) {
        std::memcpy(hit, end, sizeof(end));
    }
    if (normal != nullptr) {
        std::memcpy(normal, n, sizeof(n));
    }
    return true;
}

// What's below a point: water (its surface), or else the ground. Water over ground is water; the lure floats on
// the surface.
bool SurfaceBelow(float x, float y, float z, float above, float depth, bool* isWater, float* surface, float* ground) {
    const float from[3] = { x, y + above, z };
    const float to[3] = { x, y - depth, z };
    float waterHit[3], groundHit[3];
    const bool water = Cast(from, to, kWaterFilter, waterHit, nullptr);
    const bool solid = Cast(from, to, kSolidFilter, groundHit, nullptr);
    if (ground != nullptr) {
        *ground = solid ? groundHit[1] : -100000.0f;
    }
    if (water && (!solid || waterHit[1] >= groundHit[1] - 0.5f)) {
        *isWater = true;
        *surface = waterHit[1];
        return true;
    }
    if (solid) {
        *isWater = false;
        *surface = groundHit[1];
        return true;
    }
    return false;
}

void PlayerPosition(float position[3]) {
    f32 p[3];
    playerPosition_get(p);
    position[0] = p[0], position[1] = p[1], position[2] = p[2];
}

// Water within casting range in front of the player, deep enough for a lure to float on. `at` gets the nearest of it.
bool WaterAhead(float at[3]) {
    float position[3];
    PlayerPosition(position);
    const float yaw = yaw_get() * kPi / 180.0f;
    for (float distance = kProbeStart; distance <= kProbeEnd; distance += kProbeStep) {
        const float x = position[0] + std::sin(yaw) * distance;
        const float z = position[2] + std::cos(yaw) * distance;
        bool isWater;
        float surface, ground;
        bool found = SurfaceBelow(x, position[1], z, 220.0f, 900.0f, &isWater, &surface, &ground);
        if (!found) {
            continue;
        }
        if (isWater && surface > position[1] - 700.0f && surface < position[1] + 120.0f && surface > ground + 12.0f) {
            at[0] = x, at[1] = surface, at[2] = z;
            return true;
        }
    }
    return false;
}

// ---- Fishing ----

float DistanceXZ(const float a[3], const float b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Fishing_SpawnRipple
void SpawnRipple(State& s, const float position[3], float scale, float rotX, float alpha) {
    for (Ripple& ripple : s.ripples) {
        if (ripple.active) {
            continue;
        }
        ripple.active = true;
        std::memcpy(ripple.pos, position, sizeof(ripple.pos));
        ripple.scale = scale * 0.0025f;
        ripple.scaleTarget = rotX * 0.0025f;
        if (scale > 300.0f) {
            ripple.alpha = 0.0f;
            ripple.alphaMax = alpha;
            ripple.fadingIn = true;
            ripple.scaleStep = (ripple.scaleTarget - ripple.scale) * 0.05f;
        } else {
            ripple.alpha = alpha;
            ripple.fadingIn = false;
            ripple.scaleStep = (ripple.scaleTarget - ripple.scale) * 0.1f;
        }
        return;
    }
}

// Fishing_UpdateEffects' ripples, a tick at a time
void TickRipples(State& s) {
    for (Ripple& ripple : s.ripples) {
        if (!ripple.active) {
            continue;
        }
        // Math_ApproachF(&scale, target, 0.2, step)
        const float step = std::abs(ripple.scaleStep);
        ripple.scale += std::clamp((ripple.scaleTarget - ripple.scale) * 0.2f, -step, step);
        if (ripple.fadingIn) {
            ripple.alpha += 20.0f;
            if (ripple.alpha >= ripple.alphaMax) {
                ripple.alpha = ripple.alphaMax;
                ripple.fadingIn = false;
            }
        } else {
            ripple.alpha -= 8.0f;
            if (ripple.alpha <= 0.0f) {
                ripple.active = false;
            }
        }
    }
}

// What the fish need of the map, and a place for the ripples they make
class Pond : public PlayAsLinkFish::World {
public:
    explicit Pond(State& state) : mState(state) {
    }

    bool WaterAt(float x, float z, float refY, float* surface, float* ground) override {
        bool isWater;
        float waterSurface, floor;
        if (!SurfaceBelow(x, refY, z, 220.0f, 1200.0f, &isWater, &waterSurface, &floor) || !isWater ||
            floor < waterSurface - 3000.0f) {
            return false;
        }
        *surface = waterSurface;
        *ground = floor;
        return true;
    }

    void Ripple(const float position[3], float scale, float target, float alpha) override {
        SpawnRipple(mState, position, scale, target, alpha);
    }

private:
    State& mState;
};

void Release(PlayAsLinkFishing::Fishing::Data& data) {
    State& s = data.state;
    if (s.phase != Phase::Off) {
        LockBanjo(false);
    }
    PlayAsLinkFishingCamera::End();
    PlayAsLinkFishingCatch::Abandon();
    if (data.school != nullptr) {
        PlayAsLinkFish::Clear(*data.school);
    }
    // The buttons are still held, and must not count as pressed again
    State fresh;
    fresh.aHeld = s.aHeld;
    fresh.bHeld = s.bHeld;
    fresh.downHeld = s.downHeld;
    s = fresh;
}

// The lure hangs below the rod's tip (Ready) or is on its way there
void HangLure(State& s, float out[3]) {
    out[0] = s.tip[0];
    out[1] = s.tip[1] - kLureHang;
    out[2] = s.tip[2];
}

void Launch(State& s) {
    float player[3];
    PlayerPosition(player);
    const float yaw = yaw_get() * kPi / 180.0f;
    const float fx = std::sin(yaw);
    const float fz = std::cos(yaw);
    const float t = Clamp01(s.charge / (kFullChargeSeconds - kTapSeconds));
    const float speed = kMinCastSpeed + (kMaxCastSpeed - kMinCastSpeed) * t;

    if (s.tipValid) {
        std::memcpy(s.lure, s.tip, sizeof(s.lure));
    } else {
        s.lure[0] = player[0], s.lure[1] = player[1] + 150.0f, s.lure[2] = player[2];
    }
    s.lure[0] += fx * 12.0f;
    s.lure[2] += fz * 12.0f;
    s.velocity[0] = fx * speed;
    s.velocity[1] = kCastUpSpeed;
    s.velocity[2] = fz * speed;
    s.flightTime = 0.0f;
    s.phase = Phase::Flying;
}

void Land(State& s, const float position[3], bool water) {
    std::memcpy(s.lure, position, sizeof(s.lure));
    s.onWater = water;
    s.surfaceY = position[1];
    s.velocity[0] = s.velocity[1] = s.velocity[2] = 0.0f;
    s.landedTime = 0.0f;
    s.reelSpeed = kReelStartSpeed;
    s.bob = 1.0f;
    s.phase = Phase::Landed;
    if (water) {
        SpawnRipple(s, position, 100.0f, 800.0f, 150.0f);
    }
}

// The lure in the air (Fishing_UpdateLure case 1): a throw under gravity that stops at the first water or ground
// it meets, and at walls
void UpdateFlight(State& s, const Input& in, float dt) {
    s.flightTime += dt;
    const float steps = std::ceil(dt / (1.0f / 60.0f));
    const float h = dt / steps;
    for (int i = 0; i < (int)steps; i++) {
        float previous[3] = { s.lure[0], s.lure[1], s.lure[2] };
        if (in.a) {
            const float drag = std::exp(-kAirReelDrag * h);
            s.velocity[0] *= drag;
            s.velocity[2] *= drag;
        }
        s.velocity[1] -= kGravity * h;
        for (int k = 0; k < 3; k++) {
            s.lure[k] += s.velocity[k] * h;
        }

        float waterHit[3], solidHit[3], normal[3];
        const bool water = s.velocity[1] < 0.0f && Cast(previous, s.lure, kWaterFilter, waterHit, nullptr);
        const bool solid = Cast(previous, s.lure, kSolidFilter, solidHit, normal);
        if (water && (!solid || waterHit[1] >= solidHit[1])) {
            Land(s, waterHit, true);
            return;
        }
        if (solid) {
            if (normal[1] > 0.5f) {
                Land(s, solidHit, false);
                return;
            }
            // A wall or ceiling stops the lure's travel; it drops from there
            for (int k = 0; k < 3; k++) {
                s.lure[k] = solidHit[k] + normal[k] * 2.0f;
            }
            s.velocity[0] = s.velocity[2] = 0.0f;
            if (normal[1] < -0.5f && s.velocity[1] > 0.0f) {
                s.velocity[1] = 0.0f;
            }
        }
    }
    // A lure that never comes down (the map has no floor there) is reeled back
    if (s.flightTime > kMaxFlightSeconds || s.lure[1] < -10000.0f) {
        s.returnTime = 0.0f;
        std::memcpy(s.returnFrom, s.lure, sizeof(s.returnFrom));
        s.phase = Phase::Returning;
    }
}

// The lure on the water, or on the ground (Fishing_UpdateLure case 3): A pulls it toward Link, the stick drags it
void UpdateLanded(State& s, const Input& in, float dt) {
    float player[3];
    PlayerPosition(player);
    s.landedTime += dt;
    s.bob = std::max(0.0f, s.bob - dt * 0.7f);

    const bool reeling = in.a && s.landedTime >= kLandedDelay;

    // Twitching the lure with a flick of the stick or B (Fishing_UpdateLure case 3) draws the fish's eye. Reeling
    // uses the stick to steer the lure instead.
    s.wiggleDelay = std::max(0.0f, s.wiggleDelay - dt);
    s.wigglePos = std::max(0.0f, s.wigglePos - 6.0f * dt); // sLureWigglePosY falls 0.3 a tick
    if (s.onWater && !reeling && s.wiggleDelay <= 0.0f && s.landedTime >= kLandedDelay) {
        const float x = in.stickX * 80.0f, y = in.stickY * 80.0f; // OoT's stick range
        float wiggle = 0.0f;
        if (std::abs(x) > 30.0f) {
            wiggle = std::abs(x - s.prevStickX * 80.0f) / 60.0f;
        } else if (std::abs(y) > 30.0f) {
            wiggle = std::abs(y - s.prevStickY * 80.0f) / 60.0f;
        }
        wiggle = std::min(wiggle, 1.0f);
        if (in.bPressed) {
            wiggle = 0.5f;
        }
        if (wiggle > 0.2f && s.wigglePos < 4.0f) {
            s.wiggleDelay = 5.0f * kTickSeconds;
            s.wiggleLevel = wiggle > 0.8f ? 2 : 1;
            s.wigglePos = wiggle * 6.0f;
            s.wiggleSign = -s.wiggleSign;
            s.lureYaw += wiggle * s.wiggleSign * 0.6f;
            s.bob = std::max(s.bob, 0.6f);
            float at[3] = { s.lure[0], s.surfaceY, s.lure[2] };
            SpawnRipple(s, at, 30.0f, 300.0f, 150.0f);
        }
    }
    // The direction from the lure to Link, and the one across it
    float toLink[2] = { player[0] - s.lure[0], player[2] - s.lure[2] };
    const float distance = std::sqrt(toLink[0] * toLink[0] + toLink[1] * toLink[1]);
    if (distance > 0.001f) {
        toLink[0] /= distance, toLink[1] /= distance;
    }
    // The stick is Link's: right is his right as he faces the lure's way
    const float yaw = yaw_get() * kPi / 180.0f;
    const float rightX = -std::cos(yaw);
    const float rightZ = std::sin(yaw);
    const float forwardX = std::sin(yaw);
    const float forwardZ = std::cos(yaw);

    if (reeling) {
        s.reelSpeed = std::min(kReelMaxSpeed, s.reelSpeed + kReelAcceleration * dt);
    } else {
        s.reelSpeed = kReelStartSpeed;
    }
    float moveX = 0.0f;
    float moveZ = 0.0f;
    if (reeling) {
        // Reeling pulls toward Link, and the stick drags the lure across the water or pushes it back out
        moveX = toLink[0] * s.reelSpeed + rightX * in.stickX * kDragSpeed + forwardX * in.stickY * kDragSpeed * 0.6f;
        moveZ = toLink[1] * s.reelSpeed + rightZ * in.stickX * kDragSpeed + forwardZ * in.stickY * kDragSpeed * 0.6f;
    }
    s.lure[0] += moveX * dt;
    s.lure[2] += moveZ * dt;
    // A twitch jerks the lure toward Link a little
    if (s.wigglePos > 0.0f) {
        const float jerk = s.wigglePos * kWorldScale * 0.25f * (dt / kTickSeconds);
        s.lure[0] += toLink[0] * jerk;
        s.lure[2] += toLink[1] * jerk;
    }

    // Follow the surface under the lure's new place: the water ends at the shore
    bool isWater;
    float surface, ground;
    if (SurfaceBelow(s.lure[0], s.surfaceY, s.lure[2], 150.0f, 700.0f, &isWater, &surface, &ground)) {
        s.onWater = isWater;
        s.surfaceY = surface;
    }
    s.lure[1] = s.surfaceY + (s.onWater ? std::sin(s.landedTime * 9.0f) * 0.8f * s.bob : 0.0f);

    // Turn the lure to face the way it is pulled (the line's end toward Link)
    const float heading = std::atan2(toLink[0], toLink[1]);
    float turn = heading - s.lureYaw;
    while (turn > kPi) {
        turn -= 2.0f * kPi;
    }
    while (turn < -kPi) {
        turn += 2.0f * kPi;
    }
    s.lureYaw += turn * (1.0f - std::exp(-8.0f * dt));
    // Nose up on the water; flat while it is reeled (OoT: -pi / 6 plus a wobble, then 0)
    Approach(s.lurePitch, reeling ? 0.0f : -kPi / 6.0f + std::sin(s.landedTime * 4.0f) * 0.08f * s.bob, 6.0f, dt);

    // Ripples: while reeling every few ticks, while it sits about every three seconds
    if (s.onWater) {
        s.idleRipple += dt;
        const float every = reeling ? 0.2f : 3.2f;
        if (s.idleRipple >= every) {
            s.idleRipple = 0.0f;
            const float at[3] = { s.lure[0], s.surfaceY, s.lure[2] };
            SpawnRipple(s, at, 30.0f, 300.0f, 150.0f);
        }
    }

    if (DistanceXZ(s.lure, player) < kLureHomeDistance) {
        s.returnTime = 0.0f;
        std::memcpy(s.returnFrom, s.lure, sizeof(s.returnFrom));
        s.phase = Phase::Returning;
    }
}

// How long Link holds the fish up before the catch is announced
constexpr float kCatchTextSeconds = 0.5f;
// If the game won't open its dialog box for this long, the catch is announced with notifications instead, and Link
// holds the fish up for kCatchHoldSeconds before it goes back
constexpr float kCatchBoxGiveUpSeconds = 3.0f;
constexpr float kCatchHoldSeconds = 3.4f;
constexpr float kCatchEndSeconds = 0.7f; // the arms coming down

// The strain on the line shows as the rod bending harder and shuddering, the line reddening, and a creak
constexpr float kStrainBend = 0.6f;        // extra rod bend at the line's limit
constexpr float kStrainShudder = 0.14f;    // how far the rod shakes at the limit, in the bend's units
constexpr float kStrainBodyShake = 0.035f; // radians Link's body shakes at the limit
constexpr float kCreakFrom = 0.45f;        // the line creaks from this much tension
constexpr float kWarnFrom = 0.7f;          // the line is straining, for the player's benefit
constexpr float kCreakSeconds = 0.5f;      // between creaks at the limit
constexpr float kCameraAimAhead = 450.0f;  // where the camera looks while the lure is still in Link's hand

// OoT's weight for a fish (z_fishing.c: SQ(length) * 0.0036 + 0.5); a Hylian Loach counts twice its length
int FishWeight(float length, bool loach) {
    const int counted = (int)(loach ? 2.0f * length : length);
    return (int)((float)(counted * counted) * 0.0036f + 0.5f);
}

// What the catch box says, as notifications: for when the game can't open its dialog box
void AnnounceCatchFallback(const PlayAsLinkFishingCatch::Catch& caught) {
    Notification::Emit({ .prefix = caught.loach ? "Caught a Hylian Loach!" : "Caught a fish!",
                         .prefixColor = ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                         .message = "It weighs " + std::to_string(caught.pounds) +
                                    (caught.pounds == 1 ? " pound." : " pounds.") +
                                    (caught.newRecord ? " New record!" : ""),
                         .messageColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                         .remainingTime = 6.0f });
    std::string rewards;
    if (caught.eggs > 0) {
        rewards = std::to_string(caught.eggs) + (caught.eggs == 1 ? " egg" : " eggs");
    }
    if (caught.notes > 0) {
        rewards +=
            (rewards.empty() ? "" : " and ") + std::to_string(caught.notes) + (caught.notes == 1 ? " note" : " notes");
    }
    if (!rewards.empty()) {
        Notification::Emit({ .prefix = "Reward!",
                             .prefixColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                             .message = rewards + " popped out.",
                             .messageColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                             .remainingTime = 6.0f });
    }
}

PlayAsLinkFish::Frame MakeFishFrame(const State& s, const Input& in, float dt) {
    PlayAsLinkFish::Frame frame;
    frame.dt = dt;
    PlayerPosition(frame.player);
    frame.playerYaw = yaw_get();
    std::memcpy(frame.tip, s.tip, sizeof(frame.tip));
    std::memcpy(frame.lure, s.lure, sizeof(frame.lure));
    frame.lureYaw = s.lureYaw;
    if (s.phase == Phase::Flying) {
        frame.castState = 1;
    } else if (s.phase == Phase::Landed && s.onWater) {
        // OoT lets the lure sit for 10 ticks before fish pay it any mind and it can be reeled
        frame.castState = s.landedTime >= 0.5f ? 3 : 2;
        frame.lureSplash = s.landedTime < 0.5f;
    }
    frame.wiggle = s.wiggleLevel;
    frame.wiggling = s.wigglePos > 1.0f;
    frame.a = in.a;
    frame.aPressed = in.aPressed;
    frame.bPressed = in.bPressed && s.phase == Phase::Landed;
    frame.stickX = in.stickX;
    frame.stickY = in.stickY;
    return frame;
}

// The rod and Link's body, as a fish pulls (Fishing_DrawRod's D_80B7A6C0 and D_80B7A6BC, and the player's tilt), a
// game tick at a time
void TickRod(State& s, const PlayAsLinkFish::Fight& fight, bool onTheLine) {
    s.rodTicks++;
    auto approach = [](float& value, float target, float fraction, float step) {
        const float stepSize = std::clamp((target - value) * fraction, -step, step);
        value += stepSize;
    };
    auto approachZero = [](float& value, float fraction, float step) {
        value -= std::clamp(value * fraction, -step, step);
    };
    float leanTarget = 0.0f;
    float leanScale = 5.0f;
    float leanStep = 1000.0f;
    if (s.hitTicks > 0) {
        // The hook is set: the rod snaps up and Link is pulled back
        s.hitTicks--;
        approach(s.rodTilt, 35.0f, 1.0f, 100.0f);
        approach(s.fightBend, -0.8f, 1.0f, 0.4f);
        leanTarget = -4000.0f;
        leanScale = 2.0f;
        leanStep = 15000.0f;
    } else if (onTheLine && fight.hooked) {
        leanTarget = std::sin(s.rodTicks * 25600.0f * kPi / 32768.0f) * 1500.0f;
    } else {
        approachZero(s.rodTilt, 0.1f, 10.0f);
        approachZero(s.fightBend, 1.0f, 0.05f);
    }
    s.bodyLean += std::clamp((leanTarget - s.bodyLean) / leanScale, -leanStep, leanStep);
    if (onTheLine && fight.pulling) {
        approach(s.rodTilt, 35.0f, 0.1f, 3.5f);
        approach(s.fightBend, fight.bendTarget, 0.3f, 0.1f);
    }
}

// The line's strain as the player sees and hears it: the rod's shudder and the creak both grow with the tension the
// fish module keeps (it decides when the line snaps)
void UpdateStrain(State& s, float tension, float dt) {
    if (s.phase != Phase::Hooked) {
        tension = 0.0f;
    }
    Approach(s.tension, tension, 12.0f, dt);
    s.shakeTime += dt;
    s.creakTimer -= dt;
    // The camera keeps well back during a fight, so the rod's strain is small on screen: say it too
    if (s.tension >= kWarnFrom && !s.strainWarned) {
        s.strainWarned = true;
        Notification::Emit({ .prefix = "Line straining!",
                             .prefixColor = ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                             .message = "Ease off A!",
                             .messageColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                             .remainingTime = 2.5f });
    } else if (s.tension < kWarnFrom - 0.3f) {
        s.strainWarned = false;
    }
    if (s.tension >= kCreakFrom && s.creakTimer <= 0.0f) {
        const float t = (s.tension - kCreakFrom) / (1.0f - kCreakFrom);
        gcsfx_playWithPitch(SFX_69_WHIPCRACK_CREAKING, 0.85f + 0.4f * t, 14000 + (int)(10000.0f * t));
        s.creakTimer = 1.0f + (kCreakSeconds - 1.0f) * t;
    }
}

// What the fish did this frame: the bite, the hook, the catch or the escape
void OnFish(PlayAsLinkFishing::Fishing::Data& data, State& s, const PlayAsLinkFish::Result& result) {
    const PlayAsLinkFish::Fight& fight = PlayAsLinkFish::GetFight(*data.school);
    if (result.bit && s.phase == Phase::Landed) {
        s.phase = Phase::Hooked;
        s.catchTime = 0.0f;
        // OoT lets the controller rumble; here the rod tugs, and the player is told what to do
        Notification::Emit({ .prefix = "Bite!",
                             .prefixColor = ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                             .message = "Press A or pull back!",
                             .messageColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                             .remainingTime = 3.0f });
    }
    if (result.hookSet) {
        s.hitTicks = 10;
    }
    for (int i = 0; i < result.ticks; i++) {
        TickRod(s, fight, s.phase == Phase::Hooked);
    }
    if (s.phase != Phase::Hooked) {
        return;
    }
    std::memcpy(s.mouth, fight.mouth, sizeof(s.mouth));
    if (result.caught) {
        s.phase = Phase::Catching;
        s.catchTime = 0.0f;
        s.catchReleased = false;
        s.catchTextShown = false;
        s.catchLength = fight.length;
        s.catchLoach = fight.loach;
        s.hitTicks = 0;
    } else if (result.snapped) {
        // The line breaks and the fish is gone. OoT would lose the lure; here Link just has the rod ready again,
        // with the lure back at its tip.
        Notification::Emit({ .prefix = "The line snapped!",
                             .prefixColor = ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                             .message = "Ease off when a big fish pulls.",
                             .messageColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                             .remainingTime = 4.0f });
        gcsfx_playWithPitch(SFX_111_WHIPCRACK_DEATH, 1.2f, 26000);
        s.tension = 0.0f;
        s.flick = 1.0f; // the rod whips back
        s.hitTicks = 0;
        s.returnTime = 0.0f;
        std::memcpy(s.returnFrom, s.mouth, sizeof(s.returnFrom));
        s.lurePitch = 0.0f;
        s.phase = Phase::Returning;
    } else if (result.gotAway) {
        // The lure is where the fish had it, afloat again
        const bool missed = result.missed;
        Notification::Emit({ .prefix = missed ? "Too slow..." : "It got away...",
                             .prefixColor = ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                             .message = missed ? "The fish let go" : "The line went slack",
                             .messageColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                             .remainingTime = 3.0f });
        bool isWater;
        float surface, ground;
        std::memcpy(s.lure, s.mouth, sizeof(s.lure));
        if (SurfaceBelow(s.lure[0], s.lure[1], s.lure[2], 300.0f, 1200.0f, &isWater, &surface, &ground)) {
            s.onWater = isWater;
            s.surfaceY = surface;
            s.lure[1] = surface;
            s.velocity[0] = s.velocity[1] = s.velocity[2] = 0.0f;
            s.landedTime = kLandedDelay + 0.6f;
            s.reelSpeed = kReelStartSpeed;
            s.bob = 1.0f;
            s.phase = Phase::Landed;
        } else {
            s.returnTime = 0.0f;
            std::memcpy(s.returnFrom, s.mouth, sizeof(s.returnFrom));
            s.phase = Phase::Returning;
        }
    }
}

// What the camera frames: the water Link casts at (the lure, or ahead of him while it is in his hand), the fish
// he fights, then him holding it up
void UpdateCamera(const State& s) {
    PlayAsLinkFishingCamera::Request request;
    PlayerPosition(request.link);
    request.yaw = yaw_get();
    const float yaw = request.yaw * kPi / 180.0f;
    if (s.phase == Phase::Catching) {
        request.view = PlayAsLinkFishingCamera::View::Catch;
    } else if (s.phase == Phase::Hooked) {
        request.view = PlayAsLinkFishingCamera::View::Fight;
        std::memcpy(request.aim, s.mouth, sizeof(request.aim));
    } else if (s.phase == Phase::Flying || s.phase == Phase::Landed) {
        request.view = PlayAsLinkFishingCamera::View::Rod;
        std::memcpy(request.aim, s.lure, sizeof(request.aim));
    } else {
        request.view = PlayAsLinkFishingCamera::View::Rod;
        request.aim[0] = request.link[0] + std::sin(yaw) * kCameraAimAhead;
        request.aim[1] = request.link[1];
        request.aim[2] = request.link[2] + std::cos(yaw) * kCameraAimAhead;
    }
    PlayAsLinkFishingCamera::Set(request);
}

} // namespace

namespace PlayAsLinkFishing {

LinkPose GetLinkPose(const Fishing& fishing) {
    const State& s = fishing.data->state;
    LinkPose pose;
    pose.active = s.phase != Phase::Off;
    if (!pose.active) {
        return pose;
    }
    pose.throwing = s.phase == Phase::Charging || s.throwPlaying;
    pose.throwFrame = s.throwFrame;
    if (s.phase == Phase::Flying || s.phase == Phase::Landed || s.phase == Phase::Hooked) {
        pose.leanX = s.leanX;
        pose.leanY = s.leanY;
    }
    pose.tilt = s.bodyLean * kPi / 32768.0f + std::sin(s.shakeTime * 41.0f) * kStrainBodyShake * s.tension * s.tension;
    if (s.phase == Phase::Catching) {
        pose.catching = s.catchReleased ? LinkPose::CatchEnd : LinkPose::CatchHold;
        pose.catchFrame = (s.catchReleased ? s.catchTime - s.releaseTime : s.catchTime) * 30.0f;
    }
    return pose;
}

bool IsActive(const Fishing& fishing) {
    return fishing.data->state.phase != Phase::Off;
}

void Stop(Fishing& fishing) {
    Release(*fishing.data);
}

void Update(Fishing& fishing, float dt) {
    PlayAsLinkFishing::Fishing::Data& data = *fishing.data;
    State& s = data.state;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const Input in = ReadInput(s);

    const bool valid = PlayAsLink_shouldDraw(baModel_getModelId()) && player_getTransformation() == TRANSFORM_1_BANJO &&
                       bs_getState() == BS_1_IDLE && !gctransition_8030BDC0();
    if (s.phase == Phase::Off) {
        float water[3];
        if (in.downPressed && valid && !player_inWater() && WaterAhead(water)) {
            s = State();
            s.phase = Phase::Ready;
            s.downHeld = in.down;
            s.aHeld = in.a;
            s.bHeld = in.b;
            s.prevStickX = in.stickX;
            s.prevStickY = in.stickY;
            LockBanjo(true);
            PlayAsLinkFishingCamera::Begin();
            // The fish are in the water in front of Link from the moment the rod comes out
            if (data.school != nullptr) {
                float player[3];
                PlayerPosition(player);
                Pond pond(s);
                PlayAsLinkFish::Spawn(*data.school, pond, water, player, yaw_get());
            }
        }
        return;
    }
    // The catch's dialog box has the player's attention: the rod stays out until it is dismissed
    const bool boxUp = s.phase == Phase::Catching && PlayAsLinkFishingCatch::IsCatchShown();
    if (!valid || (in.downPressed && !boxUp)) {
        Release(data);
        return;
    }
    // Banjo's input is read before this runs, so keep it switched off (something else may have switched it on)
    LockBanjo(true);

    // Stick tilt for Link's reeling animations and the rod's bend
    const bool lureOut = s.phase == Phase::Flying || s.phase == Phase::Landed || s.phase == Phase::Hooked;
    s.leanXPrev = s.leanX;
    s.leanYPrev = s.leanY;
    Approach(s.leanX, lureOut ? in.stickX : 0.0f, 7.0f, dt);
    Approach(s.leanY, lureOut ? in.stickY : 0.0f, 7.0f, dt);

    // Rod: it follows how the stick moves, whipping with it (Fishing_DrawRod), and rests with a little sway
    s.idleTime += dt;
    if (lureOut && dt > 0.0f) {
        const float perTick = kTickSeconds / dt;
        Approach(s.bendY, -(s.leanX - s.leanXPrev) * perTick * 0.7f, 30.0f, dt);
        Approach(s.bendX, (s.leanY - s.leanYPrev) * perTick * 0.7f, 30.0f, dt);
    } else {
        Approach(s.bendX, std::sin(s.idleTime * 5.75f) * 0.025f - 0.03f, 8.0f, dt);
        Approach(s.bendY, 0.0f, 8.0f, dt);
    }
    const float flickTarget = (s.phase == Phase::Casting && s.throwFrame > kReleaseFrame - 4.0f) ? 0.8f : 0.0f;
    Approach(s.flick, flickTarget, 12.0f, dt);

    // Ripples tick at the game's 20 Hz
    s.rippleTick += dt;
    while (s.rippleTick >= kTickSeconds) {
        s.rippleTick -= kTickSeconds;
        TickRipples(s);
    }

    // The line sags with the slack: taut in the air, loose when the lure sits, tighter while it is reeled
    float sagTarget = 0.0f;
    if (lureOut) {
        const float dx = s.lure[0] - s.tip[0], dy = s.lure[1] - s.tip[1], dz = s.lure[2] - s.tip[2];
        const float span = std::sqrt(dx * dx + dy * dy + dz * dz);
        // A fish on the line keeps it taut
        sagTarget = std::min(span * (s.phase == Phase::Flying || s.phase == Phase::Hooked ? 0.03f
                                     : in.a                                               ? 0.04f
                                                                                          : 0.12f),
                             90.0f);
        sagTarget *= 1.0f - 0.8f * s.tension; // a line under strain is drawn straight
    }
    Approach(s.sag, sagTarget, 4.0f, dt);

    // Stick turns Link, before the cast
    if (s.phase == Phase::Ready || s.phase == Phase::Charging) {
        if (std::abs(in.stickX) > 0.05f) {
            // A positive yaw turns left, so the stick going right takes it down
            const float yaw = yaw_get() - in.stickX * kTurnSpeed * dt;
            yaw_set(yaw);
            yaw_setIdeal(yaw);
        }
    }

    switch (s.phase) {
        case Phase::Ready:
            // Only a press that starts here counts: B may still be down from before the lure came back
            if (in.bPressed) {
                s.bArmed = true;
                s.bTime = 0.0f;
            }
            if (s.bArmed && in.b) {
                s.bTime += dt;
                if (s.bTime >= kTapSeconds) {
                    s.phase = Phase::Charging;
                    s.charge = 0.0f;
                    s.throwFrame = kWindUpFrame;
                }
            } else if (s.bArmed) {
                // A quick press of B with the lure in puts the rod away
                Release(data);
                return;
            }
            break;
        case Phase::Charging:
            s.bTime += dt;
            s.charge = std::min(s.bTime - kTapSeconds, kFullChargeSeconds - kTapSeconds);
            s.throwFrame = kWindUpFrame;
            if (!in.b) {
                s.phase = Phase::Casting;
                s.throwPlaying = true;
            }
            break;
        case Phase::Casting:
            s.throwFrame += dt * 30.0f * kThrowSpeed;
            if (s.throwFrame >= kReleaseFrame) {
                Launch(s);
            }
            break;
        case Phase::Flying:
            UpdateFlight(s, in, dt);
            break;
        case Phase::Landed:
            UpdateLanded(s, in, dt);
            break;
        case Phase::Hooked: {
            // Link turns to face the fish (the player's yaw approaches the way to it, at most 7 degrees a tick)
            const PlayAsLinkFish::Fight& fight = PlayAsLinkFish::GetFight(*data.school);
            const float yaw = yaw_get();
            const float delta = std::remainder(fight.faceYaw - yaw, 360.0f);
            const float step = std::clamp(delta * (1.0f - std::exp(-4.5f * dt)), -140.0f * dt, 140.0f * dt);
            yaw_set(yaw + step);
            yaw_setIdeal(yaw + step);
            break;
        }
        case Phase::Catching:
            s.catchTime += dt;
            if (!s.catchCounted && s.catchTime >= kCatchTextSeconds) {
                // The fish is in Link's hands: it counts, and what it earns pops out
                s.catchCounted = true;
                const int pounds = FishWeight(s.catchLength, s.catchLoach);
                float player[3];
                PlayerPosition(player);
                const PlayAsLinkFishingRewards::Given given =
                    PlayAsLinkFishingRewards::Give(pounds, s.catchLoach, player, yaw_get());
                s.catchInfo.pounds = pounds;
                s.catchInfo.loach = s.catchLoach;
                s.catchInfo.newRecord = PlayAsLinkFishingCatch::RecordCatch(pounds, s.catchLoach);
                s.catchInfo.eggs = given.eggs;
                s.catchInfo.notes = given.notes;
            }
            if (s.catchCounted && !s.catchTextShown) {
                // Link keeps the fish up and the camera stays on him until the player has read the box
                if (PlayAsLinkFishingCatch::ShowCatch(s.catchInfo)) {
                    s.catchTextShown = true;
                } else if ((s.catchBoxWait += dt) >= kCatchBoxGiveUpSeconds) {
                    s.catchTextShown = true;
                    s.catchBoxFallback = true;
                    s.catchFallbackTime = s.catchTime;
                    AnnounceCatchFallback(s.catchInfo);
                }
            }
            if (s.catchTextShown && !s.catchReleased) {
                const bool read = s.catchBoxFallback ? s.catchTime >= s.catchFallbackTime + kCatchHoldSeconds
                                                     : !PlayAsLinkFishingCatch::IsCatchShown();
                if (read) {
                    // Back in the water
                    s.catchReleased = true;
                    s.releaseTime = s.catchTime;
                    Pond pond(s);
                    float player[3];
                    PlayerPosition(player);
                    PlayAsLinkFish::ReleaseCaught(*data.school, pond, player, yaw_get());
                }
            }
            if (s.catchReleased && s.catchTime >= s.releaseTime + kCatchEndSeconds) {
                // The lure comes back to the rod from where the fish was held
                s.returnTime = 0.0f;
                std::memcpy(s.returnFrom, s.mouth, sizeof(s.returnFrom));
                s.lurePitch = 0.0f;
                s.phase = Phase::Returning;
            }
            break;
        case Phase::Returning:
            s.returnTime += dt;
            if (s.returnTime >= kLureReturnSeconds) {
                s.phase = Phase::Ready;
                s.bArmed = false;
            }
            break;
        case Phase::Off:
            break;
    }

    // The fish: they swim whenever the rod is out, and the lure's doings are theirs to answer
    if (data.school != nullptr) {
        Pond pond(s);
        const PlayAsLinkFish::Result result = PlayAsLinkFish::Update(*data.school, pond, MakeFishFrame(s, in, dt));
        s.wiggleLevel = 0;
        OnFish(data, s, result);
        UpdateStrain(s, PlayAsLinkFish::GetFight(*data.school).tension, dt);
    }
    s.prevStickX = in.stickX;
    s.prevStickY = in.stickY;
    UpdateCamera(s);

    // The throw plays out after the cast, then Link waits again
    if (s.throwPlaying && s.phase != Phase::Casting && s.phase != Phase::Charging) {
        s.throwFrame += dt * 30.0f * kThrowSpeed;
        if (s.throwFrame >= kThrowLastFrame) {
            s.throwPlaying = false;
        }
    }
    if (s.phase == Phase::Ready || s.phase == Phase::Returning) {
        s.throwPlaying = false;
    }
}

void Draw(Fishing& fishing, Gfx** gfx, Mtx** mtx, const MtxF& hand, const MtxF& rightHand) {
    Fishing::Data& data = *fishing.data;
    State& s = data.state;
    if (s.phase == Phase::Off) {
        return;
    }
    float camera[3];
    viewport_getPosition_vec3f(camera);

    Mtx* pool = *mtx;
    *mtx += kMatrixCount;
    int used = 0;
    // Every matrix the lists use is recorded for frame interpolation, in a scope of its own so that the number of
    // ripples or line segments can't throw the others' pairing off
    auto load = [&](const MtxF& m) -> Mtx* {
        if (used >= kMatrixCount) {
            return nullptr;
        }
        Mtx* out = &pool[used++];
        core1_7F60_guMtxF2L(const_cast<float(*)[4]>(m.mf), out);
        FrameInterpolation_RecordMatrixToMtx(out, m.mf);
        return out;
    };
    static const int sScopeKey = 0;
    auto draw = [&](const MtxF& m, Gfx* list) {
        if (Mtx* out = load(m)) {
            *(*gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)out);
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)list);
        }
    };

    // ---- The rod (Fishing_DrawRod), on the left hand's matrix ----
    FrameInterpolation_RecordOpenChild(&sScopeKey, 1);
    *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)data.rodMaterial);
    gDPSetPrimColor((*gfx)++, 0, 0, 255, 155, 0, 255);
    MtxF rod = hand;
    MtxTranslate(rod, 0.0f, 400.0f, 0.0f);                               // adult Link
    MtxRotateY(rod, (s.phase == Phase::Catching ? 0.56f : 0.41f) * kPi); // swung aside for the fish in Link's hands
    MtxRotateX(rod, -kPi / 5.0000003f);
    MtxRotateZ(rod, s.leanX * 0.5f + 3.0f * kPi / 20.0f);
    MtxRotateX(rod, (s.rodTilt + 20.0f) * 0.01f * kPi);
    MtxScale(rod, 0.70000005f, 0.70000005f, 0.70000005f);
    // Under strain the rod bends further and shudders
    const float strain = s.tension * s.tension;
    const float bend = s.bendX + s.flick + s.fightBend * (((s.leanY - 1.0f) * -0.25f) + 0.5f) -
                       kStrainBend * s.tension + std::sin(s.shakeTime * 58.0f) * kStrainShudder * strain;
    const float sway = s.bendY + std::sin(s.shakeTime * 47.0f + 1.3f) * kStrainShudder * 0.7f * strain;
    MtxTranslate(rod, 0.0f, 0.0f, -1300.0f);
    for (int i = 0; i < 22; i++) {
        MtxRotateY(rod, sway * kRodBendRatios[i] * 0.5f);
        MtxRotateX(rod, bend * kRodBendRatios[i] * 0.5f);

        MtxF segment = rod;
        MtxScale(segment, kRodScales[i], kRodScales[i], 0.52f);
        if (Mtx* out = load(segment)) {
            *(*gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)out);
            const uint32_t texture = i < 5                     ? kRodTextures[0]
                                     : (i < 8 || (i % 2) == 0) ? kRodTextures[1]
                                                               : kRodTextures[2];
            gDPLoadTextureBlock((*gfx)++, data.fish.data() + texture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)data.rodSegment);
        }
        MtxTranslate(rod, 0.0f, 0.0f, 500.0f);
    }
    float tipRelative[3];
    MtxTransform(rod, 0.0f, 0.0f, 0.0f, tipRelative);
    for (int k = 0; k < 3; k++) {
        s.tip[k] = tipRelative[k] + camera[k];
    }
    s.tipValid = true;
    FrameInterpolation_RecordCloseChild();

    // ---- The lure (Fishing_DrawLureAndLine) ----
    float lure[3];
    float yaw = s.lureYaw;
    float pitch = s.lurePitch;
    if (s.phase == Phase::Ready) {
        HangLure(s, lure);
        pitch = -kPi / 2.0f;
        yaw = std::sin(s.idleTime * 2.0f) * 0.3f;
    } else if (s.phase == Phase::Casting || s.phase == Phase::Charging) {
        HangLure(s, lure);
        pitch = -kPi / 2.0f;
        yaw = s.lureYaw;
    } else if (s.phase == Phase::Returning) {
        float home[3];
        HangLure(s, home);
        const float t = Clamp01(s.returnTime / kLureReturnSeconds);
        for (int k = 0; k < 3; k++) {
            lure[k] = s.returnFrom[k] + (home[k] - s.returnFrom[k]) * t;
        }
        pitch = s.lurePitch + (-kPi / 2.0f - s.lurePitch) * t;
    } else if (s.phase == Phase::Flying) {
        std::memcpy(lure, s.lure, sizeof(lure));
        pitch = 0.0f;
        yaw = std::atan2(-s.velocity[0], -s.velocity[2]);
        s.lureYaw = yaw;
    } else if ((s.phase == Phase::Hooked || s.phase == Phase::Catching) && data.school != nullptr) {
        // In the fish's mouth, with the line straight to it
        const PlayAsLinkFish::Fight& fight = PlayAsLinkFish::GetFight(*data.school);
        std::memcpy(lure, fight.mouth, sizeof(lure));
        pitch = 0.0f;
        yaw = fight.mouthYaw;
    } else {
        std::memcpy(lure, s.lure, sizeof(lure));
    }
    float lureRelative[3] = { lure[0] - camera[0], lure[1] - camera[1], lure[2] - camera[2] };

    FrameInterpolation_RecordOpenChild(&sScopeKey, 2);
    MtxF lureMatrix = MtxAt(lureRelative);
    MtxRotateY(lureMatrix, yaw);
    MtxRotateX(lureMatrix, pitch);
    const float lureScale = 0.0039999997f * kWorldScale;
    MtxScale(lureMatrix, lureScale, lureScale, lureScale);
    MtxRotateZ(lureMatrix, kPi / 2.0f);
    MtxRotateY(lureMatrix, kPi / 2.0f);
    draw(lureMatrix, data.lureFloat);

    float tie[3], hookPositions[2][3];
    MtxTransform(lureMatrix, -850.0f, 0.0f, 0.0f, tie);
    if (s.phase == Phase::Hooked || s.phase == Phase::Catching) {
        std::memcpy(tie, lureRelative, sizeof(tie));
    }
    MtxTransform(lureMatrix, 500.0f, 0.0f, -300.0f, hookPositions[0]);
    MtxTransform(lureMatrix, 2100.0f, 0.0f, -50.0f, hookPositions[1]);
    // The hooks hang down from the lure
    for (int h = 0; h < 2; h++) {
        MtxF hook = MtxAt(hookPositions[h]);
        MtxRotateY(hook, yaw);
        MtxRotateX(hook, -kPi / 2.0f);
        MtxScale(hook, 0.0039999997f * kWorldScale, 0.0039999997f * kWorldScale, 0.005f * kWorldScale);
        MtxRotateY(hook, kPi);
        draw(hook, data.lureHook);
        MtxRotateZ(hook, kPi / 2.0f);
        draw(hook, data.lureHook);
    }
    FrameInterpolation_RecordCloseChild();

    // ---- The fish ----
    if (data.school != nullptr) {
        PlayAsLinkFish::Draw(*data.school, gfx, mtx, camera, rightHand, yaw_get());
    }

    // ---- Ripples (Fishing_DrawEffects) ----
    FrameInterpolation_RecordOpenChild(&sScopeKey, 3);
    bool ripplesSetUp = false;
    for (const Ripple& ripple : s.ripples) {
        if (!ripple.active) {
            continue;
        }
        if (!ripplesSetUp) {
            ripplesSetUp = true;
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)data.rippleMaterial);
            gDPSetEnvColor((*gfx)++, 155, 155, 155, 0);
        }
        gDPSetPrimColor((*gfx)++, 0, 0, 255, 255, 255, (u8)std::clamp(ripple.alpha, 0.0f, 255.0f));
        const float at[3] = { ripple.pos[0] - camera[0], ripple.pos[1] - camera[1] + 0.5f, ripple.pos[2] - camera[2] };
        MtxF m = MtxAt(at);
        MtxScale(m, ripple.scale * kWorldScale, 1.0f, ripple.scale * kWorldScale);
        draw(m, data.rippleModel);
    }
    FrameInterpolation_RecordCloseChild();

    // ---- The line: translucent, drawn as pairs of thin crossed strips ----
    FrameInterpolation_RecordOpenChild(&sScopeKey, 4);
    gDPPipeSync((*gfx)++);
    gDPSetOtherMode((*gfx)++,
                    G_AD_NOTPATTERN | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_AA_ZB_XLU_SURF | G_RM_AA_ZB_XLU_SURF2);
    gDPSetCombineMode((*gfx)++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    // The line reddens as it nears snapping
    const u8 fade = (u8)(255.0f * (1.0f - 0.75f * s.tension));
    gDPSetPrimColor((*gfx)++, 0, 0, 255, fade, fade, (u8)(150.0f + 100.0f * s.tension));
    *(*gfx)++ = MakeGfx(0xD9000000, 0x00000001 | 0x00000004); // G_GEOMETRYMODE: G_ZBUFFER | G_SHADE, nothing else

    // A sagging curve from the rod's tip to the lure's eye. Where it dips below the water it lies on the surface.
    constexpr int kSegments = 16;
    float points[kSegments + 1][3];
    const float surfaceRelative = s.surfaceY - camera[1];
    const bool onSurface = s.phase == Phase::Landed;
    for (int i = 0; i <= kSegments; i++) {
        const float t = (float)i / kSegments;
        for (int k = 0; k < 3; k++) {
            points[i][k] = tipRelative[k] + (tie[k] - tipRelative[k]) * t;
        }
        points[i][1] -= s.sag * 4.0f * t * (1.0f - t);
        if (onSurface && i > 0 && i < kSegments) {
            points[i][1] = std::max(points[i][1], surfaceRelative + 0.6f);
        }
    }
    for (int i = 0; i < kSegments; i++) {
        // Thin strips would vanish into the distance, so the line gets wider the farther it is from the camera
        const float mx = (points[i][0] + points[i + 1][0]) * 0.5f;
        const float my = (points[i][1] + points[i + 1][1]) * 0.5f;
        const float mz = (points[i][2] + points[i + 1][2]) * 0.5f;
        const float width = std::max(1.2f, std::sqrt(mx * mx + my * my + mz * mz) * 0.004f) / 200.0f;
        draw(MtxAlong(points[i], points[i + 1], width, 0.0f), data.lineModel);
        draw(MtxAlong(points[i], points[i + 1], width, 1.0f), data.lineModel);
    }
    FrameInterpolation_RecordCloseChild();
}

} // namespace PlayAsLinkFishing
