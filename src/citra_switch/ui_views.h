// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The launcher's larger views: the library (a shelf of covers standing on a reflective floor),
// the bar under it, the navigation drawer of the options screen, and the small sheets shared
// with it. Colours come from the theme in ui_canvas.h.
//
// Everything here draws on a Ui::Canvas from plain data and knows nothing about the console,
// which is what lets tools/ui-preview render these views on a PC.

#pragma once

#include <functional>
#include <span>

#include "citra_switch/ui_canvas.h"

namespace SwitchFrontend::Ui {

// ---------------------------------------------------------------------------------------------
// Pixels

/// An RGBA picture in the canvas's own pixel layout (MakeColor).
struct Image {
    int w{};
    int h{};
    std::vector<u32> px;

    bool Empty() const {
        return w <= 0 || h <= 0 || px.empty();
    }
};

inline u32 MixColor(u32 a, u32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<u32>(x + (y - x) * t + 0.5f) & 0xFF;
    };
    return (0xFFu << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

inline u32 WithAlpha(u32 color, u8 alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<u32>(alpha) << 24);
}

inline void FillGradientV(Canvas& c, int x, int y, int w, int h, u32 top, u32 bottom) {
    for (int row = 0; row < h; ++row) {
        c.FillRect(x, y + row, w, 1, MixColor(top, bottom, h <= 1 ? 0.0f : row / float(h - 1)));
    }
}

/// A disc with a soft edge. `r_inner` > 0 makes it a ring.
inline void FillDisc(Canvas& c, float cx, float cy, float r, u32 color, float r_inner = 0.0f) {
    const int x0 = static_cast<int>(std::floor(cx - r - 1)), x1 = static_cast<int>(cx + r + 2);
    const int y0 = static_cast<int>(std::floor(cy - r - 1)), y1 = static_cast<int>(cy + r + 2);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) +
                                      (y + 0.5f - cy) * (y + 0.5f - cy));
            float cover = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (r_inner > 0.0f) {
                cover *= std::clamp(d - r_inner + 0.5f, 0.0f, 1.0f);
            }
            if (cover > 0.0f) {
                c.Blend(x, y, color, static_cast<u8>(cover * 255.0f));
            }
        }
    }
}

/// A straight stroke with round ends.
inline void StrokeLine(Canvas& c, float ax, float ay, float bx, float by, float width, u32 color) {
    const float half = width / 2;
    const int x0 = static_cast<int>(std::floor(std::min(ax, bx) - half - 1));
    const int x1 = static_cast<int>(std::max(ax, bx) + half + 2);
    const int y0 = static_cast<int>(std::floor(std::min(ay, by) - half - 1));
    const int y1 = static_cast<int>(std::max(ay, by) + half + 2);
    const float dx = bx - ax, dy = by - ay;
    const float len2 = std::max(dx * dx + dy * dy, 0.0001f);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f - ax, py = y + 0.5f - ay;
            const float t = std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f);
            const float ex = px - dx * t, ey = py - dy * t;
            const float cover = std::clamp(half - std::sqrt(ex * ex + ey * ey) + 0.5f, 0.0f, 1.0f);
            if (cover > 0.0f) {
                c.Blend(x, y, color, static_cast<u8>(cover * 255.0f));
            }
        }
    }
}

/// The ring round whatever has the focus. It breathes: `time` is in seconds.
inline void DrawFocusRing(Canvas& c, int x, int y, int w, int h, int radius, float time,
                          u32 inner) {
    const float pulse = 0.5f + 0.5f * std::sin(time * 4.2f);
    const u32 ring = MixColor(kColFocus, kColAccent, pulse * 0.55f);
    c.RoundBorder(x - 3, y - 3, w + 6, h + 6, radius + 3, 3, ring, inner);
}

/// Bilinear sample of `img` at (u, v) in pixels.
inline u32 SampleImage(const Image& img, float u, float v) {
    u = std::clamp(u, 0.0f, static_cast<float>(img.w - 1));
    v = std::clamp(v, 0.0f, static_cast<float>(img.h - 1));
    const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
    const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    const float fx = u - x0, fy = v - y0;
    const u32 c00 = img.px[y0 * img.w + x0], c10 = img.px[y0 * img.w + x1];
    const u32 c01 = img.px[y1 * img.w + x0], c11 = img.px[y1 * img.w + x1];
    u32 out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const float top = ((c00 >> shift) & 0xFF) * (1 - fx) + ((c10 >> shift) & 0xFF) * fx;
        const float bot = ((c01 >> shift) & 0xFF) * (1 - fx) + ((c11 >> shift) & 0xFF) * fx;
        out |= (static_cast<u32>(top * (1 - fy) + bot * fy + 0.5f) & 0xFF) << shift;
    }
    return out;
}

/// Where a picture lands on screen: a quad whose left and right edges are vertical, which is
/// all a shelf of covers turned about a vertical axis needs.
struct Quad {
    float x0{}, x1{};   // Left and right edge.
    float top0{}, h0{}; // Top and height at the left edge.
    float top1{}, h1{}; // Top and height at the right edge.
};

/// Draws the part of `img` between `u0` and `u1` (0..1 across the picture) into `q`.
/// `shade` moves the picture towards `shade_to` (0 = as is, 1 = that colour). With `mirror`
/// the picture is drawn upside down under the quad and fades out downwards, as a reflection
/// in the floor; `mirror_gap` is how far above the floor the picture hangs. `smooth` filters
/// the picture; without it the nearest pixel is taken, which is several times cheaper and is
/// what the covers at the sides and the reflections get - a shelf redraws every frame, in
/// software.
inline void BlitQuad(Canvas& c, const Image& img, const Quad& q, float u0, float u1, float shade,
                     u32 shade_to, bool mirror, bool smooth, float mirror_gap = 0.0f) {
    if (img.Empty() || q.x1 <= q.x0) {
        return;
    }
    const int xa = std::max(0, static_cast<int>(std::floor(q.x0)));
    const int xb = std::min(c.Width(), static_cast<int>(std::ceil(q.x1)));
    const u32 keep = static_cast<u32>((1.0f - std::clamp(shade, 0.0f, 1.0f)) * 256.0f);
    const u32 lose = 256 - keep;
    const u32 tr = shade_to & 0xFF, tg = (shade_to >> 8) & 0xFF, tb = (shade_to >> 16) & 0xFF;
    u32* const pixels = c.Data();
    const int stride = c.Width();
    for (int x = xa; x < xb; ++x) {
        const float t = std::clamp((x + 0.5f - q.x0) / (q.x1 - q.x0), 0.0f, 1.0f);
        const float top = q.top0 + (q.top1 - q.top0) * t;
        const float h = q.h0 + (q.h1 - q.h0) * t;
        if (h < 1.0f) {
            continue;
        }
        const float u = (u0 + (u1 - u0) * t) * (img.w - 1);
        const int ui = std::clamp(static_cast<int>(u + 0.5f), 0, img.w - 1);
        // The reflection hangs as far below the floor as the picture does above it.
        const float floor = top + h + mirror_gap;
        const float y_begin = mirror ? floor + mirror_gap : top;
        const float y_end = mirror ? y_begin + h * 0.42f : top + h;
        const int ya = std::max(0, static_cast<int>(std::floor(y_begin)));
        const int yb = std::min(c.Height(), static_cast<int>(std::ceil(y_end)));
        const float v_step = (img.h - 1) / h;
        for (int y = ya; y < yb; ++y) {
            float v;
            u32 fade = 256;
            if (mirror) {
                v = (img.h - 1) - (y + 0.5f - y_begin) * v_step; // Bottom row first.
                const float f = 1.0f - (y + 0.5f - y_begin) / (y_end - y_begin);
                fade = static_cast<u32>(64.0f * std::clamp(f * f, 0.0f, 1.0f));
            } else {
                v = (y + 0.5f - top) * v_step;
            }
            if (v < 0.0f || v > static_cast<float>(img.h - 1)) {
                continue;
            }
            const u32 color =
                smooth ? SampleImage(img, u, v)
                       : img.px[static_cast<std::size_t>(static_cast<int>(v + 0.5f)) * img.w + ui];
            // A see-through pixel (round icons have them) leaves what is behind it alone.
            const u32 alpha = color >> 24;
            if (alpha < 32) {
                continue;
            }
            u32 r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
            if (keep < 256) {
                r = (r * keep + tr * lose) >> 8;
                g = (g * keep + tg * lose) >> 8;
                b = (b * keep + tb * lose) >> 8;
            }
            u32& dst = pixels[static_cast<std::size_t>(y) * stride + x];
            if (fade < 256) {
                const u32 inv = 256 - fade;
                r = (r * fade + (dst & 0xFF) * inv) >> 8;
                g = (g * fade + ((dst >> 8) & 0xFF) * inv) >> 8;
                b = (b * fade + ((dst >> 16) & 0xFF) * inv) >> 8;
            }
            dst = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Icons, drawn from primitives so they scale and need no assets.

enum class Icon { Gear, Power, Search, Flip, Shelf, Info, Refresh, Back, Warning, Check };

inline void DrawIcon(Canvas& c, Icon icon, float cx, float cy, float size, u32 color) {
    const float r = size / 2;
    const float w = std::max(2.0f, size / 9);
    switch (icon) {
    case Icon::Gear:
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f;
            StrokeLine(c, cx + std::cos(a) * r * 0.62f, cy + std::sin(a) * r * 0.62f,
                       cx + std::cos(a) * r * 0.98f, cy + std::sin(a) * r * 0.98f, w * 1.5f, color);
        }
        FillDisc(c, cx, cy, r * 0.72f, color, r * 0.30f);
        break;
    case Icon::Power:
        for (int i = 0; i <= 26; ++i) {
            const float a = -1.5707963f + 0.75f + i * (6.2831853f - 1.5f) / 26;
            FillDisc(c, cx + std::cos(a) * r * 0.78f, cy + std::sin(a) * r * 0.78f + r * 0.08f,
                     w / 2, color);
        }
        StrokeLine(c, cx, cy - r * 0.95f, cx, cy - r * 0.05f, w, color);
        break;
    case Icon::Search:
        FillDisc(c, cx - r * 0.18f, cy - r * 0.18f, r * 0.62f, color, r * 0.62f - w);
        StrokeLine(c, cx + r * 0.30f, cy + r * 0.30f, cx + r * 0.88f, cy + r * 0.88f, w * 1.2f,
                   color);
        break;
    case Icon::Flip:
        StrokeLine(c, cx - r * 0.75f, cy - r * 0.25f, cx + r * 0.55f, cy - r * 0.25f, w, color);
        StrokeLine(c, cx + r * 0.55f, cy - r * 0.25f, cx + r * 0.20f, cy - r * 0.62f, w, color);
        StrokeLine(c, cx + r * 0.75f, cy + r * 0.25f, cx - r * 0.55f, cy + r * 0.25f, w, color);
        StrokeLine(c, cx - r * 0.55f, cy + r * 0.25f, cx - r * 0.20f, cy + r * 0.62f, w, color);
        break;
    case Icon::Shelf:
        for (int i = -1; i <= 1; ++i) {
            const float h = i == 0 ? r * 1.5f : r * 1.05f;
            const float bw = i == 0 ? r * 0.62f : r * 0.40f;
            c.FillRoundRect(static_cast<int>(cx + i * r * 0.64f - bw / 2),
                            static_cast<int>(cy - h / 2), static_cast<int>(bw),
                            static_cast<int>(h), 2, color);
        }
        break;
    case Icon::Info:
        FillDisc(c, cx, cy, r * 0.95f, color, r * 0.95f - w);
        FillDisc(c, cx, cy - r * 0.42f, w * 0.75f, color);
        StrokeLine(c, cx, cy - r * 0.10f, cx, cy + r * 0.48f, w, color);
        break;
    case Icon::Refresh:
        for (int i = 0; i <= 24; ++i) {
            const float a = -0.6f + i * 4.9f / 24;
            FillDisc(c, cx + std::cos(a) * r * 0.75f, cy + std::sin(a) * r * 0.75f, w / 2, color);
        }
        StrokeLine(c, cx + r * 0.62f, cy - r * 0.42f, cx + r * 0.95f, cy - r * 0.80f, w, color);
        StrokeLine(c, cx + r * 0.62f, cy - r * 0.42f, cx + r * 0.20f, cy - r * 0.55f, w, color);
        break;
    case Icon::Back:
        StrokeLine(c, cx + r * 0.75f, cy, cx - r * 0.70f, cy, w, color);
        StrokeLine(c, cx - r * 0.70f, cy, cx - r * 0.15f, cy - r * 0.55f, w, color);
        StrokeLine(c, cx - r * 0.70f, cy, cx - r * 0.15f, cy + r * 0.55f, w, color);
        break;
    case Icon::Warning:
        StrokeLine(c, cx, cy - r * 0.85f, cx - r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx, cy - r * 0.85f, cx + r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx - r * 0.92f, cy + r * 0.72f, cx + r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx, cy - r * 0.30f, cx, cy + r * 0.22f, w, color);
        FillDisc(c, cx, cy + r * 0.48f, w * 0.7f, color);
        break;
    case Icon::Check:
        StrokeLine(c, cx - r * 0.70f, cy + r * 0.05f, cx - r * 0.18f, cy + r * 0.55f, w * 1.2f,
                   color);
        StrokeLine(c, cx - r * 0.18f, cy + r * 0.55f, cx + r * 0.78f, cy - r * 0.50f, w * 1.2f,
                   color);
        break;
    }
}

/// Word-wraps `text` to `max_w` pixels.
inline std::vector<std::string> WrapLines(Font& font, std::string_view text, int size, int max_w) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t end = std::min(text.find(' ', i), text.size());
        const std::string word{text.substr(i, end - i)};
        const std::string candidate = line.empty() ? word : line + ' ' + word;
        if (!line.empty() && font.Measure(candidate, size) > max_w) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        i = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------
// The shelf

enum class ShelfStyle { Coverflow, Shelf, Carousel, Arc, Stack, Spotlight, Count };

inline const char* ShelfStyleName(ShelfStyle style) {
    switch (style) {
    case ShelfStyle::Coverflow:
        return "Coverflow";
    case ShelfStyle::Shelf:
        return "Shelf";
    case ShelfStyle::Carousel:
        return "Carousel";
    case ShelfStyle::Arc:
        return "Arc";
    case ShelfStyle::Stack:
        return "Stack";
    case ShelfStyle::Spotlight:
        return "Spotlight";
    default:
        return "";
    }
}

/// One game on the shelf: its cover if there is one, its icon otherwise.
struct ShelfItem {
    const Image* cover{}; // Front, or the full wrap (back + spine + front) when `full_wrap`.
    bool full_wrap{};
    const std::vector<u32>* icon{}; // Square, `icon_size` a side.
    int icon_size{};
    std::string_view title;
    std::string_view publisher;
    std::string_view badge;             // File type and the like.
    std::vector<std::string> back_text; // Shown on the back when there is no artwork for it.
};

/// A 3DS case is a little wider than tall.
constexpr float kCaseAspect = 1.11f;
/// In a full wrap the back and the front each take this share of the width; the spine the rest.
constexpr float kWrapFace = 0.4667f;

struct ShelfPose {
    float cx{};     // Centre.
    float width{};  // On-screen width.
    float h_near{}; // Height of the edge towards the middle of the shelf.
    float h_far{};  // Height of the edge away from it.
    float lift{};   // How far above the floor it hangs.
    float shade{};
};

/// Where the cover `d` places from the selected one sits. `d` is fractional while the shelf
/// slides, which is the whole animation: the caller eases its position and redraws.
inline ShelfPose PoseFor(ShelfStyle style, float d, float cx, float case_h) {
    const float case_w = case_h * kCaseAspect;
    const float ad = std::abs(d);
    const float sign = d < 0 ? -1.0f : 1.0f;
    const float t = std::min(ad, 1.0f); // 0 in the middle, 1 once fully to the side.
    const float beyond = std::max(0.0f, ad - 1.0f);
    ShelfPose p;
    switch (style) {
    case ShelfStyle::Shelf: // Upright cases side by side, the outer ones turned a little inwards.
        p.cx = cx + sign * case_w * (0.96f * t + 0.70f * beyond);
        p.width = case_w * (1.0f - 0.26f * t);
        p.h_near = case_h * (1.0f - 0.20f * t);
        p.h_far = case_h * (1.0f - 0.27f * t);
        p.shade = std::min(0.45f, 0.06f * t + 0.10f * beyond);
        break;
    case ShelfStyle::Carousel: { // Flat, shrinking with distance.
        const float scale = std::max(0.42f, 1.0f - 0.20f * ad);
        p.cx = cx + sign * case_w * (0.94f * t + 0.74f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.55f, 0.16f * ad);
        break;
    }
    case ShelfStyle::Arc: { // Flat, rising towards the sides like the rim of a bowl.
        const float scale = std::max(0.46f, 1.0f - 0.16f * ad);
        p.cx = cx + sign * case_w * (0.86f * t + 0.62f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.lift = case_h * 0.055f * ad * ad;
        p.shade = std::min(0.50f, 0.12f * ad);
        break;
    }
    case ShelfStyle::Stack: { // A fanned hand of cards.
        const float scale = std::max(0.50f, 1.0f - 0.10f * ad);
        p.cx = cx + sign * case_w * (0.46f * t + 0.20f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.70f, 0.22f * ad);
        break;
    }
    case ShelfStyle::Spotlight: { // One large case, the rest small along the floor.
        const float scale = 1.18f - 0.74f * t;
        p.cx = cx + sign * case_w * (1.02f * t + 0.52f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.50f, 0.30f * t + 0.05f * beyond);
        break;
    }
    default: // Coverflow: side covers turn away and close ranks.
        p.cx = cx + sign * case_w * (0.70f * t + 0.30f * beyond);
        p.width = case_w * (1.0f - 0.60f * t);
        p.h_near = case_h * (1.0f - 0.14f * t);
        p.h_far = case_h * (1.0f - 0.30f * t);
        p.shade = std::min(0.60f, 0.12f * t + 0.10f * beyond);
        break;
    }
    return p;
}

/// The back of a case that has no back artwork: what is known about the game, as text.
inline void DrawTextBack(Canvas& c, Font& font, const ShelfItem& item, int x, int y, int w, int h) {
    c.FillRoundRect(x, y, w, h, 10, kColSurface);
    c.FillRoundRect(x, y, std::max(3, w / 40), h, 2, kColAccent);
    if (w < 120) {
        return;
    }
    const int pad = w / 12;
    int ty = y + pad + 22;
    for (const std::string& line : WrapLines(font, item.title, 22, w - 2 * pad)) {
        font.Draw(c, x + pad, ty, line, 22, kColText);
        ty += 28;
    }
    if (!item.publisher.empty()) {
        font.Draw(c, x + pad, ty, font.Truncate(item.publisher, 16, w - 2 * pad), 16, kColAccent);
        ty += 26;
    }
    ty += 8;
    for (const std::string& line : item.back_text) {
        if (ty > y + h - pad) {
            break;
        }
        font.Draw(c, x + pad, ty, font.Truncate(line, 16, w - 2 * pad), 16, kColTextDim);
        ty += 23;
    }
}

struct ShelfView {
    int x{}, y{}, w{}, h{}; // Area the shelf may use.
    ShelfStyle style{ShelfStyle::Coverflow};
    float position{}; // Eased index of the selected game.
    int count{};
    float flip{}; // 0 = front, 1 = back, in between while turning.
    float time{}; // Seconds; drives the gentle bobbing of the games that are not selected.
};

inline float ShelfCaseHeight(const ShelfView& v) {
    return std::min(v.h * 0.58f, v.w * 0.40f / kCaseAspect * 1.6f);
}

/// Y of the floor the games stand on (and are reflected in).
inline float ShelfFloorY(const ShelfView& v) {
    return v.y + v.h * 0.47f + ShelfCaseHeight(v) / 2;
}

/// Draws the shelf. `get` supplies each game on demand, so nothing is built for games that are
/// off screen.
inline void DrawShelf(Canvas& c, Font& font, const ShelfView& v,
                      const std::function<ShelfItem(int)>& get) {
    if (v.count <= 0) {
        return;
    }
    const float case_h = ShelfCaseHeight(v);
    const float cx = v.x + v.w / 2.0f;
    const float floor = ShelfFloorY(v);
    const int centre = static_cast<int>(std::lround(v.position));
    constexpr int reach = 5;
    // Covers darken into the backdrop: towards black on dark, towards the floor on light.
    const u32 shade_to = g_theme_light ? kColShelfHorizon : MakeColor(0, 0, 0);
    static Image icon_image; // Reused: an icon is copied into it to be drawn like a cover.

    const auto draw_one = [&](int index) {
        const float d = index - v.position;
        const float away = std::min(std::abs(d), 1.0f);
        const ShelfPose p = PoseFor(v.style, d, cx, case_h);
        // Games that are not selected hover, each to its own beat.
        const float bob = away * case_h * 0.014f * (1.0f + std::sin(v.time * 1.7f + index * 1.9f));
        const float gap = p.lift + bob;
        const bool left_side = d < 0;

        const ShelfItem item = get(index);
        const bool selected = index == centre;
        const float turn = selected ? v.flip : 0.0f;
        const bool show_back = turn > 0.5f;
        // Turning about the vertical axis: the case narrows to nothing, then widens again
        // showing its other side.
        const float squash =
            turn > 0.0f ? std::max(std::abs(std::cos(turn * 3.14159265f)), 0.02f) : 1.0f;

        const bool has_art = item.cover != nullptr && !item.cover->Empty();
        const bool has_icon = item.icon != nullptr && !item.icon->empty() && item.icon_size > 0;

        if (!has_art) {
            // No cover: the game's icon on its own, standing on the floor like the cases do.
            const float side = p.h_near * 0.62f;
            if (show_back) {
                const float w = case_h * kCaseAspect * squash;
                DrawTextBack(c, font, item, static_cast<int>(p.cx - w / 2),
                             static_cast<int>(floor - case_h), static_cast<int>(w),
                             static_cast<int>(case_h));
                return;
            }
            Quad q;
            q.x0 = p.cx - side * squash / 2;
            q.x1 = p.cx + side * squash / 2;
            q.h0 = q.h1 = side;
            q.top0 = q.top1 = floor - gap - side;
            if (has_icon) {
                icon_image.w = icon_image.h = item.icon_size;
                icon_image.px = *item.icon;
                BlitQuad(c, icon_image, q, 0.0f, 1.0f, p.shade, shade_to, true, false, gap);
                BlitQuad(c, icon_image, q, 0.0f, 1.0f, p.shade, shade_to, false, selected);
            } else {
                // Not even an icon: a soft disc with the first letter of the title.
                const float r = side / 2;
                FillDisc(c, p.cx, floor - gap - r, r * squash, MixColor(kColBadge, shade_to, p.shade));
                if (!item.title.empty() && squash > 0.6f) {
                    const std::string letter{item.title.substr(0, 1)};
                    const int size = static_cast<int>(r);
                    font.Draw(c, static_cast<int>(p.cx) - font.Measure(letter, size) / 2,
                              static_cast<int>(floor - gap - r + size * 0.35f), letter, size,
                              MixColor(kColText, shade_to, p.shade));
                }
            }
            return;
        }

        Quad q;
        const float half = p.width * squash / 2;
        q.x0 = p.cx - half;
        q.x1 = p.cx + half;
        // The edge towards the middle of the shelf is the tall one.
        const float h_left = left_side ? p.h_far : p.h_near;
        const float h_right = left_side ? p.h_near : p.h_far;
        q.h0 = p.h_near + (h_left - p.h_near) * away;
        q.h1 = p.h_near + (h_right - p.h_near) * away;
        q.top0 = floor - gap - q.h0;
        q.top1 = floor - gap - q.h1;

        float u0 = 0.0f, u1 = 1.0f;
        if (item.full_wrap) {
            // back | spine | front
            u0 = show_back ? 0.0f : 1.0f - kWrapFace;
            u1 = show_back ? kWrapFace : 1.0f;
        }
        if (show_back && !item.full_wrap) {
            DrawTextBack(c, font, item, static_cast<int>(q.x0), static_cast<int>(q.top0),
                         static_cast<int>(q.x1 - q.x0), static_cast<int>(q.h0));
            return;
        }
        BlitQuad(c, *item.cover, q, u0, u1, p.shade, shade_to, true, false, gap);
        BlitQuad(c, *item.cover, q, u0, u1, p.shade, shade_to, false, selected);
    };

    // Far to near, so nearer covers overlap the ones behind; the selected one goes last.
    for (int k = reach; k >= 1; --k) {
        if (centre - k >= 0) {
            draw_one(centre - k);
        }
        if (centre + k < v.count) {
            draw_one(centre + k);
        }
    }
    if (centre >= 0 && centre < v.count) {
        draw_one(centre);
    }
}

// ---------------------------------------------------------------------------------------------
// Buttons and bars

/// A round button with an icon, and optionally the pad button that triggers it as a small
/// chip. Hit-testing is the caller's (centre + radius).
inline void DrawRoundButton(Canvas& c, Font& font, Icon icon, int cx, int cy, int radius,
                            std::string_view pad_hint, bool highlighted) {
    const u32 fill = highlighted ? kColAccent : kColSurfaceHi;
    const u32 fg = highlighted ? kColOnAccent : kColText;
    FillDisc(c, static_cast<float>(cx), static_cast<float>(cy), radius + 1.5f, kColLine);
    FillDisc(c, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(radius), fill);
    DrawIcon(c, icon, static_cast<float>(cx), static_cast<float>(cy), radius * 1.05f, fg);
    if (!pad_hint.empty()) {
        const int tw = font.Measure(pad_hint, 15);
        const int chip_w = std::max(22, tw + 12);
        const int chip_x = cx + radius - chip_w / 2 - 2;
        const int chip_y = cy - radius - 6;
        c.FillRoundRect(chip_x, chip_y, chip_w, 22, 11, kColBadge);
        font.Draw(c, chip_x + (chip_w - tw) / 2, CenterBaseline(chip_y, 22, 15), pad_hint, 15,
                  kColText);
    }
}

/// Width DrawPill gives a pill.
inline int PillWidth(Font& font, std::string_view pad_hint, std::string_view label) {
    const int hint_w = pad_hint.empty() ? 0 : std::max(24, font.Measure(pad_hint, 16) + 14);
    return 14 + hint_w + (hint_w ? 8 : 0) + font.Measure(label, 17) + 16;
}

/// A text button (pill).
inline int DrawPill(Canvas& c, Font& font, int x, int y, std::string_view pad_hint,
                    std::string_view label, bool highlighted = false) {
    constexpr int h = 34;
    const int hint_w = pad_hint.empty() ? 0 : std::max(24, font.Measure(pad_hint, 16) + 14);
    const int w = PillWidth(font, pad_hint, label);
    c.FillRoundRect(x, y, w, h, h / 2, highlighted ? kColAccent : kColSurfaceHi);
    int pen = x + 14;
    if (hint_w) {
        c.FillRoundRect(pen - 6, y + 5, hint_w, h - 10, (h - 10) / 2,
                        highlighted ? kColOnAccent : kColBadge);
        font.Draw(c, pen - 6 + (hint_w - font.Measure(pad_hint, 16)) / 2,
                  CenterBaseline(y + 5, h - 10, 16), pad_hint, 16,
                  highlighted ? kColAccent : kColText);
        pen += hint_w + 2;
    }
    font.Draw(c, pen, CenterBaseline(y, h, 17), label, 17, highlighted ? kColOnAccent : kColText);
    return w;
}

// ---------------------------------------------------------------------------------------------
// Options: the navigation drawer and its companions

struct DrawerItem {
    std::string_view label;
    bool header{};   // A section title, not selectable.
    bool advanced{}; // Listed under the Advanced section.
};

constexpr int kDrawerW = 268;
constexpr int kDrawerItemH = 46;
constexpr int kDrawerHeaderH = 38;

inline int DrawerItemHeight(const DrawerItem& item) {
    return item.header ? kDrawerHeaderH : kDrawerItemH;
}

/// Y of item `index` relative to the top of the list.
inline int DrawerItemOffset(std::span<const DrawerItem> items, int index) {
    int y = 0;
    for (int i = 0; i < index && i < static_cast<int>(items.size()); ++i) {
        y += DrawerItemHeight(items[i]);
    }
    return y;
}

/// Draws the drawer. `active` is the page on screen, `cursor` the highlighted item while the
/// drawer has the focus. `scroll` is in pixels, `time` in seconds (the focus ring breathes).
inline void DrawDrawer(Canvas& c, Font& font, int x, int y, int h, std::string_view title,
                       std::string_view subtitle, std::span<const DrawerItem> items, int active,
                       int cursor, bool focused, int scroll, float time) {
    c.FillRect(x, y, kDrawerW, h, kColRail);
    const int list_top = y + 84;
    const int list_bottom = y + h - 8;
    int iy = list_top - scroll;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const DrawerItem& item = items[i];
        const int ih = DrawerItemHeight(item);
        if (iy + ih > list_top && iy < list_bottom) {
            if (item.header) {
                if (iy + 8 > list_top) {
                    c.FillRect(x + 20, iy + 8, kDrawerW - 40, 1, kColLine);
                }
                const u32 color = item.advanced ? kColWarnText : kColTextDim;
                int tx = x + 24;
                if (item.advanced) {
                    DrawIcon(c, Icon::Warning, tx + 7.0f, iy + 24.0f, 15.0f, color);
                    tx += 22;
                }
                font.Draw(c, tx, iy + 30, item.label, 14, color);
            } else {
                const bool on_cursor = focused && i == cursor;
                const bool is_active = i == active;
                if (on_cursor) {
                    DrawFocusRing(c, x + 14, iy + 4, kDrawerW - 28, ih - 8, 6, time, kColSurfaceHi);
                }
                if (is_active) {
                    // The page on screen: accent text and a bar at its left, as a console's
                    // settings list marks it.
                    c.FillRoundRect(x + 22, iy + 11, 4, ih - 22, 2, kColAccent);
                }
                font.Draw(c, x + 38, CenterBaseline(iy, ih, 19), item.label, 19,
                          is_active ? kColAccent : kColText);
            }
        }
        iy += ih;
    }
    // The title stays put; the list scrolls under it.
    c.FillRect(x, y, kDrawerW, 84, kColRail);
    font.Draw(c, x + 24, y + 44, title, 26, kColText);
    if (!subtitle.empty()) {
        font.Draw(c, x + 24, y + 66, subtitle, 14, kColTextDim);
    }
    c.FillRect(x + 20, y + 83, kDrawerW - 40, 1, kColLine);
    c.FillRect(x + kDrawerW - 1, y, 1, h, kColLine);
}

/// A notice across the top of a page. Returns its height.
inline int DrawBanner(Canvas& c, Font& font, int x, int y, int w, std::string_view title,
                      std::string_view text) {
    const std::vector<std::string> lines = WrapLines(font, text, 16, w - 76);
    const int h = 40 + static_cast<int>(lines.size()) * 21 + 10;
    c.FillRoundRect(x, y, w, h, 8, kColWarnBg);
    c.FillRoundRect(x, y, 6, h, 3, kColWarnText);
    DrawIcon(c, Icon::Warning, x + 34.0f, y + 26.0f, 24.0f, kColWarnText);
    font.Draw(c, x + 58, y + 30, title, 18, kColWarnText);
    int ty = y + 54;
    for (const std::string& line : lines) {
        font.Draw(c, x + 58, ty, line, 16, kColWarnText);
        ty += 21;
    }
    return h;
}

/// Geometry of the info sheet, for hit-testing a tap outside it.
struct SheetRect {
    int x{}, y{}, w{}, h{};
};

/// A sheet rising from the bottom that explains the focused option.
inline SheetRect DrawInfoSheet(Canvas& c, Font& font, int screen_w, int screen_h, int left,
                               std::string_view title, std::string_view body,
                               std::span<const std::string> facts,
                               std::string_view dismiss_hint) {
    const int w = std::min(screen_w - left - 48, 860);
    const int x = left + (screen_w - left - w) / 2;
    const std::vector<std::string> lines = WrapLines(font, body, 19, w - 64);
    const int h = 96 + static_cast<int>(lines.size()) * 27 + static_cast<int>(facts.size()) * 24 +
                  (facts.empty() ? 0 : 14) + 44;
    const int y = screen_h - h - 28;
    // Scrim.
    c.FillRect(left, 0, screen_w - left, screen_h, WithAlpha(MakeColor(0, 0, 0), 110));
    c.FillRoundRect(x - 1, y - 1, w + 2, h + 2, 13, kColLine);
    c.FillRoundRect(x, y, w, h, 12, kColSurfaceHi);
    c.FillRoundRect(x + w / 2 - 24, y + 10, 48, 4, 2, kColBadge);
    DrawIcon(c, Icon::Info, x + 44.0f, y + 54.0f, 26.0f, kColAccent);
    font.Draw(c, x + 70, y + 62, font.Truncate(title, 24, w - 110), 24, kColText);
    int ty = y + 104;
    for (const std::string& line : lines) {
        font.Draw(c, x + 32, ty, line, 19, kColText);
        ty += 27;
    }
    if (!facts.empty()) {
        ty += 6;
        for (const std::string& fact : facts) {
            font.Draw(c, x + 32, ty, font.Truncate(fact, 16, w - 64), 16, kColTextDim);
            ty += 24;
        }
    }
    const int hw = font.Measure(dismiss_hint, 15);
    font.Draw(c, x + w - 32 - hw, y + h - 18, dismiss_hint, 15, kColTextDim);
    return {x, y, w, h};
}

// ---------------------------------------------------------------------------------------------
// The home screen: the shelf with its title and the bar of buttons under it

struct HomeView {
    int screen_w{};
    int screen_h{};
    ShelfStyle style{ShelfStyle::Coverflow};
    float position{};
    int count{};
    float flip{};
    float time{};
    std::string_view app_name;
    std::string_view app_tag;   // "for Switch 2"
    std::string_view title;     // Selected game.
    std::string_view publisher;
    std::string_view counter;   // "3 of 12"
    std::string_view search;    // Active filter, if any.
    bool compact{};             // Narrow (rotated) screen: fewer pills.
};

/// One pill of the bar, with where it ended up (for touch).
struct HomePill {
    int x{}, y{}, w{}, h{};
    char action{}; // 'A' play, 'X' flip, 'Y' search, 'S' style, 'D' details
};

/// Where the touchable parts of the home screen ended up.
struct HomeLayout {
    int options_cx{}, options_cy{};
    int exit_cx{}, exit_cy{};
    int button_radius{};
    int prev_x{}, next_x{}, arrows_y{}, arrow_radius{}; // The two arrows beside the title.
    int shelf_x{}, shelf_y{}, shelf_w{}, shelf_h{};
    HomePill pills[5]{};
    int pill_count{};
};

inline HomeLayout LayoutHome(int screen_w, int screen_h) {
    HomeLayout l;
    l.button_radius = 30;
    l.options_cx = 26 + l.button_radius;
    l.exit_cx = screen_w - 26 - l.button_radius;
    l.options_cy = l.exit_cy = screen_h - 22 - l.button_radius;
    l.shelf_x = 0;
    l.shelf_y = 58;
    l.shelf_w = screen_w;
    l.shelf_h = screen_h - 58 - 164;
    l.arrow_radius = 22;
    l.arrows_y = screen_h - 132;
    l.prev_x = screen_w / 2 - std::min(430, screen_w / 2 - 60);
    l.next_x = screen_w / 2 + std::min(430, screen_w / 2 - 60);
    return l;
}

inline HomeLayout DrawHome(Canvas& c, Font& font, const HomeView& v,
                           const std::function<ShelfItem(int)>& get) {
    HomeLayout l = LayoutHome(v.screen_w, v.screen_h);

    ShelfView shelf;
    shelf.x = l.shelf_x;
    shelf.y = l.shelf_y;
    shelf.w = l.shelf_w;
    shelf.h = l.shelf_h;
    shelf.style = v.style;
    shelf.position = v.position;
    shelf.count = v.count;
    shelf.flip = v.flip;
    shelf.time = v.time;

    // Backdrop: a wall that brightens towards the horizon, then a floor that takes the
    // reflections and falls away into the bottom of the screen.
    const int floor = static_cast<int>(ShelfFloorY(shelf));
    FillGradientV(c, 0, 0, v.screen_w, floor, kColShelfTop, kColShelfHorizon);
    FillGradientV(c, 0, floor, v.screen_w, v.screen_h - floor, kColFloorNear, kColFloorFar);

    // Top line: the app on the left, where we are in the library on the right.
    const int name_w = font.Draw(c, 28, 40, v.app_name, 24, kColText);
    if (!v.app_tag.empty()) {
        font.Draw(c, 28 + name_w + 10, 40, v.app_tag, 15, kColTextDim);
    }
    std::string right{v.counter};
    if (!v.search.empty()) {
        right = "\"" + std::string{v.search} + "\"   " + right;
    }
    font.Draw(c, v.screen_w - 28 - font.Measure(right, 17), 38, right, 17, kColTextDim);

    DrawShelf(c, font, shelf, get);

    // Title block, centred under the shelf, with an arrow either side of it.
    const int text_w = l.next_x - l.prev_x - 2 * l.arrow_radius - 40;
    const std::string title = font.Truncate(v.title, 28, text_w);
    font.Draw(c, (v.screen_w - font.Measure(title, 28)) / 2, v.screen_h - 126, title, 28, kColText);
    if (!v.publisher.empty()) {
        const std::string pub = font.Truncate(v.publisher, 17, text_w);
        font.Draw(c, (v.screen_w - font.Measure(pub, 17)) / 2, v.screen_h - 100, pub, 17,
                  kColTextDim);
    }
    if (v.count > 1) {
        for (const int dir : {-1, 1}) {
            const int ax = dir < 0 ? l.prev_x : l.next_x;
            FillDisc(c, static_cast<float>(ax), static_cast<float>(l.arrows_y), l.arrow_radius + 1.5f,
                     kColLine);
            FillDisc(c, static_cast<float>(ax), static_cast<float>(l.arrows_y),
                     static_cast<float>(l.arrow_radius), kColSurfaceHi);
            const float tip = ax + dir * 7.0f, tail = ax - dir * 5.0f;
            StrokeLine(c, tail, l.arrows_y - 9.0f, tip, static_cast<float>(l.arrows_y), 3.0f,
                       kColText);
            StrokeLine(c, tail, l.arrows_y + 9.0f, tip, static_cast<float>(l.arrows_y), 3.0f,
                       kColText);
        }
    }

    // The bar: round buttons in the corners, actions in the middle.
    DrawRoundButton(c, font, Icon::Gear, l.options_cx, l.options_cy, l.button_radius, "-", false);
    DrawRoundButton(c, font, Icon::Power, l.exit_cx, l.exit_cy, l.button_radius, "", false);
    struct Pill {
        const char* hint;
        const char* label;
        char action;
    };
    const Pill full[] = {{"A", "Play", 'A'},     {"X", "Flip", 'X'},  {"Y", "Search", 'Y'},
                         {"L R", "Style", 'S'}, {"+", "Details", 'D'}};
    const Pill narrow[] = {{"A", "Play", 'A'}, {"X", "Flip", 'X'}, {"Y", "Search", 'Y'}};
    const std::span<const Pill> pills = v.compact ? std::span<const Pill>{narrow}
                                                  : std::span<const Pill>{full};
    if (v.count > 0) {
        int total = 0;
        for (const Pill& p : pills) {
            total += PillWidth(font, p.hint, p.label) + 10;
        }
        int px = (v.screen_w - (total - 10)) / 2;
        for (const Pill& p : pills) {
            const int w = DrawPill(c, font, px, l.options_cy - 17, p.hint, p.label, p.action == 'A');
            l.pills[l.pill_count++] = {px, l.options_cy - 17, w, 34, p.action};
            px += w + 10;
        }
    }
    return l;
}

} // namespace SwitchFrontend::Ui
