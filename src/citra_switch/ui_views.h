// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The launcher's larger views: the cover library (a WiiFlow-style shelf), the bar under it,
// the navigation drawer of the options screen, and the small sheets shared with it.
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
    float x0{}, x1{};      // Left and right edge.
    float top0{}, h0{};    // Top and height at the left edge.
    float top1{}, h1{};    // Top and height at the right edge.
};

/// Draws the part of `img` between `u0` and `u1` (0..1 across the picture) into `q`.
/// `shade` darkens (0 = as is, 1 = black). With `mirror` the picture is drawn upside down
/// under the quad and fades out downwards, as a reflection. `smooth` filters the picture;
/// without it the nearest pixel is taken, which is several times cheaper and is what the
/// covers at the sides and the reflections get - a shelf redraws every frame, in software.
inline void BlitQuad(Canvas& c, const Image& img, const Quad& q, float u0 = 0.0f, float u1 = 1.0f,
                     float shade = 0.0f, bool mirror = false, bool smooth = true) {
    if (img.Empty() || q.x1 <= q.x0) {
        return;
    }
    const int xa = std::max(0, static_cast<int>(std::floor(q.x0)));
    const int xb = std::min(c.Width(), static_cast<int>(std::ceil(q.x1)));
    const u32 keep = static_cast<u32>((1.0f - std::clamp(shade, 0.0f, 1.0f)) * 256.0f);
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
        const float y_begin = mirror ? top + h : top;
        const float y_end = mirror ? top + h + h * 0.34f : top + h;
        const int ya = std::max(0, static_cast<int>(std::floor(y_begin)));
        const int yb = std::min(c.Height(), static_cast<int>(std::ceil(y_end)));
        const float v_step = (img.h - 1) / h;
        for (int y = ya; y < yb; ++y) {
            float v = (y + 0.5f - top) * v_step; // Row of the picture, 0 at its top.
            u32 fade = 256;
            if (mirror) {
                v = 2.0f * (img.h - 1) - v; // Fold back up from the bottom edge.
                fade = static_cast<u32>(
                    77.0f * std::clamp(1.0f - (y + 0.5f - y_begin) / (y_end - y_begin), 0.0f, 1.0f));
            }
            if (v < 0.0f || v > static_cast<float>(img.h - 1)) {
                continue;
            }
            u32 color = smooth ? SampleImage(img, u, v)
                               : img.px[static_cast<std::size_t>(static_cast<int>(v + 0.5f)) * img.w + ui];
            u32 r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
            if (keep < 256) {
                r = r * keep >> 8;
                g = g * keep >> 8;
                b = b * keep >> 8;
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
// The cover shelf

enum class ShelfStyle { Coverflow, Carousel, Stack, Grid, Count };

inline const char* ShelfStyleName(ShelfStyle style) {
    switch (style) {
    case ShelfStyle::Coverflow:
        return "Coverflow";
    case ShelfStyle::Carousel:
        return "Carousel";
    case ShelfStyle::Stack:
        return "Stack";
    case ShelfStyle::Grid:
        return "Grid";
    default:
        return "";
    }
}

/// One game on the shelf. A game without artwork still gets a case, built from its icon.
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
    float shade{};
};

/// Where the cover `d` places from the selected one sits. `d` is fractional while the shelf
/// slides, which is the whole animation: the caller eases its position and redraws.
inline ShelfPose PoseFor(ShelfStyle style, float d, float cx, float case_h) {
    const float case_w = case_h * kCaseAspect;
    const float ad = std::abs(d);
    const float sign = d < 0 ? -1.0f : 1.0f;
    const float t = std::min(ad, 1.0f); // 0 in the middle, 1 once fully to the side.
    ShelfPose p;
    switch (style) {
    case ShelfStyle::Carousel: {
        const float scale = std::max(0.42f, 1.0f - 0.20f * ad);
        p.cx = cx + sign * case_w * (0.94f * t + 0.74f * std::max(0.0f, ad - 1.0f));
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.55f, 0.16f * ad);
        break;
    }
    case ShelfStyle::Stack: {
        const float scale = std::max(0.50f, 1.0f - 0.10f * ad);
        p.cx = cx + sign * case_w * (0.46f * t + 0.20f * std::max(0.0f, ad - 1.0f));
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.70f, 0.22f * ad);
        break;
    }
    default: { // Coverflow: side covers turn away and close ranks.
        p.cx = cx + sign * case_w * (0.70f * t + 0.30f * std::max(0.0f, ad - 1.0f));
        p.width = case_w * (1.0f - 0.60f * t);
        p.h_near = case_h * (1.0f - 0.14f * t);
        p.h_far = case_h * (1.0f - 0.30f * t);
        p.shade = std::min(0.60f, 0.12f * t + 0.10f * std::max(0.0f, ad - 1.0f));
        break;
    }
    }
    return p;
}

/// A case for a game with no artwork: its icon on a plate, the title underneath.
inline void DrawPlaceholderCase(Canvas& c, Font& font, const ShelfItem& item, const Quad& q,
                                float shade) {
    const int x = static_cast<int>(q.x0), w = static_cast<int>(q.x1 - q.x0);
    const float top_f = std::min(q.top0, q.top1);
    const float h_f = std::max(q.h0, q.h1);
    const int y = static_cast<int>(top_f), h = static_cast<int>(h_f);
    if (w < 6 || h < 6) {
        return;
    }
    const u32 plate_top = MixColor(MakeColor(0x3A, 0x3E, 0x47), MakeColor(0, 0, 0), shade);
    const u32 plate_bottom = MixColor(MakeColor(0x22, 0x24, 0x2A), MakeColor(0, 0, 0), shade);
    // The skewed outline: one column at a time, like a blit.
    for (int col = 0; col < w; ++col) {
        const float t = (col + 0.5f) / w;
        const float top = q.top0 + (q.top1 - q.top0) * t;
        const float hh = q.h0 + (q.h1 - q.h0) * t;
        FillGradientV(c, x + col, static_cast<int>(top), 1, static_cast<int>(hh), plate_top,
                      plate_bottom);
    }
    // A coloured band along the spine edge, as the cases have.
    c.FillRect(x, y + h / 14, std::max(2, w / 28), h - h / 7,
               MixColor(MakeColor(0xFA, 0xAA, 0x49), MakeColor(0, 0, 0), shade));
    if (w < 70) {
        return; // Too narrow (turned away) for the icon and the text to read.
    }
    const int icon = std::min(w * 46 / 100, h * 46 / 100);
    const int ix = x + (w - icon) / 2, iy = y + h * 16 / 100;
    if (item.icon != nullptr && !item.icon->empty() && item.icon_size > 0) {
        c.BlitIcon(*item.icon, item.icon_size, ix, iy, icon);
    } else {
        c.FillRoundRect(ix, iy, icon, icon, icon / 8, MakeColor(0x4A, 0x4E, 0x58));
        DrawIcon(c, Icon::Shelf, ix + icon / 2.0f, iy + icon / 2.0f, icon * 0.5f,
                 MakeColor(0x9B, 0xA0, 0xA6));
    }
    const int size = std::clamp(w / 13, 13, 24);
    const std::vector<std::string> lines = WrapLines(font, item.title, size, w - w / 6);
    int ty = iy + icon + size + h / 20;
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
        const std::string text = font.Truncate(lines[i], size, w - w / 6);
        font.Draw(c, x + (w - font.Measure(text, size)) / 2, ty, text, size,
                  MixColor(MakeColor(0xF1, 0xF2, 0xF4), MakeColor(0, 0, 0), shade));
        ty += size + 6;
    }
    if (!item.badge.empty()) {
        const int bs = std::max(12, size - 4);
        const int bw = font.Measure(item.badge, bs) + 14;
        c.FillRoundRect(x + (w - bw) / 2, y + h - bs - h / 12, bw, bs + 8, 7,
                        MakeColor(0x4A, 0x4E, 0x58));
        font.Draw(c, x + (w - bw) / 2 + 7, y + h - h / 12 - 2, item.badge, bs,
                  MakeColor(0xC8, 0xCC, 0xD2));
    }
}

/// The back of a case that has no back artwork: what is known about the game, as text.
inline void DrawTextBack(Canvas& c, Font& font, const ShelfItem& item, int x, int y, int w, int h) {
    FillGradientV(c, x, y, w, h, MakeColor(0x30, 0x33, 0x3B), MakeColor(0x1E, 0x20, 0x26));
    c.FillRect(x + w - std::max(2, w / 28), y + h / 14, std::max(2, w / 28), h - h / 7,
               MakeColor(0xFA, 0xAA, 0x49));
    if (w < 120) {
        return;
    }
    const int pad = w / 12;
    int ty = y + pad + 22;
    for (const std::string& line : WrapLines(font, item.title, 22, w - 2 * pad)) {
        font.Draw(c, x + pad, ty, line, 22, MakeColor(0xF1, 0xF2, 0xF4));
        ty += 28;
    }
    if (!item.publisher.empty()) {
        font.Draw(c, x + pad, ty, font.Truncate(item.publisher, 16, w - 2 * pad), 16,
                  MakeColor(0xFA, 0xAA, 0x49));
        ty += 26;
    }
    ty += 8;
    for (const std::string& line : item.back_text) {
        if (ty > y + h - pad) {
            break;
        }
        font.Draw(c, x + pad, ty, font.Truncate(line, 16, w - 2 * pad), 16,
                  MakeColor(0xB4, 0xB8, 0xBF));
        ty += 23;
    }
}

struct ShelfView {
    int x{}, y{}, w{}, h{};  // Area the shelf may use.
    ShelfStyle style{ShelfStyle::Coverflow};
    float position{};        // Eased index of the selected game.
    int count{};
    float flip{};            // 0 = front, 1 = back, in between while turning.
    bool reflections{true};
};

/// Draws the shelf. `get` supplies each game on demand, so nothing is built for games that are
/// off screen.
inline void DrawShelf(Canvas& c, Font& font, const ShelfView& v,
                      const std::function<ShelfItem(int)>& get) {
    if (v.count <= 0) {
        return;
    }
    const float case_h = std::min(v.h * 0.60f, v.w * 0.40f / kCaseAspect * 1.6f);
    const float cx = v.x + v.w / 2.0f;
    const float base = v.y + v.h * 0.46f + case_h / 2; // The shelf's floor.
    const int centre = static_cast<int>(std::lround(v.position));
    constexpr int reach = 5;

    const auto draw_one = [&](int index) {
        const float d = index - v.position;
        const ShelfPose p = PoseFor(v.style, d, cx, case_h);
        const bool left_side = d < 0;
        Quad q;
        q.x0 = p.cx - p.width / 2;
        q.x1 = p.cx + p.width / 2;
        // The edge towards the middle of the shelf is the tall one.
        const float h_left = left_side ? p.h_far : p.h_near;
        const float h_right = left_side ? p.h_near : p.h_far;
        const float lean = std::min(std::abs(d), 1.0f);
        q.h0 = p.h_near + (h_left - p.h_near) * lean;
        q.h1 = p.h_near + (h_right - p.h_near) * lean;
        q.top0 = base - q.h0;
        q.top1 = base - q.h1;

        const ShelfItem item = get(index);
        const bool selected = index == centre;
        float turn = selected ? v.flip : 0.0f;
        bool show_back = false;
        if (turn > 0.0f) {
            // Turning about the vertical axis: the case narrows to nothing, then widens again
            // showing its other side.
            const float squash = std::abs(std::cos(turn * 3.14159265f));
            show_back = turn > 0.5f;
            const float mid = (q.x0 + q.x1) / 2, half = (q.x1 - q.x0) / 2 * std::max(squash, 0.02f);
            q.x0 = mid - half;
            q.x1 = mid + half;
        }

        const bool has_art = item.cover != nullptr && !item.cover->Empty();
        if (has_art) {
            float u0 = 0.0f, u1 = 1.0f;
            if (item.full_wrap) {
                // back | spine | front
                u0 = show_back ? 0.0f : 1.0f - kWrapFace;
                u1 = show_back ? kWrapFace : 1.0f;
            }
            if (show_back && !item.full_wrap) {
                DrawTextBack(c, font, item, static_cast<int>(q.x0), static_cast<int>(q.top0),
                             static_cast<int>(q.x1 - q.x0), static_cast<int>(q.h0));
            } else {
                if (v.reflections) {
                    BlitQuad(c, *item.cover, q, u0, u1, p.shade, true, false);
                }
                BlitQuad(c, *item.cover, q, u0, u1, p.shade, false, selected);
            }
        } else if (show_back) {
            DrawTextBack(c, font, item, static_cast<int>(q.x0), static_cast<int>(q.top0),
                         static_cast<int>(q.x1 - q.x0), static_cast<int>(q.h0));
        } else {
            DrawPlaceholderCase(c, font, item, q, p.shade);
        }
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
/// chip. Returns nothing; hit-testing is the caller's (centre + radius).
inline void DrawRoundButton(Canvas& c, Font& font, Icon icon, int cx, int cy, int radius,
                            std::string_view pad_hint, bool highlighted) {
    const u32 fill = highlighted ? MakeColor(0xFA, 0xAA, 0x49) : MakeColor(0x30, 0x33, 0x39);
    const u32 fg = highlighted ? MakeColor(0x17, 0x18, 0x1B) : MakeColor(0xF1, 0xF2, 0xF4);
    FillDisc(c, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(radius), fill);
    DrawIcon(c, icon, static_cast<float>(cx), static_cast<float>(cy), radius * 1.05f, fg);
    if (!pad_hint.empty()) {
        const int tw = font.Measure(pad_hint, 15);
        const int chip_w = std::max(22, tw + 12);
        const int chip_x = cx + radius - chip_w / 2 - 2;
        const int chip_y = cy - radius - 6;
        c.FillRoundRect(chip_x, chip_y, chip_w, 22, 11, MakeColor(0x17, 0x18, 0x1B));
        c.FillRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, 20, 10, MakeColor(0x4A, 0x4E, 0x58));
        font.Draw(c, chip_x + (chip_w - tw) / 2, CenterBaseline(chip_y, 22, 15), pad_hint, 15,
                  MakeColor(0xF1, 0xF2, 0xF4));
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
    const int label_w = font.Measure(label, 17);
    const int w = 14 + hint_w + (hint_w ? 8 : 0) + label_w + 16;
    c.FillRoundRect(x, y, w, h, h / 2,
                    highlighted ? MakeColor(0xFA, 0xAA, 0x49) : MakeColor(0x2A, 0x2D, 0x33));
    int pen = x + 14;
    if (hint_w) {
        c.FillRoundRect(pen - 6, y + 5, hint_w, h - 10, (h - 10) / 2, MakeColor(0x4A, 0x4E, 0x58));
        font.Draw(c, pen - 6 + (hint_w - font.Measure(pad_hint, 16)) / 2,
                  CenterBaseline(y + 5, h - 10, 16), pad_hint, 16, MakeColor(0xF1, 0xF2, 0xF4));
        pen += hint_w + 2;
    }
    font.Draw(c, pen, CenterBaseline(y, h, 17), label, 17,
              highlighted ? MakeColor(0x17, 0x18, 0x1B) : MakeColor(0xD6, 0xD9, 0xDE));
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
constexpr int kDrawerItemH = 44;
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
/// drawer has the focus. `scroll` is in pixels.
inline void DrawDrawer(Canvas& c, Font& font, int x, int y, int h, std::string_view title,
                       std::string_view subtitle, std::span<const DrawerItem> items, int active,
                       int cursor, bool focused, int scroll) {
    c.FillRect(x, y, kDrawerW, h, MakeColor(0x1E, 0x20, 0x24));
    font.Draw(c, x + 24, y + 44, title, 26, MakeColor(0xF1, 0xF2, 0xF4));
    if (!subtitle.empty()) {
        font.Draw(c, x + 24, y + 66, subtitle, 14, MakeColor(0x9B, 0xA0, 0xA6));
    }
    const int list_top = y + 84;
    const int list_bottom = y + h - 8;
    int iy = list_top - scroll;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const DrawerItem& item = items[i];
        const int ih = DrawerItemHeight(item);
        if (iy + ih > list_top && iy < list_bottom) {
            if (item.header) {
                if (iy + 8 > list_top) {
                    c.FillRect(x + 20, iy + 8, kDrawerW - 40, 1, MakeColor(0x30, 0x33, 0x39));
                }
                const u32 color =
                    item.advanced ? MakeColor(0xE8, 0x9A, 0x4A) : MakeColor(0x9B, 0xA0, 0xA6);
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
                    c.FillRoundRect(x + 12, iy + 2, kDrawerW - 24, ih - 4, (ih - 4) / 2,
                                    MakeColor(0xFA, 0xAA, 0x49));
                } else if (is_active) {
                    c.FillRoundRect(x + 12, iy + 2, kDrawerW - 24, ih - 4, (ih - 4) / 2,
                                    MakeColor(0x3A, 0x32, 0x26));
                }
                const u32 fg = on_cursor   ? MakeColor(0x17, 0x18, 0x1B)
                               : is_active ? MakeColor(0xFA, 0xAA, 0x49)
                                           : MakeColor(0xD6, 0xD9, 0xDE);
                font.Draw(c, x + 32, CenterBaseline(iy, ih, 18), item.label, 18, fg);
            }
        }
        iy += ih;
    }
    // Fade the list out under the title so scrolled items do not run into it.
    c.FillRect(x, y + 72, kDrawerW, 12, MakeColor(0x1E, 0x20, 0x24));
    c.FillRect(x + kDrawerW - 1, y, 1, h, MakeColor(0x2A, 0x2D, 0x33));
}

/// A notice across the top of a page. Returns its height.
inline int DrawBanner(Canvas& c, Font& font, int x, int y, int w, std::string_view title,
                      std::string_view text) {
    const std::vector<std::string> lines = WrapLines(font, text, 16, w - 76);
    const int h = 40 + static_cast<int>(lines.size()) * 21 + 10;
    c.FillRoundRect(x, y, w, h, 14, MakeColor(0x4A, 0x30, 0x16));
    c.FillRoundRect(x, y, 6, h, 3, MakeColor(0xE8, 0x9A, 0x4A));
    DrawIcon(c, Icon::Warning, x + 34.0f, y + 26.0f, 24.0f, MakeColor(0xFF, 0xC4, 0x7A));
    font.Draw(c, x + 58, y + 30, title, 18, MakeColor(0xFF, 0xD8, 0xA8));
    int ty = y + 54;
    for (const std::string& line : lines) {
        font.Draw(c, x + 58, ty, line, 16, MakeColor(0xE6, 0xC9, 0xA6));
        ty += 21;
    }
    return h;
}

/// A sheet rising from the bottom that explains the focused option.
inline void DrawInfoSheet(Canvas& c, Font& font, int screen_w, int screen_h, int left,
                          std::string_view title, std::string_view body,
                          std::span<const std::string> facts, std::string_view dismiss_hint) {
    const int w = std::min(screen_w - left - 48, 860);
    const int x = left + (screen_w - left - w) / 2;
    const std::vector<std::string> lines = WrapLines(font, body, 19, w - 64);
    const int h = 96 + static_cast<int>(lines.size()) * 27 + static_cast<int>(facts.size()) * 24 +
                  (facts.empty() ? 0 : 14) + 44;
    const int y = screen_h - h - 28;
    // Scrim.
    c.FillRect(left, 0, screen_w - left, screen_h, WithAlpha(MakeColor(0, 0, 0), 120));
    c.FillRoundRect(x, y, w, h, 24, MakeColor(0x2A, 0x2D, 0x33));
    c.FillRoundRect(x + w / 2 - 24, y + 10, 48, 4, 2, MakeColor(0x5A, 0x5E, 0x68));
    DrawIcon(c, Icon::Info, x + 44.0f, y + 54.0f, 26.0f, MakeColor(0xFA, 0xAA, 0x49));
    font.Draw(c, x + 70, y + 62, font.Truncate(title, 24, w - 110), 24, MakeColor(0xF1, 0xF2, 0xF4));
    int ty = y + 104;
    for (const std::string& line : lines) {
        font.Draw(c, x + 32, ty, line, 19, MakeColor(0xD6, 0xD9, 0xDE));
        ty += 27;
    }
    if (!facts.empty()) {
        ty += 6;
        for (const std::string& fact : facts) {
            font.Draw(c, x + 32, ty, font.Truncate(fact, 16, w - 64), 16, MakeColor(0x9B, 0xA0, 0xA6));
            ty += 24;
        }
    }
    const int hw = font.Measure(dismiss_hint, 15);
    font.Draw(c, x + w - 32 - hw, y + h - 18, dismiss_hint, 15, MakeColor(0x9B, 0xA0, 0xA6));
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
    std::string_view app_name;
    std::string_view app_tag;   // "for Switch 2"
    std::string_view title;     // Selected game.
    std::string_view publisher;
    std::string_view counter;   // "3 of 12"
    std::string_view search;    // Active filter, if any.
    bool compact{};             // Narrow (rotated) screen: fewer pills.
};

/// Where the touchable parts of the home screen ended up.
struct HomeLayout {
    int options_cx{}, options_cy{};
    int exit_cx{}, exit_cy{};
    int button_radius{};
    int prev_x{}, next_x{}, arrows_y{}, arrow_radius{}; // The two arrows beside the title.
    int shelf_x{}, shelf_y{}, shelf_w{}, shelf_h{};
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
    const HomeLayout l = LayoutHome(v.screen_w, v.screen_h);
    // Backdrop: dark, a little lighter where the shelf stands.
    FillGradientV(c, 0, 0, v.screen_w, v.screen_h * 62 / 100, MakeColor(0x14, 0x15, 0x18),
                  MakeColor(0x24, 0x26, 0x2C));
    FillGradientV(c, 0, v.screen_h * 62 / 100, v.screen_w, v.screen_h - v.screen_h * 62 / 100,
                  MakeColor(0x1B, 0x1C, 0x21), MakeColor(0x10, 0x11, 0x13));

    // Top line: the app on the left, where we are in the library on the right.
    const int name_w = font.Draw(c, 28, 40, v.app_name, 24, MakeColor(0xF1, 0xF2, 0xF4));
    if (!v.app_tag.empty()) {
        font.Draw(c, 28 + name_w + 10, 40, v.app_tag, 15, MakeColor(0x9B, 0xA0, 0xA6));
    }
    std::string right{v.counter};
    if (!v.search.empty()) {
        right = "\"" + std::string{v.search} + "\"   " + right;
    }
    font.Draw(c, v.screen_w - 28 - font.Measure(right, 17), 38, right, 17,
              MakeColor(0x9B, 0xA0, 0xA6));

    ShelfView shelf;
    shelf.x = l.shelf_x;
    shelf.y = l.shelf_y;
    shelf.w = l.shelf_w;
    shelf.h = l.shelf_h;
    shelf.style = v.style;
    shelf.position = v.position;
    shelf.count = v.count;
    shelf.flip = v.flip;
    DrawShelf(c, font, shelf, get);

    // Title block, centred under the shelf, with the two arrows WiiFlow puts beside it.
    const int text_w = l.next_x - l.prev_x - 2 * l.arrow_radius - 40;
    const std::string title = font.Truncate(v.title, 28, text_w);
    font.Draw(c, (v.screen_w - font.Measure(title, 28)) / 2, v.screen_h - 126, title, 28,
              MakeColor(0xF1, 0xF2, 0xF4));
    if (!v.publisher.empty()) {
        const std::string pub = font.Truncate(v.publisher, 17, text_w);
        font.Draw(c, (v.screen_w - font.Measure(pub, 17)) / 2, v.screen_h - 100, pub, 17,
                  MakeColor(0x9B, 0xA0, 0xA6));
    }
    if (v.count > 1) {
        for (const int dir : {-1, 1}) {
            const int ax = dir < 0 ? l.prev_x : l.next_x;
            FillDisc(c, static_cast<float>(ax), static_cast<float>(l.arrows_y),
                     static_cast<float>(l.arrow_radius), MakeColor(0x2A, 0x2D, 0x33));
            const float tip = ax + dir * 7.0f, tail = ax - dir * 5.0f;
            StrokeLine(c, tail, l.arrows_y - 9.0f, tip, static_cast<float>(l.arrows_y), 3.0f,
                       MakeColor(0xD6, 0xD9, 0xDE));
            StrokeLine(c, tail, l.arrows_y + 9.0f, tip, static_cast<float>(l.arrows_y), 3.0f,
                       MakeColor(0xD6, 0xD9, 0xDE));
        }
    }

    // The bar: round buttons in the corners, actions in the middle.
    DrawRoundButton(c, font, Icon::Gear, l.options_cx, l.options_cy, l.button_radius, "-", false);
    DrawRoundButton(c, font, Icon::Power, l.exit_cx, l.exit_cy, l.button_radius, "", false);
    struct Pill {
        const char* hint;
        const char* label;
        bool primary;
    };
    const Pill full[] = {{"A", "Play", true},     {"X", "Flip", false},  {"Y", "Search", false},
                         {"L R", "Style", false}, {"+", "Details", false}};
    const Pill narrow[] = {{"A", "Play", true}, {"X", "Flip", false}, {"Y", "Search", false}};
    const std::span<const Pill> pills = v.compact ? std::span<const Pill>{narrow}
                                                  : std::span<const Pill>{full};
    if (v.count > 0) {
        // Measure first so the row can be centred.
        int total = 0;
        for (const Pill& p : pills) {
            total += PillWidth(font, p.hint, p.label) + 10;
        }
        int px = (v.screen_w - (total - 10)) / 2;
        for (const Pill& p : pills) {
            px += DrawPill(c, font, px, l.options_cy - 17, p.hint, p.label, p.primary) + 10;
        }
    }
    return l;
}

} // namespace SwitchFrontend::Ui
