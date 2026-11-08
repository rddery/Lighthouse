#include "PlayAsLinkFishing.h"
#include "PlayAsLink.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>

#include <libultraship/libultraship.h>
#include <fast/ucodehandlers.h>

#include "port/Interpolation/FrameInterpolation.h"

extern "C" {
#include "functions.h"
#include "bk_time.h"
#include "core1/joy.h"
#include "core2/gc/transition.h"
#include "core2/yaw.h"

void core1_7F60_guMtxF2L(float mf[4][4], Mtx* m);
BKCollisionTriangle* mapModel_intersectLine(f32 start[3], f32 end[3], f32 normal[3], u32 filter);
}

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

constexpr int kRippleCount = 8;
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

uint16_t Be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

Gfx MakeGfx(uintptr_t w0, uintptr_t w1) {
    Gfx gfx;
    std::memset(&gfx, 0, sizeof(gfx));
    gfx.words.w0 = w0;
    gfx.words.w1 = w1;
    return gfx;
}

// The renderer marks a segmented address with its lowest bit
uintptr_t SegmentRef(uint32_t address) {
    return (uintptr_t)(address & 0x0FFFFFFE) | 1;
}

constexpr uintptr_t kF3dex2Mtx = 0xDA380003; // G_MTX: G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH
constexpr uintptr_t kF3dex2Dl = 0xDE000000;  // G_DL, pushed

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// Moves `value` toward `target` the way a smoothing filter with the given rate (per second) does, at any frame rate
void Approach(float& value, float target, float rate, float dt) {
    value += (target - value) * (1.0f - std::exp(-rate * dt));
}

// ---- Matrices, with the operations OoT's sys_matrix.c has (applied on the right, to the local frame) ----

void MtxTranslate(MtxF& m, float x, float y, float z) {
    m.xw += m.xx * x + m.xy * y + m.xz * z;
    m.yw += m.yx * x + m.yy * y + m.yz * z;
    m.zw += m.zx * x + m.zy * y + m.zz * z;
    m.ww += m.wx * x + m.wy * y + m.wz * z;
}

void MtxScale(MtxF& m, float x, float y, float z) {
    m.xx *= x;
    m.yx *= x;
    m.zx *= x;
    m.wx *= x;
    m.xy *= y;
    m.yy *= y;
    m.zy *= y;
    m.wy *= y;
    m.xz *= z;
    m.yz *= z;
    m.zz *= z;
    m.wz *= z;
}

void MtxRotateX(MtxF& m, float angle) {
    if (angle == 0.0f) {
        return;
    }
    const float sin = std::sin(angle);
    const float cos = std::cos(angle);
    float a, b;
    a = m.xy, b = m.xz, m.xy = a * cos + b * sin, m.xz = b * cos - a * sin;
    a = m.yy, b = m.yz, m.yy = a * cos + b * sin, m.yz = b * cos - a * sin;
    a = m.zy, b = m.zz, m.zy = a * cos + b * sin, m.zz = b * cos - a * sin;
    a = m.wy, b = m.wz, m.wy = a * cos + b * sin, m.wz = b * cos - a * sin;
}

void MtxRotateY(MtxF& m, float angle) {
    if (angle == 0.0f) {
        return;
    }
    const float sin = std::sin(angle);
    const float cos = std::cos(angle);
    float a, b;
    a = m.xx, b = m.xz, m.xx = a * cos - b * sin, m.xz = a * sin + b * cos;
    a = m.yx, b = m.yz, m.yx = a * cos - b * sin, m.yz = a * sin + b * cos;
    a = m.zx, b = m.zz, m.zx = a * cos - b * sin, m.zz = a * sin + b * cos;
    a = m.wx, b = m.wz, m.wx = a * cos - b * sin, m.wz = a * sin + b * cos;
}

void MtxRotateZ(MtxF& m, float angle) {
    if (angle == 0.0f) {
        return;
    }
    const float sin = std::sin(angle);
    const float cos = std::cos(angle);
    float a, b;
    a = m.xx, b = m.xy, m.xx = a * cos + b * sin, m.xy = b * cos - a * sin;
    a = m.yx, b = m.yy, m.yx = a * cos + b * sin, m.yy = b * cos - a * sin;
    a = m.zx, b = m.zy, m.zx = a * cos + b * sin, m.zy = b * cos - a * sin;
    a = m.wx, b = m.wy, m.wx = a * cos + b * sin, m.wy = b * cos - a * sin;
}

// Matrix_MultVec3f
void MtxTransform(const MtxF& m, float x, float y, float z, float out[3]) {
    out[0] = m.xx * x + m.xy * y + m.xz * z + m.xw;
    out[1] = m.yx * x + m.yy * y + m.yz * z + m.yw;
    out[2] = m.zx * x + m.zy * y + m.zz * z + m.zw;
}

MtxF MtxAt(const float position[3]) {
    MtxF m;
    std::memset(&m, 0, sizeof(m));
    m.xx = m.yy = m.zz = m.ww = 1.0f;
    m.xw = position[0];
    m.yw = position[1];
    m.zw = position[2];
    return m;
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

    // The rod
    float bendY = 0.0f; // sRodBendRotY
    float bendX = 0.0f; // D_80B7A6AC
    float flick = 0.0f; // D_80B7A6B8: the whip just before the cast
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
    return fishing;
}

} // namespace PlayAsLinkFishing

namespace {

// ---- Controller and Banjo ----

struct Input {
    bool a, b, down;
    bool bPressed, downPressed;
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

// Water within casting range in front of the player, deep enough for a lure to float on
bool WaterAhead() {
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

void Release(State& s) {
    if (s.phase != Phase::Off) {
        LockBanjo(false);
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
    if (s.phase == Phase::Flying || s.phase == Phase::Landed) {
        pose.leanX = s.leanX;
        pose.leanY = s.leanY;
    }
    return pose;
}

bool IsActive(const Fishing& fishing) {
    return fishing.data->state.phase != Phase::Off;
}

void Stop(Fishing& fishing) {
    Release(fishing.data->state);
}

void Update(Fishing& fishing, float dt) {
    State& s = fishing.data->state;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const Input in = ReadInput(s);

    const bool valid = PlayAsLink_shouldDraw(baModel_getModelId()) && player_getTransformation() == TRANSFORM_1_BANJO &&
                       bs_getState() == BS_1_IDLE && !gctransition_8030BDC0();
    if (s.phase == Phase::Off) {
        if (in.downPressed && valid && !player_inWater() && WaterAhead()) {
            s = State();
            s.phase = Phase::Ready;
            s.downHeld = in.down;
            s.aHeld = in.a;
            s.bHeld = in.b;
            LockBanjo(true);
        }
        return;
    }
    if (!valid || in.downPressed) {
        Release(s);
        return;
    }
    // Banjo's input is read before this runs, so keep it switched off (something else may have switched it on)
    LockBanjo(true);

    // Stick tilt for Link's reeling animations and the rod's bend
    const bool lureOut = s.phase == Phase::Flying || s.phase == Phase::Landed;
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
        sagTarget = std::min(span * (s.phase == Phase::Flying ? 0.03f : in.a ? 0.04f : 0.12f), 90.0f);
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
                Release(s);
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

void Draw(Fishing& fishing, Gfx** gfx, Mtx** mtx, const MtxF& hand) {
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
    MtxTranslate(rod, 0.0f, 400.0f, 0.0f); // adult Link
    MtxRotateY(rod, 0.41f * kPi);
    MtxRotateX(rod, -kPi / 5.0000003f);
    MtxRotateZ(rod, s.leanX * 0.5f + 3.0f * kPi / 20.0f);
    MtxRotateX(rod, 20.0f * 0.01f * kPi);
    MtxScale(rod, 0.70000005f, 0.70000005f, 0.70000005f);
    const float bend = s.bendX + s.flick;
    MtxTranslate(rod, 0.0f, 0.0f, -1300.0f);
    for (int i = 0; i < 22; i++) {
        MtxRotateY(rod, s.bendY * kRodBendRatios[i] * 0.5f);
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
    gDPSetPrimColor((*gfx)++, 0, 0, 255, 255, 255, 150);
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
