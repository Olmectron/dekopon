// SPDX-License-Identifier: GPL-2.0-or-later

// Toronja Link's screen: where the shared picture goes, the tab in the corner, the menu it
// opens, the FPS readout and the short notices. Drawn with the launcher's canvas so both
// apps look alike.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#include "citra_switch/ui_views.h"

namespace Link {

using namespace SwitchFrontend::Ui;

constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 720;
constexpr const char* SETTINGS_FILE = "sdmc:/switch/toronja-link.ini";
constexpr const char* OLD_SETTINGS_FILE = "sdmc:/switch/dekopon-link.ini"; // Before the rename.

/// Which screens go where: here and on the console that runs the game.
enum class Screens {
    BottomHereTopThere, // The pairing: the other console switches to the top screen alone.
    TopHereBottomThere,
    BottomHere, // The other console keeps whatever layout it has.
    TopHere,
    BothHere,
    Count,
};

enum class BothLayout { SideBySide, Stacked, LargeTop, LargeBottom, Count };

struct Rect {
    int x{}, y{}, w{}, h{};
    bool Contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

struct Settings {
    int screens = static_cast<int>(Screens::BottomHere);
    int both_layout = static_cast<int>(BothLayout::SideBySide);
    bool audio = true;
    bool fps_here = false;
    bool fps_there = false;
    int tab_opacity = 2; // 0 hidden, 1 faint, 2 visible
    int tab_corner = 1;  // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    bool light = false;  // Light theme.

    void Load() {
        std::FILE* file = std::fopen(SETTINGS_FILE, "r");
        if (file == nullptr) {
            // Settings from before the files were renamed come along.
            std::rename(OLD_SETTINGS_FILE, SETTINGS_FILE);
            file = std::fopen(SETTINGS_FILE, "r");
        }
        if (file == nullptr) {
            return;
        }
        char key[48];
        int value = 0;
        while (std::fscanf(file, " %47[^=]=%d", key, &value) == 2) {
            const std::string name{key};
            if (name == "screens") {
                screens = std::clamp(value, 0, static_cast<int>(Screens::Count) - 1);
            } else if (name == "both_layout") {
                both_layout = std::clamp(value, 0, static_cast<int>(BothLayout::Count) - 1);
            } else if (name == "audio") {
                audio = value != 0;
            } else if (name == "fps_here") {
                fps_here = value != 0;
            } else if (name == "fps_there") {
                fps_there = value != 0;
            } else if (name == "tab_opacity") {
                tab_opacity = std::clamp(value, 0, 2);
            } else if (name == "tab_corner") {
                tab_corner = std::clamp(value, 0, 3);
            } else if (name == "light") {
                light = value != 0;
            }
        }
        std::fclose(file);
    }

    void Save() const {
        std::FILE* file = std::fopen(SETTINGS_FILE, "w");
        if (file == nullptr) {
            return;
        }
        std::fprintf(file,
                     "screens=%d\nboth_layout=%d\naudio=%d\nfps_here=%d\nfps_there=%d\n"
                     "tab_opacity=%d\ntab_corner=%d\nlight=%d\n",
                     screens, both_layout, audio ? 1 : 0, fps_here ? 1 : 0, fps_there ? 1 : 0,
                     tab_opacity, tab_corner, light ? 1 : 0);
        std::fclose(file);
    }
};

/// A request for the host, sent as a COMMAND packet.
struct Command {
    u8 what{};
    u8 value{};
};

class Screen {
public:
    Settings settings;
    Font font;
    bool font_ready = false;

    void Init() {
        settings.Load();
        ApplyTheme(settings.light);
        canvas.Resize(SCREEN_W, SCREEN_H);
        if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && font.InitLibrary()) {
            for (const PlSharedFontType type :
                 {PlSharedFontType_Standard, PlSharedFontType_NintendoExt}) {
                PlFontData data{};
                if (R_SUCCEEDED(plGetSharedFontByType(&data, type))) {
                    font.AddMemoryFace(data.address, data.size);
                }
            }
            font_ready = font.Valid();
        }
        QueueHostState();
    }

    // ------------------------------------------------------------------ what to ask the host

    /// The screen to ask for in HELLO: 0 top, 1 bottom, 2 both.
    u8 RequestedScreen() const {
        if (lobby) {
            return 3; // No game yet: the other console's companion screen.
        }
        switch (static_cast<Screens>(settings.screens)) {
        case Screens::TopHereBottomThere:
        case Screens::TopHere:
            return 0;
        case Screens::BothHere:
            return 2;
        default:
            return 1;
        }
    }

    /// What the other console should show: 0 its own layout, 1 top only, 2 bottom only.
    u8 HostScreen() const {
        switch (static_cast<Screens>(settings.screens)) {
        case Screens::BottomHereTopThere:
            return 1;
        case Screens::TopHereBottomThere:
            return 2;
        default:
            return 0;
        }
    }

    /// Asks the host for the state the settings describe. Called whenever one changes, and
    /// again when a host is (re)found.
    void QueueHostState() {
        commands.push_back({1, HostScreen()});
        hello_due = true;
    }

    /// Something to ask the host for outright (3: its in-game menu).
    void QueueCommand(u8 what, u8 value) {
        commands.push_back({what, value});
    }

    /// The other console is in its library: no game is running.
    bool IsLobby() const {
        return lobby;
    }

    std::vector<Command> TakeCommands() {
        std::vector<Command> out;
        out.swap(commands);
        return out;
    }

    /// True once after something the HELLO carries has changed.
    bool TakeHelloDue() {
        const bool due = hello_due;
        hello_due = false;
        return due;
    }

    /// Quick switch between the top and the bottom screen (both stick clicks).
    void ToggleView() {
        if (lobby) {
            return; // There are no game screens to switch between yet.
        }
        switch (static_cast<Screens>(settings.screens)) {
        case Screens::BottomHereTopThere:
            settings.screens = static_cast<int>(Screens::TopHereBottomThere);
            break;
        case Screens::TopHereBottomThere:
            settings.screens = static_cast<int>(Screens::BottomHereTopThere);
            break;
        case Screens::TopHere:
            settings.screens = static_cast<int>(Screens::BottomHere);
            break;
        default:
            settings.screens = static_cast<int>(Screens::TopHere);
            break;
        }
        settings.Save();
        QueueHostState();
        dirty = true;
    }

    // ------------------------------------------------------------------ status from the host

    void OnStatus(u8 flags, int game_fps) {
        const bool now_lobby = (flags & 16) != 0;
        if (now_lobby != lobby) {
            // A game started or ended on the other console: ask for the right picture.
            lobby = now_lobby;
            hello_due = true;
            dirty = true;
        }
        host_allows_audio = (flags & 8) != 0;
        amiibo_wanted = (flags & 1) != 0;
        if (game_fps != fps) {
            fps = game_fps;
            dirty = dirty || settings.fps_here;
        }
    }

    bool AmiiboWanted() const {
        return amiibo_wanted;
    }

    void Notify(const std::string& text, u64 now_ns, u64 duration_ns = 3'000'000'000ULL) {
        notice = text;
        notice_until_ns = now_ns + duration_ns;
        dirty = true;
    }

    // ------------------------------------------------------------------ menu

    bool IsOpen() const {
        return open;
    }

    void Open() {
        open = true;
        selected = 0;
        dirty = true;
    }

    void Close() {
        open = false;
        dirty = true;
        settings.Save();
    }

    bool WantsQuit() const {
        return quit;
    }

    bool NeedsRedraw(u64 now_ns) {
        if (!notice.empty() && now_ns >= notice_until_ns) {
            notice.clear();
            dirty = true;
        }
        return dirty;
    }

    /// Pad input while the menu is open.
    void HandlePad(u64 pressed) {
        if (!open) {
            return;
        }
        const int count = static_cast<int>(Rows().size());
        if (pressed & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            selected = (selected + count - 1) % count;
        }
        if (pressed & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            selected = (selected + 1) % count;
        }
        if (pressed & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
            Step(selected, -1);
        }
        if (pressed & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) {
            Step(selected, +1);
        }
        if (pressed & (HidNpadButton_B | HidNpadButton_Plus)) {
            Close();
        }
        dirty = true;
    }

    /// Touch: returns true when the menu or its tab took it (so it is not a tap on the game).
    bool HandleTouch(bool down, int x, int y) {
        const Rect tab = TabRect();
        // A generous zone round the tab: it has to be findable when it is hidden.
        const Rect zone{tab.x - 60, tab.y - 60, tab.w + 120, tab.h + 120};
        if (!down) {
            const bool was_gesture = gesture;
            if (gesture && !open) {
                // Released: a tap on the tab, or a swipe away from its corner, opens the menu.
                const int dx = last_x - start_x, dy = last_y - start_y;
                if (dx * dx + dy * dy >= 50 * 50 || tab.Contains(start_x, start_y)) {
                    Open();
                }
            }
            gesture = false;
            touching = false;
            return was_gesture;
        }
        if (!touching) {
            touching = true;
            start_x = x;
            start_y = y;
            if (open) {
                TouchMenu(x, y);
                gesture = true;
                return true;
            }
            gesture = zone.Contains(x, y);
        }
        last_x = x;
        last_y = y;
        return gesture || open;
    }

    // ------------------------------------------------------------------ picture placement

    /// Where the bottom screen is on this console, if it is shown at all.
    bool BottomScreenRect(int picture_w, int picture_h, Rect& out) const {
        Rect top{}, bottom{};
        bool has_top = false, has_bottom = false;
        Place(picture_w, picture_h, top, has_top, bottom, has_bottom);
        out = bottom;
        return has_bottom;
    }

    /// Draws everything into `pixels` (the framebuffer). `rgb` is the newest picture, or null.
    void Compose(u32* pixels, u32 row_pixels, const u8* rgb, int picture_w, int picture_h) {
        dirty = false;
        canvas.Clear(MakeColor(0, 0, 0));
        Rect top{}, bottom{};
        bool has_top = false, has_bottom = false;
        if (rgb != nullptr && picture_w > 0) {
            Place(picture_w, picture_h, top, has_top, bottom, has_bottom);
            if (picture_w == 640 && picture_h == 480) {
                // The companion screen: one picture, drawn for this console.
                Blit(rgb, picture_w, 0, 0, 640, 480, bottom);
            } else if (picture_h > 240) {
                // Both screens arrive as the 3DS has them: top above, bottom centred under it.
                if (has_top) {
                    Blit(rgb, picture_w, 0, 0, 400, 240, top);
                }
                if (has_bottom) {
                    Blit(rgb, picture_w, 40, 240, 320, 240, bottom);
                }
            } else if (has_top) {
                Blit(rgb, picture_w, 0, 0, picture_w, picture_h, top);
            } else if (has_bottom) {
                Blit(rgb, picture_w, 0, 0, picture_w, picture_h, bottom);
            }
        }
        if (font_ready) {
            if (settings.fps_here && fps > 0) {
                DrawFps(has_top ? &top : nullptr, has_bottom ? &bottom : nullptr);
            }
            DrawTab();
            if (!notice.empty()) {
                DrawNotice();
            }
            if (open) {
                DrawMenu();
            }
        }
        const u32* src = canvas.Data();
        for (int y = 0; y < SCREEN_H; ++y) {
            std::memcpy(pixels + static_cast<std::size_t>(y) * row_pixels, src + y * SCREEN_W,
                        SCREEN_W * sizeof(u32));
        }
    }

    // ------------------------------------------------------------------ before the picture

    /// 0: choosing how to connect, 1: Wi-Fi / LAN, 2: local wireless.
    int startup_mode = 0;

    /// Why local wireless cannot be offered here; empty when it can. Its card is then
    /// greyed out and says this instead.
    std::string wireless_blocked;

    /// A line of progress, shown in the status card (a trailing newline is dropped; several
    /// lines may come in one call).
    void AddLog(std::string_view text) {
        std::size_t begin = 0;
        while (begin < text.size()) {
            std::size_t end = text.find('\n', begin);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            if (end > begin) {
                log.emplace_back(text.substr(begin, end - begin));
            }
            begin = end + 1;
        }
        constexpr std::size_t keep = 15;
        if (log.size() > keep) {
            log.erase(log.begin(), log.end() - keep);
        }
    }

    /// Which choice a touch at (x, y) lands on: 1 Wi-Fi / LAN, 2 local wireless, 3 close.
    int StartupHit(int x, int y) const {
        for (int i = 0; i < 3; ++i) {
            if (CardRect(i).Contains(x, y)) {
                return i + 1;
            }
        }
        return 0;
    }

    /// The screen shown until the first picture arrives: the ways to connect on the left,
    /// what is happening on the right.
    void ComposeStartup(u32* pixels, u32 row_pixels, u64 now_ns) {
        FillGradientV(canvas, 0, 0, SCREEN_W, SCREEN_H, kColShelfTop,
                      kColShelfHorizon);
        if (font_ready) {
            font.Draw(canvas, 64, 104, "Toronja Link", 44, kColText);
            font.Draw(canvas, 64, 138, "Play here what is running on the other console", 18,
                      kColTextDim);
            struct Card {
                const char* hint;
                const char* title;
                const char* text;
            };
            const Card cards[3] = {
                {"A", "Wi-Fi / LAN", "Both consoles on the same network."},
                {"X", "Local wireless",
                 "Console to console, no router. Start this app from the Album's Homebrew Menu."},
                {"+", "Close", "Back to the Homebrew Menu."},
            };
            for (int i = 0; i < 3; ++i) {
                const Rect r = CardRect(i);
                const bool active = startup_mode == i + 1;
                if (i == 1 && !wireless_blocked.empty()) {
                    // Not on offer: flat, dim, and saying why.
                    canvas.RoundBorder(r.x, r.y, r.w, r.h, 18, 2, kColLine, kColBg);
                    font.Draw(canvas, r.x + 30, r.y + 36, "Local wireless: not available here", 20,
                              kColTextDim);
                    int by = r.y + 60;
                    for (const std::string& line : WrapLines(font, wireless_blocked, 15, r.w - 60)) {
                        font.Draw(canvas, r.x + 30, by, line, 15, kColTextDim);
                        by += 19;
                    }
                    continue;
                }
                if (active) {
                    canvas.RoundBorder(r.x, r.y, r.w, r.h, 18, 3, kColAccent,
                                       kColSurfaceHi);
                } else {
                    canvas.FillRoundRect(r.x, r.y, r.w, r.h, 18, kColSurface);
                }
                FillDisc(canvas, r.x + 46.0f, r.y + r.h / 2.0f, 22.0f,
                         active ? kColAccent : kColBadge);
                const int hw = font.Measure(cards[i].hint, 22);
                font.Draw(canvas, r.x + 46 - hw / 2, r.y + r.h / 2 + 8, cards[i].hint, 22,
                          active ? kColOnAccent : kColText);
                font.Draw(canvas, r.x + 86, r.y + 38, cards[i].title, 22, kColText);
                int ty = r.y + 62;
                for (const std::string& line : WrapLines(font, cards[i].text, 15, r.w - 110)) {
                    font.Draw(canvas, r.x + 86, ty, line, 15, kColTextDim);
                    ty += 19;
                }
            }
            font.Draw(canvas, 64, SCREEN_H - 40,
                      "While playing: hold + and - for the menu, click both sticks for the other screen",
                      15, kColTextDim);

            // Status card.
            constexpr int sx = 688, sy = 72, sw = 528, sh = 576;
            canvas.FillRoundRect(sx, sy, sw, sh, 22, kColSurface);
            const char* title = startup_mode == 0   ? "Choose how to connect"
                                : startup_mode == 1 ? "Looking for the other console"
                                                    : "Joining the other console";
            std::string heading = title;
            if (startup_mode != 0) {
                heading.append(static_cast<std::size_t>((now_ns / 400'000'000ULL) % 4), '.');
            }
            font.Draw(canvas, sx + 28, sy + 46, heading, 22, kColAccent);
            const char* tip = startup_mode == 0
                                  ? "On the other console: turn GameShare on and start a game."
                                  : "Start a game on the other console, with GameShare on.";
            font.Draw(canvas, sx + 28, sy + 72, tip, 15, kColTextDim);
            canvas.FillRect(sx + 28, sy + 90, sw - 56, 1, kColLine);
            int ly = sy + 120;
            for (std::size_t i = 0; i < log.size(); ++i) {
                const bool newest = i + 1 == log.size();
                font.Draw(canvas, sx + 28, ly, font.Truncate(log[i], 15, sw - 56), 15,
                          newest ? kColText : kColTextDim);
                ly += 30;
            }
        }
        const u32* src = canvas.Data();
        for (int y = 0; y < SCREEN_H; ++y) {
            std::memcpy(pixels + static_cast<std::size_t>(y) * row_pixels, src + y * SCREEN_W,
                        SCREEN_W * sizeof(u32));
        }
    }

private:
    static Rect CardRect(int index) {
        return {64, 188 + index * 124, 560, 108};
    }

    std::vector<std::string> log;

    struct Row {
        const char* label;
        std::string value;
        const char* help;
        bool action{};
    };

    static const char* ScreensName(int value) {
        switch (static_cast<Screens>(value)) {
        case Screens::BottomHereTopThere:
            return "Bottom here, top on the other console";
        case Screens::TopHereBottomThere:
            return "Top here, bottom on the other console";
        case Screens::BottomHere:
            return "Bottom screen";
        case Screens::TopHere:
            return "Top screen";
        default:
            return "Both screens";
        }
    }

    static const char* BothName(int value) {
        switch (static_cast<BothLayout>(value)) {
        case BothLayout::SideBySide:
            return "Side by side";
        case BothLayout::Stacked:
            return "One above the other";
        case BothLayout::LargeTop:
            return "Large top, small bottom";
        default:
            return "Large bottom, small top";
        }
    }

    std::vector<Row> Rows() const {
        static const char* const opacity[] = {"Hidden", "Faint", "Visible"};
        static const char* const corner[] = {"Top left", "Top right", "Bottom left",
                                             "Bottom right"};
        const bool both = static_cast<Screens>(settings.screens) == Screens::BothHere;
        std::vector<Row> rows;
        rows.push_back({"Screens", ScreensName(settings.screens),
                        "Which screen this console shows. The first two also switch the other "
                        "console to the remaining screen; it goes back to its own layout when you "
                        "pick another option, and can still be changed there by hand."});
        rows.push_back({"Two-screen layout", both ? BothName(settings.both_layout) : "-",
                        both ? "How the two screens are arranged here. Sending both takes about "
                               "twice the data: on a weak link expect a lower frame rate."
                             : "Only used when Screens is set to Both screens."});
        rows.push_back({"Sound", !settings.audio        ? "Off"
                                 : host_allows_audio    ? "On"
                                                        : "On (off on the other console)",
                        "Plays the game's sound here as well. The other console can refuse it "
                        "(GameShare > Share The Sound)."});
        rows.push_back({"Game FPS", settings.fps_here ? "On" : "Off",
                        "Shows the frame rate the game is running at on the other console, "
                        "small, in a corner beside the picture when there is room."});
        rows.push_back({"Menu tab", opacity[settings.tab_opacity],
                        "The tab that opens this menu. Hidden, it still works: tap its corner or "
                        "swipe away from it. (+ and - together open the other console's "
                        "in-game menu; held for three seconds they close this app.)"});
        rows.push_back({"Tab corner", corner[settings.tab_corner], "Where the tab sits."});
        rows.push_back({"Theme", settings.light ? "Light" : "Dark",
                        "The look of this app's own screens."});
        rows.push_back({"Close Toronja Link", "", "Stops and returns to the Homebrew Menu.", true});
        return rows;
    }

    void Step(int row, int dir) {
        const auto cycle = [dir](int value, int count) { return (value + dir + count) % count; };
        switch (row) {
        case 0:
            settings.screens = cycle(settings.screens, static_cast<int>(Screens::Count));
            QueueHostState();
            break;
        case 1:
            if (static_cast<Screens>(settings.screens) == Screens::BothHere) {
                settings.both_layout =
                    cycle(settings.both_layout, static_cast<int>(BothLayout::Count));
            }
            break;
        case 2:
            settings.audio = !settings.audio;
            hello_due = true;
            break;
        case 3:
            settings.fps_here = !settings.fps_here;
            break;
        case 4:
            settings.tab_opacity = cycle(settings.tab_opacity, 3);
            break;
        case 5:
            settings.tab_corner = cycle(settings.tab_corner, 4);
            break;
        case 6:
            settings.light = !settings.light;
            ApplyTheme(settings.light);
            break;
        case 7:
            quit = true;
            break;
        default:
            break;
        }
        dirty = true;
    }

    // Menu geometry.
    static constexpr int MENU_W = 600;
    static constexpr int MENU_X = SCREEN_W - MENU_W;
    static constexpr int ROW_TOP = 96;
    static constexpr int ROW_H = 54;

    void TouchMenu(int x, int y) {
        if (x < MENU_X) {
            Close(); // A tap outside the sheet dismisses it.
            return;
        }
        const int row = (y - ROW_TOP) / ROW_H;
        if (y >= ROW_TOP && row >= 0 && row < static_cast<int>(Rows().size())) {
            selected = row;
            Step(row, x < MENU_X + MENU_W * 55 / 100 ? -1 : +1);
        }
    }

    void DrawMenu() {
        const std::vector<Row> rows = Rows();
        canvas.FillRect(0, 0, MENU_X, SCREEN_H, WithAlpha(MakeColor(0, 0, 0), 110));
        canvas.FillRect(MENU_X, 0, MENU_W, SCREEN_H, kColRail);
        canvas.FillRect(MENU_X, 0, 1, SCREEN_H, kColLine);
        font.Draw(canvas, MENU_X + 28, 52, "Toronja Link", 28, kColText);
        font.Draw(canvas, MENU_X + 28, 76,
                  lobby ? "No game is running: these options are for games"
                        : "The game keeps running on the other console",
                  15, kColTextDim);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const Row& row = rows[i];
            const int y = ROW_TOP + i * ROW_H;
            const bool on = i == selected;
            if (on) {
                canvas.FillRoundRect(MENU_X + 14, y + 3, MENU_W - 28, ROW_H - 6, 14,
                                     kColAccent);
            }
            const u32 fg = on ? kColOnAccent : kColText;
            const u32 dim = on ? kColOnAccent : kColAccent;
            font.Draw(canvas, MENU_X + 32, CenterBaseline(y, ROW_H, 19), row.label, 19, fg);
            if (!row.value.empty()) {
                const std::string value = font.Truncate(row.value, 16, MENU_W * 58 / 100);
                font.Draw(canvas, MENU_X + MENU_W - 32 - font.Measure(value, 16),
                          CenterBaseline(y, ROW_H, 16), value, 16, dim);
            }
        }
        // What the selected row does.
        int ty = ROW_TOP + static_cast<int>(rows.size()) * ROW_H + 30;
        canvas.FillRect(MENU_X + 28, ty - 22, MENU_W - 56, 1, kColLine);
        if (selected >= 0 && selected < static_cast<int>(rows.size())) {
            for (const std::string& line : WrapLines(font, rows[selected].help, 16, MENU_W - 56)) {
                font.Draw(canvas, MENU_X + 28, ty, line, 16, kColTextDim);
                ty += 22;
            }
        }
        font.Draw(canvas, MENU_X + 28, SCREEN_H - 22, "Left / Right  Change      B  Close", 15,
                  kColTextDim);
    }

    Rect TabRect() const {
        constexpr int w = 34, h = 92;
        const bool right = settings.tab_corner == 1 || settings.tab_corner == 3;
        const bool low = settings.tab_corner >= 2;
        return {right ? SCREEN_W - w : 0, low ? SCREEN_H - h - 70 : 70, w, h};
    }

    void DrawTab() {
        if (open || settings.tab_opacity == 0) {
            return;
        }
        const Rect r = TabRect();
        const u8 alpha = settings.tab_opacity == 1 ? 70 : 215;
        const bool right = r.x > 0;
        // A tongue sticking out of the edge: square against it, round on the other side.
        canvas.FillRoundRect(right ? r.x : r.x - 16, r.y, r.w + 16, r.h, 16,
                             WithAlpha(kColSurfaceHi, alpha));
        const int cx = r.x + r.w / 2 + (right ? 2 : -2);
        for (int i = -1; i <= 1; ++i) {
            canvas.FillRoundRect(cx - 2, r.y + r.h / 2 + i * 12 - 2, 4, 4, 2,
                                 WithAlpha(kColAccent, alpha));
        }
    }

    void DrawFps(const Rect* top, const Rect* bottom) {
        char text[24];
        std::snprintf(text, sizeof(text), "%d fps", fps);
        constexpr int size = 15;
        const int tw = font.Measure(text, size);
        // Beside the picture when there is a margin (the bottom screen leaves one on each
        // side); over a corner of it otherwise.
        const Rect* edge = bottom != nullptr ? bottom : top;
        int x = 14, y = 12;
        bool backing = true;
        if (edge != nullptr && edge->x >= tw + 28 && settings.tab_corner != 0) {
            x = (edge->x - tw) / 2;
            backing = false;
        } else if (edge != nullptr && edge->x >= tw + 28) {
            x = edge->x + edge->w + (SCREEN_W - edge->x - edge->w - tw) / 2;
            backing = false;
        }
        if (backing) {
            canvas.FillRoundRect(x - 7, y - 2, tw + 14, 24, 7, WithAlpha(MakeColor(0, 0, 0), 150));
        }
        // Over the black bars beside the picture it has to read on black whatever the theme.
        font.Draw(canvas, x, y + 16, text, size, MakeColor(0x9A, 0xE8, 0xDC));
    }

    void DrawNotice() {
        const std::string text = font.Truncate(notice, 18, SCREEN_W - 160);
        const int tw = font.Measure(text, 18);
        const int w = tw + 48, x = (SCREEN_W - w) / 2, y = SCREEN_H - 78;
        canvas.FillRoundRect(x, y, w, 44, 22, WithAlpha(kColSurfaceHi, 235));
        font.Draw(canvas, x + 24, CenterBaseline(y, 44, 18), text, 18, kColText);
    }

    /// Largest rectangle of aspect w:h inside `area`, centred.
    static Rect Fit(const Rect& area, int w, int h) {
        const float scale = std::min(area.w / float(w), area.h / float(h));
        Rect r;
        r.w = static_cast<int>(w * scale);
        r.h = static_cast<int>(h * scale);
        r.x = area.x + (area.w - r.w) / 2;
        r.y = area.y + (area.h - r.h) / 2;
        return r;
    }

    void Place(int picture_w, int picture_h, Rect& top, bool& has_top, Rect& bottom,
               bool& has_bottom) const {
        const Rect all{0, 0, SCREEN_W, SCREEN_H};
        if (picture_w == 640 && picture_h == 480) {
            // The companion screen takes the whole display; touches on it go back as
            // bottom-screen touches do.
            bottom = Fit(all, 640, 480);
            has_bottom = true;
            return;
        }
        if (picture_h <= 240) {
            // One screen: which one is told by its width.
            if (picture_w >= 400) {
                top = Fit(all, 400, 240);
                has_top = true;
            } else {
                bottom = Fit(all, 320, 240);
                has_bottom = true;
            }
            return;
        }
        has_top = has_bottom = true;
        switch (static_cast<BothLayout>(settings.both_layout)) {
        case BothLayout::Stacked:
            top = Fit({0, 0, SCREEN_W, SCREEN_H / 2}, 400, 240);
            bottom = Fit({0, SCREEN_H / 2, SCREEN_W, SCREEN_H / 2}, 320, 240);
            break;
        case BothLayout::LargeTop:
            top = Fit({0, 0, 960, SCREEN_H}, 400, 240);
            bottom = Fit({960, SCREEN_H - 240, 320, 240}, 320, 240);
            break;
        case BothLayout::LargeBottom:
            bottom = Fit({0, 0, 960, SCREEN_H}, 320, 240);
            top = Fit({960, 0, 320, 240}, 400, 240);
            break;
        default: // Side by side, sharing the width in proportion.
            top = Fit({0, 0, 704, SCREEN_H}, 400, 240);
            bottom = Fit({704, 0, 576, SCREEN_H}, 320, 240);
            break;
        }
    }

    /// Nearest-neighbour copy of part of the picture into `dst`.
    void Blit(const u8* rgb, int picture_w, int sx, int sy, int sw, int sh, const Rect& dst) {
        u32* out = canvas.Data();
        for (int y = 0; y < dst.h; ++y) {
            const int src_y = sy + y * sh / dst.h;
            const u8* row = rgb + (static_cast<std::size_t>(src_y) * picture_w + sx) * 3;
            u32* line = out + static_cast<std::size_t>(dst.y + y) * SCREEN_W + dst.x;
            if (y > 0 && src_y == sy + (y - 1) * sh / dst.h) {
                std::memcpy(line, line - SCREEN_W, dst.w * sizeof(u32)); // Same source row.
                continue;
            }
            for (int x = 0; x < dst.w; ++x) {
                const u8* p = row + (x * sw / dst.w) * 3;
                line[x] = MakeColor(p[0], p[1], p[2]);
            }
        }
    }

    Canvas canvas;
    std::vector<Command> commands;
    bool hello_due = false;
    bool lobby = false;
    bool dirty = true;
    bool open = false;
    bool quit = false;
    int selected = 0;
    bool host_allows_audio = true;
    bool amiibo_wanted = false;

    bool touching = false;
    bool gesture = false;
    int start_x = 0, start_y = 0, last_x = 0, last_y = 0;

    std::string notice;
    u64 notice_until_ns = 0;
    int fps = 0;
    int frames_this_second = 0;
    u64 fps_window_ns = 0;
};

// -------------------------------------------------------------------------------------------
// Sound

/// Plays the PCM the host sends (32728 Hz stereo) on the console's 48 kHz output.
class AudioPlayer {
public:
    bool Start() {
        if (started) {
            return true;
        }
        if (R_FAILED(audoutInitialize())) {
            return false;
        }
        if (R_FAILED(audoutStartAudioOut())) {
            audoutExit();
            return false;
        }
        for (Buffer& buffer : buffers) {
            buffer.data = static_cast<s16*>(aligned_alloc(0x1000, BUFFER_BYTES));
            buffer.busy = false;
            if (buffer.data == nullptr) {
                return false;
            }
        }
        started = true;
        return true;
    }

    void Stop() {
        if (!started) {
            return;
        }
        audoutStopAudioOut();
        audoutExit();
        for (Buffer& buffer : buffers) {
            std::free(buffer.data);
            buffer.data = nullptr;
        }
        fifo.clear();
        started = false;
        primed = false;
    }

    /// `samples`: interleaved stereo at the 3DS's rate.
    void Push(const s16* samples, std::size_t frames) {
        if (!started || frames < 2) {
            return;
        }
        // Linear resampling to 48 kHz; `phase` carries the position across packets.
        // The last frame of the previous packet goes in front, so the interpolation runs
        // across the packet boundary instead of leaving a step there 200 times a second.
        constexpr double step = 32728.0 / 48000.0;
        joined.resize((frames + 1) * 2);
        joined[0] = last_frame[0];
        joined[1] = last_frame[1];
        std::memcpy(joined.data() + 2, samples, frames * 2 * sizeof(s16));
        while (phase < static_cast<double>(frames)) {
            const std::size_t i = static_cast<std::size_t>(phase);
            const double f = phase - static_cast<double>(i);
            for (int ch = 0; ch < 2; ++ch) {
                const double a = joined[i * 2 + ch], b = joined[(i + 1) * 2 + ch];
                fifo.push_back(static_cast<s16>(a + (b - a) * f));
            }
            phase += step;
        }
        phase -= static_cast<double>(frames);
        last_frame[0] = samples[(frames - 1) * 2];
        last_frame[1] = samples[(frames - 1) * 2 + 1];
        // Too far behind is worse than a skip: drop the oldest sound.
        constexpr std::size_t limit = 48000 / 4 * 2; // A quarter of a second.
        if (fifo.size() > limit) {
            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(fifo.size() - limit / 2));
        }
    }

    /// Hands queued sound to the output. Call every pass of the main loop.
    void Pump() {
        if (!started) {
            return;
        }
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count > 0) {
            for (Buffer& buffer : buffers) {
                if (&buffer.out == released) {
                    buffer.busy = false;
                }
            }
            released = nullptr;
            count = 0;
        }
        // Wait for a little sound before starting, so the output does not run dry at once.
        if (!primed) {
            if (fifo.size() < BUFFER_FRAMES * 2 * 2) {
                return;
            }
            primed = true;
        }
        for (Buffer& buffer : buffers) {
            if (buffer.busy || fifo.size() < BUFFER_FRAMES * 2) {
                continue;
            }
            std::memcpy(buffer.data, fifo.data(), BUFFER_FRAMES * 2 * sizeof(s16));
            fifo.erase(fifo.begin(), fifo.begin() + BUFFER_FRAMES * 2);
            buffer.out = {};
            buffer.out.buffer = buffer.data;
            buffer.out.buffer_size = BUFFER_BYTES;
            buffer.out.data_size = BUFFER_FRAMES * 2 * sizeof(s16);
            if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&buffer.out))) {
                buffer.busy = true;
            }
        }
        if (fifo.empty()) {
            primed = false;
        }
    }

private:
    static constexpr std::size_t BUFFER_FRAMES = 768; // 16 ms at 48 kHz.
    static constexpr std::size_t BUFFER_BYTES = 0x1000;
    struct Buffer {
        AudioOutBuffer out{};
        s16* data{};
        bool busy{};
    };
    Buffer buffers[6];
    std::vector<s16> fifo;
    std::vector<s16> joined;
    s16 last_frame[2]{};
    double phase = 0.0;
    bool started = false;
    bool primed = false;
};

} // namespace Link
