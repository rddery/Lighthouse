#include "PlayAsLinkHorse.h"

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

void core1_7F60_guMtxF2L(float mf[4][4], Mtx* m);
}

// Everything below comes from the OoT decompilation (object_horse.xml, z_skin.c, z_skin_awb.c, z_skin_matrix.c,
// z_skelanime.c and ovl_En_Horse). Epona is a "skin" skeleton: most limbs are rigid display lists, but the body
// is one limb whose vertices are each blended from several limb matrices every frame.
namespace {

constexpr uint32_t kSkeleton = 0x9D74; // gEponaSkel
constexpr int kLimbCount = 47;
constexpr int kJointCount = kLimbCount + 1; // the animation's root translation and one rotation per limb
constexpr size_t kObjectSize = 0xDF10;
constexpr int kSaddleLimb = 30; // EnHorse_PostDraw seats the rider from here
// The head (limb 13) draws the eye from segment 8, which EnHorse_OverrideLimbDraw points at one of these
constexpr uint32_t kEyeTextures[] = { 0x9F80, 0xA180, 0xA380 }; // open, half, closed

constexpr uint32_t kAnimOffsets[] = {
    0x6D50, // gEponaIdleAnim
    0x75F0, // gEponaWalkingAnim
    0x32B0, // gEponaTrottingAnim
    0x1E2C, // gEponaGallopingAnim
    0x2470, // gEponaJumpingAnim
};

// EnHorse_PostDraw: Skin_GetLimbPos(skin, 30, { 600, -1670, 0 }, &riderPos)
constexpr float kRiderOffset[3] = { 600.0f, -1670.0f, 0.0f };
// Player's riding code (z_player.c) stands Link at riderPos - 27 world units (the horse actor is scaled 0.01), and
// Link's riding animations keep his standing root height (gLinkAdultSkel's 3377) on top of their own, so that's
// taken off too for his hips to land in the saddle
constexpr float kRiderDrop = 2700.0f + 3377.0f;

// How fast each gait plays at speed 1, in animation frames per 20 Hz game tick. These are what EnHorse sets as
// playSpeed at its usual speeds: idle sPlaybackSpeeds (2/3), walking 0.75 * 3, trotting 0.375 * 6, galloping
// 0.3 * 8 and the jumps 1.5.
constexpr float kFramesPerTick[] = { 2.0f / 3.0f, 2.25f, 2.25f, 2.4f, 1.5f };
constexpr float kTicksPerSecond = 20.0f;
// Gait changes blend over EnHorse's 3 frames of morph
constexpr float kMorphSeconds = 3.0f / kTicksPerSecond;

// While jumping, Banjo's own physics carries the arc, so the animation's root height (OoT moves the actor with it)
// would lift Epona twice. She holds her standing height instead and only tucks her legs.
constexpr float kJumpRootY = 4848.0f;

// Skinned vertices are rewritten every frame, but the renderer may still be drawing the last frames' display
// lists (the game thread runs ahead of it), so each frame gets its own buffer from a ring. The renderer is at
// most one or two frames behind: four slots leave a spare.
constexpr int kRing = 4;

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

// ---- Model ----

struct SkinVertex {
    uint16_t index;
    int16_t s, t;
    int8_t normal[3];
    uint8_t alpha;
};

struct SkinTransform {
    uint8_t limb;
    float pos[3];
    float weight; // OoT's percentage, scaled to 0..1
};

// The vertices of one limb that share the same set of limb matrices
struct SkinModif {
    std::vector<SkinVertex> vertices;
    std::vector<SkinTransform> transforms;
    int normalTransform; // which transform's matrix turns the normals (SkinLimbModif::unk_4)
};

struct Limb {
    int16_t jointPos[3];
    uint8_t child;
    uint8_t sibling;
    int32_t type;        // 4 (skinned), 11 (rigid display list) or something without a display list
    uint32_t segment;    // the display list, or SkinAnimatedLimbData
    int skinned = -1;    // index into Data::skinned
    Gfx* list = nullptr; // rigid limbs
};

struct SkinnedLimb {
    int limb;
    uint32_t displayList;
    uint32_t vertexCount; // total over all modifs
    std::vector<SkinModif> modifs;
    std::vector<Vtx> buffers[kRing];
    Gfx* lists[kRing]; // the limb's display list for each buffer
};

struct AnimInfo {
    uint32_t frameCount;
    uint32_t frameData;
    uint32_t jointIndices;
    uint32_t staticIndexMax;
};

struct Pose {
    int16_t joints[kJointCount][3];
};

// Display lists are converted once per "context": rigid limbs share one, and each skinned limb's list is converted
// once per ring slot because its vertices (segment 8 in OoT) point at that slot's buffer
struct ListContext {
    Vtx* segment8 = nullptr;
    uint32_t segment8Count = 0;
    std::unordered_map<uint32_t, Gfx*> converted;
};

} // namespace

struct PlayAsLinkHorse::Epona::Data {
    std::vector<uint8_t> obj;
    Limb limbs[kLimbCount];
    std::vector<SkinnedLimb> skinned;
    AnimInfo anims[5];
    int rigidCount = 0;

    // Converted display lists and vertices stay put: the renderer holds pointers to them
    std::deque<std::vector<Vtx>> vertexArrays;
    std::unordered_map<uint64_t, Vtx*> convertedVertices;
    std::deque<std::vector<Gfx>> listStorage;

    // Animation state
    Gait gait = Gait::Idle;
    bool started = false;
    float frame = 0.0f;
    Pose last = {};
    Pose morphFrom = {};
    float morph = 1.0f; // 1 once the blend from the previous gait is done
    uint32_t ring = 0;
    // EnHorse_Update starts a blink 2.5% of the time every 20 Hz tick and steps open, half, closed, half
    float blinkClock = 0.0f;
    int blinkStep = 0;
    uint32_t random = 12345;

    bool Convert(std::string& error);
    Vtx* ConvertVertices(uint32_t address, uint32_t count);
    Gfx* ConvertList(uint32_t address, ListContext& context, int depth);
    bool ReadAnimation(int index, std::string& error);
    void ReadPose(const AnimInfo& anim, uint32_t frame, Pose& pose) const;
    float NextRandom();
};

PlayAsLinkHorse::Epona::Epona() : data(std::make_unique<Data>()) {
}

PlayAsLinkHorse::Epona::~Epona() = default;

namespace PlayAsLinkHorse {

// N64 vertices are big-endian with 16-bit positions; the renderer's are native with float positions.
Vtx* Epona::Data::ConvertVertices(uint32_t address, uint32_t count) {
    const uint32_t offset = address & 0xFFFFFF;
    if ((address >> 24) != 6 || count == 0 || offset + (size_t)count * 16 > obj.size()) {
        return nullptr;
    }
    // One native array per (address, count): loads can overlap and some arrays are only 8-byte aligned
    const uint64_t key = ((uint64_t)address << 8) | count;
    if (auto it = convertedVertices.find(key); it != convertedVertices.end()) {
        return it->second;
    }
    std::vector<Vtx> out(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* src = &obj[offset + i * 16];
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

// Epona's lists stay F3DEX2 commands (they're drawn with that microcode), widened to native commands with their
// addresses resolved: object data become pointers, and the eye texture (segment 8 on the head) stays segmented
// so that it can be switched per draw. Vertices in segment 8 are the skinned body's: they point into the buffer
// of the context's ring slot instead.
Gfx* Epona::Data::ConvertList(uint32_t address, ListContext& context, int depth) {
    if (auto it = context.converted.find(address); it != context.converted.end()) {
        return it->second;
    }
    if ((address >> 24) != 6 || (address & 7) != 0 || depth > 8) {
        return nullptr;
    }

    std::vector<Gfx> out;
    for (size_t offset = address & 0xFFFFFF; offset + 8 <= obj.size(); offset += 8) {
        const uint32_t w0 = Be32(&obj[offset]);
        const uint32_t w1 = Be32(&obj[offset + 4]);
        const uint8_t op = w0 >> 24;
        const uint8_t refSegment = w1 >> 24;
        uintptr_t nativeW1 = w1;
        bool end = false;

        switch (op) {
            case 0x01: { // G_VTX
                const uint32_t count = (w0 >> 12) & 0xFF;
                if (refSegment == 8) {
                    const uint32_t index = (w1 & 0xFFFFFF) / 16;
                    if (context.segment8 == nullptr || (w1 & 15) != 0 || index + count > context.segment8Count) {
                        return nullptr;
                    }
                    nativeW1 = (uintptr_t)(context.segment8 + index);
                } else {
                    nativeW1 = (uintptr_t)ConvertVertices(w1, count);
                }
                if (nativeW1 == 0) {
                    return nullptr;
                }
                break;
            }
            case 0xDA: // G_MTX
                nativeW1 = SegmentRef(w1);
                break;
            case 0xDE: // G_DL
                if (refSegment == 6) {
                    nativeW1 = (uintptr_t)ConvertList(w1, context, depth + 1);
                    if (nativeW1 == 0) {
                        return nullptr;
                    }
                } else {
                    nativeW1 = SegmentRef(w1);
                }
                end = ((w0 >> 16) & 0xFF) == 1; // G_DL_NOPUSH: a branch
                break;
            case 0xFD: // G_SETTIMG
                if (refSegment == 6) {
                    if ((w1 & 0xFFFFFF) >= obj.size()) {
                        return nullptr;
                    }
                    nativeW1 = (uintptr_t)(obj.data() + (w1 & 0xFFFFFF));
                } else {
                    nativeW1 = SegmentRef(w1);
                }
                break;
            case 0xDB:          // G_MOVEWORD
            case 0xDC:          // G_MOVEMEM
                return nullptr; // Epona's lists don't use these
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
            context.converted[address] = list;
            return list;
        }
    }
    return nullptr;
}

bool Epona::Data::ReadAnimation(int index, std::string& error) {
    const uint32_t header = kAnimOffsets[index];
    if (header + 16 > obj.size()) {
        error = "Epona's animations weren't found";
        return false;
    }
    AnimInfo& anim = anims[index];
    anim.frameCount = Be16(&obj[header]);
    anim.frameData = Be32(&obj[header + 4]);
    anim.jointIndices = Be32(&obj[header + 8]);
    anim.staticIndexMax = Be16(&obj[header + 12]);
    if ((anim.frameData >> 24) != 6 || (anim.jointIndices >> 24) != 6 || anim.frameCount == 0) {
        error = "Epona's animations are damaged";
        return false;
    }
    anim.frameData &= 0xFFFFFF;
    anim.jointIndices &= 0xFFFFFF;
    if (anim.jointIndices + (size_t)kJointCount * 6 > obj.size()) {
        error = "Epona's animations are damaged";
        return false;
    }
    // Check every value ReadPose can reach, so that it doesn't have to
    for (int joint = 0; joint < kJointCount; joint++) {
        for (int k = 0; k < 3; k++) {
            const uint32_t value = Be16(&obj[anim.jointIndices + joint * 6 + k * 2]);
            const size_t last = value >= anim.staticIndexMax ? (size_t)value + anim.frameCount - 1 : value;
            if (anim.frameData + last * 2 + 2 > obj.size()) {
                error = "Epona's animations are damaged";
                return false;
            }
        }
    }
    return true;
}

// SkelAnime_GetFrameData: each joint axis is either one value for the whole animation or one per frame
void Epona::Data::ReadPose(const AnimInfo& anim, uint32_t frame, Pose& pose) const {
    frame %= anim.frameCount;
    for (int joint = 0; joint < kJointCount; joint++) {
        for (int k = 0; k < 3; k++) {
            const uint32_t value = Be16(&obj[anim.jointIndices + joint * 6 + k * 2]);
            const uint32_t at = value >= anim.staticIndexMax ? value + frame : value;
            pose.joints[joint][k] = (int16_t)Be16(&obj[anim.frameData + at * 2]);
        }
    }
}

float Epona::Data::NextRandom() {
    random = random * 1664525u + 1013904223u;
    return (float)(random >> 8) / (float)(1 << 24);
}

bool Epona::Data::Convert(std::string& error) {
    if (obj.size() != kObjectSize) {
        error = "Epona's data has an unexpected size";
        return false;
    }

    // SkeletonHeader: the limb table, then the limb count
    const uint32_t limbTable = Be32(&obj[kSkeleton]);
    if ((limbTable >> 24) != 6 || obj[kSkeleton + 4] != kLimbCount ||
        (limbTable & 0xFFFFFF) + kLimbCount * 4 > obj.size()) {
        error = "Epona's skeleton wasn't found";
        return false;
    }
    for (int i = 0; i < kLimbCount; i++) {
        const uint32_t limbOffset = Be32(&obj[(limbTable & 0xFFFFFF) + i * 4]) & 0xFFFFFF;
        if (limbOffset + 16 > obj.size()) {
            error = "Epona's skeleton is damaged";
            return false;
        }
        // SkinLimb
        const uint8_t* src = &obj[limbOffset];
        Limb& limb = limbs[i];
        limb.jointPos[0] = (int16_t)Be16(src);
        limb.jointPos[1] = (int16_t)Be16(src + 2);
        limb.jointPos[2] = (int16_t)Be16(src + 4);
        limb.child = src[6];
        limb.sibling = src[7];
        limb.type = (int32_t)Be32(src + 8);
        limb.segment = Be32(src + 12);
        if ((limb.child != 0xFF && limb.child >= kLimbCount) || (limb.sibling != 0xFF && limb.sibling >= kLimbCount)) {
            error = "Epona's skeleton is damaged";
            return false;
        }
    }

    // Skinned limbs: SkinAnimatedLimbData -> SkinLimbModif -> SkinVertex and SkinTransformation
    for (int i = 0; i < kLimbCount; i++) {
        Limb& limb = limbs[i];
        if (limb.type != 4 || limb.segment == 0) {
            continue;
        }
        const uint32_t dataOffset = limb.segment & 0xFFFFFF;
        if ((limb.segment >> 24) != 6 || dataOffset + 12 > obj.size()) {
            error = "Epona's body is damaged";
            return false;
        }
        SkinnedLimb skinnedLimb;
        skinnedLimb.limb = i;
        skinnedLimb.vertexCount = Be16(&obj[dataOffset]);
        const uint32_t modifCount = Be16(&obj[dataOffset + 2]);
        const uint32_t modifTable = Be32(&obj[dataOffset + 4]);
        skinnedLimb.displayList = Be32(&obj[dataOffset + 8]);
        if ((modifTable >> 24) != 6 || (modifTable & 0xFFFFFF) + (size_t)modifCount * 16 > obj.size()) {
            error = "Epona's body is damaged";
            return false;
        }
        for (uint32_t m = 0; m < modifCount; m++) {
            const uint8_t* src = &obj[(modifTable & 0xFFFFFF) + m * 16];
            const uint32_t vertexCount = Be16(src);
            const uint32_t transformCount = Be16(src + 2);
            const uint32_t vertexTable = Be32(src + 8);
            const uint32_t transformTable = Be32(src + 12);
            SkinModif modif;
            modif.normalTransform = Be16(src + 4);
            if ((vertexTable >> 24) != 6 || (transformTable >> 24) != 6 ||
                (vertexTable & 0xFFFFFF) + (size_t)vertexCount * 10 > obj.size() ||
                (transformTable & 0xFFFFFF) + (size_t)transformCount * 10 > obj.size() || transformCount == 0 ||
                modif.normalTransform >= (int)transformCount) {
                error = "Epona's body is damaged";
                return false;
            }
            for (uint32_t v = 0; v < vertexCount; v++) {
                const uint8_t* vertex = &obj[(vertexTable & 0xFFFFFF) + v * 10];
                SkinVertex skinVertex;
                skinVertex.index = Be16(vertex);
                skinVertex.s = (int16_t)Be16(vertex + 2);
                skinVertex.t = (int16_t)Be16(vertex + 4);
                skinVertex.normal[0] = (int8_t)vertex[6];
                skinVertex.normal[1] = (int8_t)vertex[7];
                skinVertex.normal[2] = (int8_t)vertex[8];
                skinVertex.alpha = vertex[9];
                if (skinVertex.index >= skinnedLimb.vertexCount) {
                    error = "Epona's body is damaged";
                    return false;
                }
                modif.vertices.push_back(skinVertex);
            }
            for (uint32_t t = 0; t < transformCount; t++) {
                const uint8_t* transform = &obj[(transformTable & 0xFFFFFF) + t * 10];
                SkinTransform skinTransform;
                skinTransform.limb = transform[0];
                skinTransform.pos[0] = (int16_t)Be16(transform + 2);
                skinTransform.pos[1] = (int16_t)Be16(transform + 4);
                skinTransform.pos[2] = (int16_t)Be16(transform + 6);
                skinTransform.weight = transform[8] * 0.01f;
                if (skinTransform.limb >= kLimbCount) {
                    error = "Epona's body is damaged";
                    return false;
                }
                modif.transforms.push_back(skinTransform);
            }
            skinnedLimb.modifs.push_back(std::move(modif));
        }

        // Skin_InitAnimatedLimb: the texture coordinates and alpha never change, so they're written once. Each
        // ring slot gets its own copy.
        for (std::vector<Vtx>& buffer : skinnedLimb.buffers) {
            buffer.resize(skinnedLimb.vertexCount);
            std::memset(buffer.data(), 0, buffer.size() * sizeof(Vtx));
            for (const SkinModif& modif : skinnedLimb.modifs) {
                for (const SkinVertex& vertex : modif.vertices) {
                    Vtx& vtx = buffer[vertex.index];
                    vtx.n.flag = 0;
                    vtx.n.tc[0] = vertex.s;
                    vtx.n.tc[1] = vertex.t;
                    vtx.n.a = vertex.alpha;
                }
            }
        }
        limb.skinned = (int)skinned.size();
        skinned.push_back(std::move(skinnedLimb));
    }

    // Display lists
    ListContext rigidContext;
    for (int i = 0; i < kLimbCount; i++) {
        Limb& limb = limbs[i];
        if (limb.type == 11 && limb.segment != 0) {
            limb.list = ConvertList(limb.segment, rigidContext, 0);
            if (limb.list == nullptr) {
                error = "Epona's model couldn't be converted";
                return false;
            }
            rigidCount++;
        }
    }
    for (SkinnedLimb& skinnedLimb : skinned) {
        for (int slot = 0; slot < kRing; slot++) {
            ListContext context;
            context.segment8 = skinnedLimb.buffers[slot].data();
            context.segment8Count = skinnedLimb.vertexCount;
            skinnedLimb.lists[slot] = ConvertList(skinnedLimb.displayList, context, 0);
            if (skinnedLimb.lists[slot] == nullptr) {
                error = "Epona's body couldn't be converted";
                return false;
            }
        }
    }
    if (skinned.empty() || rigidCount + 1 != PlayAsLinkHorse::kMatrixCount) {
        error = "Epona's skeleton wasn't what was expected";
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (!ReadAnimation(i, error)) {
            return false;
        }
    }
    return true;
}

} // namespace PlayAsLinkHorse

namespace {

using Data = PlayAsLinkHorse::Epona::Data;
using PlayAsLinkHorse::Gait;

// ---- Posing ----

float SinS(int16_t angle) {
    return std::sin(angle * (float)M_PI / 32768.0f);
}

float CosS(int16_t angle) {
    return std::cos(angle * (float)M_PI / 32768.0f);
}

// OoT's Matrix_TranslateRotateZYX (sys_matrix.c), on the same MtxF layout. On an identity matrix it builds what
// z_skin_matrix.c's SkinMatrix_SetTranslateRotateZYX does.
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

MtxF Identity() {
    MtxF m;
    std::memset(&m, 0, sizeof(m));
    m.xx = m.yy = m.zz = m.ww = 1.0f;
    return m;
}

// SkinMatrix_MtxFMtxFMult: dest = a * b, where the fields (xx, xy, xz, xw) are a matrix's first row and
// (xx, yx, zx, wx) its first column. That layout is the one OoT's MtxF and the renderer's share, with mf[c][r]
// holding row r, column c, so applying `a * b` to a point applies b first.
MtxF Multiply(const MtxF& a, const MtxF& b) {
    MtxF dest;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.mf[k][r] * b.mf[c][k];
            }
            dest.mf[c][r] = sum;
        }
    }
    return dest;
}

// SkinMatrix_Vec3fMtxFMultXYZ
void Transform(const MtxF& m, const float src[3], float dest[3]) {
    dest[0] = m.xw + src[0] * m.xx + src[1] * m.xy + src[2] * m.xz;
    dest[1] = m.yw + src[0] * m.yx + src[1] * m.yy + src[2] * m.yz;
    dest[2] = m.zw + src[0] * m.zx + src[1] * m.zy + src[2] * m.zz;
}

int8_t ToNormalByte(float value) {
    return (int8_t)std::clamp(value, -128.0f, 127.0f);
}

// func_800A698C: a limb's matrix is its parent's times its own; its sibling shares the parent
void VisitLimb(const Data& data, const MtxF* local, MtxF* out, const MtxF& parent, int index) {
    out[index] = Multiply(parent, local[index]);
    const Limb& limb = data.limbs[index];
    if (limb.child != 0xFF) {
        VisitLimb(data, local, out, out[index], limb.child);
    }
    if (limb.sibling != 0xFF) {
        VisitLimb(data, local, out, parent, limb.sibling);
    }
}

// Skin_ApplyAnimTransformations: every limb's matrix, from the root down, with the parents'
// matrices already applied. `matrices` are in the model's own space.
void LimbMatrices(const Data& data, const Pose& pose, MtxF* matrices) {
    MtxF local[kLimbCount];
    for (int i = 0; i < kLimbCount; i++) {
        float translation[3];
        for (int k = 0; k < 3; k++) {
            translation[k] = i == 0 ? pose.joints[0][k] : data.limbs[i].jointPos[k];
        }
        local[i] = Identity();
        TranslateRotateZYX(&local[i], translation, pose.joints[i + 1]);
    }

    VisitLimb(data, local, matrices, Identity(), 0);
}

// Skin_ApplyLimbModifications (called with arg3 = 0, as EnHorse_Draw does) and Skin_UpdateVertices: every
// vertex of the limb is moved to the weighted sum of its transforms, and its normal is turned by one limb's matrix
void SkinVertices(SkinnedLimb& limb, const MtxF* matrices, std::vector<Vtx>& buffer) {
    for (const SkinModif& modif : limb.modifs) {
        float point[3] = { 0.0f, 0.0f, 0.0f };
        if (modif.transforms.size() == 1) {
            Transform(matrices[modif.transforms[0].limb], modif.transforms[0].pos, point);
        } else {
            for (const SkinTransform& transform : modif.transforms) {
                float moved[3];
                Transform(matrices[transform.limb], transform.pos, moved);
                for (int k = 0; k < 3; k++) {
                    point[k] += moved[k] * transform.weight;
                }
            }
        }

        MtxF rotation = matrices[modif.transforms[modif.normalTransform].limb];
        rotation.xw = rotation.yw = rotation.zw = 0.0f;
        for (const SkinVertex& vertex : modif.vertices) {
            Vtx& vtx = buffer[vertex.index];
            vtx.n.ob[0] = point[0];
            vtx.n.ob[1] = point[1];
            vtx.n.ob[2] = point[2];
            const float normal[3] = { (float)vertex.normal[0], (float)vertex.normal[1], (float)vertex.normal[2] };
            float turned[3];
            Transform(rotation, normal, turned);
            vtx.n.n[0] = ToNormalByte(turned[0]);
            vtx.n.n[1] = ToNormalByte(turned[1]);
            vtx.n.n[2] = ToNormalByte(turned[2]);
        }
    }
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
}

bool Loops(Gait gait) {
    return gait != Gait::Jump;
}

// Walking, trotting and galloping are the same stride at different paces, so a gait change keeps its place in the
// stride instead of restarting it
bool Strides(Gait gait) {
    return gait == Gait::Walk || gait == Gait::Trot || gait == Gait::Gallop;
}

// Steps the animation and writes the pose to draw
void UpdatePose(Data& data, Gait gait, float speed, float dt, Pose& pose) {
    const AnimInfo* anim = &data.anims[(int)gait];

    if (!data.started || gait != data.gait) {
        const AnimInfo& previous = data.anims[(int)data.gait];
        if (data.started) {
            data.morphFrom = data.last;
            data.morph = 0.0f;
            data.frame = Strides(data.gait) && Strides(gait)
                             ? data.frame / (float)previous.frameCount * (float)anim->frameCount
                             : 0.0f;
        } else {
            data.frame = 0.0f;
        }
        data.gait = gait;
        data.started = true;
    } else {
        data.frame += dt * kTicksPerSecond * kFramesPerTick[(int)gait] * std::max(speed, 0.0f);
    }

    const float count = (float)anim->frameCount;
    if (Loops(gait)) {
        data.frame = std::fmod(data.frame, count);
    } else {
        data.frame = std::min(data.frame, count - 1.0f);
    }

    // Blend neighbouring frames: OoT only steps whole frames, but the game here runs faster than 20 Hz
    const uint32_t frame0 = (uint32_t)data.frame;
    const uint32_t frame1 = Loops(gait) ? (frame0 + 1) % anim->frameCount : std::min(frame0 + 1, anim->frameCount - 1);
    Pose a, b;
    data.ReadPose(*anim, frame0, a);
    data.ReadPose(*anim, frame1, b);
    BlendPose(a, b, data.frame - (float)frame0, pose);

    if (data.morph < 1.0f) {
        data.morph = std::min(1.0f, data.morph + dt / kMorphSeconds);
        Pose blended;
        BlendPose(data.morphFrom, pose, data.morph, blended);
        pose = blended;
    }
    data.last = pose;

    if (gait == Gait::Jump) {
        // The jump keeps the animation's own forward offset but not its height (see kJumpRootY)
        pose.joints[0][1] = (int16_t)kJumpRootY;
    }
}

Lights1 sLights = gdSPDefLights1(0x60, 0x60, 0x60, 0xFF, 0xFF, 0xFF, 0x28, 0x50, 0x28);

} // namespace

namespace PlayAsLinkHorse {

std::unique_ptr<Epona> Build(std::vector<uint8_t> objectHorse, std::string& error) {
    auto epona = std::make_unique<Epona>();
    epona->data->obj = std::move(objectHorse);
    if (!epona->data->Convert(error)) {
        return nullptr;
    }
    return epona;
}

void Draw(Epona& epona, Gfx** gfx, Mtx** mtx, const MtxF& base, Gait gait, float speed, float dt, MtxF* rider) {
    Data& data = *epona.data;
    dt = std::clamp(dt, 0.0f, 0.1f);

    Pose pose;
    UpdatePose(data, gait, speed, dt, pose);
    MtxF matrices[kLimbCount];
    LimbMatrices(data, pose, matrices);

    // The next slot of the ring (see kRing)
    const int slot = (int)(data.ring++ % kRing);
    for (SkinnedLimb& skinned : data.skinned) {
        SkinVertices(skinned, matrices, skinned.buffers[slot]);
    }

    // Where Link sits: EnHorse_PostDraw's Skin_GetLimbPos on the saddle limb, lowered by what Player's riding code
    // subtracts. Rotation is the horse's (Link faces where she does).
    float seat[3];
    Transform(matrices[kSaddleLimb], kRiderOffset, seat);
    MtxF seatMatrix = Identity();
    seatMatrix.xw = seat[0];
    seatMatrix.yw = seat[1] - kRiderDrop;
    seatMatrix.zw = seat[2];
    if (rider != nullptr) {
        *rider = Multiply(base, seatMatrix);
    }

    // Eyes: blinking ticks at 20 Hz like the game's
    data.blinkClock += dt * kTicksPerSecond;
    while (data.blinkClock >= 1.0f) {
        data.blinkClock -= 1.0f;
        if (data.blinkStep == 0) {
            if (data.NextRandom() < 0.025f) {
                data.blinkStep = 1;
            }
        } else if (++data.blinkStep >= 4) {
            data.blinkStep = 0;
        }
    }
    static const uint8_t kBlinkEyes[] = { 0, 1, 2, 1 }; // eyeBlinkIndices

    // One matrix for the skinned body (its vertices are already in model space), one per rigid limb
    Mtx* pool = *mtx;
    *mtx += kMatrixCount;
    Mtx* bodyMtx = &pool[0];
    MtxF bodyMatrix = base;
    core1_7F60_guMtxF2L(bodyMatrix.mf, bodyMtx);
    FrameInterpolation_RecordMatrixToMtx(bodyMtx, bodyMatrix.mf);

    gDPPipeSync((*gfx)++);
    gSPSegment((*gfx)++, 0x08, (uintptr_t)(data.obj.data() + kEyeTextures[kBlinkEyes[data.blinkStep]]));
    gSPSetLights1((*gfx)++, sLights);
    gDPSetPrimColor((*gfx)++, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF);
    gDPSetCombineMode((*gfx)++, G_CC_MODULATEIDECALA, G_CC_MODULATEIA_PRIM2);
    gDPSetOtherMode((*gfx)++,
                    G_AD_NOTPATTERN | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_PERSP | G_CYC_2CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | G_RM_PASS | G_RM_AA_ZB_OPA_SURF2);

    // Epona's lists are F3DEX2: switch the renderer to that microcode for them, and back after
    *(*gfx)++ = MakeGfx(((uintptr_t)0xDD << 24) | ucode_f3dex2, 0);
    *(*gfx)++ = MakeGfx(0xD7000002, 0xFFFFFFFF); // G_TEXTURE: on, full scale
    // G_GEOMETRYMODE: G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING | G_SHADING_SMOOTH
    *(*gfx)++ = MakeGfx(0xD9000000, 0x00000001 | 0x00000004 | 0x00000400 | 0x00020000 | 0x00200000);

    // Skin_DrawImpl: limbs in index order
    int next = 1;
    for (int i = 0; i < kLimbCount; i++) {
        const Limb& limb = data.limbs[i];
        if (limb.skinned >= 0) {
            *(*gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)bodyMtx);
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)data.skinned[limb.skinned].lists[slot]);
        } else if (limb.list != nullptr) {
            Mtx* limbMtx = &pool[next++];
            MtxF world = Multiply(base, matrices[i]);
            core1_7F60_guMtxF2L(world.mf, limbMtx);
            FrameInterpolation_RecordMatrixToMtx(limbMtx, world.mf);
            *(*gfx)++ = MakeGfx(kF3dex2Mtx, (uintptr_t)limbMtx);
            *(*gfx)++ = MakeGfx(kF3dex2Dl, (uintptr_t)limb.list);
        }
    }
    *(*gfx)++ = MakeGfx(((uintptr_t)0xDD << 24) | ucode_f3dex, 0);

    gDPPipeSync((*gfx)++);
    gSPClearGeometryMode((*gfx)++, 0xFFFFFFFF);
    gSPSetGeometryMode((*gfx)++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
}

} // namespace PlayAsLinkHorse
