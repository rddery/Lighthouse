#ifndef PORT_ENHANCEMENTS_PLAY_AS_LINK_UTIL_H
#define PORT_ENHANCEMENTS_PLAY_AS_LINK_UTIL_H

// Small helpers the Play as Link files share for reading Ocarina of Time's object data and for doing OoT's matrix
// math (sys_matrix.c) on the renderer's MtxF.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <ultra64.h>

namespace PlayAsLinkUtil {

inline uint16_t Be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

inline uint32_t Be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

inline Gfx MakeGfx(uintptr_t w0, uintptr_t w1) {
    Gfx gfx;
    std::memset(&gfx, 0, sizeof(gfx));
    gfx.words.w0 = w0;
    gfx.words.w1 = w1;
    return gfx;
}

// The renderer marks a segmented address with its lowest bit
inline uintptr_t SegmentRef(uint32_t address) {
    return (uintptr_t)(address & 0x0FFFFFFE) | 1;
}

constexpr uintptr_t kF3dex2Mtx = 0xDA380003; // G_MTX: G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH
constexpr uintptr_t kF3dex2Dl = 0xDE000000;  // G_DL, pushed

// ---- Matrices, with the operations OoT's sys_matrix.c has (applied on the right, to the local frame) ----

inline MtxF MtxIdentity() {
    MtxF m;
    std::memset(&m, 0, sizeof(m));
    m.xx = m.yy = m.zz = m.ww = 1.0f;
    return m;
}

inline MtxF MtxAt(const float position[3]) {
    MtxF m = MtxIdentity();
    m.xw = position[0];
    m.yw = position[1];
    m.zw = position[2];
    return m;
}

inline void MtxTranslate(MtxF& m, float x, float y, float z) {
    m.xw += m.xx * x + m.xy * y + m.xz * z;
    m.yw += m.yx * x + m.yy * y + m.yz * z;
    m.zw += m.zx * x + m.zy * y + m.zz * z;
    m.ww += m.wx * x + m.wy * y + m.wz * z;
}

inline void MtxScale(MtxF& m, float x, float y, float z) {
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

inline void MtxRotateX(MtxF& m, float angle) {
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

inline void MtxRotateY(MtxF& m, float angle) {
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

inline void MtxRotateZ(MtxF& m, float angle) {
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
inline void MtxTransform(const MtxF& m, float x, float y, float z, float out[3]) {
    out[0] = m.xx * x + m.xy * y + m.xz * z + m.xw;
    out[1] = m.yx * x + m.yy * y + m.yz * z + m.yw;
    out[2] = m.zx * x + m.zy * y + m.zz * z + m.zw;
}

// Matrix_TranslateRotateZYX: OoT's limb transform, from a translation and 16-bit angles
inline void MtxTranslateRotateZYX(MtxF& m, const float translation[3], const int16_t rotation[3]) {
    constexpr float kToRad = 3.14159265358979f / 32768.0f;
    MtxTranslate(m, translation[0], translation[1], translation[2]);
    // The rotations are the matrix's own Z, then Y, then X, which is what the three helpers apply in this order
    MtxRotateZ(m, rotation[2] * kToRad);
    MtxRotateY(m, rotation[1] * kToRad);
    MtxRotateX(m, rotation[0] * kToRad);
}

} // namespace PlayAsLinkUtil

#endif
