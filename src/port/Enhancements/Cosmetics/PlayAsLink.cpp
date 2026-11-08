#include "PlayAsLink.h"
#include "PlayAsLinkHorse.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

#include <libultraship/libultraship.h>
#include <fast/ucodehandlers.h>

#include "port/FilePicker.h"
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
#endif
#include "port/Interpolation/FrameInterpolation.h"
#include "port/FrameHost.h"
#include "port/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "bk_time.h"
#include "core2/ba/physics.h"

void core1_7F60_guMtxF2L(float mf[4][4], Mtx* m);
}

// Link's data comes from Ocarina of Time (USA 1.0): adult Link's object, gameplay_keep (animation headers and a
// few textures) and link_animetion (animation frames). The offsets below are from the OoT decompilation.
namespace {

constexpr int kFileAnimetion = 7;
constexpr int kFileGameplayKeep = 498;
constexpr int kFileLinkBoy = 502;
constexpr size_t kAnimetionSize = 0x265C30;
constexpr size_t kGameplayKeepSize = 0x5BCE0;
constexpr size_t kLinkBoySize = 0x37800;

constexpr uint32_t kSkeleton = 0x377F4;                         // gLinkAdultSkel
constexpr uint32_t kSheathWithShield = 0x20A78;                 // gLinkAdultBackHylianShieldSwordInSheathNearDL
constexpr uint32_t kHandSword = 0x21F78;                        // gLinkAdultLeftHandHoldingMasterSwordNearDL
constexpr uint32_t kHandShield = 0x22970;                       // gLinkAdultRightHandHoldingHylianShieldNearDL
constexpr uint32_t kBackEmptySheath = 0x249D8;                  // gLinkAdultBackMasterSheathNearDL
constexpr uint32_t kBackSwordSheathed = 0x23160;                // gLinkAdultBackMasterSwordInSheathNearDL
constexpr uint32_t kEyeTextures[] = { 0x0000, 0x0800, 0x1000 }; // open, half, closed
constexpr uint32_t kMouthClosedTexture = 0x4000;                // gLinkAdultMouthClosedTex
constexpr int kSheathLimb = 19;
constexpr int kLeftHandLimb = 15;
constexpr int kRightHandLimb = 18;
// Limbs 0 to 8 (root, waist and both legs) are the lower body; 9 to 20 (torso, head, arms, sheath) the upper body.
// Joint 0 is the root translation, so joints below this index belong to the lower body.
constexpr int kFirstUpperJoint = 10;
constexpr int kJointCount = 22;                 // root translation and 21 limb rotations
constexpr int kFrameSize = kJointCount * 6 + 2; // and the face (eyes and mouth) indices

constexpr const char* kCacheName = "oot_link.bin";
// On Android the system file picker copies the chosen ROM here (see LighthouseActivity.pickOotRom)
constexpr const char* kAndroidImportName = "oot_link_import.rom";
// Version 2 adds object_horse (Epona); a version 1 cache still gives Link, without her
constexpr char kCacheMagicV1[8] = { 'O', 'O', 'T', 'L', 'I', 'N', 'K', '1' };
constexpr char kCacheMagic[8] = { 'O', 'O', 'T', 'L', 'I', 'N', 'K', '2' };

// Link stands about 6000 units tall in his model space; Banjo about 150 in his.
constexpr float kLinkScale = 0.026f;

uint16_t Be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---- Reading the ROM ----

bool Yaz0Decode(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out) {
    if (srcSize < 16 || std::memcmp(src, "Yaz0", 4) != 0) {
        return false;
    }
    const size_t size = Be32(src + 4);
    out.clear();
    out.reserve(size);
    size_t i = 16;
    while (out.size() < size) {
        if (i >= srcSize) {
            return false;
        }
        const uint8_t code = src[i++];
        for (int bit = 0; bit < 8 && out.size() < size; bit++) {
            if (code & (0x80 >> bit)) {
                if (i >= srcSize) {
                    return false;
                }
                out.push_back(src[i++]);
            } else {
                if (i + 1 >= srcSize) {
                    return false;
                }
                const uint8_t b1 = src[i++];
                const uint8_t b2 = src[i++];
                const size_t dist = (((b1 & 0xF) << 8) | b2) + 1;
                size_t count = b1 >> 4;
                if (count == 0) {
                    if (i >= srcSize) {
                        return false;
                    }
                    count = src[i++] + 0x12;
                } else {
                    count += 2;
                }
                if (dist > out.size()) {
                    return false;
                }
                for (size_t n = 0; n < count; n++) {
                    out.push_back(out[out.size() - dist]);
                }
            }
        }
    }
    return true;
}

// Brings a .v64 (byte-swapped) or .n64 (little-endian) ROM to the big-endian .z64 layout.
void NormalizeRom(std::vector<uint8_t>& rom) {
    if (rom.size() < 4) {
        return;
    }
    if (rom[0] == 0x37 && rom[1] == 0x80) {
        for (size_t i = 0; i + 1 < rom.size(); i += 2) {
            std::swap(rom[i], rom[i + 1]);
        }
    } else if (rom[0] == 0x40 && rom[1] == 0x12) {
        for (size_t i = 0; i + 3 < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
}

bool ExtractFile(const std::vector<uint8_t>& rom, size_t dmadata, int index, std::vector<uint8_t>& out) {
    const size_t entry = dmadata + (size_t)index * 16;
    if (entry + 16 > rom.size()) {
        return false;
    }
    const uint32_t vromStart = Be32(&rom[entry]);
    const uint32_t vromEnd = Be32(&rom[entry + 4]);
    const uint32_t romStart = Be32(&rom[entry + 8]);
    const uint32_t romEnd = Be32(&rom[entry + 12]);
    if (vromEnd <= vromStart) {
        return false;
    }
    if (romEnd == 0) {
        if ((size_t)romStart + (vromEnd - vromStart) > rom.size()) {
            return false;
        }
        out.assign(rom.begin() + romStart, rom.begin() + romStart + (vromEnd - vromStart));
        return true;
    }
    if (romEnd <= romStart || romEnd > rom.size()) {
        return false;
    }
    return Yaz0Decode(&rom[romStart], romEnd - romStart, out) && out.size() == vromEnd - vromStart;
}

// ---- Link's model, converted for the renderer ----

struct Limb {
    int16_t jointPos[3];
    uint8_t child;
    uint8_t sibling;
    uint32_t displayList;
};

struct AnimHeader {
    uint32_t frameCount;
    uint32_t dataOffset;
};

class LinkModel {
public:
    std::vector<uint8_t> keep, obj, anim;
    std::vector<Limb> limbs;
    int matrixCount = 0;
    std::vector<Gfx*> limbLists; // converted display list of each limb, or nullptr
    std::vector<uint8_t> horse;  // object_horse, when the cache has it
    std::unique_ptr<PlayAsLinkHorse::Epona> epona;
    // Hand and back lists for the poses that show his equipment
    Gfx* handSword = nullptr;
    Gfx* handShield = nullptr;
    Gfx* backEmptySheath = nullptr;
    Gfx* backSwordSheathed = nullptr;

    bool Build(std::string& error);
    AnimHeader GetAnim(uint32_t headerOffset) const;
    const uint8_t* FrameData(const AnimHeader& anim, uint32_t frame) const;
    uint8_t* SegmentData(uint8_t segment, uint32_t offset);

private:
    // One native array per (address, count) a list loads: some of Link's arrays are only 8-byte aligned, and
    // loads can overlap
    std::deque<std::vector<Vtx>> mVertices;
    std::unordered_map<uint64_t, Vtx*> mConvertedVertices;
    std::deque<std::vector<Gfx>> mLists;
    std::unordered_map<uint32_t, Gfx*> mConverted;

    Vtx* ConvertVertices(uint32_t address, uint32_t count);
    Gfx* ConvertList(uint32_t address, int depth);
};

uint8_t* LinkModel::SegmentData(uint8_t segment, uint32_t offset) {
    std::vector<uint8_t>* data = segment == 4 ? &keep : segment == 6 ? &obj : nullptr;
    return data != nullptr && offset < data->size() ? data->data() + offset : nullptr;
}

AnimHeader LinkModel::GetAnim(uint32_t headerOffset) const {
    AnimHeader header = { Be16(&keep[headerOffset]), Be32(&keep[headerOffset + 4]) & 0xFFFFFF };
    if (header.frameCount == 0 || header.dataOffset + (size_t)header.frameCount * kFrameSize > anim.size()) {
        header.frameCount = 0;
    }
    return header;
}

const uint8_t* LinkModel::FrameData(const AnimHeader& header, uint32_t frame) const {
    if (header.frameCount == 0) {
        return nullptr;
    }
    return &anim[header.dataOffset + (size_t)(frame % header.frameCount) * kFrameSize];
}

// N64 vertices are big-endian with 16-bit positions; the renderer's are native with float positions.
Vtx* LinkModel::ConvertVertices(uint32_t address, uint32_t count) {
    const uint8_t segment = address >> 24;
    const uint32_t offset = address & 0xFFFFFF;
    std::vector<uint8_t>& data = segment == 4 ? keep : obj;
    if ((segment != 4 && segment != 6) || count == 0 || offset + (size_t)count * 16 > data.size()) {
        return nullptr;
    }
    const uint64_t key = ((uint64_t)address << 8) | count;
    if (auto it = mConvertedVertices.find(key); it != mConvertedVertices.end()) {
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
    mVertices.push_back(std::move(out));
    Vtx* converted = mVertices.back().data();
    mConvertedVertices[key] = converted;
    return converted;
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

// Link's display lists stay F3DEX2 commands (they're drawn with that microcode), widened to native commands with
// their addresses resolved: object and gameplay_keep data become pointers, while the per-draw segments stay
// segmented (0x08 eyes, 0x09 mouth, 0x0C culling, 0x0D limb matrices).
Gfx* LinkModel::ConvertList(uint32_t address, int depth) {
    if (auto it = mConverted.find(address); it != mConverted.end()) {
        return it->second;
    }
    const uint8_t segment = address >> 24;
    std::vector<uint8_t>& data = segment == 4 ? keep : obj;
    if ((segment != 4 && segment != 6) || depth > 8) {
        return nullptr;
    }

    std::vector<Gfx> out;
    for (size_t offset = address & 0xFFFFFF; offset + 8 <= data.size(); offset += 8) {
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
            case 0xFD: // G_SETTIMG
                if (refSegment == 4 || refSegment == 6) {
                    nativeW1 = (uintptr_t)SegmentData(refSegment, w1 & 0xFFFFFF);
                    if (nativeW1 == 0) {
                        return nullptr;
                    }
                } else {
                    nativeW1 = SegmentRef(w1);
                }
                break;
            case 0xDB:          // G_MOVEWORD
            case 0xDC:          // G_MOVEMEM
                return nullptr; // Link's lists don't use these
            case 0xDF:          // G_ENDDL
                end = true;
                break;
            default:
                break;
        }
        out.push_back(MakeGfx(w0, nativeW1));
        if (end) {
            mLists.push_back(std::move(out));
            Gfx* list = mLists.back().data();
            mConverted[address] = list;
            return list;
        }
    }
    return nullptr;
}

bool LinkModel::Build(std::string& error) {
    if (obj.size() != kLinkBoySize || keep.size() != kGameplayKeepSize || anim.size() != kAnimetionSize) {
        error = "Link's data has an unexpected size";
        return false;
    }

    const uint32_t limbTable = Be32(&obj[kSkeleton]) & 0xFFFFFF;
    const uint8_t limbCount = obj[kSkeleton + 4];
    matrixCount = obj[kSkeleton + 8];
    if (limbCount != kJointCount - 1 || limbTable + limbCount * 4 > obj.size()) {
        error = "Link's skeleton wasn't found";
        return false;
    }
    for (int i = 0; i < limbCount; i++) {
        const uint32_t limbOffset = Be32(&obj[limbTable + i * 4]) & 0xFFFFFF;
        if (limbOffset + 16 > obj.size()) {
            error = "Link's skeleton is damaged";
            return false;
        }
        const uint8_t* src = &obj[limbOffset];
        Limb limb;
        limb.jointPos[0] = (int16_t)Be16(src);
        limb.jointPos[1] = (int16_t)Be16(src + 2);
        limb.jointPos[2] = (int16_t)Be16(src + 4);
        limb.child = src[6];
        limb.sibling = src[7];
        limb.displayList = Be32(src + 8); // the high-detail list
        if (i == kSheathLimb) {
            limb.displayList = 0x06000000 | kSheathWithShield;
        }
        limbs.push_back(limb);
    }
    for (const Limb& limb : limbs) {
        Gfx* list = nullptr;
        if (limb.displayList != 0) {
            list = ConvertList(limb.displayList, 0);
            if (list == nullptr) {
                error = "Link's model couldn't be converted";
                return false;
            }
        }
        limbLists.push_back(list);
    }
    handSword = ConvertList(0x06000000 | kHandSword, 0);
    handShield = ConvertList(0x06000000 | kHandShield, 0);
    backEmptySheath = ConvertList(0x06000000 | kBackEmptySheath, 0);
    backSwordSheathed = ConvertList(0x06000000 | kBackSwordSheathed, 0);
    if (handSword == nullptr || handShield == nullptr || backEmptySheath == nullptr || backSwordSheathed == nullptr) {
        error = "Link's equipment couldn't be converted";
        return false;
    }
    // Epona is optional: without her, Link runs through the Talon Trot on foot
    if (!horse.empty()) {
        std::string horseError;
        epona = PlayAsLinkHorse::Build(horse, horseError);
    }
    return true;
}

// Read and replaced on the game thread only. A replaced model stays alive: a frame being rendered may still use
// its lists.
std::unique_ptr<LinkModel> sModel;
std::vector<std::unique_ptr<LinkModel>> sRetiredModels;
std::atomic<bool> sAvailable = false;
std::atomic<bool> sHasEpona = false;
std::string sStatus; // the menu's (render thread)

std::string CachePath() {
    return Ship::Context::GetPathRelativeToAppDirectory(kCacheName);
}

std::unique_ptr<LinkModel> LoadCache() {
    std::ifstream file(CachePath(), std::ios::binary);
    if (!file) {
        return nullptr;
    }
    char magic[8];
    if (!file.read(magic, 8)) {
        return nullptr;
    }
    const bool withHorse = std::memcmp(magic, kCacheMagic, 8) == 0;
    if (!withHorse && std::memcmp(magic, kCacheMagicV1, 8) != 0) {
        return nullptr;
    }
    auto model = std::make_unique<LinkModel>();
    std::vector<std::vector<uint8_t>*> parts = { &model->keep, &model->obj, &model->anim };
    if (withHorse) {
        parts.push_back(&model->horse);
    }
    for (std::vector<uint8_t>* part : parts) {
        uint8_t sizeBytes[4];
        if (!file.read((char*)sizeBytes, 4)) {
            return nullptr;
        }
        const uint32_t size = Be32(sizeBytes);
        if (size > 0x400000) {
            return nullptr;
        }
        part->resize(size);
        if (!file.read((char*)part->data(), size)) {
            return nullptr;
        }
    }
    std::string error;
    if (!model->Build(error)) {
        return nullptr;
    }
    return model;
}

LinkModel* GetModel() {
    return sModel.get();
}

// ---- Animation ----

// What Link carries in his hands and on his back
enum class Gear {
    Sheathed, // sword and shield on his back
    Sword,    // sword in the left hand, shield in the right, empty sheath
    Guard,    // shield in the right hand, sword in its sheath
};

struct AnimChoice {
    uint32_t header; // offset in gameplay_keep
    bool loop;
    float speed;
    Gear gear = Gear::Sheathed;
    uint32_t upperHeader = 0; // when set, the upper body plays this animation and `header` only drives the lower body
    // When set, the animation is a two-part cycle: `header` then this one (OoT splits its climb into left and right)
    uint32_t secondHeader = 0;
    // Holds the root at its first frame's height: for animations that move Link with their root, where Banjo
    // already moves himself
    bool holdRootHeight = false;
};

// gameplay_keep offsets of the animations used
enum : uint32_t {
    ANIM_WAIT = 0x3240,        // link_normal_wait_free
    ANIM_WALK = 0x3290,        // link_normal_walk_free
    ANIM_RUN = 0x3140,         // link_normal_run_free
    ANIM_JUMP = 0x3148,        // link_normal_run_jump
    ANIM_FALL = 0x2F10,        // link_normal_fall
    ANIM_FALL_UP = 0x2F20,     // link_normal_fall_up_free
    ANIM_LANDING = 0x3028,     // link_normal_landing_free
    ANIM_ROLL = 0x3038,        // link_normal_landing_roll_free
    ANIM_SLASH = 0x2A80,       // link_fighter_normal_kiru
    ANIM_THRUST = 0x2AA8,      // link_fighter_pierce_kiru
    ANIM_CROUCH = 0x2ED8,      // link_normal_defense_wait_free
    ANIM_JUMP_SLASH = 0x2A48,  // link_fighter_jump_kiru
    ANIM_GUARD = 0x2ED0,       // link_normal_defense_wait: shield raised in front
    ANIM_THROW = 0x31C8,       // link_normal_throw_free
    ANIM_FLY = 0x2C98,         // link_hook_fly_wait
    ANIM_SWIM = 0x32F0,        // link_swimer_swim
    ANIM_SWIM_WAIT = 0x3328,   // link_swimer_swim_wait
    ANIM_SWIM_DEAD = 0x32F8,   // link_swimer_swim_dead
    ANIM_CLIMB_L = 0x2E80,     // link_normal_climb_upL
    ANIM_CLIMB_R = 0x2E88,     // link_normal_climb_upR
    ANIM_HIT = 0x2F70,         // link_normal_front_hit
    ANIM_DOWN = 0x2F58,        // link_normal_front_downA
    ANIM_GET_ITEM = 0x2788,    // link_demo_get_itemB
    ANIM_SLIP = 0x2EE0,        // link_normal_down_slope_slip
    ANIM_BRAKE = 0x2DA0,       // link_normal_back_brake
    ANIM_TALK = 0x31A8,        // link_normal_talk_free_wait
    ANIM_RIDE_WAIT = 0x33B8,   // link_uma_wait_1
    ANIM_RIDE_WALK = 0x3380,   // link_uma_anim_walk
    ANIM_RIDE_TROT = 0x3358,   // link_uma_anim_slowrun
    ANIM_RIDE_GALLOP = 0x3338, // link_uma_anim_fastrun
    ANIM_RIDE_JUMP = 0x3348,   // link_uma_anim_jump100
};

// The Talon Trot is Link riding Epona: everything but leaving it
bool IsRiding(s32 state) {
    switch (state) {
        case BS_14_BTROT_ENTER:
        case BS_15_BTROT_IDLE:
        case BS_16_BTROT_WALK:
        case BS_8_BTROT_JUMP:
        case BS_71_BTROT_FALL:
        case BS_45_BTROT_SLIDE:
        case BS_79_BTROT_LOCKED:
        case BS_7B_BTROT_OW:
        case BS_9A_BTROT_DRONE:
            return true;
        default:
            return false;
    }
}

// Epona's gait follows Banjo's speed (the Talon Trot goes from 30 to 700, 1000 with the Turbo Talon Trainers), and
// Link plays the riding animation OoT pairs with it, at the same rate so they stay in step
struct Ride {
    PlayAsLinkHorse::Gait gait;
    float speed;
    AnimChoice link;
};

Ride ChooseRide(s32 state) {
    using PlayAsLinkHorse::Gait;
    const float speed = baphysics_get_horizontal_velocity();
    if (state == BS_8_BTROT_JUMP || state == BS_71_BTROT_FALL) {
        return { Gait::Jump, 1.0f, { ANIM_RIDE_JUMP, false, 1.0f } };
    }
    if (speed < 40.0f) {
        return { Gait::Idle, 1.0f, { ANIM_RIDE_WAIT, true, 1.0f } };
    }
    if (speed < 250.0f) {
        const float rate = std::clamp(speed / 150.0f, 0.6f, 1.6f);
        return { Gait::Walk, rate, { ANIM_RIDE_WALK, true, rate } };
    }
    if (speed < 500.0f) {
        const float rate = std::clamp(speed / 375.0f, 0.7f, 1.4f);
        return { Gait::Trot, rate, { ANIM_RIDE_TROT, true, rate } };
    }
    const float rate = std::clamp(speed / 650.0f, 0.8f, 1.6f);
    return { Gait::Gallop, rate, { ANIM_RIDE_GALLOP, true, rate } };
}

// Banjo's height on the last frame he climbed, for which way the climb plays
float sClimbLastY = 0.0f;

AnimChoice ChooseAnim(s32 state) {
    const float speed = baphysics_get_horizontal_velocity();
    switch (state) {
        // Wonderwing: shield up on the upper body while the legs follow the movement
        case BS_1A_WONDERWING_ENTER:
        case BS_1B_WONDERWING_IDLE:
        case BS_1E_WONDERWING_EXIT:
        case BS_A4_WONDERWING_DRONE:
        case BS_A5_WONDERWING_UNKA5:
            return { ANIM_WAIT, true, 1.0f, Gear::Guard, ANIM_GUARD };
        case BS_1C_WONDERWING_WALK:
            return { ANIM_RUN, true, std::clamp(speed / 350.0f, 0.6f, 2.0f), Gear::Guard, ANIM_GUARD };
        case BS_1D_WONDERWING_JUMP:
            return { ANIM_JUMP, false, 1.0f, Gear::Guard, ANIM_GUARD };
        case BS_2_WALK_SLOW:
        case BS_3_WALK:
        case BS_1F_WALK_CREEP:
        case BS_7A_WALK_MUD:
        case BS_3B_CARRY_WALK:
        case BS_27_LONGLEG_WALK:
            return { ANIM_WALK, true, std::clamp(speed / 120.0f, 0.5f, 2.0f) };
        case BS_4_WALK_FAST:
        case BS_16_BTROT_WALK:
            return { ANIM_RUN, true, std::clamp(speed / 350.0f, 0.6f, 2.0f) };
        case BS_5_JUMP:
        case BS_8_BTROT_JUMP:
        case BS_12_BFLIP:
        case BS_22_BSHOCK_JUMP:
        case BS_28_LONGLEG_JUMP:
            return { ANIM_JUMP, false, 1.0f };
        case BS_2F_FALL:
        case BS_71_BTROT_FALL:
        case BS_3D_FALL_TUMBLING:
        case BS_F_BBUSTER:
            return { ANIM_FALL, true, 1.0f };
        // The Feathery Flap is a second jump: the leap plays again
        case BS_10_BFLAP:
            return { ANIM_JUMP, false, 1.0f };
        // Rat-a-tat Rap pecks about every 0.2 s: one 10-frame jump slash takes 0.33 s at normal speed
        case BS_11_BPECK:
            return { ANIM_JUMP_SLASH, true, 1.3f, Gear::Sword };
        case BS_20_LANDING:
            return { ANIM_LANDING, false, 1.0f };
        case BS_31_ROLL:
            return { ANIM_ROLL, false, 1.5f };
        // OoT's slashes are only a few frames long. Banjo's claw swipe is three swipes in about 0.7 s, so the slash
        // repeats at that rhythm; the barge is one long dash, so the thrust plays once and holds its last frame
        case BS_6_CLAW:
            return { ANIM_SLASH, true, 0.7f, Gear::Sword };
        case BS_13_BBARGE:
            return { ANIM_THRUST, false, 0.35f, Gear::Sword };
        case BS_7_CROUCH:
        case BS_21_BSHOCK_CHARGE:
            return { ANIM_CROUCH, true, 1.0f };
        case BS_9_EGG_HEAD:
        case BS_A_EGG_ASS:
        case BS_5B_CARRY_THROW:
            return { ANIM_THROW, false, 1.5f };
        case BS_23_FLY_ENTER:
        case BS_24_FLY:
        case BS_2A_BOMB:
        case BS_18_FLY_KNOCKBACK:
        case BS_91_FLY_OW:
            return { ANIM_FLY, true, 1.0f };
        case BS_2E_SWIM:
        case BS_2C_DIVE_B:
        case BS_39_DIVE_A:
        case BS_30_DIVE_ENTER:
            return { ANIM_SWIM, true, 1.0f };
        case BS_2D_SWIM_IDLE:
        case BS_2B_DIVE_IDLE:
        case BS_4C_LANDING_IN_WATER:
            return { ANIM_SWIM_WAIT, true, 1.0f };
        case BS_54_SWIM_DIE:
            return { ANIM_SWIM_DEAD, false, 1.0f };
        // OoT's ladder climb, a left then a right reach, played backwards on the way down
        case BS_4F_CLIMB_IDLE:
        case BS_50_CLIMB_MOVE: {
            f32 position[3];
            playerPosition_get(position);
            const float rise = position[1] - sClimbLastY;
            sClimbLastY = position[1];
            const float rate = state == BS_4F_CLIMB_IDLE ? 0.0f : rise < -0.01f ? -1.2f : 1.2f;
            return { ANIM_CLIMB_L, true, rate, Gear::Sheathed, 0, ANIM_CLIMB_R, true };
        }
        case BS_E_OW:
        case BS_56_RECOIL:
        case BS_7F_DIVE_OW:
        case BS_7B_BTROT_OW:
            return { ANIM_HIT, false, 1.0f };
        case BS_41_DIE:
        case BS_72_SPLAT:
            return { ANIM_DOWN, false, 1.0f };
        case BS_44_JIG_JIGGY:
        case BS_34_JIG_NOTEDOOR:
            return { ANIM_GET_ITEM, false, 1.0f };
        case BS_32_SLIDE:
        case BS_45_BTROT_SLIDE:
        case BS_55_LONGLEG_SLIDE:
            return { ANIM_SLIP, true, 1.0f };
        case BS_C_SKID:
            return { ANIM_BRAKE, false, 1.0f };
        case BS_3C_TALK:
            return { ANIM_TALK, true, 1.0f };
        default:
            return { ANIM_WAIT, true, 1.0f };
    }
}

struct Pose {
    int16_t joints[kJointCount][3];
    uint16_t face;
};

void ReadPose(const uint8_t* frame, Pose& pose) {
    for (int j = 0; j < kJointCount; j++) {
        for (int k = 0; k < 3; k++) {
            pose.joints[j][k] = (int16_t)Be16(frame + j * 6 + k * 2);
        }
    }
    pose.face = Be16(frame + kJointCount * 6);
}

// Rotations blend along the short way around; the root translation linearly
void BlendPose(const Pose& from, const Pose& to, float t, Pose& out) {
    for (int k = 0; k < 3; k++) {
        out.joints[0][k] = (int16_t)(from.joints[0][k] + (to.joints[0][k] - from.joints[0][k]) * t);
    }
    for (int j = 1; j < kJointCount; j++) {
        for (int k = 0; k < 3; k++) {
            const int16_t diff = (int16_t)(to.joints[j][k] - from.joints[j][k]);
            out.joints[j][k] = (int16_t)(from.joints[j][k] + (int16_t)(diff * t));
        }
    }
    out.face = t < 0.5f ? from.face : to.face;
}

struct Animator {
    uint32_t header = 0;
    float frame = 0.0f;
    uint32_t upperHeader = 0;
    float upperFrame = 0.0f;
    s32 state = 0;
    Pose morphFrom = {};
    float morph = 1.0f; // 1 once the blend from the previous animation is done
    Pose last = {};
    bool hasLast = false;
    float blinkTimer = 2.0f;
};
Animator sAnimator;

void InstallModel(std::unique_ptr<LinkModel> model) {
    if (model == nullptr) {
        return;
    }
    sHasEpona = model->epona != nullptr;
    LinkModel* raw = model.release();
    Lighthouse::RunOnGameThread([raw]() {
        if (sModel != nullptr) {
            sRetiredModels.push_back(std::move(sModel));
        }
        sModel.reset(raw);
        sAnimator = Animator{};
    });
    sAvailable = true;
}

// The pose of an animation at a frame, interpolated between the two frames around it. Looping animations wrap
// around, others hold their last frame; `frame` is brought into range.
bool SamplePose(const LinkModel& model, uint32_t header, float& frame, bool loop, Pose& pose) {
    const AnimHeader anim = model.GetAnim(header);
    if (anim.frameCount == 0) {
        return false;
    }
    if (loop) {
        frame = std::fmod(frame, (float)anim.frameCount);
    } else {
        frame = std::min(frame, (float)(anim.frameCount - 1));
    }
    const uint32_t frame0 = (uint32_t)frame;
    const uint32_t frame1 = loop ? (frame0 + 1) % anim.frameCount : std::min(frame0 + 1, anim.frameCount - 1);
    Pose a, b;
    ReadPose(model.FrameData(anim, frame0), a);
    ReadPose(model.FrameData(anim, frame1), b);
    BlendPose(a, b, frame - (float)frame0, pose);
    return true;
}

// SamplePose for a two-part cycle: `frame` runs over both animations, in either direction
bool SampleCycle(const LinkModel& model, uint32_t first, uint32_t second, float& frame, Pose& pose) {
    const float firstCount = (float)model.GetAnim(first).frameCount;
    const float secondCount = (float)model.GetAnim(second).frameCount;
    if (firstCount == 0.0f || secondCount == 0.0f) {
        return false;
    }
    frame = std::fmod(frame, firstCount + secondCount);
    if (frame < 0.0f) {
        frame += firstCount + secondCount;
    }
    float part = frame < firstCount ? frame : frame - firstCount;
    return SamplePose(model, frame < firstCount ? first : second, part, false, pose);
}

void UpdatePose(LinkModel& model, Pose& pose, const AnimChoice& choice, s32 state) {
    const float dt = std::clamp(time_getDelta(), 0.0f, 0.1f);

    // A new animation starts from its first frame, and so does a one-shot one when a new move reuses it (the
    // Feathery Flap's leap after the jump's)
    if (choice.header != sAnimator.header || (!choice.loop && state != sAnimator.state)) {
        if (sAnimator.hasLast) {
            sAnimator.morphFrom = sAnimator.last;
            sAnimator.morph = 0.0f;
        }
        sAnimator.header = choice.header;
        sAnimator.frame = 0.0f;
    } else {
        // OoT's animations play at 30 frames per second
        sAnimator.frame += dt * 30.0f * choice.speed;
    }

    sAnimator.state = state;
    const bool sampled = choice.secondHeader != 0
                             ? SampleCycle(model, choice.header, choice.secondHeader, sAnimator.frame, pose)
                             : SamplePose(model, choice.header, sAnimator.frame, choice.loop, pose);
    if (!sampled) {
        std::memset(&pose, 0, sizeof(pose));
        return;
    }
    if (choice.holdRootHeight) {
        pose.joints[0][1] = (int16_t)Be16(model.FrameData(model.GetAnim(choice.header), 0) + 2);
    }
    if (choice.upperHeader != 0) {
        if (choice.upperHeader != sAnimator.upperHeader) {
            sAnimator.upperHeader = choice.upperHeader;
            sAnimator.upperFrame = 0.0f;
        } else {
            sAnimator.upperFrame += dt * 30.0f;
        }
        Pose upper;
        if (SamplePose(model, choice.upperHeader, sAnimator.upperFrame, true, upper)) {
            std::memcpy(&pose.joints[kFirstUpperJoint], &upper.joints[kFirstUpperJoint],
                        sizeof(pose.joints[0]) * (kJointCount - kFirstUpperJoint));
        }
    } else {
        sAnimator.upperHeader = 0;
    }

    if (sAnimator.morph < 1.0f) {
        sAnimator.morph = std::min(1.0f, sAnimator.morph + dt / 0.12f);
        Pose blended;
        BlendPose(sAnimator.morphFrom, pose, sAnimator.morph, blended);
        pose = blended;
    }
    sAnimator.last = pose;
    sAnimator.hasLast = true;
}

// ---- Drawing ----

float SinS(int16_t angle) {
    return std::sin(angle * (float)M_PI / 32768.0f);
}

float CosS(int16_t angle) {
    return std::cos(angle * (float)M_PI / 32768.0f);
}

// OoT's Matrix_TranslateRotateZYX (sys_matrix.c), on the same MtxF layout
void TranslateRotateZYX(MtxF* m, const float translation[3], const int16_t rotation[3]) {
    float sin = SinS(rotation[2]);
    float cos = CosS(rotation[2]);
    float temp1, temp2;

    temp1 = m->xx;
    temp2 = m->xy;
    m->xw += temp1 * translation[0] + temp2 * translation[1] + m->xz * translation[2];
    m->xx = temp1 * cos + temp2 * sin;
    m->xy = temp2 * cos - temp1 * sin;

    temp1 = m->yx;
    temp2 = m->yy;
    m->yw += temp1 * translation[0] + temp2 * translation[1] + m->yz * translation[2];
    m->yx = temp1 * cos + temp2 * sin;
    m->yy = temp2 * cos - temp1 * sin;

    temp1 = m->zx;
    temp2 = m->zy;
    m->zw += temp1 * translation[0] + temp2 * translation[1] + m->zz * translation[2];
    m->zx = temp1 * cos + temp2 * sin;
    m->zy = temp2 * cos - temp1 * sin;

    temp1 = m->wx;
    temp2 = m->wy;
    m->ww += temp1 * translation[0] + temp2 * translation[1] + m->wz * translation[2];
    m->wx = temp1 * cos + temp2 * sin;
    m->wy = temp2 * cos - temp1 * sin;

    if (rotation[1] != 0) {
        sin = SinS(rotation[1]);
        cos = CosS(rotation[1]);

        temp1 = m->xx;
        temp2 = m->xz;
        m->xx = temp1 * cos - temp2 * sin;
        m->xz = temp1 * sin + temp2 * cos;

        temp1 = m->yx;
        temp2 = m->yz;
        m->yx = temp1 * cos - temp2 * sin;
        m->yz = temp1 * sin + temp2 * cos;

        temp1 = m->zx;
        temp2 = m->zz;
        m->zx = temp1 * cos - temp2 * sin;
        m->zz = temp1 * sin + temp2 * cos;

        temp1 = m->wx;
        temp2 = m->wz;
        m->wx = temp1 * cos - temp2 * sin;
        m->wz = temp1 * sin + temp2 * cos;
    }

    if (rotation[0] != 0) {
        sin = SinS(rotation[0]);
        cos = CosS(rotation[0]);

        temp1 = m->xy;
        temp2 = m->xz;
        m->xy = temp1 * cos + temp2 * sin;
        m->xz = temp2 * cos - temp1 * sin;

        temp1 = m->yy;
        temp2 = m->yz;
        m->yy = temp1 * cos + temp2 * sin;
        m->yz = temp2 * cos - temp1 * sin;

        temp1 = m->zy;
        temp2 = m->zz;
        m->zy = temp1 * cos + temp2 * sin;
        m->zz = temp2 * cos - temp1 * sin;

        temp1 = m->wy;
        temp2 = m->wz;
        m->wy = temp1 * cos + temp2 * sin;
        m->wz = temp2 * cos - temp1 * sin;
    }
}

struct DrawState {
    LinkModel* model;
    const Pose* pose;
    Mtx* matrices;
    int next;
    Gfx** gfx;
    Gear gear;
};

// The display list of a limb, with the hands and back swapped for the gear Link is showing
Gfx* LimbList(const DrawState& state, int index) {
    const LinkModel& model = *state.model;
    switch (state.gear) {
        case Gear::Sword:
            if (index == kLeftHandLimb) {
                return model.handSword;
            } else if (index == kRightHandLimb) {
                return model.handShield;
            } else if (index == kSheathLimb) {
                return model.backEmptySheath;
            }
            break;
        case Gear::Guard:
            if (index == kRightHandLimb) {
                return model.handShield;
            } else if (index == kSheathLimb) {
                return model.backSwordSheathed;
            }
            break;
        case Gear::Sheathed:
            break;
    }
    return model.limbLists[index];
}

constexpr uintptr_t kF3dex2Mtx = 0xDA380003; // G_MTX: G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH
constexpr uintptr_t kF3dex2Dl = 0xDE000000;  // G_DL, pushed

// SkelAnime_DrawFlexLimbLod: every limb with a display list gets the next matrix (the lists reach each other's
// matrices through segment 0x0D)
void DrawLimb(DrawState& state, int index, const MtxF& parent) {
    const Limb& limb = state.model->limbs[index];
    MtxF m = parent;
    float translation[3];
    if (index == 0) {
        for (int k = 0; k < 3; k++) {
            translation[k] = state.pose->joints[0][k];
        }
    } else {
        for (int k = 0; k < 3; k++) {
            translation[k] = limb.jointPos[k];
        }
    }
    TranslateRotateZYX(&m, translation, state.pose->joints[index + 1]);

    Gfx* list = LimbList(state, index);
    if (list != nullptr && state.next < state.model->matrixCount) {
        Mtx* mtx = &state.matrices[state.next++];
        core1_7F60_guMtxF2L(m.mf, mtx);
        FrameInterpolation_RecordMatrixToMtx(mtx, m.mf);
        *(*state.gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)mtx);
        *(*state.gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)list);
    }
    if (limb.child != 0xFF) {
        DrawLimb(state, limb.child, m);
    }
    if (limb.sibling != 0xFF) {
        DrawLimb(state, limb.sibling, parent);
    }
}

Gfx sCullBackList[2];
Gfx sScratch[8192];

Lights1 sLights = gdSPDefLights1(0x60, 0x60, 0x60, 0xFF, 0xFF, 0xFF, 0x28, 0x50, 0x28);

} // namespace

// ---- Public ----

namespace PlayAsLink {

std::string ImportRom(const std::string& romPath) {
    std::ifstream file(romPath, std::ios::binary);
    if (!file) {
        return "Couldn't open the ROM";
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    NormalizeRom(rom);

    // The file table starts with its own first entry: the makerom file at 0, 0x1060 bytes long
    static const uint8_t kFirstEntry[16] = { 0, 0, 0, 0, 0, 0, 0x10, 0x60, 0, 0, 0, 0, 0, 0, 0, 0 };
    size_t dmadata = std::string::npos;
    for (size_t i = 0x1000; i + 16 <= rom.size() && i < 0x20000; i += 16) {
        if (std::memcmp(&rom[i], kFirstEntry, 16) == 0) {
            dmadata = i;
            break;
        }
    }
    if (rom.size() < 0x40 || std::memcmp(&rom[0x20], "THE LEGEND OF ZELDA", 19) != 0 || dmadata == std::string::npos) {
        return "This isn't an Ocarina of Time ROM";
    }

    auto model = std::make_unique<LinkModel>();
    if (!ExtractFile(rom, dmadata, kFileGameplayKeep, model->keep) ||
        !ExtractFile(rom, dmadata, kFileLinkBoy, model->obj) ||
        !ExtractFile(rom, dmadata, kFileAnimetion, model->anim) ||
        !ExtractFile(rom, dmadata, PlayAsLinkHorse::kFileIndex, model->horse) ||
        model->keep.size() != kGameplayKeepSize || model->obj.size() != kLinkBoySize ||
        model->anim.size() != kAnimetionSize) {
        return "Only the USA 1.0 release of Ocarina of Time is supported";
    }
    std::string error;
    if (!model->Build(error)) {
        return error;
    }

    std::ofstream cache(CachePath(), std::ios::binary | std::ios::trunc);
    cache.write(kCacheMagic, 8);
    for (const std::vector<uint8_t>* part : { &model->keep, &model->obj, &model->anim, &model->horse }) {
        const uint32_t size = (uint32_t)part->size();
        const uint8_t sizeBytes[4] = { (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8),
                                       (uint8_t)size };
        cache.write((const char*)sizeBytes, 4);
        cache.write((const char*)part->data(), size);
    }
    if (!cache) {
        return "Couldn't save Link's data";
    }
    InstallModel(std::move(model));
    return "";
}

#ifdef __ANDROID__
bool sAndroidImportPending = false;

// LighthouseActivity copies the ROM picked with the system file picker into the app's folder (scoped storage keeps
// the in-game browser out of Downloads), and the import happens once it's there
void PollAndroidImport() {
    if (!sAndroidImportPending) {
        return;
    }
    const std::string path = Ship::Context::GetPathRelativeToAppDirectory(kAndroidImportName);
    const std::string done = path + ".done";
    if (!std::filesystem::exists(done)) {
        return;
    }
    sAndroidImportPending = false;
    const std::string error = std::filesystem::exists(path) ? ImportRom(path) : "No ROM was picked";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(done, ec);
    sStatus = error.empty() ? "Link is ready" : error;
    if (error.empty()) {
        CVarSetInteger(CVAR_PLAY_AS_LINK, 1);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
}
#endif

bool IsAvailable() {
    return sAvailable;
}

bool HasEpona() {
    return sHasEpona;
}

const std::string& GetStatus() {
#ifdef __ANDROID__
    PollAndroidImport();
#endif
    return sStatus;
}

void PickRom() {
#ifdef __ANDROID__
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env != nullptr && activity != nullptr) {
        jclass activityClass = env->GetObjectClass(activity);
        jmethodID pick = env->GetMethodID(activityClass, "pickOotRom", "(Ljava/lang/String;)V");
        if (pick != nullptr) {
            jstring dest = env->NewStringUTF(Ship::Context::GetPathRelativeToAppDirectory(kAndroidImportName).c_str());
            sAndroidImportPending = true;
            sStatus = "Waiting for the ROM...";
            env->CallVoidMethod(activity, pick, dest);
            env->DeleteLocalRef(dest);
        } else {
            env->ExceptionClear();
            sStatus = "This build can't open the file picker";
        }
        env->DeleteLocalRef(activityClass);
        env->DeleteLocalRef(activity);
    }
    return;
#endif
    Ship::FileBrowserRequest request;
    request.Title = "Select an Ocarina of Time (USA 1.0) ROM";
    request.Filters = { { "N64 ROMs (*.z64, *.n64, *.v64)", { "*.z64", "*.n64", "*.v64" } }, { "All Files", { "*" } } };
    Lighthouse::PickFile(std::move(request), [](std::optional<std::filesystem::path> path) {
        if (!path) {
            return;
        }
        const std::string error = ImportRom(path->string());
        sStatus = error.empty() ? "Link is ready" : error;
        if (error.empty()) {
            CVarSetInteger(CVAR_PLAY_AS_LINK, 1);
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    });
}

} // namespace PlayAsLink

extern "C" {

s32 PlayAsLink_shouldDraw(s32 modelId) {
    if (!CVarGetInteger(CVAR_PLAY_AS_LINK, 0)) {
        return false;
    }
    if (modelId != ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY && modelId != ASSET_34D_MODEL_BANJOKAZOOIE_LOW_POLY) {
        return false;
    }
    return GetModel() != nullptr;
}

Gfx* PlayAsLink_getScratchGfx(void) {
    return sScratch;
}

void PlayAsLink_draw(Gfx** gfx, Mtx** mtx, const MtxF* modelMatrix, s32 kazooieFacing) {
    LinkModel* model = GetModel();
    if (model == nullptr) {
        return;
    }

    const s32 playerState = bs_getState();
    const bool riding = model->epona != nullptr && IsRiding(playerState);
    const Ride ride = riding ? ChooseRide(playerState) : Ride{};
    const AnimChoice choice = riding ? ride.link : ChooseAnim(playerState);
    Pose pose;
    UpdatePose(*model, pose, choice, playerState);

    // Eyes blink every few seconds unless the animation picks them
    sAnimator.blinkTimer -= std::clamp(time_getDelta(), 0.0f, 0.1f);
    if (sAnimator.blinkTimer < -0.15f) {
        sAnimator.blinkTimer = 2.0f + (float)(osGetCount() % 3000) / 1000.0f;
    }
    int eyes = sAnimator.blinkTimer > 0.0f     ? 0
               : sAnimator.blinkTimer > -0.05f ? 1
               : sAnimator.blinkTimer > -0.1f  ? 2
                                               : 1;
    if ((pose.face & 0xF) != 0 && (pose.face & 0xF) <= 3) {
        eyes = (pose.face & 0xF) - 1;
    }

    sCullBackList[0] = MakeGfx(0xD9FFFFFF, 0x00000400); // G_GEOMETRYMODE: set G_CULL_BACK
    sCullBackList[1] = MakeGfx(0xDF000000, 0);          // G_ENDDL

    Mtx* matrices = *mtx;
    *mtx += model->matrixCount;

    // Banjo's matrix scaled down to Link's size. In the Talon Trot and the wading boots Banjo's model is turned to
    // face Kazooie's way (backwards); Link and Epona face the way the player goes.
    MtxF base = *modelMatrix;
    if (kazooieFacing) {
        for (int col = 0; col < 4; col++) {
            base.mf[0][col] = -base.mf[0][col];
            base.mf[2][col] = -base.mf[2][col];
        }
    }
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            base.mf[row][col] *= kLinkScale;
        }
    }

    // Riding, Epona is drawn with Banjo's matrix and Link sits where OoT seats him on her saddle
    if (riding) {
        MtxF rider;
        PlayAsLinkHorse::Draw(*model->epona, gfx, mtx, base, ride.gait, ride.speed,
                              std::clamp(time_getDelta(), 0.0f, 0.1f), &rider);
        base = rider;
    }

    gDPPipeSync((*gfx)++);
    gSPSegment((*gfx)++, 0x08, (uintptr_t)(model->obj.data() + kEyeTextures[eyes]));
    gSPSegment((*gfx)++, 0x09, (uintptr_t)(model->obj.data() + kMouthClosedTexture));
    gSPSegment((*gfx)++, 0x0C, (uintptr_t)sCullBackList);
    gSPSegment((*gfx)++, 0x0D, (uintptr_t)matrices);
    gSPSetLights1((*gfx)++, sLights);
    gDPSetEnvColor((*gfx)++, 30, 105, 27, 0); // the Kokiri tunic
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATEIDECALA, G_CC_MODULATEIA_PRIM2);
    gDPSetOtherMode((*gfx)++,
                    G_AD_NOTPATTERN | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_PERSP | G_CYC_2CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_PASS | G_RM_AA_ZB_OPA_SURF2);

    // Link's lists are F3DEX2: switch the renderer to that microcode for them, and back after
    *(*gfx)++ = MakeGfx(((uintptr_t)0xDD << 24) | ucode_f3dex2, 0);
    *(*gfx)++ = MakeGfx(0xD7000002, 0xFFFFFFFF); // G_TEXTURE: on, full scale
    // G_GEOMETRYMODE: G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH
    *(*gfx)++ = MakeGfx(0xD9000000, 0x00000001 | 0x00000004 | 0x00000400 | 0x00020000 | 0x00200000);
    DrawState state = { model, &pose, matrices, 0, gfx, choice.gear };
    DrawLimb(state, 0, base);
    *(*gfx)++ = MakeGfx(((uintptr_t)0xDD << 24) | ucode_f3dex, 0);

    gDPPipeSync((*gfx)++);
    gSPClearGeometryMode((*gfx)++, 0xFFFFFFFF);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
}

} // extern "C"

// Link's data from an earlier session
static RegisterShipInitFunc sPlayAsLinkInit([]() { InstallModel(LoadCache()); }, {});
