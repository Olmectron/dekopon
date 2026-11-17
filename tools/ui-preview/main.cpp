// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Renders the launcher's views on a PC, to pictures. The views in src/citra_switch/ui_views.h
// draw on a plain canvas, so what comes out here is what the console shows - handy when
// nobody is in front of one.
//
//   g++ -std=c++20 -O2 -I src $(pkg-config --cflags freetype2) tools/ui-preview/main.cpp \
//       $(pkg-config --libs freetype2) -o ui-preview
//   ./ui-preview <font.ttf> <output directory>
//
// Output: one binary PPM per scene.

#include <cstdio>
#include <string>

#include "citra_switch/ui_views.h"

using namespace SwitchFrontend::Ui;

namespace {

struct FakeGame {
    std::string title;
    std::string publisher;
    std::string badge;
    Image cover;      // Empty: no artwork.
    bool full_wrap{};
    std::vector<u32> icon;
    std::vector<std::string> back;
};

/// A made-up cover: a diagonal gradient with a band, standing in for artwork.
Image MakeCover(int w, int h, u32 a, u32 b, bool wrap) {
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            u32 color = MixColor(a, b, (x + y) / float(w + h));
            const float fx = x / float(w);
            if (wrap) {
                // back | spine | front: darker back, bright spine.
                if (fx < kWrapFace) {
                    color = MixColor(color, MakeColor(0, 0, 0), 0.45f);
                    if ((y / 14) % 2 == 0 && x > w / 20 && fx < kWrapFace - 0.03f && y > h / 2) {
                        color = MixColor(color, MakeColor(255, 255, 255), 0.25f);
                    }
                } else if (fx < 1.0f - kWrapFace) {
                    color = MakeColor(0xE8, 0xE8, 0xE8);
                }
            }
            if (y > h * 70 / 100 && y < h * 84 / 100 && (!wrap || fx > 1.0f - kWrapFace)) {
                color = MixColor(color, MakeColor(255, 255, 255), 0.70f);
            }
            img.px[static_cast<std::size_t>(y) * w + x] = color;
        }
    }
    return img;
}

std::vector<u32> MakeIcon(u32 a, u32 b) {
    std::vector<u32> icon(48 * 48);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const bool ring = (x - 24) * (x - 24) + (y - 24) * (y - 24) < 17 * 17;
            icon[y * 48 + x] = ring ? b : a;
        }
    }
    return icon;
}

void Save(Canvas& c, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::printf("cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", c.Width(), c.Height());
    const u32* px = c.Data();
    for (int i = 0; i < c.Width() * c.Height(); ++i) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(px[i] & 0xFF),
                                      static_cast<unsigned char>((px[i] >> 8) & 0xFF),
                                      static_cast<unsigned char>((px[i] >> 16) & 0xFF)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: ui-preview <font.ttf> <output directory>\n");
        return 2;
    }
    Font font;
    if (!font.InitLibrary() || !font.AddFileFace(argv[1])) {
        std::printf("cannot load font %s\n", argv[1]);
        return 1;
    }
    const std::string out = argv[2];

    std::vector<FakeGame> games;
    const struct {
        const char* title;
        const char* publisher;
        u32 a, b;
        int art; // 0 none, 1 front, 2 full wrap
    } seed[] = {
        {"The Legend of Zelda: Majora's Mask 3D", "Nintendo", MakeColor(0x5B, 0x2A, 0x86), MakeColor(0xF0, 0xB4, 0x2C), 2},
        {"Kirby: Planet Robobot", "Nintendo", MakeColor(0xF2, 0x6D, 0x9A), MakeColor(0x4C, 0xC3, 0xF0), 1},
        {"The Legend of Zelda: Ocarina of Time 3D", "Nintendo", MakeColor(0x1F, 0x6B, 0x3A), MakeColor(0xD9, 0xC0, 0x62), 2},
        {"Homebrew Sample", "", MakeColor(0x33, 0x40, 0x55), MakeColor(0xE0, 0x7A, 0x3A), 0},
        {"Mario Kart 7", "Nintendo", MakeColor(0xD6, 0x2B, 0x2B), MakeColor(0x2B, 0x6C, 0xD6), 1},
        {"Animal Crossing: New Leaf", "Nintendo", MakeColor(0x6F, 0xBF, 0x4A), MakeColor(0xF2, 0xE3, 0x8C), 1},
        {"A Game With A Very Long Title That Needs To Wrap Somewhere", "Some Publisher", MakeColor(0x40, 0x48, 0x60), MakeColor(0x9A, 0xD0, 0xC0), 0},
        {"Fire Emblem Awakening", "Nintendo", MakeColor(0x23, 0x3E, 0x8B), MakeColor(0xC9, 0xD6, 0xF2), 2},
        {"Pokemon X", "Nintendo", MakeColor(0x1D, 0x4F, 0xA3), MakeColor(0x7E, 0xD3, 0xF7), 1},
        {"Luigi's Mansion: Dark Moon", "Nintendo", MakeColor(0x1E, 0x5A, 0x4A), MakeColor(0xA6, 0xE8, 0x7A), 0},
        {"Super Smash Bros.", "Nintendo", MakeColor(0x8B, 0x1E, 0x1E), MakeColor(0xF2, 0xD0, 0x4C), 1},
    };
    for (const auto& s : seed) {
        FakeGame g;
        g.title = s.title;
        g.publisher = s.publisher;
        g.badge = s.art == 0 ? "3DSX" : "";
        if (s.art == 1) {
            g.cover = MakeCover(400, 360, s.a, s.b, false);
        } else if (s.art == 2) {
            g.cover = MakeCover(860, 360, s.a, s.b, true);
            g.full_wrap = true;
        }
        g.icon = MakeIcon(s.a, s.b);
        g.back = {"Title ID  0004000000125500", "Format  3DS cartridge image", "Region  USA",
                  "Size  1.2 GB", "Save data  present"};
        games.push_back(std::move(g));
    }
    const auto get = [&](int i) {
        const FakeGame& g = games[i];
        ShelfItem item;
        item.cover = g.cover.Empty() ? nullptr : &g.cover;
        item.full_wrap = g.full_wrap;
        item.icon = &g.icon;
        item.icon_size = 48;
        item.title = g.title;
        item.publisher = g.publisher;
        item.badge = g.badge;
        item.back_text = g.back;
        return item;
    };

    Canvas c;
    float pv_zoom = 0.0f, pv_yaw = 0.0f, pv_pitch = 0.0f;
    const auto home = [&](const std::string& name, ShelfStyle style, float position, float flip,
                          int w, int h) {
        c.Resize(w, h);
        const int sel = static_cast<int>(std::lround(position));
        HomeView v;
        v.screen_w = w;
        v.screen_h = h;
        v.style = style;
        v.position = position;
        v.count = static_cast<int>(games.size());
        v.flip = flip;
        v.zoom = pv_zoom;
        v.spin_yaw = pv_yaw;
        v.spin_pitch = pv_pitch;
        v.forward = pv_zoom > 0.5f;
        v.time = 1.3f;
        v.app_name = "Toronja";
        v.app_tag = "for Switch 2";
        v.title = games[sel].title;
        v.publisher = games[sel].publisher;
        const std::string counter =
            std::to_string(sel + 1) + " of " + std::to_string(games.size());
        v.counter = counter;
        v.compact = w < 1000;
        DrawHome(c, font, v, get);
        Save(c, out + "/" + name + ".ppm");
    };

    const auto options = [&](const std::string& prefix) {
        c.Resize(1280, 720);
        c.Clear(kColBg);
        const DrawerItem items[] = {
            {"Games"},          {"SETTINGS", true}, {"General"},     {"Display"},
            {"Audio"},          {"Controls"},       {"Amiibo"},      {"GameShare"},
            {"System"},         {"TOOLS", true},    {"Install Titles"}, {"Folders"},
            {"Artic Base"},     {"ADVANCED", true, true}, {"Graphics", false, true},
            {"Enhancements", false, true},
        };
        DrawDrawer(c, font, 0, 0, 720, "Toronja", "Version 0.9.5", items, 3, 3, false, 0, 0.4f);
        const int px = kDrawerW + 44, pw = 1280 - kDrawerW - 88;
        font.Draw(c, px, 56, "Display", 28, kColText);
        const char* rows[][2] = {{"Internal Resolution", "2x (800x480)"}, {"Integer Scaling", "Off"},
                                 {"Screen Layout", "Large Screen"}, {"Swap Screens", "Off"},
                                 {"Async Shader Compilation (Experimental)", "On"},
                                 {"Theme", "Dark"}};
        int y = 96;
        for (int i = 0; i < 6; ++i) {
            if (i == 2) {
                DrawFocusRing(c, px - 8, y + 2, pw + 16, 48, 6, 0.4f, kColSurfaceHi);
            }
            font.Draw(c, px + 12, CenterBaseline(y, 52, 21), rows[i][0], 21, kColText);
            font.Draw(c, px + pw - 12 - font.Measure(rows[i][1], 21), CenterBaseline(y, 52, 21),
                      rows[i][1], 21, kColAccent);
            c.FillRect(px, y + 52, pw, 1, kColLine);
            y += 53;
        }
        y += 20;
        DrawBanner(c, font, px, y, pw, "Advanced settings",
                   "Only change these if you know what they do. A wrong value here can make games "
                   "slow, glitchy or unable to start.");
        Save(c, out + "/" + prefix + "-options.ppm");

        const std::string facts[] = {"Now: Large Screen", "Takes effect at once"};
        DrawInfoSheet(c, font, 1280, 720, kDrawerW, "Screen Layout",
                      "How the two 3DS screens are arranged on this console's screen. The right "
                      "stick click cycles through the layouts while a game runs.",
                      facts, "A / B / X  Close");
        Save(c, out + "/" + prefix + "-options-info.ppm");

        c.Clear(kColBg);
        DrawDrawer(c, font, 0, 0, 720, "Toronja", "Version 0.9.5", items, 3, 6, true, 0, 0.9f);
        Save(c, out + "/" + prefix + "-options-drawer-focused.ppm");
    };

    for (const bool light : {false, true}) {
        ApplyTheme(light);
        const std::string tag = light ? "light" : "dark";
        home(tag + "-01-coverflow", ShelfStyle::Coverflow, 4.0f, 0.0f, 1280, 720);
        home(tag + "-02-shelf", ShelfStyle::Shelf, 4.0f, 0.0f, 1280, 720);
        home(tag + "-03-shelf-icons", ShelfStyle::Shelf, 3.0f, 0.0f, 1280, 720);
        home(tag + "-04-carousel", ShelfStyle::Carousel, 5.0f, 0.0f, 1280, 720);
        home(tag + "-05-arc", ShelfStyle::Arc, 5.0f, 0.0f, 1280, 720);
        home(tag + "-06-stack", ShelfStyle::Stack, 5.0f, 0.0f, 1280, 720);
        home(tag + "-07-spotlight", ShelfStyle::Spotlight, 4.0f, 0.0f, 1280, 720);
        home(tag + "-08-sliding", ShelfStyle::Shelf, 2.45f, 0.0f, 1280, 720);
        home(tag + "-09-flip-wrap", ShelfStyle::Shelf, 0.0f, 1.0f, 1280, 720);
        home(tag + "-10-flip-text", ShelfStyle::Shelf, 3.0f, 1.0f, 1280, 720);
        home(tag + "-12-bookshelf", ShelfStyle::Bookshelf, 4.0f, 0.0f, 1280, 720);
        home(tag + "-13-bookshelf-sliding", ShelfStyle::Bookshelf, 4.5f, 0.0f, 1280, 720);
        home(tag + "-14-pile", ShelfStyle::Pile, 4.0f, 0.0f, 1280, 720);
        home(tag + "-15-pile-sliding", ShelfStyle::Pile, 4.4f, 0.0f, 1280, 720);
        home(tag + "-16-showroom", ShelfStyle::Showroom, 4.0f, 0.0f, 1280, 720);
        home(tag + "-17-flip-halfway", ShelfStyle::Coverflow, 0.0f, 0.35f, 1280, 720);
        pv_yaw = -0.9f;
        pv_pitch = 0.35f;
        home(tag + "-18-turned-by-hand", ShelfStyle::Shelf, 2.0f, 0.0f, 1280, 720);
        pv_zoom = 1.0f;
        home(tag + "-19-forward-turned", ShelfStyle::Coverflow, 2.0f, 0.0f, 1280, 720);
        pv_yaw = pv_pitch = 0.0f;
        home(tag + "-20-forward", ShelfStyle::Coverflow, 4.0f, 0.0f, 1280, 720);
        home(tag + "-21-forward-back", ShelfStyle::Coverflow, 4.0f, 1.0f, 1280, 720);
        pv_zoom = 0.0f;
        options(tag + "-11");
    }
    return 0;
}
