// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The 3D banner a 3DS shows above a game's icon in the HOME Menu, drawn in software.
//
// A game's banner (the "banner" file of its ExeFS) is a CBMD container: an LZ11-compressed
// CGFX scene and a sound. The scene holds one model - meshes skinned to a small skeleton,
// a handful of textures, a material per mesh - and usually a skeletal animation that loops
// every eight or ten seconds. This reads all of that and draws a frame of it into a pixel
// buffer, depth-buffered, seen square on (the caller may turn it).
//
// Materials are evaluated as the 3DS does: up to three textures, each with its own
// coordinates, transform and wrap mode (or mapped by the normal, for reflections), the six
// combiner stages with their constants and the combiner buffer, then blending - opaque
// meshes first, the blended ones over them. The 3DS's lights are not read; a fixed light
// from the upper right stands in for them, feeding the combiner the two colours a light
// would. Material and visibility animations and the camera are left out.
//
// The sound is the jingle the HOME Menu plays with the banner: a BCWAV of about three
// seconds, decoded here to plain samples.
//
// The layout below was worked out against the banners of five retail games and checked two
// ways: the rest pose rebuilt from each bone's scale, rotation and translation, times the
// inverse base matrix stored beside it, is the identity to five decimals in all of them;
// and frames rendered from the animation show what those banners show on a 3DS.
//
// Everything read from the file is bounds-checked: a banner that does not fit this layout
// yields no model, never a bad read.
namespace SwitchFrontend::Banner {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

struct Texture {
    int w{}, h{};
    std::vector<u32> px; // 0xAABBGGRR, top row first
};

struct Vertex {
    float p[3]{};
    float n[3]{0.0f, 0.0f, 1.0f};
    float uv[3][2]{};
    u8 color[4]{255, 255, 255, 255};
    u8 bone[4]{};
    float weight[4]{1.0f, 0.0f, 0.0f, 0.0f};
};

/// One combiner stage: three arguments for colour and three for alpha, each a source
/// seen through an operand, and how they are combined.
struct Stage {
    u8 src_rgb[3]{}, src_a[3]{};
    u8 op_rgb[3]{}, op_a[3]{};
    u8 comb_rgb{}, comb_a{};
    u8 constant{};
    float scale_rgb{1.0f}, scale_a{1.0f};
    bool buffer_rgb{}, buffer_a{}; // Its result goes into the combiner buffer too.
};

struct Coordinator {
    int source{};  // Which of the vertex's texture coordinates.
    int method{};  // 0 those coordinates, 2 by the normal (a reflection).
    float m[6]{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}; // u' = m0 u + m1 v + m2; v' = m3 u + m4 v + m5
};

struct Material {
    float emission[4]{}, ambient[4]{}, diffuse[4]{1, 1, 1, 1}, spec0[4]{}, spec1[4]{};
    float constant[6][4]{};
    float buffer[4]{0.0f, 0.0f, 0.0f, 1.0f};
    int texture[3]{-1, -1, -1};
    u8 wrap[3][2]{{2, 2}, {2, 2}, {2, 2}}; // 0, 1 clamp; 2 repeat; 3 mirror
    Coordinator coord[3];
    std::vector<Stage> stages;
    u32 blend{0x01010000}; // The 3DS's blend word: equation, source and destination factors.
    u32 uses{};            // Bit per combiner source the stages read.

    bool Opaque() const {
        return ((blend >> 16) & 15) == 1 && ((blend >> 20) & 15) == 0;
    }
};

struct Mesh {
    int material{-1};
    bool skinned{};
    std::vector<Vertex> verts;
    std::vector<u32> indices;
    std::vector<u32> bones; // The skeleton's bones this mesh's vertices refer to.
};

using Mat = std::array<float, 12>; // 3 rows of 4

struct Bone {
    std::string name;
    int parent{-1};
    float s[3]{1, 1, 1}, r[3]{}, t[3]{};
    Mat inverse_base{};
};

struct Key {
    float frame{}, value{}, slope{};
};

struct Component {
    int kind{}; // 0 as the bone has it, 1 constant, 2 curve
    float value{};
    std::vector<Key> keys;
};

struct Track {
    bool present{};
    float start{};
    int width{};
    std::vector<float> values;
};

struct Member {
    int type{}; // 0 none, 5 scale / rotation / translation curves, 8 baked per frame
    std::array<Component, 10> comps; // SX SY SZ RX RY RZ RW TX TY TZ
    Track rotation, translation, scale;
};

struct Model {
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Bone> bones;
    std::vector<Member> members; // One per bone.
    float frames{};
    // Where the model is and how large, seen from the front over the whole animation.
    float centre[3]{};
    float half_w{1}, half_h{1};
    // The jingle: interleaved stereo, `sound_rate` frames a second. Empty when there is none.
    std::vector<std::int16_t> sound;
    int sound_rate{};

    bool Animated() const {
        return frames > 1.0f;
    }
};

namespace detail {

class Reader {
public:
    Reader(const u8* data_, std::size_t size_) : data{data_}, size{size_} {}

    bool In(std::size_t at, std::size_t length) const {
        return at <= size && length <= size - at;
    }
    u32 U32(std::size_t at) const {
        if (!In(at, 4)) {
            return 0;
        }
        u32 v;
        std::memcpy(&v, data + at, 4);
        return v;
    }
    float F32(std::size_t at) const {
        if (!In(at, 4)) {
            return 0.0f;
        }
        float v;
        std::memcpy(&v, data + at, 4);
        return std::isfinite(v) ? v : 0.0f;
    }
    u8 U8(std::size_t at) const {
        return In(at, 1) ? data[at] : 0;
    }
    /// A pointer stored as a distance from where it is written. 0 when null or out of range.
    std::size_t Rel(std::size_t at) const {
        const u32 v = U32(at);
        if (v == 0 || v >= size || at + v >= size) {
            return 0;
        }
        return at + v;
    }
    std::string Str(std::size_t at) const {
        std::string out;
        while (at != 0 && at < size && data[at] != 0 && out.size() < 64) {
            out.push_back(static_cast<char>(data[at++]));
        }
        return out;
    }
    bool Tag(std::size_t at, const char* tag) const {
        return In(at, 4) && std::memcmp(data + at, tag, 4) == 0;
    }

    const u8* data;
    std::size_t size;
};

inline std::vector<std::pair<std::string, std::size_t>> Dict(const Reader& r, std::size_t at) {
    std::vector<std::pair<std::string, std::size_t>> out;
    if (at == 0 || !r.Tag(at, "DICT")) {
        return out;
    }
    const u32 count = std::min<u32>(r.U32(at + 8), 512);
    for (u32 i = 0; i < count; ++i) {
        const std::size_t entry = at + 0x1C + i * 16;
        const std::size_t data = r.Rel(entry + 12);
        if (data != 0) {
            out.emplace_back(r.Str(r.Rel(entry + 8)), data);
        }
    }
    return out;
}

inline bool Lz11(const u8* in, std::size_t in_size, std::vector<u8>& out) {
    if (in_size < 8 || in[0] != 0x11) {
        return false;
    }
    std::size_t size = in[1] | (in[2] << 8) | (static_cast<std::size_t>(in[3]) << 16);
    std::size_t pos = 4;
    if (size == 0) {
        std::memcpy(&size, in + 4, 4);
        size &= 0xFFFFFFFFu;
        pos = 8;
    }
    if (size == 0 || size > (8u << 20)) {
        return false;
    }
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        if (pos >= in_size) {
            return false;
        }
        const u8 flags = in[pos++];
        for (int bit = 0; bit < 8 && out.size() < size; ++bit) {
            if ((flags & (0x80 >> bit)) == 0) {
                if (pos >= in_size) {
                    return false;
                }
                out.push_back(in[pos++]);
                continue;
            }
            if (pos + 1 >= in_size) {
                return false;
            }
            const u8 b0 = in[pos];
            std::size_t length, disp;
            if ((b0 >> 4) == 0) {
                if (pos + 2 >= in_size) {
                    return false;
                }
                length = (((b0 & 0xF) << 4) | (in[pos + 1] >> 4)) + 0x11;
                disp = (((in[pos + 1] & 0xF) << 8) | in[pos + 2]) + 1;
                pos += 3;
            } else if ((b0 >> 4) == 1) {
                if (pos + 3 >= in_size) {
                    return false;
                }
                length = (((b0 & 0xF) << 12) | (in[pos + 1] << 4) | (in[pos + 2] >> 4)) + 0x111;
                disp = (((in[pos + 2] & 0xF) << 8) | in[pos + 3]) + 1;
                pos += 4;
            } else {
                length = (b0 >> 4) + 1;
                disp = (((b0 & 0xF) << 8) | in[pos + 1]) + 1;
                pos += 2;
            }
            if (disp > out.size()) {
                return false;
            }
            for (std::size_t i = 0; i < length && out.size() < size; ++i) {
                out.push_back(out[out.size() - disp]);
            }
        }
    }
    return true;
}

inline u32 Rgba(u32 r, u32 g, u32 b, u32 a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

inline int Clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/// One 4x4 ETC1 block as the 3DS stores it (a little-endian 64-bit word).
inline void Etc1Block(const u8* block, u32 out[16]) {
    static constexpr int mods[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                       {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    u32 lo, hi;
    std::memcpy(&lo, block, 4);
    std::memcpy(&hi, block + 4, 4);
    int base[2][3];
    if ((hi >> 1) & 1) {
        const int r = (hi >> 27) & 31, g = (hi >> 19) & 31, b = (hi >> 11) & 31;
        int dr = (hi >> 24) & 7, dg = (hi >> 16) & 7, db = (hi >> 8) & 7;
        dr -= dr >= 4 ? 8 : 0;
        dg -= dg >= 4 ? 8 : 0;
        db -= db >= 4 ? 8 : 0;
        const int r2 = r + dr, g2 = g + dg, b2 = b + db;
        base[0][0] = (r << 3) | (r >> 2);
        base[0][1] = (g << 3) | (g >> 2);
        base[0][2] = (b << 3) | (b >> 2);
        base[1][0] = ((r2 << 3) | (r2 >> 2)) & 255;
        base[1][1] = ((g2 << 3) | (g2 >> 2)) & 255;
        base[1][2] = ((b2 << 3) | (b2 >> 2)) & 255;
    } else {
        base[0][0] = static_cast<int>((hi >> 28) & 15) * 17;
        base[0][1] = static_cast<int>((hi >> 20) & 15) * 17;
        base[0][2] = static_cast<int>((hi >> 12) & 15) * 17;
        base[1][0] = static_cast<int>((hi >> 24) & 15) * 17;
        base[1][1] = static_cast<int>((hi >> 16) & 15) * 17;
        base[1][2] = static_cast<int>((hi >> 8) & 15) * 17;
    }
    const int table[2] = {static_cast<int>((hi >> 5) & 7), static_cast<int>((hi >> 2) & 7)};
    const bool flip = (hi & 1) != 0;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int i = x * 4 + y;
            const int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
            int mod = mods[table[sub]][(lo >> i) & 1];
            if ((lo >> (16 + i)) & 1) {
                mod = -mod;
            }
            out[y * 4 + x] = Rgba(static_cast<u32>(Clamp8(base[sub][0] + mod)),
                                  static_cast<u32>(Clamp8(base[sub][1] + mod)),
                                  static_cast<u32>(Clamp8(base[sub][2] + mod)), 255);
        }
    }
}

inline bool DecodeTexture(const Reader& r, std::size_t at, Texture& out) {
    const u32 h = r.U32(at + 0x18), w = r.U32(at + 0x1C);
    const u32 format = r.U32(at + 0x34);
    const std::size_t image = r.Rel(at + 0x38);
    const std::size_t data = image != 0 ? r.Rel(image + 0xC) : 0;
    if (w < 8 || h < 8 || w > 1024 || h > 1024 || (w & 7) != 0 || (h & 7) != 0 || data == 0) {
        return false;
    }
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.assign(static_cast<std::size_t>(w) * h, Rgba(255, 255, 255, 255));
    std::size_t pos = data;
    if (format == 12 || format == 13) {
        const std::size_t block = format == 13 ? 16 : 8;
        if (!r.In(data, static_cast<std::size_t>(w / 4) * (h / 4) * block)) {
            return false;
        }
        for (u32 ty = 0; ty < h; ty += 8) {
            for (u32 tx = 0; tx < w; tx += 8) {
                for (int quad = 0; quad < 4; ++quad) {
                    const u32 bx = tx + (quad & 1) * 4, by = ty + (quad >> 1) * 4;
                    std::uint64_t alpha = ~std::uint64_t{0};
                    if (format == 13) {
                        std::memcpy(&alpha, r.data + pos, 8);
                        pos += 8;
                    }
                    u32 colors[16];
                    Etc1Block(r.data + pos, colors);
                    pos += 8;
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            const u32 a = static_cast<u32>((alpha >> ((x * 4 + y) * 4)) & 15) * 17;
                            out.px[static_cast<std::size_t>(by + y) * w + bx + x] =
                                (colors[y * 4 + x] & 0x00FFFFFFu) | (a << 24);
                        }
                    }
                }
            }
        }
        return true;
    }
    // The plain formats: 8x8 tiles, the pixels of each in Morton order.
    static constexpr int bits[] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4};
    if (format >= 12 || !r.In(data, static_cast<std::size_t>(w) * h * bits[format] / 8)) {
        return false;
    }
    for (u32 ty = 0; ty < h; ty += 8) {
        for (u32 tx = 0; tx < w; tx += 8) {
            for (u32 i = 0; i < 64; ++i) {
                const u32 x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
                const u32 y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
                const u8* p = r.data + pos;
                u32 red = 255, green = 255, blue = 255, a = 255;
                switch (format) {
                case 0: // RGBA8
                    a = p[0], blue = p[1], green = p[2], red = p[3];
                    pos += 4;
                    break;
                case 1: // RGB8
                    blue = p[0], green = p[1], red = p[2];
                    pos += 3;
                    break;
                case 2: { // RGBA5551
                    const u32 v = p[0] | (p[1] << 8);
                    red = ((v >> 11) & 31) * 255 / 31, green = ((v >> 6) & 31) * 255 / 31;
                    blue = ((v >> 1) & 31) * 255 / 31, a = (v & 1) * 255;
                    pos += 2;
                    break;
                }
                case 3: { // RGB565
                    const u32 v = p[0] | (p[1] << 8);
                    red = ((v >> 11) & 31) * 255 / 31, green = ((v >> 5) & 63) * 255 / 63;
                    blue = (v & 31) * 255 / 31;
                    pos += 2;
                    break;
                }
                case 4: { // RGBA4
                    const u32 v = p[0] | (p[1] << 8);
                    red = ((v >> 12) & 15) * 17, green = ((v >> 8) & 15) * 17;
                    blue = ((v >> 4) & 15) * 17, a = (v & 15) * 17;
                    pos += 2;
                    break;
                }
                case 5: // LA8
                    a = p[0], red = green = blue = p[1];
                    pos += 2;
                    break;
                case 6: // HILO8: not a colour; shown as grey
                    red = green = blue = p[1];
                    pos += 2;
                    break;
                case 7: // L8
                    red = green = blue = p[0];
                    pos += 1;
                    break;
                case 8: // A8
                    a = p[0];
                    pos += 1;
                    break;
                case 9: // LA4
                    red = green = blue = static_cast<u32>(p[0] >> 4) * 17;
                    a = static_cast<u32>(p[0] & 15) * 17;
                    pos += 1;
                    break;
                default: { // L4, A4: two pixels to a byte
                    const u32 nibble = (i & 1) ? static_cast<u32>(p[0] >> 4) : static_cast<u32>(p[0] & 15);
                    if (format == 10) {
                        red = green = blue = nibble * 17;
                    } else {
                        a = nibble * 17;
                    }
                    pos += i & 1;
                    break;
                }
                }
                out.px[static_cast<std::size_t>(ty + y) * w + tx + x] = Rgba(red, green, blue, a);
            }
        }
    }
    return true;
}

inline Mat Multiply(const Mat& a, const Mat& b) {
    Mat out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] +
                                 a[row * 4 + 2] * b[8 + col] + (col == 3 ? a[row * 4 + 3] : 0.0f);
        }
    }
    return out;
}

/// Scale, then rotation about X, Y and Z in that order, then translation.
inline Mat FromSrt(const float s[3], const float r[3], const float t[3]) {
    const float sx = std::sin(r[0]), cx = std::cos(r[0]);
    const float sy = std::sin(r[1]), cy = std::cos(r[1]);
    const float sz = std::sin(r[2]), cz = std::cos(r[2]);
    Mat m{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz, t[0],
          cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz, t[1],
          -sy,     sx * cy,                cx * cy,                t[2]};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            m[row * 4 + col] *= s[col];
        }
    }
    return m;
}

inline Mat FromQuaternion(const float q[4], const float s[3], const float t[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 1e-6f) {
        x /= length, y /= length, z /= length, w /= length;
    } else {
        x = y = z = 0.0f, w = 1.0f;
    }
    Mat m{1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     t[0],
          2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     t[1],
          2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), t[2]};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            m[row * 4 + col] *= s[col];
        }
    }
    return m;
}

inline float Hermite(const std::vector<Key>& keys, float frame) {
    if (keys.empty()) {
        return 0.0f;
    }
    if (frame <= keys.front().frame) {
        return keys.front().value;
    }
    if (frame >= keys.back().frame) {
        return keys.back().value;
    }
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const Key& a = keys[i];
        const Key& b = keys[i + 1];
        if (frame < a.frame || frame > b.frame) {
            continue;
        }
        const float dt = b.frame - a.frame;
        if (dt <= 0.0f) {
            return b.value;
        }
        const float t = (frame - a.frame) / dt, t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * dt * a.slope +
               (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * dt * b.slope;
    }
    return keys.back().value;
}

/// A baked track at `frame`, between the two stored frames either side of it.
inline bool Sample(const Track& track, float frame, float* out) {
    if (!track.present || track.width <= 0 || track.values.empty()) {
        return false;
    }
    const int count = static_cast<int>(track.values.size()) / track.width;
    const float at = std::clamp(frame - track.start, 0.0f, static_cast<float>(count - 1));
    const int i0 = static_cast<int>(at);
    const int i1 = std::min(count - 1, i0 + 1);
    const float f = at - static_cast<float>(i0);
    for (int k = 0; k < track.width; ++k) {
        const float a = track.values[static_cast<std::size_t>(i0) * track.width + k];
        const float b = track.values[static_cast<std::size_t>(i1) * track.width + k];
        out[k] = a + (b - a) * f;
    }
    return true;
}

/// Each bone's matrix for the vertices bound to it: its place at `frame` times the inverse
/// of its place at rest (the identity while nothing moves).
inline void Pose(const Model& model, float frame, std::vector<Mat>& skin) {
    static thread_local std::vector<Mat> world;
    world.resize(model.bones.size());
    skin.resize(model.bones.size());
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const Bone& bone = model.bones[i];
        float s[3] = {bone.s[0], bone.s[1], bone.s[2]};
        float r[3] = {bone.r[0], bone.r[1], bone.r[2]};
        float t[3] = {bone.t[0], bone.t[1], bone.t[2]};
        Mat local;
        bool have_local = false;
        const Member* member = i < model.members.size() ? &model.members[i] : nullptr;
        if (member != nullptr && member->type == 5) {
            float* const slot[10] = {&s[0], &s[1], &s[2], &r[0], &r[1],
                                     &r[2], nullptr, &t[0], &t[1], &t[2]};
            for (int k = 0; k < 10; ++k) {
                const Component& comp = member->comps[static_cast<std::size_t>(k)];
                if (slot[k] == nullptr || comp.kind == 0) {
                    continue;
                }
                *slot[k] = comp.kind == 1 ? comp.value : Hermite(comp.keys, frame);
            }
        } else if (member != nullptr && member->type == 8) {
            float q[4];
            Sample(member->translation, frame, t);
            Sample(member->scale, frame, s);
            if (Sample(member->rotation, frame, q)) {
                local = FromQuaternion(q, s, t);
                have_local = true;
            }
        }
        if (!have_local) {
            local = FromSrt(s, r, t);
        }
        world[i] = bone.parent >= 0 && bone.parent < static_cast<int>(i)
                       ? Multiply(world[static_cast<std::size_t>(bone.parent)], local)
                       : local;
        skin[i] = Multiply(world[i], bone.inverse_base);
    }
}

inline void Apply(const Mat& m, const float p[3], float out[3]) {
    out[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    out[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    out[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

/// A vertex where the skeleton puts it, and its normal turned the same way (not normalised).
inline void Skin(const Mesh& mesh, const Vertex& v, const std::vector<Mat>& skin, float out[3],
                 float normal[3]) {
    normal[0] = v.n[0], normal[1] = v.n[1], normal[2] = v.n[2];
    out[0] = v.p[0], out[1] = v.p[1], out[2] = v.p[2];
    if (!mesh.skinned) {
        return;
    }
    float sum[3] = {0, 0, 0}, turned[3] = {0, 0, 0};
    float total = 0.0f;
    for (int k = 0; k < 4; ++k) {
        if (v.weight[k] <= 0.0f || v.bone[k] >= mesh.bones.size()) {
            continue;
        }
        const u32 bone = mesh.bones[v.bone[k]];
        if (bone >= skin.size()) {
            continue;
        }
        const Mat& m = skin[bone];
        float p[3];
        Apply(m, v.p, p);
        const float w = v.weight[k];
        sum[0] += p[0] * w;
        sum[1] += p[1] * w;
        sum[2] += p[2] * w;
        turned[0] += (m[0] * v.n[0] + m[1] * v.n[1] + m[2] * v.n[2]) * w;
        turned[1] += (m[4] * v.n[0] + m[5] * v.n[1] + m[6] * v.n[2]) * w;
        turned[2] += (m[8] * v.n[0] + m[9] * v.n[1] + m[10] * v.n[2]) * w;
        total += w;
    }
    if (total <= 0.0f) {
        return;
    }
    out[0] = sum[0] / total, out[1] = sum[1] / total, out[2] = sum[2] / total;
    normal[0] = turned[0], normal[1] = turned[1], normal[2] = turned[2];
}

struct Attribute {
    std::size_t data{};
    u32 stride{}, format{}, count{}, offset{};
    float scale{1.0f};
    bool present{};
};

inline float ReadComponent(const Reader& r, const Attribute& a, u32 vertex, u32 component) {
    if (!a.present || component >= a.count) {
        return 0.0f;
    }
    const std::size_t size = a.format == 0x1406 ? 4 : (a.format == 0x1402 || a.format == 0x1403) ? 2 : 1;
    const std::size_t at = a.data + static_cast<std::size_t>(vertex) * a.stride + a.offset + component * size;
    if (!r.In(at, size)) {
        return 0.0f;
    }
    switch (a.format) {
    case 0x1400:
        return static_cast<float>(static_cast<std::int8_t>(r.data[at])) * a.scale;
    case 0x1401:
        return static_cast<float>(r.data[at]) * a.scale;
    case 0x1402: {
        std::int16_t v;
        std::memcpy(&v, r.data + at, 2);
        return static_cast<float>(v) * a.scale;
    }
    case 0x1403: {
        u16 v;
        std::memcpy(&v, r.data + at, 2);
        return static_cast<float>(v) * a.scale;
    }
    default:
        return r.F32(at) * a.scale;
    }
}

inline void ReadAnimation(const Reader& r, std::size_t at, Model& model) {
    model.frames = std::clamp(r.F32(at + 0x14), 0.0f, 36000.0f);
    for (const auto& [name, m] : Dict(r, r.Rel(at + 0x1C))) {
        const auto bone = std::find_if(model.bones.begin(), model.bones.end(),
                                       [&](const Bone& b) { return b.name == name; });
        if (bone == model.bones.end()) {
            continue;
        }
        Member& member = model.members[static_cast<std::size_t>(bone - model.bones.begin())];
        const u32 flags = r.U32(m);
        const u32 type = r.U32(m + 0x10);
        if (type == 5) {
            member.type = 5;
            for (u32 k = 0; k < 10; ++k) {
                const std::size_t slot = m + 0x14 + 4 * k;
                Component& comp = member.comps[k];
                if (flags & (1u << (6 + k))) {
                    comp.kind = 1;
                    comp.value = r.F32(slot);
                    continue;
                }
                const std::size_t curve = (flags & (1u << (16 + k))) ? 0 : r.Rel(slot);
                if (curve == 0) {
                    continue;
                }
                bool readable = true;
                const u32 segments = std::min<u32>(r.U32(curve + 0x10), 64);
                for (u32 si = 0; si < segments && readable; ++si) {
                    const std::size_t seg = r.Rel(curve + 0x14 + si * 4);
                    if (seg == 0) {
                        readable = false;
                        break;
                    }
                    const u32 quantization = (r.U32(seg + 8) >> 5) & 7;
                    const u32 keys = std::min<u32>(r.U32(seg + 0xC), 4096);
                    // The three layouts that are plain floats; the packed ones leave the
                    // component as the bone has it.
                    const u32 width = quantization == 3 ? 12 : quantization == 0 ? 16
                                                             : quantization == 6 ? 8 : 0;
                    if (width == 0 || !r.In(seg + 0x14, static_cast<std::size_t>(keys) * width)) {
                        readable = false;
                        break;
                    }
                    for (u32 key = 0; key < keys; ++key) {
                        const std::size_t ka = seg + 0x14 + static_cast<std::size_t>(key) * width;
                        Key out;
                        out.frame = r.F32(ka);
                        out.value = r.F32(ka + 4);
                        out.slope = quantization == 3 ? r.F32(ka + 8)
                                    : quantization == 0 ? r.F32(ka + 12) : 0.0f;
                        comp.keys.push_back(out);
                    }
                }
                if (readable && !comp.keys.empty()) {
                    std::sort(comp.keys.begin(), comp.keys.end(),
                              [](const Key& a, const Key& b) { return a.frame < b.frame; });
                    comp.kind = 2;
                } else {
                    comp.keys.clear();
                }
            }
        } else if (type == 8) {
            member.type = 8;
            const auto track = [&](std::size_t pointer, int width, Track& out) {
                const std::size_t t = r.Rel(pointer);
                if (t == 0) {
                    return;
                }
                const float start = r.F32(t), end = r.F32(t + 4);
                const bool constant = (r.U32(t + 0xC) & 1) != 0;
                const int count = constant ? 1 : std::clamp(static_cast<int>(end - start) + 1, 1, 6000);
                const std::size_t step = static_cast<std::size_t>(width) * 4 + 4;
                out.start = start;
                out.width = width;
                for (int k = 0; k < count; ++k) {
                    const std::size_t at_frame = t + 0x10 + static_cast<std::size_t>(k) * step;
                    if (!r.In(at_frame, static_cast<std::size_t>(width) * 4)) {
                        break;
                    }
                    for (int c = 0; c < width; ++c) {
                        out.values.push_back(r.F32(at_frame + static_cast<std::size_t>(c) * 4));
                    }
                }
                out.present = !out.values.empty();
            };
            track(m + 0x14, 4, member.rotation);
            track(m + 0x18, 3, member.translation);
            track(m + 0x1C, 3, member.scale);
        }
    }
}

inline void Unpack(u32 packed, float out[4]) {
    for (int k = 0; k < 4; ++k) {
        out[k] = static_cast<float>((packed >> (8 * k)) & 255) / 255.0f;
    }
}

/// A material: its colours, where its textures come from and how they are laid on, the
/// combiner stages and the blending. `end` is where the next thing in the file starts.
inline Material ReadMaterial(const Reader& r, std::size_t at, std::size_t end,
                             const std::vector<std::string>& texture_names) {
    Material m;
    u32 packed[11];
    for (u32 i = 0; i < 11; ++i) {
        packed[i] = r.U32(at + 0xD8 + 4 * i);
    }
    Unpack(packed[0], m.emission);
    Unpack(packed[1], m.ambient);
    Unpack(packed[2], m.diffuse);
    Unpack(packed[3], m.spec0);
    Unpack(packed[4], m.spec1);
    for (int i = 0; i < 6; ++i) {
        Unpack(packed[5 + i], m.constant[i]);
    }
    for (u32 c = 0; c < 3; ++c) {
        const std::size_t base = at + 0x16C + c * 0x58;
        Coordinator& coord = m.coord[c];
        coord.source = static_cast<int>(std::min<u32>(r.U32(base), 2));
        coord.method = static_cast<int>(r.U32(base + 4));
        static constexpr int cells[6] = {0, 1, 3, 4, 5, 7};
        for (int k = 0; k < 6; ++k) {
            coord.m[k] = r.F32(base + 0x28 + static_cast<std::size_t>(cells[k]) * 4);
        }
        // A transform that was never filled in would collapse the texture to a point.
        if (coord.m[0] == 0.0f && coord.m[1] == 0.0f && coord.m[3] == 0.0f && coord.m[4] == 0.0f) {
            coord.m[0] = coord.m[4] = 1.0f;
        }
    }
    for (u32 i = 0; i < 3; ++i) {
        const std::size_t mapper = r.Rel(at + 0x274 + 4 * i);
        if (mapper == 0) {
            continue;
        }
        const std::size_t reference = r.Rel(mapper + 8);
        const std::string wanted = reference != 0 ? r.Str(r.Rel(reference + 0x18)) : std::string{};
        const auto found = std::find(texture_names.begin(), texture_names.end(), wanted);
        if (found != texture_names.end() && !wanted.empty()) {
            m.texture[i] = static_cast<int>(found - texture_names.begin());
        }
        // The sampler's settings sit in the mapper as the GPU command that sets them.
        for (std::size_t o = 0; o < 0x80; o += 4) {
            const u32 header = r.U32(mapper + o);
            if (header == 0x809F0081u || header == 0x809F0091u || header == 0x809F0099u) {
                const u32 params = r.U32(mapper + o + 8);
                m.wrap[i][0] = static_cast<u8>((params >> 12) & 7);
                m.wrap[i][1] = static_cast<u8>((params >> 8) & 7);
                break;
            }
        }
    }
    // The rest is found by the GPU commands the material carries for it.
    static constexpr u32 stage_headers[6] = {0x804F00C0u, 0x804F00C8u, 0x804F00D0u,
                                             0x804F00D8u, 0x804F00F0u, 0x804F00F8u};
    u32 buffer_update = 0;
    std::vector<Stage> stages;
    for (std::size_t o = at + 8; o + 20 <= end; o += 4) {
        const u32 v = r.U32(o);
        if (stages.size() < 6 && v == stage_headers[stages.size()]) {
            const u32 src = r.U32(o - 4), opr = r.U32(o + 4), comb = r.U32(o + 8), scale = r.U32(o + 16);
            Stage st;
            st.constant = static_cast<u8>(std::min<u32>(r.U32(o - 8), 6));
            for (u32 k = 0; k < 3; ++k) {
                st.src_rgb[k] = static_cast<u8>((src >> (4 * k)) & 15);
                st.src_a[k] = static_cast<u8>((src >> (16 + 4 * k)) & 15);
                st.op_rgb[k] = static_cast<u8>((opr >> (4 * k)) & 15);
                st.op_a[k] = static_cast<u8>((opr >> (12 + 4 * k)) & 7);
            }
            st.comb_rgb = static_cast<u8>(comb & 15);
            st.comb_a = static_cast<u8>((comb >> 16) & 15);
            st.scale_rgb = static_cast<float>(1u << std::min<u32>(scale & 3, 2));
            st.scale_a = static_cast<float>(1u << std::min<u32>((scale >> 16) & 3, 2));
            stages.push_back(st);
        } else if (v == 0x803F0100u) {
            m.blend = r.U32(o + 4);
        } else if (v == 0x000200E0u) {
            buffer_update = r.U32(o - 4);
        } else if (v == 0x000F00FDu) {
            Unpack(r.U32(o - 4), m.buffer);
        }
    }
    for (std::size_t i = 0; i < stages.size(); ++i) {
        Stage& st = stages[i];
        st.buffer_rgb = i < 4 && ((buffer_update >> (8 + i)) & 1) != 0;
        st.buffer_a = i < 4 && ((buffer_update >> (12 + i)) & 1) != 0;
        // A stage that only hands on what it was given costs time for nothing.
        const bool passes = i > 0 && st.comb_rgb == 0 && st.comb_a == 0 && st.src_rgb[0] == 15 &&
                            st.src_a[0] == 15 && st.op_rgb[0] == 0 && st.op_a[0] == 0 &&
                            st.scale_rgb == 1.0f && st.scale_a == 1.0f && !st.buffer_rgb && !st.buffer_a;
        if (passes) {
            continue;
        }
        // How many arguments the combination reads: the others are not looked up.
        static constexpr int arguments[10] = {1, 2, 2, 2, 3, 2, 2, 2, 3, 3};
        const int rgb_n = st.comb_rgb < 10 ? arguments[st.comb_rgb] : 1;
        const int a_n = st.comb_a < 10 ? arguments[st.comb_a] : 1;
        for (int k = 0; k < 3; ++k) {
            if (k >= rgb_n) {
                st.src_rgb[k] = 15;
                st.op_rgb[k] = 0;
            }
            if (k >= a_n) {
                st.src_a[k] = 15;
                st.op_a[k] = 0;
            }
            m.uses |= (1u << st.src_rgb[k]) | (1u << st.src_a[k]);
        }
        m.stages.push_back(st);
    }
    if (m.stages.empty()) {
        // Nothing readable: the first texture times the vertex colour.
        Stage st;
        st.src_rgb[0] = st.src_a[0] = 3;
        st.src_rgb[1] = st.src_a[1] = 0;
        st.comb_rgb = st.comb_a = 1;
        m.uses = (1u << 3) | 1u;
        m.stages.push_back(st);
    }
    return m;
}

/// The banner's jingle: a BCWAV, as 16-bit samples or as the DSP's ADPCM.
inline void ReadSound(const u8* data, std::size_t size, Model& model) {
    const Reader r{data, size};
    if (!r.Tag(0, "CWAV")) {
        return;
    }
    const std::size_t info = r.U32(0x18), body = r.U32(0x24);
    if (info == 0 || body == 0 || !r.In(info, 0x40) || !r.In(body, 8)) {
        return;
    }
    const u32 encoding = r.U8(info + 8);
    const u32 rate = r.U32(info + 0xC);
    const u32 total = std::min<u32>(r.U32(info + 0x14), 48000u * 20);
    const u32 channels = std::min<u32>(r.U32(info + 0x1C), 2);
    if ((encoding != 1 && encoding != 2) || rate < 4000 || rate > 96000 || total == 0 || channels == 0) {
        return;
    }
    const std::size_t table = info + 0x1C;
    std::vector<std::int16_t> out(static_cast<std::size_t>(total) * 2, 0);
    for (u32 ch = 0; ch < channels; ++ch) {
        const std::size_t channel = table + r.U32(table + 4 + ch * 8 + 4);
        std::size_t pos = body + 8 + r.U32(channel + 4);
        if (encoding == 1) {
            if (!r.In(pos, static_cast<std::size_t>(total) * 2)) {
                return;
            }
            for (u32 i = 0; i < total; ++i) {
                std::memcpy(&out[static_cast<std::size_t>(i) * 2 + ch], data + pos + i * 2, 2);
            }
            continue;
        }
        const std::size_t adpcm = channel + r.U32(channel + 12);
        if (!r.In(adpcm, 0x26) || !r.In(pos, (static_cast<std::size_t>(total) + 13) / 14 * 8)) {
            return;
        }
        std::int16_t coefficients[16], history[2];
        std::memcpy(coefficients, data + adpcm, sizeof(coefficients));
        std::memcpy(history, data + adpcm + 0x22, sizeof(history));
        int h1 = history[0], h2 = history[1];
        for (u32 done = 0; done < total; pos += 8) {
            const u8 head = data[pos];
            const int predictor = (head >> 4) & 7, shift = head & 15;
            const int c1 = coefficients[predictor * 2], c2 = coefficients[predictor * 2 + 1];
            for (u32 k = 0; k < 14 && done < total; ++k, ++done) {
                const u8 byte = data[pos + 1 + k / 2];
                int nibble = (k & 1) == 0 ? byte >> 4 : byte & 15;
                nibble -= nibble >= 8 ? 16 : 0;
                const int sample = std::clamp(
                    static_cast<int>((static_cast<std::int64_t>(nibble) * (std::int64_t{1} << shift) * 2048 + 1024 +
                                      static_cast<std::int64_t>(c1) * h1 +
                                      static_cast<std::int64_t>(c2) * h2) >> 11),
                    -32768, 32767);
                out[static_cast<std::size_t>(done) * 2 + ch] = static_cast<std::int16_t>(sample);
                h2 = h1;
                h1 = sample;
            }
        }
    }
    if (channels == 1) {
        for (u32 i = 0; i < total; ++i) {
            out[static_cast<std::size_t>(i) * 2 + 1] = out[static_cast<std::size_t>(i) * 2];
        }
    }
    model.sound = std::move(out);
    model.sound_rate = static_cast<int>(rate);
}

} // namespace detail

/// Reads a game's banner file. Null when it is not a banner this understands.
inline std::shared_ptr<Model> Parse(const std::vector<u8>& banner) {
    using namespace detail;
    if (banner.size() < 0x88 || std::memcmp(banner.data(), "CBMD", 4) != 0) {
        return nullptr;
    }
    u32 start = 0, sound = 0;
    std::memcpy(&start, banner.data() + 8, 4);
    std::memcpy(&sound, banner.data() + 0x84, 4);
    std::size_t end = sound != 0 && sound <= banner.size() ? sound : banner.size();
    if (start < 0x88 || start >= end) {
        return nullptr;
    }
    std::vector<u8> cgfx;
    if (!Lz11(banner.data() + start, end - start, cgfx) || cgfx.size() < 0x100) {
        return nullptr;
    }
    const Reader r{cgfx.data(), cgfx.size()};
    if (!r.Tag(0, "CGFX") || !r.Tag(0x14, "DATA")) {
        return nullptr;
    }
    const auto section = [&](int index) {
        const std::size_t at = 0x1C + static_cast<std::size_t>(index) * 8;
        return r.U32(at) != 0 ? Dict(r, r.Rel(at + 4)) : std::vector<std::pair<std::string, std::size_t>>{};
    };
    const auto models = section(0);
    if (models.empty() || !r.Tag(models[0].second + 4, "CMDL")) {
        return nullptr;
    }
    const std::size_t cmdl = models[0].second;
    auto model = std::make_shared<Model>();

    std::vector<std::string> texture_names;
    for (const auto& [name, at] : section(1)) {
        Texture texture;
        if (!DecodeTexture(r, at, texture)) {
            texture.w = texture.h = 1;
            texture.px.assign(1, Rgba(200, 200, 200, 255));
        }
        texture_names.push_back(name);
        model->textures.push_back(std::move(texture));
    }

    {
        const auto entries = Dict(r, r.Rel(cmdl + 0xC0));
        for (const auto& [name, at] : entries) {
            // A material runs to wherever the next one starts.
            std::size_t end = std::min(at + 0x600, cgfx.size());
            for (const auto& [other_name, other] : entries) {
                if (other > at && other < end) {
                    end = other;
                }
            }
            model->materials.push_back(ReadMaterial(r, at, end, texture_names));
        }
    }

    if (r.U32(cmdl) & 0x80) {
        if (const std::size_t skeleton = r.Rel(cmdl + 0xE0); skeleton != 0) {
            const auto entries = Dict(r, r.Rel(skeleton + 0x1C));
            model->bones.resize(entries.size());
            for (const auto& [name, at] : entries) {
                const u32 index = r.U32(at + 8);
                if (index >= model->bones.size()) {
                    continue;
                }
                Bone& bone = model->bones[index];
                bone.name = name;
                bone.parent = static_cast<s32>(r.U32(at + 0xC));
                for (u32 k = 0; k < 3; ++k) {
                    bone.s[k] = r.F32(at + 0x20 + k * 4);
                    bone.r[k] = r.F32(at + 0x2C + k * 4);
                    bone.t[k] = r.F32(at + 0x38 + k * 4);
                }
                for (u32 k = 0; k < 12; ++k) {
                    bone.inverse_base[k] = r.F32(at + 0xA4 + k * 4);
                }
            }
        }
    }
    model->members.resize(model->bones.size());

    const std::size_t mesh_table = r.Rel(cmdl + 0xB8);
    const std::size_t shape_table = r.Rel(cmdl + 0xC8);
    const u32 mesh_count = std::min<u32>(r.U32(cmdl + 0xB4), 256);
    const u32 shape_count = r.U32(cmdl + 0xC4);
    std::size_t triangles = 0;
    for (u32 mi = 0; mi < mesh_count && mesh_table != 0 && shape_table != 0; ++mi) {
        const std::size_t me = r.Rel(mesh_table + mi * 4);
        if (me == 0 || r.U8(me + 0x24) == 0) {
            continue;
        }
        const u32 shape_index = r.U32(me + 0x18), material_index = r.U32(me + 0x1C);
        if (shape_index >= shape_count) {
            continue;
        }
        const std::size_t shape = r.Rel(shape_table + shape_index * 4);
        if (shape == 0) {
            continue;
        }
        const float offset[3] = {r.F32(shape + 0x20), r.F32(shape + 0x24), r.F32(shape + 0x28)};

        // Attributes by what they are for: 0 position, 1 normal, 3 colour, 4 to 6 the
        // texture coordinates, 7 bone index, 8 bone weight.
        Attribute attributes[9];
        u32 vertex_count = 0;
        const std::size_t group_table = r.Rel(shape + 0x3C);
        for (u32 gi = 0; gi < std::min<u32>(r.U32(shape + 0x38), 8) && group_table != 0; ++gi) {
            const std::size_t group = r.Rel(group_table + gi * 4);
            if (group == 0 || r.U32(group) != 0x40000002u) {
                continue; // A constant for the whole shape: the defaults stand in for it.
            }
            const u32 length = r.U32(group + 0x14);
            const std::size_t data = r.Rel(group + 0x18);
            const u32 stride = r.U32(group + 0x24);
            if (data == 0 || stride == 0 || stride > 256 || !r.In(data, length)) {
                continue;
            }
            vertex_count = std::min<u32>(length / stride, 65536);
            const std::size_t table = r.Rel(group + 0x2C);
            for (u32 ai = 0; ai < std::min<u32>(r.U32(group + 0x28), 16) && table != 0; ++ai) {
                const std::size_t a = r.Rel(table + ai * 4);
                const u32 usage = a != 0 ? r.U32(a + 4) : 99;
                if (usage > 8) {
                    continue;
                }
                Attribute& out = attributes[usage];
                out.data = data;
                out.stride = stride;
                out.format = r.U32(a + 0x24) & 0xFFFF;
                out.count = std::min<u32>(r.U32(a + 0x28), 4);
                out.scale = r.F32(a + 0x2C);
                out.offset = r.U32(a + 0x30);
                out.present = out.offset < stride;
            }
        }
        if (vertex_count == 0 || !attributes[0].present) {
            continue;
        }

        const std::size_t set_table = r.Rel(shape + 0x30);
        for (u32 pi = 0; pi < std::min<u32>(r.U32(shape + 0x2C), 16) && set_table != 0; ++pi) {
            const std::size_t set = r.Rel(set_table + pi * 4);
            if (set == 0) {
                continue;
            }
            Mesh mesh;
            mesh.material = material_index < model->materials.size() ? static_cast<int>(material_index) : -1;
            const u32 related = std::min<u32>(r.U32(set), 32);
            const std::size_t bone_table = r.Rel(set + 4);
            for (u32 b = 0; b < related && bone_table != 0; ++b) {
                mesh.bones.push_back(r.U32(bone_table + b * 4));
            }
            const u32 skinning = r.U32(set + 8); // 0 one bone, 1 a bone per vertex, 2 weighted
            mesh.skinned = !mesh.bones.empty() && !model->bones.empty();
            mesh.verts.resize(vertex_count);
            for (u32 v = 0; v < vertex_count; ++v) {
                Vertex& out = mesh.verts[v];
                for (u32 k = 0; k < 3; ++k) {
                    out.p[k] = ReadComponent(r, attributes[0], v, k) + offset[k];
                }
                if (attributes[1].present) {
                    for (u32 k = 0; k < 3; ++k) {
                        out.n[k] = ReadComponent(r, attributes[1], v, k);
                    }
                }
                for (u32 set_index = 0; set_index < 3; ++set_index) {
                    if (attributes[4 + set_index].present) {
                        out.uv[set_index][0] = ReadComponent(r, attributes[4 + set_index], v, 0);
                        out.uv[set_index][1] = ReadComponent(r, attributes[4 + set_index], v, 1);
                    }
                }
                if (attributes[3].present) {
                    for (u32 k = 0; k < 4; ++k) {
                        const float c = k < attributes[3].count ? ReadComponent(r, attributes[3], v, k) : 1.0f;
                        out.color[k] = static_cast<u8>(Clamp8(static_cast<int>(c * 255.0f + 0.5f)));
                    }
                }
                if (skinning == 1 && attributes[7].present) {
                    out.bone[0] = static_cast<u8>(std::clamp(ReadComponent(r, attributes[7], v, 0), 0.0f, 255.0f));
                } else if (skinning == 2 && attributes[7].present && attributes[8].present) {
                    const u32 n = std::min<u32>({attributes[7].count, attributes[8].count, 4u});
                    for (u32 k = 0; k < 4; ++k) {
                        out.bone[k] = k < n ? static_cast<u8>(std::clamp(
                                                  ReadComponent(r, attributes[7], v, k), 0.0f, 255.0f))
                                            : 0;
                        out.weight[k] = k < n ? ReadComponent(r, attributes[8], v, k) : 0.0f;
                    }
                }
            }
            const std::size_t primitive_table = r.Rel(set + 0x10);
            for (u32 qi = 0; qi < std::min<u32>(r.U32(set + 0xC), 16) && primitive_table != 0; ++qi) {
                const std::size_t primitive = r.Rel(primitive_table + qi * 4);
                const std::size_t stream_table = primitive != 0 ? r.Rel(primitive + 4) : 0;
                for (u32 si = 0; si < std::min<u32>(r.U32(primitive), 16) && stream_table != 0; ++si) {
                    const std::size_t stream = r.Rel(stream_table + si * 4);
                    if (stream == 0) {
                        continue;
                    }
                    const bool wide = (r.U32(stream) & 0xFFFF) == 0x1403;
                    const u32 length = r.U32(stream + 8);
                    const std::size_t data = r.Rel(stream + 0xC);
                    if (data == 0 || !r.In(data, length)) {
                        continue;
                    }
                    const u32 count = wide ? length / 2 : length;
                    for (u32 i = 0; i + 2 < count && triangles < 60000; i += 3, ++triangles) {
                        u32 tri[3];
                        for (u32 k = 0; k < 3; ++k) {
                            if (wide) {
                                u16 index;
                                std::memcpy(&index, r.data + data + (i + k) * 2, 2);
                                tri[k] = index;
                            } else {
                                tri[k] = r.data[data + i + k];
                            }
                        }
                        if (tri[0] < vertex_count && tri[1] < vertex_count && tri[2] < vertex_count) {
                            mesh.indices.insert(mesh.indices.end(), {tri[0], tri[1], tri[2]});
                        }
                    }
                }
            }
            if (!mesh.indices.empty()) {
                model->meshes.push_back(std::move(mesh));
            }
        }
    }
    if (model->meshes.empty()) {
        return nullptr;
    }

    if (const auto animations = section(9); !animations.empty() && !model->bones.empty()) {
        ReadAnimation(r, animations[0].second, *model);
    }

    // Where it is and how large, over the whole loop, so the picture does not pump.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<Mat> skin;
    const int samples = model->Animated() ? 6 : 1;
    for (int sample = 0; sample < samples; ++sample) {
        Pose(*model, model->frames * static_cast<float>(sample) / static_cast<float>(samples), skin);
        for (const Mesh& mesh : model->meshes) {
            for (const Vertex& vertex : mesh.verts) {
                float p[3], n[3];
                Skin(mesh, vertex, skin, p, n);
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], p[k]);
                    hi[k] = std::max(hi[k], p[k]);
                }
            }
        }
    }
    for (int k = 0; k < 3; ++k) {
        model->centre[k] = (lo[k] + hi[k]) / 2;
    }
    model->half_w = std::max(1e-3f, (hi[0] - lo[0]) / 2);
    model->half_h = std::max(1e-3f, (hi[1] - lo[1]) / 2);
    if (sound != 0 && sound < banner.size()) {
        ReadSound(banner.data() + sound, banner.size() - sound, *model);
    }
    return model;
}

namespace detail {

/// A vertex ready to be drawn: where it lands, and what a light gives it.
struct Lit {
    float x{}, y{}, z{};
    float primary[3]{};   // The diffuse side of the light.
    float secondary[4]{}; // Its specular side; alpha is how much the surface turns away.
    float env[2]{};       // Texture coordinates by the normal.
};

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

inline float Wrap(u8 mode, float t) {
    if (mode < 2) {
        return Clamp01(t) * 0.9999f;
    }
    if (mode == 3) {
        const float f = t - 2.0f * std::floor(t * 0.5f);
        return (f > 1.0f ? 2.0f - f : f) * 0.9999f;
    }
    return t - std::floor(t);
}

inline void OperandRgb(u8 op, const float* c, float out[3]) {
    switch (op) {
    case 1:
        out[0] = 1 - c[0], out[1] = 1 - c[1], out[2] = 1 - c[2];
        break;
    case 2:
        out[0] = out[1] = out[2] = c[3];
        break;
    case 3:
        out[0] = out[1] = out[2] = 1 - c[3];
        break;
    case 4:
        out[0] = out[1] = out[2] = c[0];
        break;
    case 5:
        out[0] = out[1] = out[2] = 1 - c[0];
        break;
    case 8:
        out[0] = out[1] = out[2] = c[1];
        break;
    case 9:
        out[0] = out[1] = out[2] = 1 - c[1];
        break;
    case 12:
        out[0] = out[1] = out[2] = c[2];
        break;
    case 13:
        out[0] = out[1] = out[2] = 1 - c[2];
        break;
    default:
        out[0] = c[0], out[1] = c[1], out[2] = c[2];
        break;
    }
}

inline float OperandAlpha(u8 op, const float* c) {
    switch (op) {
    case 1:
        return 1 - c[3];
    case 2:
        return c[0];
    case 3:
        return 1 - c[0];
    case 4:
        return c[1];
    case 5:
        return 1 - c[1];
    case 6:
        return c[2];
    case 7:
        return 1 - c[2];
    default:
        return c[3];
    }
}

inline float Combine(u8 mode, float x, float y, float z) {
    switch (mode) {
    case 1:
        return x * y;
    case 2:
        return x + y;
    case 3:
        return x + y - 0.5f;
    case 4:
        return x * z + y * (1 - z);
    case 5:
        return x - y;
    case 8:
        return x * y + z;
    case 9:
        return (x + y) * z;
    default:
        return x;
    }
}

inline void BlendFactor(u32 code, const float* src, const float* dst, float out[3]) {
    switch (code) {
    case 0:
        out[0] = out[1] = out[2] = 0.0f;
        break;
    case 2:
        out[0] = src[0], out[1] = src[1], out[2] = src[2];
        break;
    case 3:
        out[0] = 1 - src[0], out[1] = 1 - src[1], out[2] = 1 - src[2];
        break;
    case 4:
        out[0] = dst[0], out[1] = dst[1], out[2] = dst[2];
        break;
    case 5:
        out[0] = 1 - dst[0], out[1] = 1 - dst[1], out[2] = 1 - dst[2];
        break;
    case 6:
        out[0] = out[1] = out[2] = src[3];
        break;
    case 7:
        out[0] = out[1] = out[2] = 1 - src[3];
        break;
    case 9:
        out[0] = out[1] = out[2] = 0.0f; // What is drawn on is taken as solid.
        break;
    default:
        out[0] = out[1] = out[2] = 1.0f;
        break;
    }
}

} // namespace detail

/// Draws the model as it stands `seconds` into its loop, turned by `yaw` and `pitch`
/// (radians), into `out` (`width` x `height`, 0xAABBGGRR). `out` must come in cleared to
/// zero: pixels the model does not cover stay zero, so the caller's own background can show
/// there, and `background` is the colour of that background, for what blends with it.
inline void Render(const Model& model, float seconds, float yaw, float pitch, u32* out, int width,
                   int height, u32 background = 0xFF181210u) {
    using namespace detail;
    if (width <= 0 || height <= 0) {
        return;
    }
    static thread_local std::vector<float> depth;
    static thread_local std::vector<Mat> skin;
    static thread_local std::vector<std::vector<Lit>> lit_meshes;
    depth.assign(static_cast<std::size_t>(width) * height, -1e30f);
    const float frame = model.Animated() ? std::fmod(std::max(0.0f, seconds) * 60.0f, model.frames) : 0.0f;
    Pose(model, frame, skin);

    const float scale = 0.92f * std::min(static_cast<float>(width) / (2 * model.half_w),
                                         static_cast<float>(height) / (2 * model.half_h));
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    // The light: from the upper right, a little in front. And halfway between it and the eye.
    static constexpr float light[3] = {0.25f, 0.45f, 0.86f};
    const float half_length = std::sqrt(light[0] * light[0] + light[1] * light[1] +
                                        (light[2] + 1.0f) * (light[2] + 1.0f));
    const float half[3] = {light[0] / half_length, light[1] / half_length, (light[2] + 1.0f) / half_length};
    const float back[3] = {static_cast<float>(background & 255) / 255.0f,
                           static_cast<float>((background >> 8) & 255) / 255.0f,
                           static_cast<float>((background >> 16) & 255) / 255.0f};
    static const Material plain = [] {
        Material m;
        Stage st;
        st.src_rgb[0] = st.src_a[0] = 3;
        st.src_rgb[1] = st.src_a[1] = 0;
        st.comb_rgb = st.comb_a = 1;
        m.uses = (1u << 3) | 1u;
        m.stages.push_back(st);
        return m;
    }();

    // Every vertex once: where it lands and what the light gives it.
    lit_meshes.resize(model.meshes.size());
    for (std::size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const Mesh& mesh = model.meshes[mi];
        const Material& material = mesh.material >= 0 ? model.materials[static_cast<std::size_t>(mesh.material)] : plain;
        std::vector<Lit>& lit = lit_meshes[mi];
        lit.resize(mesh.verts.size());
        for (std::size_t i = 0; i < mesh.verts.size(); ++i) {
            float p[3], n[3];
            Skin(mesh, mesh.verts[i], skin, p, n);
            const float x = p[0] - model.centre[0], y = p[1] - model.centre[1],
                        z = p[2] - model.centre[2];
            const float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
            const float y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
            Lit& l = lit[i];
            l.x = static_cast<float>(width) / 2 + x1 * scale;
            l.y = static_cast<float>(height) / 2 - y2 * scale;
            l.z = z2;
            const float nx1 = n[0] * cy + n[2] * sy, nz1 = -n[0] * sy + n[2] * cy;
            float nx = nx1, ny = n[1] * cp - nz1 * sp, nz = n[1] * sp + nz1 * cp;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 1e-6f) {
                nx /= length, ny /= length, nz /= length;
            } else {
                nx = ny = 0.0f, nz = 1.0f;
            }
            const float ndl = std::max(0.0f, nx * light[0] + ny * light[1] + nz * light[2]);
            const float ndh = std::max(0.0f, nx * half[0] + ny * half[1] + nz * half[2]);
            const float ndh2 = ndh * ndh, ndh4 = ndh2 * ndh2, gloss = ndh4 * ndh4;
            for (int k = 0; k < 3; ++k) {
                l.primary[k] = Clamp01(material.emission[k] + material.ambient[k] * 0.25f +
                                       material.diffuse[k] * (0.2f + 0.8f * ndl));
                l.secondary[k] = Clamp01(material.spec0[k] * 0.5f + material.spec1[k] * gloss);
            }
            const float away = 1.0f - Clamp01(nz);
            l.secondary[3] = away * away;
            l.env[0] = nx * 0.5f + 0.5f;
            l.env[1] = ny * 0.5f + 0.5f;
        }
    }

    // What is solid first, writing depth; then what blends, over it and behind nothing new.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t mi = 0; mi < model.meshes.size(); ++mi) {
            const Mesh& mesh = model.meshes[mi];
            const Material& material = mesh.material >= 0 ? model.materials[static_cast<std::size_t>(mesh.material)] : plain;
            const bool opaque = material.Opaque();
            if (opaque != (pass == 0)) {
                continue;
            }
            const std::vector<Lit>& lit = lit_meshes[mi];
            const u32 uses = material.uses;
            const u32 equation = material.blend & 7;
            const u32 src_factor = (material.blend >> 16) & 15, dst_factor = (material.blend >> 20) & 15;
            const Texture* textures[3] = {nullptr, nullptr, nullptr};
            for (int t = 0; t < 3; ++t) {
                const int index = material.texture[t];
                if ((uses & (8u << t)) != 0 && index >= 0 && index < static_cast<int>(model.textures.size())) {
                    textures[t] = &model.textures[static_cast<std::size_t>(index)];
                }
            }
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                const u32 ia = mesh.indices[t], ib = mesh.indices[t + 1], ic = mesh.indices[t + 2];
                const Lit& a = lit[ia];
                const Lit& b = lit[ib];
                const Lit& c = lit[ic];
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (std::abs(area) < 0.01f) {
                    continue;
                }
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
                const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
                const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
                if (x0 > x1 || y0 > y1) {
                    continue;
                }
                const Vertex& va = mesh.verts[ia];
                const Vertex& vb = mesh.verts[ib];
                const Vertex& vc = mesh.verts[ic];
                const float inv = 1.0f / area;
                const float wa_dx = (b.y - c.y) * inv, wb_dx = (c.y - a.y) * inv;
                for (int y = y0; y <= y1; ++y) {
                    const float py = static_cast<float>(y) + 0.5f, px0 = static_cast<float>(x0) + 0.5f;
                    float wa = ((b.x - px0) * (c.y - py) - (b.y - py) * (c.x - px0)) * inv;
                    float wb = ((c.x - px0) * (a.y - py) - (c.y - py) * (a.x - px0)) * inv;
                    const std::size_t row = static_cast<std::size_t>(y) * width;
                    for (int x = x0; x <= x1; ++x, wa += wa_dx, wb += wb_dx) {
                        const float wc = 1.0f - wa - wb;
                        if (wa < -0.0005f || wb < -0.0005f || wc < -0.0005f) {
                            continue;
                        }
                        const float z = wa * a.z + wb * b.z + wc * c.z;
                        if (z <= depth[row + x]) {
                            continue;
                        }
                        // What the combiner may read. Index = the 3DS's number for the source.
                        float colour[4] = {1, 1, 1, 1}, primary[4] = {1, 1, 1, 1}, secondary[4] = {0, 0, 0, 1};
                        float texel[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
                        float buffer[4] = {material.buffer[0], material.buffer[1], material.buffer[2],
                                           material.buffer[3]};
                        float previous[4] = {0, 0, 0, 1};
                        if (uses & 1u) {
                            for (int k = 0; k < 4; ++k) {
                                colour[k] = (wa * va.color[k] + wb * vb.color[k] + wc * vc.color[k]) * (1.0f / 255.0f);
                            }
                        }
                        if (uses & 2u) {
                            for (int k = 0; k < 3; ++k) {
                                primary[k] = wa * a.primary[k] + wb * b.primary[k] + wc * c.primary[k];
                            }
                        }
                        if (uses & 4u) {
                            for (int k = 0; k < 4; ++k) {
                                secondary[k] = wa * a.secondary[k] + wb * b.secondary[k] + wc * c.secondary[k];
                            }
                        }
                        for (int ti = 0; ti < 3; ++ti) {
                            const Texture* texture = textures[ti];
                            if (texture == nullptr) {
                                continue;
                            }
                            const Coordinator& coord = material.coord[ti];
                            float u, v;
                            if (coord.method == 2) {
                                u = wa * a.env[0] + wb * b.env[0] + wc * c.env[0];
                                v = wa * a.env[1] + wb * b.env[1] + wc * c.env[1];
                            } else {
                                const int s = coord.source;
                                u = wa * va.uv[s][0] + wb * vb.uv[s][0] + wc * vc.uv[s][0];
                                v = wa * va.uv[s][1] + wb * vb.uv[s][1] + wc * vc.uv[s][1];
                            }
                            const float tu = coord.m[0] * u + coord.m[1] * v + coord.m[2];
                            const float tv = coord.m[3] * u + coord.m[4] * v + coord.m[5];
                            const int tx = std::min(texture->w - 1, static_cast<int>(Wrap(material.wrap[ti][0], tu) *
                                                                                     static_cast<float>(texture->w)));
                            const int ty = std::min(texture->h - 1, static_cast<int>(Wrap(material.wrap[ti][1], 1.0f - tv) *
                                                                                     static_cast<float>(texture->h)));
                            const u32 px = texture->px[static_cast<std::size_t>(std::max(ty, 0)) * texture->w + std::max(tx, 0)];
                            texel[ti][0] = static_cast<float>(px & 255) * (1.0f / 255.0f);
                            texel[ti][1] = static_cast<float>((px >> 8) & 255) * (1.0f / 255.0f);
                            texel[ti][2] = static_cast<float>((px >> 16) & 255) * (1.0f / 255.0f);
                            texel[ti][3] = static_cast<float>(px >> 24) * (1.0f / 255.0f);
                        }
                        static constexpr float nothing[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        const float* sources[16] = {colour,   primary,  secondary, texel[0], texel[1], texel[2],
                                                    previous, previous, previous,  previous, previous, previous,
                                                    previous, buffer,   nothing,   previous};
                        for (const Stage& st : material.stages) {
                            sources[14] = st.constant < 6 ? material.constant[st.constant] : nothing;
                            float rgb[3][3], result[4];
                            for (int k = 0; k < 3; ++k) {
                                OperandRgb(st.op_rgb[k], sources[st.src_rgb[k]], rgb[k]);
                            }
                            if (st.comb_rgb == 6 || st.comb_rgb == 7) {
                                const float dot = 4.0f * ((rgb[0][0] - 0.5f) * (rgb[1][0] - 0.5f) +
                                                          (rgb[0][1] - 0.5f) * (rgb[1][1] - 0.5f) +
                                                          (rgb[0][2] - 0.5f) * (rgb[1][2] - 0.5f));
                                result[0] = result[1] = result[2] = dot;
                            } else {
                                for (int k = 0; k < 3; ++k) {
                                    result[k] = Combine(st.comb_rgb, rgb[0][k], rgb[1][k], rgb[2][k]);
                                }
                            }
                            result[3] = Combine(st.comb_a, OperandAlpha(st.op_a[0], sources[st.src_a[0]]),
                                                OperandAlpha(st.op_a[1], sources[st.src_a[1]]),
                                                OperandAlpha(st.op_a[2], sources[st.src_a[2]]));
                            for (int k = 0; k < 3; ++k) {
                                previous[k] = Clamp01(result[k] * st.scale_rgb);
                            }
                            previous[3] = Clamp01(result[3] * st.scale_a);
                            if (st.buffer_rgb) {
                                buffer[0] = previous[0], buffer[1] = previous[1], buffer[2] = previous[2];
                            }
                            if (st.buffer_a) {
                                buffer[3] = previous[3];
                            }
                        }
                        float final_colour[3] = {previous[0], previous[1], previous[2]};
                        if (opaque) {
                            depth[row + x] = z;
                        } else {
                            const u32 under = out[row + x];
                            const float dst[4] = {under != 0 ? static_cast<float>(under & 255) / 255.0f : back[0],
                                                  under != 0 ? static_cast<float>((under >> 8) & 255) / 255.0f : back[1],
                                                  under != 0 ? static_cast<float>((under >> 16) & 255) / 255.0f : back[2],
                                                  1.0f};
                            float sf[3], df[3];
                            BlendFactor(src_factor, previous, dst, sf);
                            BlendFactor(dst_factor, previous, dst, df);
                            for (int k = 0; k < 3; ++k) {
                                const float s = previous[k] * sf[k], d = dst[k] * df[k];
                                final_colour[k] = Clamp01(equation == 1 ? s - d : equation == 2 ? d - s : s + d);
                            }
                        }
                        out[row + x] = 0xFF000000u |
                                       (static_cast<u32>(final_colour[2] * 255.0f + 0.5f) << 16) |
                                       (static_cast<u32>(final_colour[1] * 255.0f + 0.5f) << 8) |
                                       static_cast<u32>(final_colour[0] * 255.0f + 0.5f);
                    }
                }
            }
        }
    }
}

} // namespace SwitchFrontend::Banner
