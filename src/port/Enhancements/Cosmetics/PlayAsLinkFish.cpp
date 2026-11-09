#include "PlayAsLinkFish.h"
#include "PlayAsLinkUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <libultraship/libultraship.h>
#include <fast/ucodehandlers.h>

#include "port/Interpolation/FrameInterpolation.h"

extern "C" {
void core1_7F60_guMtxF2L(float mf[4][4], Mtx* m);
}

using namespace PlayAsLinkUtil;

// The fish follow the Fishing actor in OoT's z_fishing.c (Fishing_UpdateFish, func_80B70ED4, Fishing_DrawFish, ...)
// tick for tick: its timers, speeds and turning rates are used as they are, with the fish moving in OoT's units (2.6
// times smaller than Banjo-Kazooie's) at OoT's 20 ticks a second. The pond is whatever water the player faces instead
// of OoT's fixed one, and the rod's line is a plain length of line instead of OoT's 200 segments.
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kBinToRad = kPi / 32768.0f;
constexpr float kUnit = 2.6f;         // Banjo-Kazooie units in one OoT unit
constexpr float kTickSeconds = 0.05f; // OoT updates 20 times a second

constexpr uint32_t kFishSkeleton = 0x29C0; // gFishingFishSkel
constexpr uint32_t kFishAnimation = 0x7C;  // gFishingFishAnim
constexpr uint32_t kLoachSkeleton = 0x11058;
constexpr uint32_t kLoachAnimation = 0xCFE0;
constexpr int kFishLimbCount = 15;
constexpr int kLoachLimbCount = 11;
constexpr int kMaxLimbs = 16;
constexpr int kFishMouthLimb = 12; // OoT's limb 0xD, counted from 1
constexpr int kLoachMouthLimb = 10;

// sFishInits: the length (in OoT's inches) and how easily each fish takes the lure
struct FishInit {
    float baseLength;
    float perception;
};
constexpr FishInit kBass[] = {
    { 38, 0.1f },  { 36, 0.1f },  { 41, 0.05f }, { 35, 0.2f }, { 39, 0.1f }, { 44, 0.05f }, { 40, 0.1f }, { 34, 0.2f },
    { 54, 0.01f }, { 47, 0.05f }, { 42, 0.06f }, { 33, 0.2f }, { 57, 0.0f }, { 63, 0.0f },  { 71, 0.0f },
};
constexpr FishInit kLoach[] = { { 45, 0.0f }, { 43, 0.0f } };

// How deep the water has to be for a fish to live in it, and how far from each other they start
constexpr float kMinDepth = 28.0f;
constexpr float kCellSize = 100.0f; // Banjo-Kazooie units
constexpr int kCellRadius = 9;      // cells, around the first place the lure could land

// The line a fish is on can't let it farther than this out past what the player has let out (OoT units)
constexpr float kCatchDistance = 50.0f; // xz distance from the player at which a fish is landed
constexpr float kBiteSlack = 20.0f;

// Line tension. OoT's line never breaks, so this is the port's own rule, built from OoT's numbers: how hard a fish
// pulls is how much of the reel it takes away (z_fishing.c's sRodReelingSpeed: 1.0 - length * 0.00899 while it
// fights, none at all while it runs, 1.3 - ... once it tires; a Hylian Loach pulls 1.4 times as hard). Reeling
// against that pull builds tension; resting the reel or a tired fish lets it ease off. Per 20 Hz tick, like the
// rest of the fish.
constexpr float kTensionLimit = 100.0f;
constexpr float kTensionRise = 7.0f;     // at the full pull of the strongest fish
constexpr float kTensionEase = 4.0f;     // how much it eases a tick when nothing pulls against the reel
constexpr float kPullPerInch = 0.00899f; // OoT's: what the reel loses to a fish, per inch of its length
constexpr float kStrongPull = 0.64f;     // the pull of a 71 inch fish, OoT's biggest

// How many 20 Hz ticks the fish takes to be thrown back, and how high the arc goes
constexpr int kTossTicks = 18;
constexpr float kTossHeight = 40.0f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

float Sq(float v) {
    return v * v;
}

// ---- OoT's math helpers (sys_math.c, z_lib.c) ----

int16_t RadToBin(float radians) {
    return (int16_t)(int32_t)(radians * (32768.0f / kPi));
}

// Math_Atan2S(x, y): the angle of the vector (y, x) in 16-bit angles. With (z, x) it is OoT's yaw of a heading.
int16_t Atan2S(float x, float y) {
    return RadToBin(std::atan2(y, x));
}

float SinS(int16_t angle) {
    return std::sin(angle * kBinToRad);
}

float CosS(int16_t angle) {
    return std::cos(angle * kBinToRad);
}

void ApproachF(float& value, float target, float fraction, float step) {
    if (value != target) {
        float stepSize = (target - value) * fraction;
        if (stepSize >= step) {
            stepSize = step;
        } else if (stepSize <= -step) {
            stepSize = -step;
        }
        value += stepSize;
    }
}

void ApproachZeroF(float& value, float fraction, float step) {
    float stepSize = value * fraction;
    if (stepSize >= step) {
        stepSize = step;
    } else if (stepSize <= -step) {
        stepSize = -step;
    }
    value -= stepSize;
}

void ApproachS(int16_t& value, int16_t target, int scale, int maxStep) {
    int16_t diff = (int16_t)(target - value);
    diff = (int16_t)(diff / scale);
    if (diff > maxStep) {
        value = (int16_t)(value + maxStep);
    } else if (diff < -maxStep) {
        value = (int16_t)(value - maxStep);
    } else {
        value = (int16_t)(value + diff);
    }
}

// Fishing_SmoothStepToS
int16_t SmoothStepToS(int16_t& value, int16_t target, int scale, int step) {
    int16_t diff = (int16_t)(target - value);
    int16_t stepSize = (int16_t)(diff / scale);
    if (stepSize > step) {
        stepSize = (int16_t)step;
    }
    if (stepSize < -step) {
        stepSize = (int16_t)-step;
    }
    value = (int16_t)(value + stepSize);
    return stepSize;
}

struct Rng {
    uint32_t state = 0x2545F491u;
    float One() {
        state = state * 1664525u + 1013904223u;
        return (float)(state >> 8) / (float)(1 << 24);
    }
    float Zero(float max) {
        return One() * max;
    }
    float Centered(float max) {
        return (One() - 0.5f) * max;
    }
};

// ---- The converted models ----

struct Limb {
    int16_t position[3];
    uint8_t child;
    uint8_t sibling;
    Gfx* list;
};

struct Skeleton {
    int limbCount = 0;
    Limb limbs[kMaxLimbs] = {};
    int16_t joints[kMaxLimbs + 1][3] = {}; // the animation's only frame: the root's translation, then rotations
    int listCount = 0;                     // limbs with a display list: matrices the skeleton draws with
};

// ---- The fish ----

// What a fish looks like at an instant: what the display needs of it, so that it can be shown between two ticks
struct Look {
    float position[3] = {};
    int16_t rotX = 0, rotY = 0, rotZ = 0;
    int16_t limb23 = 0, limb4 = 0, limbD = 0, limbEF = 0, limb89 = 0;
    int16_t loach[3] = {};
    float scale[3] = {};
    float mouthY = 500.0f;
};

enum State : int {
    kLoachDeep = -1, // state -1..-25 are the Hylian Loach's own
    kLoachSurface = -2,
    kLoachSettle = -25,
    kLoachBite = -3,
    kSwim = 0,    // swimming off, scared
    kFlee = 1,    // fleeing
    kInspect = 2, // looking at the lure
    kNibble = 3,  // coming up to it
    kStrike = 4,  // going for it
    kHooked = 5,  // on the line
    kHeld = 6,    // in the player's hands
    kHome = 10,   // on its way home
    kRest = 11    // at home
};

struct Fish {
    bool active = false;
    bool loach = false;
    int index = 0;
    float length = 40.0f;
    float perception = 0.1f;
    float basePerception = 0.1f; // what the table says; A, B and twitches add to it while the fish looks at the lure

    // The actor
    Vec3 position, previous, home, velocity, target, mouth;
    float speed = 0.0f;
    float speedTarget = 0.0f;
    float rotationStep = 0.0f;
    int16_t worldRotX = 0, worldRotY = 0; // the way it is swimming
    int16_t shapeRotX = 0, shapeRotY = 0; // the way its body points
    float xzDistToPlayer = 0.0f;
    int16_t yawTowardsPlayer = 0;
    float surface =
        0.0f; // the water's surface where it is, and the floor under it (OoT's WATER_SURFACE_Y, floorHeight)
    float floor = 0.0f;
    bool wall = false;

    // Its state
    int state = kRest;
    int stateNext = kRest;
    int stateAndTimer = 0;
    int16_t unk15E = 0;
    int16_t timers[4] = {};
    int16_t bumpTimer = 0;
    int16_t scaredShort = 0; // unk_1A2: not interested in the lure
    int16_t scaredLong = 0;  // unk_1A4: keeps away from the player
    float hop = 0.0f;        // unk_184: vertical speed out of the water
    float phase = 0.0f, phaseStep = 0.0f, phaseMag = 0.0f;
    float phaseStepTarget = 0.0f, phaseMagTarget = 0.0f;
    int16_t pitch = 0, yaw = 0, roll = 0; // unk_160, unk_162, unk_164: the body's extra turns
    int16_t rotationTarget[3] = {};
    int16_t bank = 0;      // unk_16E: leaning into a turn
    int16_t settle = 0;    // unk_178
    int16_t turnTimer = 0; // unk_152: turning toward the player (when pulled)
    int16_t turnAngle = 0; // unk_154
    bool turnHard = false; // unk_156
    int16_t limb23 = 0, limb4 = 0, limbD = 0, limbEF = 0, limb89 = 0;
    int16_t loachRot[3] = {};
    float mouthY = 500.0f;
    int wanderTimer = 0; // ticks until it picks somewhere else to rest

    // Thrown back into the water after a catch
    bool tossed = false;
    int tossTick = 0;
    Vec3 tossFrom, tossTo;

    Look from, to; // the last two ticks
};

struct Cell {
    float x, z; // OoT units
    float surface, ground;
};

} // namespace

struct PlayAsLinkFish::School::Data {
    const uint8_t* object = nullptr;
    size_t size = 0;
    Skeleton fishSkeleton, loachSkeleton;

    Fish fish[kMaxFish];
    std::vector<Cell> cells;
    Rng rng;
    float accumulated = 0.0f;
    int ticks = 0; // sLureTimer
    World* world = nullptr;

    // The player and the rod, in OoT units
    Frame frame;
    Vec3 player, tip, lure;
    bool aPressed = false, bPressed = false, kick = false; // latched until a tick takes them
    int wiggle = 0;
    float lastStickY = 0.0f;
    int lureSide = 0; // D_80B7E118: which side of the lure a fish goes to

    // The fish on the line (sRodCastState 4 and 5)
    int castState = 0;
    int hooked = -1;
    bool lineHooked = false;
    int bitTimer = 0;        // sLureBitTimer
    int pullback = 100;      // sRodPullback
    int fightTime = 0;       // sFishFightTime
    float reelSpeed = 0.0f;  // sRodReelingSpeed
    float tension = 0.0f;    // 0 to kTensionLimit
    int hitTimer = 0;        // sRodHitTimer
    float lineLength = 0.0f; // how far from the rod the fish can be (OoT units, xz)
    Fight fight;

    Result result;

    bool Parse(Skeleton& skeleton, uint32_t skeletonOffset, uint32_t animationOffset, int limbCount,
               const ListConverter& convert, std::string& error);
    void Tick();
    void TickFish(Fish& f);
    Look LookOf(const Fish& f) const;
    void SetMouth(Fish& f, const Look& look);
    void MakeLook(Fish& f);
    bool Water(Fish& f);
    bool WaterAtCell(float x, float z, float refY, float& surface, float& ground);
    void PickTarget(Fish& f, float& x, float& z);
    void Splash(Fish& f, bool ignorePosCheck);
    void FightTick(Fish& f);
    void EndFight();
};

PlayAsLinkFish::School::School() : data(std::make_unique<Data>()) {
}

PlayAsLinkFish::School::~School() = default;

namespace PlayAsLinkFish {

namespace {
using Data = School::Data;
}

namespace {

// ---- Posing ----

// Fishing_DrawFish and the limb callbacks: the matrices of every limb that has a display list (in the skeleton's
// order, as the renderer finds them through segment 0x0D), and where the mouth is. `origin` is where the fish is and
// `unit` how big a unit of its model is relative to the fish's own.
struct PoseContext {
    const Skeleton* skeleton;
    bool loach;
    const Look* look;
    MtxF* matrices;
    Gfx** lists;
    int* count;
    float* mouth;
    int mouthLimb;
};

void VisitLimb(PoseContext& c, int index, const MtxF& parent) {
    const Limb& limb = c.skeleton->limbs[index];
    int16_t rotation[3] = { c.skeleton->joints[index + 1][0], c.skeleton->joints[index + 1][1],
                            c.skeleton->joints[index + 1][2] };
    float position[3];
    for (int k = 0; k < 3; k++) {
        position[k] = index == 0 ? c.skeleton->joints[0][k] : limb.position[k];
    }
    // Fishing_FishOverrideLimbDraw and Fishing_LoachOverrideLimbDraw (their limbs are counted from 1)
    const Look& look = *c.look;
    const int number = index + 1;
    if (!c.loach) {
        if (number == 0xD) {
            rotation[2] = (int16_t)(rotation[2] - (look.limbD - 11000));
        } else if (number == 2 || number == 3) {
            rotation[1] = (int16_t)(rotation[1] + look.limb23);
        } else if (number == 4) {
            rotation[1] = (int16_t)(rotation[1] + look.limb4);
        } else if (number == 0xE) {
            rotation[1] = (int16_t)(rotation[1] - look.limbEF);
        } else if (number == 0xF) {
            rotation[1] = (int16_t)(rotation[1] + look.limbEF);
        } else if (number == 8) {
            rotation[1] = (int16_t)(rotation[1] + look.limb89);
        } else if (number == 9) {
            rotation[1] = (int16_t)(rotation[1] - look.limb89);
        }
    } else if (number >= 3 && number <= 5) {
        rotation[1] = (int16_t)(rotation[1] + look.loach[number - 3]);
    }

    MtxF m = parent;
    MtxTranslateRotateZYX(m, position, rotation);
    if (limb.list != nullptr && c.matrices != nullptr && *c.count < kMatricesPerFish) {
        c.lists[*c.count] = limb.list;
        c.matrices[(*c.count)++] = m;
    }
    if (index == c.mouthLimb && c.mouth != nullptr) {
        MtxTransform(m, 500.0f, look.mouthY, 0.0f, c.mouth);
    }
    if (limb.child != 0xFF) {
        VisitLimb(c, limb.child, m);
    }
    if (limb.sibling != 0xFF) {
        VisitLimb(c, limb.sibling, parent);
    }
}

int PoseFish(const Skeleton& skeleton, bool loach, const Look& look, const float origin[3], float unit, MtxF* matrices,
             Gfx** lists, float* mouth) {
    MtxF base = MtxAt(origin);
    MtxRotateY(base, look.rotY * kBinToRad);
    MtxRotateX(base, look.rotX * kBinToRad);
    MtxRotateZ(base, look.rotZ * kBinToRad);
    MtxScale(base, look.scale[0] * unit, look.scale[1] * unit, look.scale[2] * unit);
    if (!loach) {
        MtxRotateY(base, look.limb23 * kBinToRad - kPi / 2.0f);
        MtxTranslate(base, 0.0f, 0.0f, look.limb23 * 10.0f * 0.01f);
    } else {
        MtxTranslate(base, 0.0f, 0.0f, 3000.0f);
        MtxRotateY(base, look.limb23 * kBinToRad);
        MtxTranslate(base, 0.0f, 0.0f, -3000.0f);
        MtxRotateY(base, -kPi / 2.0f);
    }
    int count = 0;
    PoseContext context = { &skeleton, loach,  &look, matrices,
                            lists,     &count, mouth, loach ? kLoachMouthLimb : kFishMouthLimb };
    VisitLimb(context, 0, base);
    return count;
}

int16_t LerpAngle(int16_t a, int16_t b, float t) {
    return (int16_t)(a + (int16_t)((int16_t)(b - a) * t));
}

Look Blend(const Look& a, const Look& b, float t) {
    Look out = b;
    for (int k = 0; k < 3; k++) {
        out.position[k] = a.position[k] + (b.position[k] - a.position[k]) * t;
        out.scale[k] = a.scale[k] + (b.scale[k] - a.scale[k]) * t;
    }
    out.rotX = LerpAngle(a.rotX, b.rotX, t);
    out.rotY = LerpAngle(a.rotY, b.rotY, t);
    out.rotZ = LerpAngle(a.rotZ, b.rotZ, t);
    out.limb23 = (int16_t)(a.limb23 + (b.limb23 - a.limb23) * t);
    out.limb4 = (int16_t)(a.limb4 + (b.limb4 - a.limb4) * t);
    out.limbD = (int16_t)(a.limbD + (b.limbD - a.limbD) * t);
    out.limbEF = (int16_t)(a.limbEF + (b.limbEF - a.limbEF) * t);
    out.limb89 = (int16_t)(a.limb89 + (b.limb89 - a.limb89) * t);
    for (int k = 0; k < 3; k++) {
        out.loach[k] = (int16_t)(a.loach[k] + (b.loach[k] - a.loach[k]) * t);
    }
    return out;
}

} // namespace

// Standard limbs: a position, a child and a sibling, and a display list
bool Data::Parse(Skeleton& skeleton, uint32_t skeletonOffset, uint32_t animationOffset, int limbCount,
                 const ListConverter& convert, std::string& error) {
    if (skeletonOffset + 8 > size || animationOffset + 16 > size) {
        error = "The fish weren't found";
        return false;
    }
    const uint32_t table = Be32(&object[skeletonOffset]);
    if ((table >> 24) != 6 || object[skeletonOffset + 4] != limbCount || (table & 0xFFFFFF) + limbCount * 4 > size) {
        error = "The fish's skeleton wasn't found";
        return false;
    }
    skeleton.limbCount = limbCount;
    for (int i = 0; i < limbCount; i++) {
        const uint32_t offset = Be32(&object[(table & 0xFFFFFF) + i * 4]) & 0xFFFFFF;
        if (offset + 12 > size) {
            error = "The fish's skeleton is damaged";
            return false;
        }
        const uint8_t* src = &object[offset];
        Limb& limb = skeleton.limbs[i];
        for (int k = 0; k < 3; k++) {
            limb.position[k] = (int16_t)Be16(src + k * 2);
        }
        limb.child = src[6];
        limb.sibling = src[7];
        const uint32_t list = Be32(src + 8);
        limb.list = nullptr;
        if (list != 0) {
            limb.list = convert(list);
            if (limb.list == nullptr) {
                error = "The fish couldn't be converted";
                return false;
            }
            skeleton.listCount++;
        }
        if ((limb.child != 0xFF && limb.child >= limbCount) || (limb.sibling != 0xFF && limb.sibling >= limbCount)) {
            error = "The fish's skeleton is damaged";
            return false;
        }
    }
    // The animation only ever shows its first frame: the swimming is the limbs being turned by the fish's update
    const uint32_t frameData = Be32(&object[animationOffset + 4]);
    const uint32_t jointIndices = Be32(&object[animationOffset + 8]);
    const uint32_t staticIndexMax = Be16(&object[animationOffset + 12]);
    if ((frameData >> 24) != 6 || (jointIndices >> 24) != 6 ||
        (jointIndices & 0xFFFFFF) + (size_t)(limbCount + 1) * 6 > size) {
        error = "The fish's animation is damaged";
        return false;
    }
    for (int joint = 0; joint <= limbCount; joint++) {
        for (int k = 0; k < 3; k++) {
            const uint32_t value = Be16(&object[(jointIndices & 0xFFFFFF) + joint * 6 + k * 2]);
            if ((frameData & 0xFFFFFF) + (size_t)value * 2 + 2 > size) {
                error = "The fish's animation is damaged";
                return false;
            }
            skeleton.joints[joint][k] = (int16_t)Be16(&object[(frameData & 0xFFFFFF) + value * 2]);
            (void)staticIndexMax;
        }
    }
    return true;
}

std::unique_ptr<School> Build(const uint8_t* objectFish, size_t size, ListConverter convert, std::string& error) {
    auto school = std::make_unique<School>();
    Data& data = *school->data;
    data.object = objectFish;
    data.size = size;
    if (!data.Parse(data.fishSkeleton, kFishSkeleton, kFishAnimation, kFishLimbCount, convert, error) ||
        !data.Parse(data.loachSkeleton, kLoachSkeleton, kLoachAnimation, kLoachLimbCount, convert, error)) {
        return nullptr;
    }
    if (data.fishSkeleton.listCount > kMatricesPerFish || data.loachSkeleton.listCount > kMatricesPerFish) {
        error = "The fish's skeleton wasn't what was expected";
        return nullptr;
    }
    return school;
}

// ---- Water ----

// The water (and the floor under it) at a place, in OoT units. False where there is none.
bool Data::WaterAtCell(float x, float z, float refY, float& surface, float& ground) {
    float s, g;
    if (world == nullptr || !world->WaterAt(x * kUnit, z * kUnit, refY * kUnit, &s, &g)) {
        return false;
    }
    surface = s / kUnit;
    ground = g / kUnit;
    return true;
}

// Brings the fish's idea of the surface and floor up to date. False if it is somewhere without enough water: the
// map's wall for a fish.
bool Data::Water(Fish& f) {
    float surface, ground;
    if (!WaterAtCell(f.position.x, f.position.z, f.surface, surface, ground) || surface - ground < 12.0f) {
        return false;
    }
    f.surface = surface;
    f.floor = ground;
    return true;
}

// Somewhere in the pond to swim to: usually not far from the fish, which lakes of any shape let it reach
void Data::PickTarget(Fish& f, float& x, float& z) {
    if (cells.empty()) {
        x = f.home.x;
        z = f.home.z;
        return;
    }
    const Cell* pick = nullptr;
    for (int attempt = 0; attempt < 12; attempt++) {
        const Cell& cell = cells[(size_t)rng.Zero((float)cells.size() - 0.001f)];
        pick = &cell;
        if (Sq(cell.x - f.position.x) + Sq(cell.z - f.position.z) < Sq(200.0f)) {
            break;
        }
    }
    x = pick->x + rng.Centered(20.0f);
    z = pick->z + rng.Centered(20.0f);
}

// Fishing_SplashBySize (without the drops): a splash of ripples where the fish breaks the surface
void Data::Splash(Fish& f, bool ignorePosCheck) {
    if (f.position.y < f.surface - 10.0f && !ignorePosCheck) {
        return;
    }
    const float at[3] = { f.position.x * kUnit, f.surface * kUnit, f.position.z * kUnit };
    world->Ripple(at, 100.0f, 800.0f, 150.0f);
}

// ---- Fishing_UpdateFish ----

Look Data::LookOf(const Fish& f) const {
    Look look;
    look.position[0] = f.position.x;
    look.position[1] = f.position.y;
    look.position[2] = f.position.z;
    look.rotX = (int16_t)(f.shapeRotX + f.pitch);
    look.rotY = (int16_t)(f.shapeRotY + f.yaw);
    look.rotZ = f.roll;
    look.limb23 = f.limb23;
    look.limb4 = f.limb4;
    look.limbD = f.limbD;
    look.limbEF = f.limbEF;
    look.limb89 = f.limb89;
    std::memcpy(look.loach, f.loachRot, sizeof(look.loach));
    look.mouthY = f.mouthY;
    if (!f.loach) {
        look.scale[0] = look.scale[1] = look.scale[2] = f.length * 15.0f * 0.00001f;
    } else {
        look.scale[2] = f.length * 65.0f * 0.000001f;
        look.scale[0] = look.scale[1] = look.scale[2] * 1.1f;
    }
    return look;
}

// Where the mouth is, for the lure and the line
void Data::SetMouth(Fish& f, const Look& look) {
    MtxF matrices[kMatricesPerFish];
    Gfx* lists[kMatricesPerFish];
    float mouth[3];
    PoseFish(f.loach ? loachSkeleton : fishSkeleton, f.loach, look, look.position, 1.0f, matrices, lists, mouth);
    f.mouth = { mouth[0], mouth[1], mouth[2] };
}

void Data::MakeLook(Fish& f) {
    f.from = f.to;
    f.to = LookOf(f);
    SetMouth(f, f.to);
}

void Data::TickFish(Fish& f) {
    // A player standing still: Fishing_UpdateFish's playerSpeedMod
    constexpr float kPlayerSpeedMod = 0.25f;
    const Frame& input = frame;

    f.previous = f.position;
    f.xzDistToPlayer = std::sqrt(Sq(player.x - f.position.x) + Sq(player.z - f.position.z));
    f.yawTowardsPlayer = Atan2S(player.z - f.position.z, player.x - f.position.x);
    const bool inWater = Water(f);

    f.stateAndTimer++;
    for (int16_t& timer : f.timers) {
        if (timer != 0) {
            timer--;
        }
    }
    if (f.scaredLong != 0) {
        f.scaredLong--;
    }
    if (f.scaredShort != 0) {
        f.scaredShort--;
    }
    if (f.bumpTimer != 0) {
        f.bumpTimer--;
    }
    if (f.wanderTimer != 0) {
        f.wanderTimer--;
    }

    ApproachF(f.phaseStep, f.phaseStepTarget, 1.0f, 0.2f);
    if (f.state == kHeld) {
        ApproachF(f.phaseMag, f.phaseMagTarget, 0.2f, 200.0f);
    } else {
        float scale = 1.0f;
        float stepScale = 1.0f;
        if (f.position.y > f.surface) {
            scale = 1.5f;
            stepScale = 3.0f;
        }
        ApproachF(f.phaseMag, f.phaseMagTarget * scale, 1.0f, 500.0f * stepScale);
    }
    ApproachS(f.limbD, 0, 5, 500);

    if (!f.loach) {
        f.phase += f.phaseStep;
        f.limb23 = (int16_t)((int)(std::cos(f.phase) * f.phaseMag) + f.bank);
        f.limb4 = (int16_t)((int)(std::cos(f.phase + -1.2f) * f.phaseMag * 1.6f) + f.bank);
    } else {
        f.phase += f.phaseStep * 0.8f;
        for (int i = 0; i < 3; i++) {
            f.loachRot[i] = (int16_t)((int)(std::cos(f.phase + (i * 2.1f)) * f.phaseMag * 2.0f) + f.bank);
        }
        f.limb23 = (int16_t)(std::cos(f.phase + 0.4f) * f.phaseMag * 2.0f * 0.6f);
    }

    if (f.tossed) {
        // Thrown back: a short arc to the water, with the body flexing hard
        f.tossTick++;
        const float t = std::min(1.0f, (float)f.tossTick / (float)kTossTicks);
        f.position.x = f.tossFrom.x + (f.tossTo.x - f.tossFrom.x) * t;
        f.position.z = f.tossFrom.z + (f.tossTo.z - f.tossFrom.z) * t;
        f.position.y = f.tossFrom.y + (f.tossTo.y - f.tossFrom.y) * t + kTossHeight * 4.0f * t * (1.0f - t);
        f.pitch = (int16_t)(-0x2000 + (int)(t * 0x4000));
        f.phaseStepTarget = 1.6f;
        f.phaseMagTarget = 6000.0f;
        if (f.tossTick >= kTossTicks) {
            f.tossed = false;
            f.position = f.tossTo;
            Splash(f, true);
            // Fishing_UpdateFish case 6, when the fish has been let go
            f.state = f.stateNext = kSwim;
            f.scaredLong = 10000;
            f.scaredShort = 500;
            f.timers[1] = 50;
            f.timers[0] = 0;
            f.phaseStepTarget = 1.0f;
            f.phaseMagTarget = 2000.0f;
            f.speed = 2.0f;
            f.pitch = f.roll = f.yaw = 0;
            f.hop = 0.0f;
        }
        return;
    }
    if (f.state == kHeld) {
        // The player has it by the tail: Draw puts it in the hand
        ApproachS(f.limbD, 11000, 2, 4000);
        f.pitch = -0x4000;
        f.roll = f.yaw = 0;
        f.shapeRotX = 0;
        f.bank = 0;
        return;
    }

    float dx = f.target.x - f.position.x;
    float dy = f.target.y - f.position.y;
    float dz = f.target.z - f.position.z;
    int16_t rotYTarget = Atan2S(dz, dx);
    float distToTarget = std::sqrt(Sq(dx) + Sq(dz));
    int16_t rotXTarget = Atan2S(distToTarget, dy);
    distToTarget = std::sqrt(Sq(dx) + Sq(dz) + Sq(dy));

    if (f.bumpTimer != 0 && f.state != kInspect && f.state != kNibble && f.state != kStrike) {
        if ((f.stateAndTimer & 0x40) != 0) {
            rotYTarget = (int16_t)(rotYTarget + 0x4000);
        } else {
            rotYTarget = (int16_t)(rotYTarget - 0x4000);
        }
        if (((f.stateAndTimer + 0x20) & 0x40) != 0) {
            rotXTarget = (int16_t)(rotXTarget + 0x2000);
        } else {
            rotXTarget = (int16_t)(rotXTarget - 0x2000);
        }
    }

    int rotXYScale = 10;
    const bool lureResting = input.castState == 3 && castState == 0;

    auto startInspect = [&]() {
        f.state = kInspect;
        f.unk15E = 0;
        f.timers[0] = 0;
        f.timers[2] = (int16_t)((int)rng.Zero(100.0f) + 100);
        f.perception = f.basePerception;
        f.rotationStep = 0.0f;
    };
    // func_80B70ED4: does the fish notice the lure? Only fish that are idle do
    auto noticeLure = [&]() {
        const float lx = lure.x - f.position.x;
        const float ly = lure.y - f.position.y;
        const float lz = lure.z - f.position.z;
        const float lineLengthSq = Sq(lx) + Sq(ly) + Sq(lz);
        if (lureResting && f.scaredShort == 0) {
            // The lure's place relative to the way the fish faces
            MtxF turn = MtxIdentity();
            MtxRotateY(turn, -f.shapeRotY * kBinToRad);
            float ahead[3];
            MtxTransform(turn, lx, ly, lz, ahead);
            if (ahead[2] > 0.0f || f.length < 40.0f) {
                // Reeling or twitching the lure catches a fish's eye from farther away
                if ((input.a || input.wiggling) && lineLengthSq < Sq(120.0f)) {
                    startInspect();
                }
                if (f.timers[1] == 0 && lineLengthSq < Sq(70.0f)) {
                    startInspect();
                }
            }
        }
        // A big splash right next to a big fish scares it (D_80B7E114)
        if (input.lureSplash && f.length > 60.0f && lineLengthSq < Sq(30.0f) && f.state >= kHome) {
            f.stateNext = 0;
            f.state = kFlee;
            f.scaredLong = 1000;
            f.scaredShort = 100;
            f.timers[1] = 50;
        }
    };
    auto startScared = [&](int16_t longTimer, int16_t shortTimer) {
        f.stateNext = f.state = kSwim;
        f.scaredLong = longTimer;
        f.scaredShort = shortTimer;
        f.timers[1] = 50;
    };

    switch (f.state) {
        case kHome:
            f.target = f.home;
            ApproachF(f.speed, 2.0f, 1.0f, 0.5f);
            ApproachF(f.rotationStep, 4096.0f, 1.0f, 256.0f);
            if (distToTarget < 40.0f) {
                f.state = kRest;
                f.phaseStepTarget = 0.4f;
                f.phaseMagTarget = 500.0f;
            }
            noticeLure();
            if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod) {
                startScared(1000, 200);
            }
            break;

        case kRest:
            f.target = f.home;
            ApproachF(f.speed, 0.0f, 1.0f, 0.05f);
            ApproachF(f.rotationStep, 0.0f, 1.0f, 256.0f);
            if (distToTarget >= 40.0f) {
                f.state = kHome;
                f.phaseStepTarget = 1.0f;
                f.phaseMagTarget = 2000.0f;
            }
            noticeLure();
            if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod) {
                startScared(1000, 200);
            }
            // Not in OoT: a resting fish moves on to somewhere else in the pond now and then, so they wander
            if (f.wanderTimer == 0 && f.state == kRest) {
                f.wanderTimer = (int)rng.Zero(200.0f) + 100;
                float x, z;
                PickTarget(f, x, z);
                f.home.x = x;
                f.home.z = z;
                f.home.y =
                    std::clamp(f.surface - 12.0f - rng.Zero(40.0f), f.floor + f.length * 0.1f + 6.0f, f.surface - 8.0f);
                f.state = kHome;
                f.phaseStepTarget = 1.0f;
                f.phaseMagTarget = 2000.0f;
            }
            break;

        case kSwim:
            ApproachF(f.speed, 1.0f, 1.0f, 0.05f);
            ApproachF(f.rotationStep, 0.0f, 1.0f, 256.0f);
            if (f.timers[0] == 0) {
                if (f.scaredLong == 0) {
                    f.state = f.stateNext = kHome;
                } else {
                    f.state = kFlee;
                    f.timers[0] = (int16_t)((int)rng.Zero(30.0f) + 10);
                    PickTarget(f, f.target.x, f.target.z);
                    f.target.y = std::clamp((f.surface - 50.0f) - rng.Zero(50.0f), f.floor + f.length * 0.1f + 6.0f,
                                            f.surface - 8.0f);
                    f.phaseStepTarget = 1.0f;
                    f.phaseMagTarget = 2000.0f;
                }
            }
            break;

        case kFlee:
            if (f.loach) {
                f.state = kLoachDeep;
                f.scaredLong = 20000;
                f.scaredShort = 20000;
                PickTarget(f, f.target.x, f.target.z);
                f.target.y = f.floor + 6.0f;
            } else {
                ApproachF(f.rotationStep, 4096.0f, 1.0f, 256.0f);
                if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod || f.timers[1] != 0) {
                    ApproachF(f.rotationStep, 8192.0f, 1.0f, 768.0f);
                    ApproachF(f.speed, 4.2f, 1.0f, 0.75f);
                    f.phaseStepTarget = 1.2f;
                    f.phaseMagTarget = 4000.0f;
                    f.timers[0] = 20;
                } else {
                    f.phaseStepTarget = 1.0f;
                    f.phaseMagTarget = 2000.0f;
                    ApproachF(f.speed, 1.5f, 1.0f, 0.1f);
                }
                if (f.timers[0] == 0 || distToTarget < 50.0f) {
                    f.state = kSwim;
                    f.timers[0] = (int16_t)((int)rng.Zero(30.0f) + 3);
                    f.phaseStepTarget = 1.0f;
                    f.phaseMagTarget = 500.0f;
                }
            }
            break;

        case kLoachDeep:
            ApproachS(f.rotationTarget[0], 0, 0x14, 0x20);
            if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod || f.timers[1] != 0) {
                ApproachF(f.speed, 3.0f, 1.0f, 0.75f);
                f.phaseStepTarget = 1.0f;
                f.timers[0] = 20;
                f.phaseMagTarget = 4000.0f;
                ApproachF(f.rotationStep, 4096.0f, 1.0f, 256.0f);
                if ((ticks % 32) == 0) {
                    PickTarget(f, f.target.x, f.target.z);
                    f.target.y = f.floor + 6.0f;
                }
            } else if (distToTarget > 50.0f) {
                f.phaseStepTarget = 0.8f;
                f.phaseMagTarget = 1500.0f;
                ApproachF(f.speed, 1.0f, 1.0f, 0.1f);
                ApproachF(f.rotationStep, 2048.0f, 1.0f, 128.0f);
            } else {
                f.phaseStepTarget = 0.4f;
                f.phaseMagTarget = 500.0f;
                ApproachZeroF(f.speed, 1.0f, 0.02f);
                ApproachF(f.rotationStep, 0.0f, 1.0f, 256.0f);
            }
            if (f.scaredLong == 0) {
                f.state = f.stateNext = kHome;
            } else if ((f.scaredLong & 0x7FF) == 0 && f.scaredLong < 15000) {
                f.state = kLoachSurface;
                f.worldRotX = f.shapeRotX = 0;
                f.target.y = f.surface + 10.0f;
                PickTarget(f, f.target.x, f.target.z);
            }
            break;

        case kLoachSurface:
            if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod || f.timers[1] != 0) {
                f.state = kLoachDeep;
                f.target.y = f.floor + 6.0f;
            } else {
                f.phaseStepTarget = 0.6f;
                f.phaseMagTarget = 1000.0f;
                ApproachS(f.rotationTarget[0], -0x1000, 0x14, 0x100);
                if (f.position.y < f.surface - 20.0f) {
                    ApproachF(f.speed, 0.5f, 1.0f, 0.1f);
                } else {
                    ApproachZeroF(f.speed, 1.0f, 0.01f);
                    if (f.speed == 0.0f || f.position.y > f.surface - 5.0f) {
                        PickTarget(f, f.target.x, f.target.z);
                        f.target.y = f.floor + 10.0f;
                        f.state = kLoachSettle;
                        f.rotationStep = 0.0f;
                        const float at[3] = { f.mouth.x * kUnit, f.surface * kUnit, f.mouth.z * kUnit };
                        world->Ripple(at, 10.0f, 300.0f, 150.0f);
                        world->Ripple(at, 30.0f, 400.0f, 150.0f);
                        break;
                    }
                }
                ApproachF(f.rotationStep, 2048.0f, 1.0f, 128.0f);
            }
            break;

        case kLoachSettle:
            if (f.xzDistToPlayer < 250.0f * kPlayerSpeedMod || f.timers[1] != 0) {
                f.state = kLoachDeep;
                f.target.y = f.floor + 6.0f;
            } else {
                ApproachS(f.rotationTarget[0], 0x1000, 0x14, 0x6A);
                if (distToTarget > 40.0f) {
                    f.phaseStepTarget = 0.7f;
                    f.phaseMagTarget = 1200.0f;
                    ApproachF(f.speed, 0.5f, 1.0f, 0.01f);
                    ApproachF(f.rotationStep, 2048.0f, 1.0f, 128.0f);
                } else {
                    f.state = kLoachDeep;
                }
            }
            break;

        case kInspect: {
            // Over to one side of the lure or the other
            const float side = ((f.index + lureSide) & 1) != 0 ? 10.0f : -10.0f;
            MtxF turn = MtxIdentity();
            MtxRotateY(turn, input.lureYaw);
            float offset[3];
            MtxTransform(turn, side, 0.0f, 0.0f, offset);
            f.target.x = lure.x + offset[0];
            f.target.z = lure.z + offset[2];
            f.target.y = lure.y - (f.loach ? 5.0f : 15.0f);
            if (f.target.y <= f.floor) {
                f.target.y = f.floor + 3.0f;
            }
            if (f.position.y > f.target.y) {
                ApproachF(f.position.y, f.target.y, 0.1f, (f.position.y - f.target.y) * 0.1f);
            }
            ApproachF(f.rotationStep, 8192.0f, 1.0f, 384.0f);
            if (aPressed) {
                f.perception += 0.005f;
            }
            if (wiggle != 0) {
                f.perception += wiggle == 1 ? 0.01f : 0.05f;
                wiggle = 0;
            }
            if (bPressed) {
                f.perception += 0.008f;
            }

            if (distToTarget < (f.length * 0.5f) + 20.0f) {
                if (f.unk15E == 0) {
                    f.phaseStepTarget = 1.0f;
                    f.phaseMagTarget = 500.0f;
                    f.timers[0] = (int16_t)((int)rng.Zero(10.0f) + 2);
                }
                ApproachF(f.speed, -0.2f, 1.0f, 0.1f);
                f.unk15E = 1;
            } else {
                if (f.unk15E != 0) {
                    f.phaseStepTarget = 1.0f;
                    f.rotationStep = 0.0f;
                    f.phaseMagTarget = 3000.0f;
                }
                ApproachF(f.speed, 3.0f, 1.0f, 0.15f);
                f.unk15E = 0;
            }

            float multiplier = f.length >= 60.0f ? 0.3f : f.length >= 45.0f ? 0.6f : 1.0f;
            const float chance = 0.03f * multiplier;
            if ((f.timers[0] == 1 || rng.One() < chance) && rng.One() < f.perception * multiplier) {
                if (!f.loach) {
                    f.state = kNibble;
                    f.phaseStepTarget = 1.2f;
                    f.phaseMagTarget = 5000.0f;
                    f.timers[0] = (int16_t)rng.Zero(10.0f);
                } else {
                    f.state = kLoachBite;
                    f.phaseStepTarget = 1.0f;
                    f.phaseMagTarget = 3000.0f;
                    f.timers[0] = 40;
                }
                f.speedTarget = rng.Zero(1.5f) + 4.5f;
            }

            if (!lureResting || f.timers[2] == 0) {
                f.state = f.stateNext;
                f.timers[1] = (int16_t)((int)rng.Zero(30.0f) + 50);
                f.timers[0] = (int16_t)((int)rng.Zero(10.0f) + 5);
                f.phaseStepTarget = 1.0f;
                f.rotationStep = 0.0f;
                f.phaseMagTarget = 2000.0f;
            }
            if (f.xzDistToPlayer < 100.0f * kPlayerSpeedMod) {
                startScared(1000, 200);
            }
            break;
        }

        case kNibble: {
            rotXYScale = 2;
            const float side = (((int)(player.x) + lureSide) & 1) != 0 ? 30.0f : -30.0f;
            MtxF turn = MtxIdentity();
            MtxRotateY(turn, input.lureYaw);
            float offset[3];
            MtxTransform(turn, side, 0.0f, 30.0f, offset);
            f.target.x = lure.x + offset[0];
            f.target.z = lure.z + offset[2];
            f.target.y = lure.y - 10.0f;
            f.rotationStep = 4096.0f;
            ApproachF(f.speed, f.speedTarget * 0.8f, 1.0f, 1.0f);
            if (!lureResting || lure.y > f.surface + 5.0f) {
                f.state = f.stateNext;
                f.timers[0] = 0;
                f.phaseStepTarget = 1.0f;
                f.phaseMagTarget = 2000.0f;
            } else if (f.timers[0] == 0 || distToTarget < 30.0f) {
                f.state = kStrike;
                f.target = lure;
                f.rotationStep = 16384.0f;
                f.phaseStepTarget = 1.2f;
                f.phaseMagTarget = 5000.0f;
                f.timers[0] = 20;
            }
            break;
        }

        case kStrike:
            ApproachF(f.rotationStep, 16384.0f, 1.0f, 4096.0f);
            ApproachS(f.limbD, 0x4E20, 4, 0x1388);
            rotXYScale = 2;
            f.target = lure;
            ApproachF(f.speed, f.speedTarget, 1.0f, 1.0f);
            if (!lureResting || f.timers[0] == 0 || lure.y > f.surface + 5.0f) {
                f.timers[0] = 0;
                f.state = f.stateNext;
                f.phaseStepTarget = 1.0f;
                f.phaseMagTarget = 2000.0f;
            } else if (distToTarget < 10.0f) {
                Splash(f, false);
                f.state = kHooked;
                f.phaseStepTarget = 1.2f;
                f.phaseMagTarget = 5000.0f;
                f.timers[1] = 150;
                f.timers[0] = 0;
                f.timers[2] = 0;
                f.timers[3] = 120;
                castState = 4;
                hooked = f.index;
                f.mouthY = 500.0f - rng.Zero(400.0f);
                int16_t timer;
                if (f.length > 70.0f) {
                    timer = (int16_t)((int)rng.Zero(5.0f) + 10);
                } else if (f.length > 60.0f) {
                    timer = (int16_t)((int)rng.Zero(5.0f) + 15);
                } else if (f.length > 50.0f) {
                    timer = (int16_t)((int)rng.Zero(5.0f) + 17);
                } else {
                    timer = (int16_t)((int)rng.Zero(5.0f) + 25);
                }
                bitTimer = timer;
                lineHooked = false;
                tension = 0.0f;
                pullback = 100;
                fightTime = 0;
                lineLength = std::sqrt(Sq(tip.x - f.position.x) + Sq(tip.z - f.position.z)) + kBiteSlack;
                reelSpeed = 0.5f;
                result.bit = true;
            }
            break;

        case kLoachBite:
            f.target = lure;
            ApproachF(f.speed, 2.0f, 1.0f, 1.0f);
            if (!lureResting || f.timers[0] == 0 || lure.y > f.surface + 5.0f) {
                f.timers[0] = 0;
                f.state = f.stateNext;
                f.phaseStepTarget = 1.0f;
                f.phaseMagTarget = 2000.0f;
            } else if (distToTarget < 10.0f) {
                Splash(f, false);
                f.state = kHooked;
                f.phaseStepTarget = 1.2f;
                f.phaseMagTarget = 5000.0f;
                f.timers[1] = 150;
                f.timers[0] = 0;
                f.timers[2] = 0;
                f.timers[3] = 120;
                castState = 4;
                hooked = f.index;
                bitTimer = 30;
                lineHooked = false;
                tension = 0.0f;
                pullback = 100;
                fightTime = 0;
                lineLength = std::sqrt(Sq(tip.x - f.position.x) + Sq(tip.z - f.position.z)) + kBiteSlack;
                reelSpeed = 0.5f;
                result.bit = true;
            }
            break;

        case kHooked:
            FightTick(f);
            break;

        default:
            break;
    }

    ApproachS(f.limbEF, (int16_t)(SinS((int16_t)(f.stateAndTimer * 0x1000)) * 5000.0f + 5000.0f), 2, 0x7D0);

    if (f.state == kHeld) {
        return;
    }

    // ---- Moving ----
    int rotXScale, rotYScale, rotZScale, rotXStep, rotYStep;
    if (f.position.y > f.surface) {
        f.phaseStepTarget = 1.5f;
        f.phaseMagTarget = 5000.0f;
        ApproachS(f.bank, 0, 5, 0x7D0);
        rotXScale = rotYScale = rotZScale = 3;
        rotXStep = rotYStep = 0x2000;
        f.timers[2] = 0;
        f.hop -= 1.0f;
    } else {
        ApproachZeroF(f.hop, 1.0f, 2.0f);
        if (f.state != kLoachDeep && f.state != kLoachSurface && f.state != kLoachSettle) {
            f.rotationTarget[0] = 0;
        }
        f.rotationTarget[1] = f.rotationTarget[2] = 0;
        rotXScale = rotYScale = rotZScale = 4;
        rotXStep = rotYStep = 0x2000;

        int bankTarget = (int)(SmoothStepToS(f.worldRotY, rotYTarget, rotXYScale, (int)f.rotationStep) * 3.0f);
        ApproachS(f.worldRotX, rotXTarget, rotXYScale, (int)(f.rotationStep * 0.5f));
        bankTarget = (int16_t)bankTarget;
        bankTarget = std::clamp(bankTarget, -8000, 8000);
        if (f.speed >= 3.2f) {
            ApproachS(f.bank, (int16_t)bankTarget, 2, 20000);
        } else {
            ApproachS(f.bank, (int16_t)bankTarget, 3, 3000);
        }
        // Actor_UpdateVelocityXYZ
        const float speedXZ = CosS(f.worldRotX) * f.speed;
        f.velocity.x = SinS(f.worldRotY) * speedXZ;
        f.velocity.y = SinS(f.worldRotX) * f.speed;
        f.velocity.z = CosS(f.worldRotY) * speedXZ;
    }

    // Actor_UpdatePos
    f.position.x += f.velocity.x;
    f.position.y += f.velocity.y;
    f.position.z += f.velocity.z;
    f.position.y += f.hop * 1.5f;

    if (f.turnTimer != 0) {
        f.rotationTarget[1] = f.turnAngle;
        f.turnTimer--;
        if (f.turnHard) {
            rotYScale = 5;
            rotYStep = 0x4000;
        } else {
            rotYScale = 10;
            rotYStep = 0x800;
        }
        f.rotationTarget[0] = (int16_t)(-0x500 - f.shapeRotX);
        rotXScale = 5;
        rotXStep = 0x4000;
    } else {
        f.turnHard = false;
    }
    ApproachS(f.pitch, f.rotationTarget[0], rotXScale, rotXStep);
    ApproachS(f.yaw, f.rotationTarget[1], rotYScale, rotYStep);
    ApproachS(f.roll, f.rotationTarget[2], rotZScale, 0x2000);

    if (f.speed <= 0.5f) {
        ApproachS(f.shapeRotX, 0, 10, f.settle);
        ApproachS(f.settle, 0x500, 1, 0x20);
    } else {
        ApproachS(f.shapeRotX, (int16_t)-f.worldRotX, 10, 0x1000);
        f.settle = 0;
    }
    f.shapeRotY = f.worldRotY;

    const bool loachState = f.state == kLoachDeep || f.state == kLoachSurface || f.state == kLoachSettle;
    if (!loachState) {
        if (f.position.y > f.surface && f.previous.y <= f.surface) {
            Splash(f, true);
            f.hop = f.velocity.y;
            f.velocity.y = 0.0f;
            f.rotationTarget[2] = (int16_t)(int)rng.Centered(32768.0f);
        } else if (f.position.y < f.surface && f.previous.y >= f.surface) {
            if (f.hop < -5.0f) {
                f.hop = -5.0f;
            }
            f.worldRotX = -0xFA0;
            Splash(f, true);
        }
    }

    if (f.position.y < f.surface && f.position.y > f.surface - 10.0f) {
        if ((f.stateAndTimer & 1) == 0 && f.speed > 0.0f) {
            const float at[3] = { f.position.x * kUnit, f.surface * kUnit, f.position.z * kUnit };
            world->Ripple(at, 80.0f, 500.0f, 150.0f);
        }
    }

    // Actor_UpdateBgCheckInfo: the map's floor and walls (the walls being the edge of the water)
    f.wall = false;
    if (f.speed > 0.0f || f.state == kHooked) {
        const float below = f.length * 0.1f;
        float surface, ground;
        if (WaterAtCell(f.position.x, f.position.z, f.surface, surface, ground) && surface - ground >= 12.0f) {
            f.floor = ground;
            f.surface = surface;
            if (f.position.y - below < ground) {
                f.position.y = ground + below;
                f.velocity.y = std::max(f.velocity.y, 0.0f);
                if (f.position.y > f.surface) {
                    // not reachable: the fish is above the water at the floor
                }
            }
        } else {
            // Out of the water: the fish slides along the shore if one of its two directions is still open, and
            // otherwise stays where it was; either way it turns away
            const float x = f.position.x, z = f.position.z;
            f.wall = true;
            f.bumpTimer = 20;
            if (WaterAtCell(x, f.previous.z, f.surface, surface, ground) && surface - ground >= 12.0f) {
                f.position.z = f.previous.z;
                f.velocity.z = 0.0f;
            } else if (WaterAtCell(f.previous.x, z, f.surface, surface, ground) && surface - ground >= 12.0f) {
                f.position.x = f.previous.x;
                f.velocity.x = 0.0f;
            } else {
                f.position.x = f.previous.x;
                f.position.z = f.previous.z;
                f.velocity.x = f.velocity.z = 0.0f;
            }
        }
    }
    if (!inWater && !f.wall) {
        f.bumpTimer = 20;
    }
}

// ---- Fishing_UpdateFish, case 5: a fish that has the lure ----

void Data::EndFight() {
    castState = 0;
    hooked = -1;
    lineHooked = false;
    tension = 0.0f;
    bitTimer = 0;
    hitTimer = 0;
    fight.tension = 0.0f;
    fight.active = false;
    fight.hooked = false;
    fight.caught = false;
    fight.pulling = false;
}

void Data::FightTick(Fish& f) {
    const Frame& input = frame;
    fightTime++;
    ApproachS(f.limbD, 0x2AF8, 4, 0xBB8);

    // The hook is set by pulling back on the stick or by pressing A, while the fish is biting
    if (bitTimer != 0 && !lineHooked) {
        if (kick || aPressed) {
            if (kick) {
                // Pulling back swings the fish round toward the player
                const float turn = 40.0f - ((f.length - 30.0f) * 1.333333f);
                if (turn > 0.0f) {
                    f.turnTimer = (int16_t)turn;
                    f.turnAngle = (int16_t)(f.yawTowardsPlayer - f.shapeRotY);
                    f.turnHard = true;
                }
            }
            f.phaseStep = 1.7f;
            f.phaseMag = 7000.0f;
            lineHooked = true;
            hitTimer = 10;
            result.hookSet = true;
        }
    }

    // How hard the fish pulls against the reel this tick (0 to 1): all of it while it runs, what the reel loses
    // while it fights, none once it tires or is out of the water
    float pull = 0.0f;
    if (f.position.y < f.surface) {
        const bool burst = f.timers[1] > 30;
        if (burst) {
            if (f.timers[0] == 0) {
                // Away from the player, over water
                float x = f.position.x, z = f.position.z;
                for (int attempt = 0; attempt < 24; attempt++) {
                    MtxF turn = MtxIdentity();
                    MtxRotateY(turn,
                               rng.Centered(3.0f * kPi / 4.0f) + (int16_t)(f.yawTowardsPlayer + 0x8000) * kBinToRad);
                    float offset[3];
                    MtxTransform(turn, 0.0f, 0.0f, 200.0f, offset);
                    x = f.position.x + offset[0];
                    z = f.position.z + offset[2];
                    float surface, ground;
                    if (WaterAtCell(x, z, f.surface, surface, ground) && surface - ground >= kMinDepth * 0.6f) {
                        break;
                    }
                    x = f.position.x;
                    z = f.position.z;
                }
                f.target.x = x;
                f.target.z = z;

                if (rng.One() < 0.1f && f.timers[3] == 0) {
                    // A run, and a leap
                    f.timers[0] = 20;
                    f.timers[1] = 100;
                    f.timers[2] = 20;
                    f.timers[3] = 100;
                    f.target.y = f.surface + 150.0f;
                    pullback = (int)rng.Zero(30.0f) + 20;
                } else {
                    f.timers[0] = (int16_t)((int)rng.Zero(10.0f) + 3);
                    f.timers[2] = 0;
                    f.target.y = std::clamp(f.surface - 40.0f - rng.Zero(110.0f), f.floor + f.length * 0.1f + 6.0f,
                                            f.surface - 8.0f);
                }
            }

            if (f.timers[2] != 0) {
                // The fish is running: the reel does nothing
                reelSpeed = 0.0f;
                f.phaseStepTarget = 1.6f;
                f.phaseMagTarget = 6000.0f;
                ApproachF(f.speed, 7.5f, 1.0f, 1.0f);
                ApproachS(f.limbD, 20000, 2, 4000);
            } else {
                f.phaseStepTarget = 1.4f;
                f.phaseMagTarget = 5000.0f;
                ApproachF(f.speed, 5.0f, 1.0f, 0.5f);
                reelSpeed = f.loach ? 1.0f - (f.length * 0.00899f * 1.4f) : 1.0f - (f.length * 0.00899f);
            }
            pull = std::clamp(1.0f - reelSpeed, 0.0f, 1.0f);
        } else {
            // Tiring: it turns toward the player when A is held, and the reel gains
            if ((f.timers[1] & 0xF) == 0 && input.a && (!(f.length >= 60.0f) || fightTime >= 2000)) {
                f.turnTimer = (int16_t)((int)rng.Zero(30.0f) + 15);
                f.turnAngle = (int16_t)(f.yawTowardsPlayer - f.shapeRotY);
            }
            f.phaseStepTarget = 1.0f;
            f.phaseMagTarget = 4500.0f;
            reelSpeed = f.loach ? 1.3f - (f.length * 0.00899f * 1.4f) : 1.3f - (f.length * 0.00899f);
            ApproachF(f.speed, 2.0f, 1.0f, 0.5f);
            if (f.timers[1] == 0) {
                f.turnTimer = 0;
                if (fightTime < 2000) {
                    f.timers[1] = (int16_t)((int)rng.Zero(50.0f) + 50);
                } else if (fightTime < 3000) {
                    f.timers[1] = (int16_t)((int)rng.Zero(20.0f) + 30);
                } else {
                    f.timers[1] = (int16_t)((int)rng.Zero(10.0f) + 25);
                }
            }
        }
    }

    // How hard the rod is bent by it
    if (f.speed < 3.0f) {
        fight.bendTarget = (ticks & 8) != 0 ? -0.8f : -0.75f;
    } else {
        fight.bendTarget = (ticks & 4) != 0 ? -0.9f : -0.85f;
    }
    fight.pulling = true;

    // The line: the fish can't get farther from the rod than the line is let out, and is pulled back when it tries
    const float lx = f.position.x - tip.x;
    const float lz = f.position.z - tip.z;
    const float distance = std::sqrt(Sq(lx) + Sq(lz));
    if (distance > lineLength && distance > 0.001f) {
        const float step = std::max(2.0f * (f.speed * 1.5f), 6.0f);
        const float tx = tip.x + lx / distance * lineLength;
        const float tz = tip.z + lz / distance * lineLength;
        ApproachF(f.position.x, tx, 0.2f, step);
        ApproachF(f.position.z, tz, 0.2f, step);
    }

    // Keeping the line tight: A or the stick pulled back
    if (input.a || input.stickY * 80.0f < -30.0f) {
        if (pullback < 100) {
            pullback++;
        }
    } else if (pullback != 0) {
        pullback--;
    }

    // Tension: a big fish pulls harder than a small one, so the same reeling strains the line more
    if (lineHooked) {
        const float strength = f.length * kPullPerInch * (f.loach ? 1.4f : 1.0f);
        const float power = std::clamp(Sq(strength / kStrongPull), 0.0f, 1.5f);
        if (input.a && pull > 0.0f) {
            tension = std::min(kTensionLimit, tension + kTensionRise * pull * power);
        } else {
            tension = std::max(0.0f, tension - kTensionEase);
        }
    }
    const bool snapped = lineHooked && tension >= kTensionLimit;

    const bool missed = bitTimer == 0 && !lineHooked;
    if (snapped || fightTime >= 6000 || missed || pullback == 0 || ((ticks & 0x7F) == 0 && rng.One() < 0.05f)) {
        // It gets away: the line goes slack
        result.gotAway = true;
        result.snapped = snapped;
        result.missed = missed;
        f.state = f.stateNext = kSwim;
        f.scaredLong = 10000;
        f.scaredShort = 500;
        f.timers[1] = 50;
        f.timers[0] = 0;
        f.phaseStepTarget = 1.0f;
        f.phaseMagTarget = 3000.0f;
        f.turnTimer = 0;
        fight.mouth[0] = f.mouth.x * kUnit;
        fight.mouth[1] = f.mouth.y * kUnit;
        fight.mouth[2] = f.mouth.z * kUnit;
        EndFight();
    } else if (f.xzDistToPlayer < kCatchDistance || lineLength <= 15.0f) {
        f.state = kHeld;
        f.timers[0] = 100;
        castState = 5;
        result.caught = true;
        fight.caught = true;
        f.phaseStepTarget = 1.0f;
        f.phaseMagTarget = 500.0f;
        f.phaseMag = 5000.0f;
        if (f.position.y <= f.surface) {
            Splash(f, true);
        }
    }
}

// ---- All the fish, a game tick at a time ----

void Data::Tick() {
    ticks++;
    if (bitTimer != 0) {
        bitTimer--;
    }
    if (hitTimer != 0) {
        hitTimer--;
    }
    if (castState == 0) {
        fight.pulling = false;
    }
    for (Fish& f : fish) {
        if (f.active) {
            TickFish(f);
        }
    }
    // The reel: the line comes in at the fish's own pace, which is none while it runs (UpdateLure, case 4)
    if (castState == 4 && frame.a) {
        lineLength = std::max(0.0f, lineLength - std::max(reelSpeed, 0.0f) * 5.0f);
    }
    for (Fish& f : fish) {
        if (f.active) {
            MakeLook(f);
        }
    }
    aPressed = bPressed = kick = false;
    wiggle = 0;
}

// ---- The pond ----

void Clear(School& school) {
    Data& d = *school.data;
    for (Fish& f : d.fish) {
        f = Fish();
    }
    d.cells.clear();
    d.accumulated = 0.0f;
    d.aPressed = d.bPressed = d.kick = false;
    d.wiggle = 0;
    d.EndFight();
    d.fight = Fight();
}

bool HasFish(const School& school) {
    for (const Fish& f : school.data->fish) {
        if (f.active) {
            return true;
        }
    }
    return false;
}

int Spawn(School& school, World& world, const float center[3], const float player[3], float playerYaw) {
    Data& d = *school.data;
    Clear(school);
    d.world = &world;

    // The water around the place the lure is likely to land: the cells connected to it, with the water's depth
    constexpr int kWidth = kCellRadius * 2 + 1;
    struct Spot {
        bool valid = false;
        bool visited = false;
        float surface = 0.0f, ground = 0.0f;
    };
    std::vector<Spot> grid(kWidth * kWidth);
    auto at = [&](int ix, int iz) -> Spot& { return grid[iz * kWidth + ix]; };
    int seedX = -1, seedZ = -1;
    float seedDistance = 1e30f;
    for (int iz = 0; iz < kWidth; iz++) {
        for (int ix = 0; ix < kWidth; ix++) {
            const float x = center[0] + (ix - kCellRadius) * kCellSize;
            const float z = center[2] + (iz - kCellRadius) * kCellSize;
            float surface, ground;
            Spot& spot = at(ix, iz);
            if (world.WaterAt(x, z, center[1] + 40.0f, &surface, &ground) && surface - ground >= kMinDepth * kUnit &&
                std::abs(surface - center[1]) < 60.0f) {
                spot.valid = true;
                spot.surface = surface;
                spot.ground = ground;
                const float distance = Sq(x - center[0]) + Sq(z - center[2]);
                if (distance < seedDistance) {
                    seedDistance = distance;
                    seedX = ix;
                    seedZ = iz;
                }
            }
        }
    }
    if (seedX < 0) {
        return 0;
    }
    std::vector<std::pair<int, int>> open = { { seedX, seedZ } };
    at(seedX, seedZ).visited = true;
    while (!open.empty()) {
        const auto [ix, iz] = open.back();
        open.pop_back();
        const Spot& spot = at(ix, iz);
        d.cells.push_back({ (center[0] + (ix - kCellRadius) * kCellSize) / kUnit,
                            (center[2] + (iz - kCellRadius) * kCellSize) / kUnit, spot.surface / kUnit,
                            spot.ground / kUnit });
        const int neighbours[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const auto& n : neighbours) {
            const int nx = ix + n[0], nz = iz + n[1];
            if (nx < 0 || nz < 0 || nx >= kWidth || nz >= kWidth) {
                continue;
            }
            Spot& next = at(nx, nz);
            if (next.valid && !next.visited) {
                next.visited = true;
                open.push_back({ nx, nz });
            }
        }
    }

    // Small water holds fewer fish: about one for every 20 cells of it (OoT's pond has 15 for a much bigger one)
    const int cellCount = (int)d.cells.size();
    if (cellCount < 4) {
        d.cells.clear();
        return 0;
    }
    const int wanted = std::clamp((cellCount + 8) / 20, 1, kMaxFish - 2);

    // The ones in front of the player, not too near and not out of reach of a cast, are the likeliest places
    const float yaw = playerYaw * kPi / 180.0f;
    std::vector<int> candidates;
    for (int pass = 0; pass < 3 && (int)candidates.size() < wanted * 2; pass++) {
        candidates.clear();
        for (int i = 0; i < cellCount; i++) {
            const float dx = d.cells[i].x * kUnit - player[0];
            const float dz = d.cells[i].z * kUnit - player[2];
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float angle = std::abs(std::remainder(std::atan2(dx, dz) - yaw, 2.0f * kPi));
            const bool near = distance >= (pass == 2 ? 150.0f : 250.0f);
            const bool far = distance <= (pass == 0 ? 1000.0f : 1400.0f);
            const bool ahead = pass == 2 || angle < 80.0f * kPi / 180.0f;
            if (near && far && ahead) {
                candidates.push_back(i);
            }
        }
    }
    if (candidates.empty()) {
        d.cells.clear();
        return 0;
    }

    int count = 0;
    std::vector<int> homes;
    for (int n = 0; n < wanted; n++) {
        int chosen = -1;
        for (int attempt = 0; attempt < 30 && chosen < 0; attempt++) {
            const int pick = candidates[(size_t)d.rng.Zero((float)candidates.size() - 0.001f)];
            chosen = pick;
            // Fish keep some distance from each other when they start
            for (int other : homes) {
                if (Sq(d.cells[other].x - d.cells[pick].x) + Sq(d.cells[other].z - d.cells[pick].z) <
                    Sq(100.0f / kUnit)) {
                    chosen = -1;
                    break;
                }
            }
        }
        if (chosen < 0) {
            break;
        }
        homes.push_back(chosen);
        const Cell& cell = d.cells[chosen];

        Fish& f = d.fish[count];
        f = Fish();
        f.active = true;
        f.index = count;
        f.loach = n == wanted - 1 && wanted >= 3 && d.rng.One() < 0.25f;
        // Fishing_Init
        const FishInit& init = f.loach ? kLoach[(int)d.rng.Zero(1.999f)] : kBass[(int)d.rng.Zero(14.999f)];
        f.length = init.baseLength + d.rng.Zero(4.99999f);
        f.perception = f.basePerception = init.perception;
        if (f.length >= 65.0f && d.rng.One() < 0.05f) {
            f.length += d.rng.Zero(7.99999f);
        }
        const float depth = cell.surface - cell.ground;
        f.surface = cell.surface;
        f.floor = cell.ground;
        f.position.x = cell.x + d.rng.Centered(30.0f / kUnit);
        f.position.z = cell.z + d.rng.Centered(30.0f / kUnit);
        if (f.loach) {
            f.position.y = cell.ground + 6.0f;
        } else {
            f.position.y = cell.surface - std::clamp(depth * (0.3f + d.rng.Zero(0.3f)), 6.0f,
                                                     std::max(6.0f, depth - f.length * 0.1f - 6.0f));
        }
        f.home = f.previous = f.position;
        f.worldRotY = f.shapeRotY = (int16_t)(int)d.rng.Zero(65536.0f);
        f.state = f.stateNext = kHome;
        f.phaseStepTarget = 1.0f;
        f.phaseMagTarget = 2000.0f;
        f.wanderTimer = (int)d.rng.Zero(200.0f) + 100;
        f.to = d.LookOf(f);
        f.from = f.to;
        d.SetMouth(f, f.to);
        count++;
    }
    d.lureSide = (int)d.rng.Zero(1.9f);
    if (count == 0) {
        d.cells.clear();
    }
    return count;
}

Result Update(School& school, World& world, const Frame& frame) {
    Data& d = *school.data;
    d.result = Result();
    if (!HasFish(school)) {
        return d.result;
    }
    d.world = &world;
    d.frame = frame;
    auto toOot = [](const float v[3]) { return Vec3{ v[0] / kUnit, v[1] / kUnit, v[2] / kUnit }; };
    d.player = toOot(frame.player);
    d.tip = toOot(frame.tip);
    d.lure = toOot(frame.lure);

    // Presses are kept until a tick uses them: the game runs at its own rate, the fish at 20 ticks a second
    d.aPressed = d.aPressed || frame.aPressed;
    d.bPressed = d.bPressed || frame.bPressed;
    d.wiggle = std::max(d.wiggle, frame.wiggle);
    const float stick = frame.stickY * 80.0f; // OoT's stick range
    if (stick < -50.0f && d.lastStickY > -40.0f) {
        d.kick = true;
    }
    d.lastStickY = stick;

    d.accumulated += std::clamp(frame.dt, 0.0f, 0.1f);
    while (d.accumulated >= kTickSeconds) {
        d.accumulated -= kTickSeconds;
        d.Tick();
        d.result.ticks++;
    }

    Fight& fight = d.fight;
    fight.active = d.castState == 4 || d.castState == 5;
    fight.hooked = d.lineHooked;
    fight.hitTimer = d.hitTimer;
    fight.tension = d.castState == 4 && d.lineHooked ? d.tension / kTensionLimit : 0.0f;
    if (d.hooked >= 0) {
        const Fish& f = d.fish[d.hooked];
        // Part way between ticks, like the fish is drawn
        const Look look = d.castState == 5 ? d.LookOf(f) : Blend(f.from, f.to, d.accumulated / kTickSeconds);
        MtxF matrices[kMatricesPerFish];
        Gfx* lists[kMatricesPerFish];
        float mouth[3];
        PoseFish(f.loach ? d.loachSkeleton : d.fishSkeleton, f.loach, look, look.position, 1.0f, matrices, lists,
                 mouth);
        for (int k = 0; k < 3; k++) {
            fight.mouth[k] = mouth[k] * kUnit;
        }
        fight.mouthYaw = look.rotY * kBinToRad;
        fight.length = f.length;
        fight.loach = f.loach;
        fight.running = f.timers[2] != 0;
        const float dx = f.position.x - d.player.x;
        const float dz = f.position.z - d.player.z;
        fight.faceYaw = std::atan2(dx, dz) * 180.0f / kPi;
    } else {
        fight.hitTimer = 0;
    }
    return d.result;
}

const Fight& GetFight(const School& school) {
    return school.data->fight;
}

void ReleaseCaught(School& school, World& world, const float player[3], float playerYaw) {
    Data& d = *school.data;
    d.world = &world;
    if (d.hooked < 0) {
        return;
    }
    Fish& f = d.fish[d.hooked];
    d.EndFight();
    // Back into the water, a way out in front of the player
    const float yaw = playerYaw * kPi / 180.0f;
    float best = -1.0f;
    const Cell* target = nullptr;
    for (const Cell& cell : d.cells) {
        const float dx = cell.x * kUnit - player[0];
        const float dz = cell.z * kUnit - player[2];
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance < 200.0f || distance > 700.0f) {
            continue;
        }
        const float angle = std::abs(std::remainder(std::atan2(dx, dz) - yaw, 2.0f * kPi));
        // Nearer the line of sight is better, with a little luck
        const float score = (kPi - angle) + d.rng.Zero(0.6f);
        if (score > best) {
            best = score;
            target = &cell;
        }
    }
    if (target == nullptr) {
        // Nowhere in reach: straight down where it was, if that is water, else at home
        f.tossTo = f.home;
    } else {
        f.tossTo = { target->x, target->surface - 8.0f, target->z };
        f.surface = target->surface;
        f.floor = target->ground;
    }
    f.tossTo.y = std::min(f.tossTo.y, f.surface - 8.0f);
    f.tossFrom = f.position;
    f.tossed = true;
    f.tossTick = 0;
    f.state = f.stateNext = kSwim;
    f.worldRotY = f.shapeRotY = Atan2S(f.tossTo.z - f.tossFrom.z, f.tossTo.x - f.tossFrom.x);
    f.worldRotX = 0;
    f.speed = 0.0f;
    f.home = f.tossTo;
}

// ---- Drawing ----

void Draw(School& school, Gfx** gfx, Mtx** mtx, const float camera[3], const MtxF& hand, float handYaw) {
    Data& d = *school.data;
    if (!HasFish(school)) {
        return;
    }
    static const int sScopeKey = 0;
    const float t = std::clamp(d.accumulated / kTickSeconds, 0.0f, 1.0f);

    // The state Link's own lists are drawn with; the fish's lists set the rest themselves
    gDPPipeSync((*gfx)++);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATEIDECALA, G_CC_MODULATEIA_PRIM2);
    gDPSetOtherMode((*gfx)++,
                    G_AD_NOTPATTERN | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_PERSP | G_CYC_2CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_PASS | G_RM_AA_ZB_OPA_SURF2);
    *(*gfx)++ = MakeGfx(0xD7000002, 0xFFFFFFFF); // G_TEXTURE: on, full scale
    // G_GEOMETRYMODE: G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH
    *(*gfx)++ = MakeGfx(0xD9000000, 0x00000001 | 0x00000004 | 0x00000400 | 0x00020000 | 0x00200000);

    for (Fish& f : d.fish) {
        if (!f.active) {
            continue;
        }
        Look look;
        if (f.state == kHeld && d.castState == 5) {
            // In the player's hand (Fishing_UpdateFish case 6): it flies to the hand and stays there
            const float yaw = handYaw * kPi / 180.0f;
            MtxF turn = MtxIdentity();
            MtxRotateY(turn, yaw);
            float offset[3];
            MtxTransform(turn, 4.0f, -10.0f, 5.0f, offset);
            const float step = 6.0f * std::clamp(d.frame.dt, 0.0f, 0.1f) / kTickSeconds;
            ApproachF(f.position.x, (hand.xw + camera[0]) / kUnit + offset[0], 1.0f, step);
            ApproachF(f.position.y, (hand.yw + camera[1]) / kUnit + offset[1], 1.0f, step);
            ApproachF(f.position.z, (hand.zw + camera[2]) / kUnit + offset[2], 1.0f, step);
            look = d.LookOf(f);
            look.rotY = (int16_t)(RadToBin(yaw) + 0x5000);
            look.rotX = -0x4000;
            look.rotZ = 0;
            d.SetMouth(f, look);
        } else {
            look = Blend(f.from, f.to, t);
        }

        const float origin[3] = { look.position[0] * kUnit - camera[0], look.position[1] * kUnit - camera[1],
                                  look.position[2] * kUnit - camera[2] };
        MtxF matrices[kMatricesPerFish];
        Gfx* lists[kMatricesPerFish];
        const int count = PoseFish(f.loach ? d.loachSkeleton : d.fishSkeleton, f.loach, look, origin, kUnit, matrices,
                                   lists, nullptr);
        Mtx* pool = *mtx;
        *mtx += count;
        FrameInterpolation_RecordOpenChild(&sScopeKey, 100 + f.index);
        for (int i = 0; i < count; i++) {
            core1_7F60_guMtxF2L(matrices[i].mf, &pool[i]);
            FrameInterpolation_RecordMatrixToMtx(&pool[i], matrices[i].mf);
        }
        // The lists reach their neighbours' matrices through segment 0x0D (G_MOVEWORD, G_MW_SEGMENT)
        *(*gfx)++ = MakeGfx(0xDB060000 | (0x0D * 4), (uintptr_t)pool);
        for (int i = 0; i < count; i++) {
            *(*gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)&pool[i]);
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)lists[i]);
        }
        FrameInterpolation_RecordCloseChild();
    }
}

} // namespace PlayAsLinkFish
