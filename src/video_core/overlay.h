// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "common/common_types.h"

// A render-agnostic description of an on-screen overlay menu. The frontend fills it in from the
// input thread and the active renderer reads it back on the emulation thread to draw it on top of
// the game.
namespace VideoCore {

/// Shows the render scale and the size it comes to, where the FPS counter goes.
inline std::atomic<bool> g_show_resolution{false};

struct OverlayMenuItem {
    std::string label;
    std::string value;
    bool is_action{};
    bool is_header{};
    bool locked{}; // Shown, but not changeable while a game is running.
};

// One button or tile of the in-game menu's home screen. Positions are fractions of the
// menu's canvas; the frontend lays them out, since it also moves the cursor between
// them and matches touches to them.
struct OverlayCell {
    float x{}, y{}, w{}, h{};
    std::string label;
    std::string sub;
    int style{};   // 0 plain, 1 primary, 2 danger, 3 screen-layout tile
    bool active{}; // Layout tile: the layout in use.
    // Layout tile: the top and the bottom screen, as fractions of the tile's picture
    // area (x, y, w, h). A width of 0 leaves that screen out.
    float top_screen[4]{};
    float bottom_screen[4]{};
};

struct OverlayText {
    float x{}, y{};
    std::string text;
    float scale{1.0f};
    int tone{};      // 0 normal, 1 dim, 2 accent
    bool centred{};  // x is the middle of the text, not its left edge.
};

// One page of the in-game menu, as listed down its left side.
struct OverlayMenuSection {
    std::string name;
    bool advanced{}; // Listed apart, under an "Advanced" heading.
};

struct OverlayMenuState {
    bool visible{};
    std::string title;
    std::vector<OverlayMenuItem> items;
    int selected{};
    std::string hint; // Footer help text.
    std::string description; // What the selected row is for; shown above the hint.
    std::vector<OverlayMenuSection> sections; // Empty: no side list.
    int active_section{-1};
    bool side_focused{}; // The cursor is in the side list, not in the rows.
    bool light{}; // Light theme.
    // The home screen: full-screen buttons and tiles instead of the list.
    bool home{};
    std::vector<OverlayCell> cells;
    std::vector<OverlayText> texts;
    int cell_selected{-1};
};

// Clockwise rotation the renderer applies to everything it draws over the game.
void SetOverlayRotation(u32 degrees);
u32 GetOverlayRotation();

// Publishes the latest overlay description (called from the input thread).
void SetOverlayMenuState(const OverlayMenuState& state);

// Snapshots the current overlay description (called from the renderer/emulation thread).
OverlayMenuState GetOverlayMenuState();

// Where the parts of the menu ended up on screen, as fractions of its width and height,
// so a touch can be matched to them. Published by the renderer each time it lays the
// menu out.
struct OverlayHit {
    float x0{}, y0{}, x1{}, y1{};
    int kind{};  // 0 a row, 1 a page in the side list, 2 the panel itself
    int index{}; // Row on screen / page in the list.
};
void SetOverlayHitMap(std::vector<OverlayHit> hits);
std::vector<OverlayHit> GetOverlayHitMap();

// Skip the copy above when nothing is shown.
bool IsOverlayMenuVisible();

// Shader-compilation activity.
void NotifyShaderCompileBegin();
void NotifyShaderCompileEnd();
u32 GetPendingShaderCompiles();

// A short-lived message shown over the game, used to report things the player asked for.
// Posting a new one replaces whatever is showing.
void PostOverlayToast(const std::string& text, u32 duration_ms = 2500);

// Returns the live toast text, or an empty string once it has expired.
std::string GetOverlayToast();

} // namespace VideoCore
