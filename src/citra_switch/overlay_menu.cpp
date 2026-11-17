// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "citra_switch/amiibo_session.h"
#include "citra_switch/covers.h"
#include "citra_switch/game_share.h"
#include "citra_switch/camera/still_image_camera.h"
#include "citra_switch/config.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "common/settings.h"
#include "core/savestate.h"
#include "video_core/overlay.h"

namespace SwitchFrontend {

namespace {

// The kind of row. Setting rows index into the current page's row list, cheat rows into the
// engine's cheat list, save state rows into the slots, and section rows into Page.
enum class Item {
    Setting,
    Section,
    Separator,
    SaveHere,
    LoadHere,
    GameShareSwitch,
    Cheat,
    CheatsEmpty,
    SaveStateSlot,
    AmiiboFile,
    AmiibosEmpty,
    ScanRealAmiibo,
    OverwriteAmiibo,
    RemoveAmiibo,
    CameraTargetRow,
    CameraImage,
    CameraImagesEmpty,
    ClearCamera,
    Resume,
    ExitGame,
};

struct Row {
    Item item;
    int index = -1;
};

// The overlay opens on Home: a screen of buttons and tiles for what is reached for mid-game.
// "Settings" there leads to the pages below, listed down the side as in the launcher.
enum class Page {
    Home,
    Live,     // Every option that can be changed while the game runs, in one place.
    Settings, // One of the launcher's settings pages (s_category); most rows are locked.
    States,
    Cheats,
    Amiibo,
    Camera,
    Count,
};

constexpr int NumPages = static_cast<int>(Page::Count);

// Which of the launcher's pages Page::Settings is showing.
Category s_category = Category::General;

bool IsSettingsPage(Page page) {
    return page == Page::Live || page == Page::Settings;
}

const char* PageName(Page page) {
    switch (page) {
    case Page::Home:
        return "Quick Menu";
    case Page::States:
        return "Save States";
    case Page::Cheats:
        return "Cheats";
    case Page::Amiibo:
        return "Amiibo";
    case Page::Camera:
        return "Camera";
    case Page::Live:
        return "Live Tweaks";
    case Page::Settings:
        return CategoryName(s_category);
    default:
        return "";
    }
}

constexpr int kMaxVisibleRows = 10;

// Cheats past this many spill onto further sub-pages.
constexpr int kCheatsPerPage = 8;

// Save state slots past this many spill onto further sub-pages.
constexpr int kSlotsPerPage = 8;

constexpr int kAmiibosPerPage = 7;

// One row is spent on the target selector and one on the clear action.
constexpr int kCameraImagesPerPage = 6;

std::atomic<bool> s_open{false};
// The game is frozen while the in-game menu is open, unless turned off.
std::atomic<bool> s_pause_in_menu{true};

// The side list, in the order L/R steps through it: Home, the live options, the launcher's
// everyday pages, the game's own tools, then the Advanced pages.
struct NavEntry {
    Page page;
    Category category;
    bool advanced;
};

const std::vector<NavEntry>& NavList() {
    static const std::vector<NavEntry> list = [] {
        std::vector<NavEntry> entries;
        entries.push_back({Page::Home, Category::Count, false});
        entries.push_back({Page::Live, Category::Count, false});
        for (int i = 0; i < NumCategories; ++i) {
            const auto category = static_cast<Category>(i);
            if (!IsAdvancedCategory(category)) {
                entries.push_back({Page::Settings, category, false});
            }
        }
        for (const Page page : {Page::States, Page::Cheats, Page::Amiibo, Page::Camera}) {
            entries.push_back({page, Category::Count, false});
        }
        for (int i = 0; i < NumCategories; ++i) {
            const auto category = static_cast<Category>(i);
            if (IsAdvancedCategory(category)) {
                entries.push_back({Page::Settings, category, true});
            }
        }
        return entries;
    }();
    return list;
}

std::string NavName(const NavEntry& entry) {
    if (entry.page == Page::Home) {
        return "Home";
    }
    return entry.page == Page::Settings ? CategoryName(entry.category) : PageName(entry.page);
}

bool IsAdvancedPage(Page page) {
    return page == Page::Settings && IsAdvancedCategory(s_category);
}

int s_page = 0;
int s_selected = 0;
int s_cheat_page = 0;
int s_state_page = 0;
int s_amiibo_page = 0;
bool s_overwrite_armed = false;
int s_camera_page = 0;
std::vector<Row> s_rows;
std::vector<SettingsRow> s_settings;
int s_scroll = 0;
int s_home_slot = 0;
std::vector<FileEntry> s_amiibos;
std::vector<FileEntry> s_camera_images;
bool s_cheats_dirty = false;

// Slot status strings.
// Refreshed on demand so repaints don't stat the state directory.
std::array<std::string, Core::SaveStateSlotCount> s_slot_status;

void RefreshSaveStates() {
    for (u32 slot = 0; slot < Core::SaveStateSlotCount; ++slot) {
        s_slot_status[slot] = SaveStateSlotStatus(slot);
    }
}

int StatePageCount() {
    return (static_cast<int>(Core::SaveStateSlotCount) + kSlotsPerPage - 1) / kSlotsPerPage;
}

void RefreshAmiibos() {
    s_amiibos = ListAmiiboFiles();
}

int AmiiboPageCount() {
    const int count = static_cast<int>(s_amiibos.size());
    return count <= 0 ? 1 : (count + kAmiibosPerPage - 1) / kAmiibosPerPage;
}

bool LoadAmiibo(const std::string& path) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        VideoCore::PostOverlayToast("No game is running");
        return false;
    }
    auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) {
        VideoCore::PostOverlayToast("The NFC service is unavailable");
        return false;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (nfc->IsTagActive()) {
        VideoCore::PostOverlayToast("An Amiibo is already active");
        return false;
    }
    if (!nfc->IsSearchingForAmiibos()) {
        VideoCore::PostOverlayToast("The game is not scanning for Amiibo");
        return false;
    }
    if (!nfc->LoadAmiibo(path)) {
        VideoCore::PostOverlayToast("Could not load the Amiibo file");
        return false;
    }
    VideoCore::PostOverlayToast("Amiibo loaded");
    return true;
}

void RemoveAmiibo() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        VideoCore::PostOverlayToast("No game is running");
        return;
    }
    auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) {
        VideoCore::PostOverlayToast("The NFC service is unavailable");
        return;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (!nfc->IsTagActive()) {
        VideoCore::PostOverlayToast("No Amiibo is active");
        return;
    }
    nfc->RemoveAmiibo();
    VideoCore::PostOverlayToast("Amiibo removed");
}

void RefreshCameraImages() {
    s_camera_images = ListCameraImages();
}

int CameraPageCount() {
    const int count = static_cast<int>(s_camera_images.size());
    return count <= 0 ? 1 : (count + kCameraImagesPerPage - 1) / kCameraImagesPerPage;
}

// Makes a running game pick the cameras up again.
void ReloadGuestCameras() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }
    if (auto cam = Service::CAM::GetModule(system)) {
        cam->ReloadCameraDevices();
    }
}

void SelectCameraImage(const std::string& path) {
    // Decoding here rather than on the capture thread means a broken file is reported to the
    // player.
    const std::string error = Camera::StillImage::Preload(path);
    if (!error.empty()) {
        VideoCore::PostOverlayToast("Cannot use that image: " + error);
        return;
    }
    SetCameraImage(path, GetCameraTarget());
    ReloadGuestCameras();
    VideoCore::PostOverlayToast("Cameras now show this image");
}

void ClearCameraImage() {
    if (GetCameraImage().empty()) {
        VideoCore::PostOverlayToast("The cameras are already blank");
        return;
    }
    SetCameraImage("", GetCameraTarget());
    ReloadGuestCameras();
    VideoCore::PostOverlayToast("Cameras cleared");
}

void StepCameraTarget(int dir) {
    const int count = NumCameraTargets;
    const int next = (static_cast<int>(GetCameraTarget()) + dir + count) % count;
    SetCameraImage(GetCameraImage(), static_cast<CameraTarget>(next));
    ReloadGuestCameras();
}

Page CurrentPage() {
    return static_cast<Page>(std::clamp(s_page, 0, NumPages - 1));
}

Cheats::CheatEngine* GetCheatEngine() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return nullptr;
    }
    return &system.CheatEngine();
}

int CheatCount() {
    auto* engine = GetCheatEngine();
    return engine ? static_cast<int>(engine->GetCheats().size()) : 0;
}

int CheatPageCount() {
    const int count = CheatCount();
    return count <= 0 ? 1 : (count + kCheatsPerPage - 1) / kCheatsPerPage;
}

std::string CheatName(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return "";
    }
    const auto cheats = engine->GetCheats();
    return index >= 0 && index < static_cast<int>(cheats.size()) ? cheats[index]->GetName() : "";
}

bool CheatEnabled(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return false;
    }
    const auto cheats = engine->GetCheats();
    return index >= 0 && index < static_cast<int>(cheats.size()) && cheats[index]->IsEnabled();
}

void ToggleCheat(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return;
    }
    const auto cheats = engine->GetCheats();
    if (index < 0 || index >= static_cast<int>(cheats.size())) {
        return;
    }
    cheats[index]->SetEnabled(!cheats[index]->IsEnabled());
    s_cheats_dirty = true;
}

// Writes the enabled state the player just picked back to the cheat file so it sticks.
void PersistCheats() {
    if (!s_cheats_dirty) {
        return;
    }
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        u64 title_id = 0;
        system.GetAppLoader().ReadProgramId(title_id);
        system.CheatEngine().SaveCheatFile(title_id);
    }
    s_cheats_dirty = false;
}

// Rebuilds the visible rows for the active page and keeps the cursor in range.
// The home screen's buttons and tiles. Positions are in fractions of the screen.
enum class CellKind {
    Resume,
    Layout,
    SaveState,
    LoadState,
    GameShare,
    Fps,
    Amiibo,
    Cheats,
    Settings,
    Exit,
};

struct Cell {
    CellKind kind;
    int index; // Layout: the preset.
    float x, y, w, h;
};

std::vector<Cell> s_cells;
int s_cell = 0;
float s_layout_label_y = 0.0f;
float s_quick_label_y = 0.0f;

void BuildHomeCells() {
    s_cells.clear();
    constexpr float W = 1280.0f, H = 720.0f, left = 64.0f, width = 1152.0f, gap = 13.0f;
    const auto add = [&](CellKind kind, int index, float x, float y, float w, float h) {
        s_cells.push_back({kind, index, x / W, y / H, w / W, h / H});
    };
    add(CellKind::Resume, 0, left, 92.0f, width, 70.0f);

    // The layouts the user kept in the rotation (all of them if none is).
    std::vector<int> layouts;
    const std::uint32_t mask = GetLayoutCycleMask();
    for (int i = 0; i < GetScreenLayoutCount(); ++i) {
        if ((mask >> i) & 1u) {
            layouts.push_back(i);
        }
    }
    if (layouts.empty()) {
        for (int i = 0; i < GetScreenLayoutCount(); ++i) {
            layouts.push_back(i);
        }
    }
    constexpr int per_row = 5;
    constexpr float tile_h = 124.0f, row_gap = 12.0f, layout_top = 206.0f;
    const float tile_w = (width - (per_row - 1) * gap) / per_row;
    for (int k = 0; k < static_cast<int>(layouts.size()) && k < 2 * per_row; ++k) {
        add(CellKind::Layout, layouts[static_cast<std::size_t>(k)],
            left + (k % per_row) * (tile_w + gap), layout_top + (k / per_row) * (tile_h + row_gap),
            tile_w, tile_h);
    }
    const int rows = std::min(2, (static_cast<int>(layouts.size()) + per_row - 1) / per_row);
    s_layout_label_y = (layout_top - 28.0f) / H;

    const float quick_top = layout_top + rows * (tile_h + row_gap) + 40.0f;
    s_quick_label_y = (quick_top - 28.0f) / H;
    constexpr CellKind quick[] = {CellKind::SaveState, CellKind::LoadState, CellKind::GameShare,
                                  CellKind::Fps,       CellKind::Amiibo,    CellKind::Cheats};
    constexpr int quick_count = 6;
    const float quick_w = (width - (quick_count - 1) * gap) / quick_count;
    for (int i = 0; i < quick_count; ++i) {
        add(quick[i], 0, left + i * (quick_w + gap), quick_top, quick_w, 62.0f);
    }

    const float bottom_top = quick_top + 62.0f + 22.0f;
    const float half = (width - gap) / 2.0f;
    add(CellKind::Settings, 0, left, bottom_top, half, 58.0f);
    add(CellKind::Exit, 0, left + half + gap, bottom_top, half, 58.0f);
    s_cell = std::clamp(s_cell, 0, static_cast<int>(s_cells.size()) - 1);
}

// Where the two screens go in each layout preset, as fractions of a tile's picture: top
// screen, then bottom screen. A width of 0 leaves that screen out.
struct LayoutPicture {
    float top[4];
    float bottom[4];
};

LayoutPicture PictureOfLayout(int preset) {
    switch (preset) {
    case 1: // Side by side
        return {{0.03f, 0.24f, 0.52f, 0.52f}, {0.58f, 0.28f, 0.39f, 0.44f}};
    case 2: // Large top, small bottom
        return {{0.03f, 0.12f, 0.68f, 0.76f}, {0.74f, 0.36f, 0.23f, 0.30f}};
    case 3: // Large bottom, small top
        return {{0.72f, 0.36f, 0.25f, 0.26f}, {0.03f, 0.08f, 0.66f, 0.84f}};
    case 4: // Top screen only
        return {{0.10f, 0.08f, 0.80f, 0.84f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    case 5: // Bottom screen only
        return {{0.0f, 0.0f, 0.0f, 0.0f}, {0.18f, 0.06f, 0.64f, 0.88f}};
    case 6: // Vertical stack, console turned
        return {{0.22f, 0.08f, 0.27f, 0.84f}, {0.53f, 0.16f, 0.24f, 0.68f}};
    case 7: // Hybrid
        return {{0.03f, 0.10f, 0.62f, 0.80f}, {0.68f, 0.52f, 0.29f, 0.38f}};
    case 8: // Vertical stack, console turned the other way
        return {{0.51f, 0.08f, 0.27f, 0.84f}, {0.23f, 0.16f, 0.24f, 0.68f}};
    case 9: // Bottom screen overlaid on the top one
        return {{0.06f, 0.06f, 0.88f, 0.88f}, {0.66f, 0.54f, 0.26f, 0.36f}};
    default: // Vertical stack
        return {{0.28f, 0.03f, 0.44f, 0.47f}, {0.32f, 0.52f, 0.36f, 0.45f}};
    }
}

bool IsSkippable(const Row& row);

void RebuildRows() {
    s_rows.clear();
    s_settings.clear();
    const Page page = CurrentPage();
    if (page == Page::Home) {
        BuildHomeCells();
    } else if (IsSettingsPage(page)) {
        if (page == Page::Live) {
            // Everything the in-game menu may change, section after section.
            for (int section = 1; section < NumQuickSections; ++section) {
                std::vector<SettingsRow> rows = BuildQuickRows(static_cast<QuickSection>(section));
                if (rows.empty()) {
                    continue;
                }
                SettingsRow heading;
                heading.label = QuickSectionName(static_cast<QuickSection>(section));
                heading.is_header = true;
                s_settings.push_back(std::move(heading));
                for (SettingsRow& row : rows) {
                    row.live = true;
                    s_settings.push_back(std::move(row));
                }
            }
        } else {
            s_settings = BuildCategoryRows(s_category);
        }
        for (int i = 0; i < static_cast<int>(s_settings.size()); ++i) {
            s_rows.push_back({Item::Setting, i});
        }
    } else if (page == Page::States) {
        s_state_page = std::clamp(s_state_page, 0, StatePageCount() - 1);
        const int first = s_state_page * kSlotsPerPage;
        const int last =
            std::min(static_cast<int>(Core::SaveStateSlotCount), first + kSlotsPerPage);
        for (int slot = first; slot < last; ++slot) {
            s_rows.push_back({Item::SaveStateSlot, slot});
        }
    } else if (page == Page::Cheats) {
        const int count = CheatCount();
        s_cheat_page = std::clamp(s_cheat_page, 0, CheatPageCount() - 1);
        const int first = s_cheat_page * kCheatsPerPage;
        const int last = std::min(count, first + kCheatsPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::Cheat, i});
        }
        if (s_rows.empty()) {
            s_rows.push_back({Item::CheatsEmpty});
        }
    } else if (page == Page::Amiibo) {
        s_amiibo_page = std::clamp(s_amiibo_page, 0, AmiiboPageCount() - 1);
        const int first = s_amiibo_page * kAmiibosPerPage;
        const int last = std::min(static_cast<int>(s_amiibos.size()), first + kAmiibosPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::AmiiboFile, i});
        }
        if (s_amiibos.empty()) {
            s_rows.push_back({Item::AmiibosEmpty});
        }
        s_rows.push_back({Item::ScanRealAmiibo});
        if (HasPendingAmiiboOverwrite()) {
            s_rows.push_back({Item::OverwriteAmiibo});
        }
        s_rows.push_back({Item::RemoveAmiibo});
    } else {
        s_rows.push_back({Item::CameraTargetRow});
        s_camera_page = std::clamp(s_camera_page, 0, CameraPageCount() - 1);
        const int first = s_camera_page * kCameraImagesPerPage;
        const int last =
            std::min(static_cast<int>(s_camera_images.size()), first + kCameraImagesPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::CameraImage, i});
        }
        if (s_camera_images.empty()) {
            s_rows.push_back({Item::CameraImagesEmpty});
        }
        s_rows.push_back({Item::ClearCamera});
    }
    if (s_rows.empty()) {
        s_selected = 0;
        s_scroll = 0;
        return;
    }
    s_selected = std::clamp(s_selected, 0, static_cast<int>(s_rows.size()) - 1);
    while (s_selected + 1 < static_cast<int>(s_rows.size()) &&
           IsSkippable(s_rows[static_cast<std::size_t>(s_selected)])) {
        ++s_selected;
    }
    const int max_scroll = std::max(0, static_cast<int>(s_rows.size()) - kMaxVisibleRows);
    s_scroll = std::clamp(s_scroll, std::max(0, s_selected - kMaxVisibleRows + 1),
                          std::min(s_selected, max_scroll));
}

// Rows that only respond to A.
// A heading, or anything else the cursor passes over.
bool IsSkippable(const Row& row) {
    return row.item == Item::Separator ||
           (row.item == Item::Setting && s_settings[static_cast<std::size_t>(row.index)].is_header);
}

// A setting that has to wait until no game is running.
bool IsLocked(const Row& row) {
    if (row.item != Item::Setting) {
        return false;
    }
    const SettingsRow& setting = s_settings[static_cast<std::size_t>(row.index)];
    return !setting.is_header && !setting.live;
}

void SayLocked() {
    VideoCore::PostOverlayToast("Locked while a game is running: change it from the library", 3000);
}

bool IsAction(const Row& row) {
    switch (row.item) {
    case Item::Setting:
    case Item::Cheat:
    case Item::CameraTargetRow:
    case Item::SaveHere:
    case Item::LoadHere:
    case Item::GameShareSwitch:
        return false;
    default:
        return true;
    }
}

const char* SeparatorLabel(int index) {
    return index == 0 ? "Pages" : "Advanced";
}

std::string SlotSummary(int slot) {
    const std::string& status = s_slot_status[static_cast<std::size_t>(slot)];
    return SaveStateSlotName(static_cast<unsigned int>(slot)) +
           (status.empty() ? " - empty" : " - " + status);
}

std::string Label(const Row& row) {
    switch (row.item) {
    case Item::Setting:
        return s_settings[static_cast<std::size_t>(row.index)].label;
    case Item::Section:
        return std::string{PageName(static_cast<Page>(row.index))} + "  >";
    case Item::Separator:
        return SeparatorLabel(row.index);
    case Item::SaveHere:
        return "Save State";
    case Item::LoadHere:
        return "Load State";
    case Item::GameShareSwitch:
        return "GameShare";
    case Item::Cheat:
        return CheatName(row.index);
    case Item::CheatsEmpty:
        return "No cheats loaded";
    case Item::SaveStateSlot:
        return SaveStateSlotName(static_cast<unsigned int>(row.index));
    case Item::AmiiboFile:
        return s_amiibos[static_cast<std::size_t>(row.index)].name;
    case Item::AmiibosEmpty:
        return "No .bin files in amiibo/";
    case Item::ScanRealAmiibo:
        if (IsRealAmiiboActive()) {
            return "Amiibo on the reader";
        }
        return IsRealAmiiboScanning() ? "Waiting for an Amiibo..." : "Scan a real Amiibo";
    case Item::OverwriteAmiibo:
        return s_overwrite_armed
                   ? "Press A again to erase " + PendingAmiiboOverwriteOwner()
                   : "Save anyway (erases " + PendingAmiiboOverwriteOwner() + ")";
    case Item::RemoveAmiibo:
        return "Remove active Amiibo";
    case Item::CameraTargetRow:
        return "Feed image to";
    case Item::CameraImage:
        return s_camera_images[static_cast<std::size_t>(row.index)].name;
    case Item::CameraImagesEmpty:
        return "No .png/.jpg files in camera/";
    case Item::ClearCamera:
        return "Show nothing";
    case Item::Resume:
        return "Resume Game";
    case Item::ExitGame:
        return "Exit to Library";
    }
    return "";
}

std::string Value(const Row& row) {
    switch (row.item) {
    case Item::Setting: {
        // Headings have no value (and no function to ask for one).
        const SettingsRow& setting = s_settings[static_cast<std::size_t>(row.index)];
        return setting.is_header || !setting.value ? std::string{} : setting.value();
    }
    case Item::SaveHere:
    case Item::LoadHere:
        return SlotSummary(s_home_slot);
    case Item::GameShareSwitch:
        return GameShare::IsEnabled()
                   ? std::string{"On ("} + GameShare::ModeName() + ")"
                   : std::string{"Off"};
    case Item::Cheat:
        return CheatEnabled(row.index) ? "On" : "Off";
    case Item::SaveStateSlot: {
        const std::string& status = s_slot_status[static_cast<std::size_t>(row.index)];
        return status.empty() ? "Empty" : status;
    }
    case Item::CameraTargetRow:
        return CameraTargetName(GetCameraTarget());
    case Item::CameraImage:
        return s_camera_images[static_cast<std::size_t>(row.index)].path == GetCameraImage()
                   ? "In use"
                   : "";
    default:
        return "";
    }
}

// Left/right on a value row. `dir` is -1 or +1.
void Adjust(const Row& row, int dir) {
    switch (row.item) {
    case Item::Setting:
        if (IsLocked(row)) {
            SayLocked();
            break;
        }
        if (s_settings[static_cast<std::size_t>(row.index)].step) {
            s_settings[static_cast<std::size_t>(row.index)].step(dir);
        }
        break;
    case Item::SaveHere:
    case Item::LoadHere:
        s_home_slot = (s_home_slot + dir + static_cast<int>(Core::SaveStateSlotCount)) %
                      static_cast<int>(Core::SaveStateSlotCount);
        break;
    case Item::GameShareSwitch:
        GameShare::SetEnabled(!GameShare::IsEnabled());
        break;
    case Item::Cheat:
        ToggleCheat(row.index);
        break;
    case Item::CameraTargetRow:
        StepCameraTarget(dir);
        break;
    default:
        break;
    }
}

// Pressing 'a' on a row advances the list by one, or flips it when it is an On/Off row.
void Activate(const Row& row) {
    if (row.item != Item::Setting) {
        Adjust(row, 1);
        return;
    }
    if (IsLocked(row)) {
        SayLocked();
        return;
    }
    const SettingsRow& setting = s_settings[static_cast<std::size_t>(row.index)];
    if (setting.step) {
        setting.step(setting.boolean && setting.boolean() ? -1 : 1);
    }
}

std::string ListSuffix(int page, int pages) {
    return pages > 1 ? "  List " + std::to_string(page + 1) + "/" + std::to_string(pages)
                     : std::string{};
}

const char* HintFor(Page page) {
    switch (page) {
    case Page::Home:
        return "A Select   +/- Close";
    case Page::States:
        return "A Load   X Save   Y Delete   ZL/ZR List   B Back";
    case Page::Cheats:
        return "A Toggle   ZL/ZR List   B Back";
    case Page::Amiibo:
        return "A Load/Remove   ZL/ZR List   B Back";
    case Page::Camera:
        return "A Select   ZL/ZR List   B Back";
    default:
        return "A Change   L/R Section   B Back";
    }
}

// Fills in the home screen: its cells, the texts round them, the hint.
void PaintHome(VideoCore::OverlayMenuState& state) {
    state.home = true;
    state.cell_selected = s_cell;
    const bool paused = IsEmulationPaused();
    state.texts.push_back({64.0f / 1280.0f, 20.0f / 720.0f, paused ? "Game paused" : "Game running",
                           1.25f, 0, false});
    state.texts.push_back(
        {64.0f / 1280.0f, 58.0f / 720.0f,
         paused ? "Your game is open behind this menu, frozen until you go back to it."
                : "Your game is open behind this menu and keeps running.",
         0.66f, 1, false});
    state.texts.push_back({64.0f / 1280.0f, s_layout_label_y, "SCREEN LAYOUT", 0.60f, 1, false});
    state.texts.push_back({64.0f / 1280.0f, s_quick_label_y, "QUICK", 0.60f, 1, false});

    const int layout_in_use = GetScreenLayoutIndex();
    for (const Cell& cell : s_cells) {
        VideoCore::OverlayCell out;
        out.x = cell.x;
        out.y = cell.y;
        out.w = cell.w;
        out.h = cell.h;
        switch (cell.kind) {
        case CellKind::Resume:
            out.label = "Back to the game";
            out.style = 1;
            break;
        case CellKind::Layout: {
            out.label = GetScreenLayoutName(cell.index);
            out.style = 3;
            out.active = cell.index == layout_in_use;
            const LayoutPicture picture = PictureOfLayout(cell.index);
            std::copy(std::begin(picture.top), std::end(picture.top), out.top_screen);
            std::copy(std::begin(picture.bottom), std::end(picture.bottom), out.bottom_screen);
            break;
        }
        case CellKind::SaveState:
            out.label = "Save State";
            out.sub = SlotSummary(s_home_slot);
            break;
        case CellKind::LoadState:
            out.label = "Load State";
            out.sub = SlotSummary(s_home_slot);
            break;
        case CellKind::GameShare:
            out.label = "GameShare";
            out.sub = GameShare::IsEnabled() ? std::string{"On: "} + GameShare::ModeName() : "Off";
            break;
        case CellKind::Fps:
            out.label = "FPS Counter";
            out.sub = Settings::values.show_fps.GetValue() ? "On" : "Off";
            break;
        case CellKind::Amiibo:
            out.label = "Amiibo";
            out.sub = IsRealAmiiboActive() ? "On the reader" : "Files and reader";
            break;
        case CellKind::Cheats:
            out.label = "Cheats";
            out.sub = std::to_string(CheatCount()) + " loaded";
            break;
        case CellKind::Settings:
            out.label = "Settings";
            out.sub = "Everything else. What needs the game closed is locked.";
            break;
        case CellKind::Exit:
            out.label = "Exit to Library";
            out.sub = "Closes the game";
            out.style = 2;
            break;
        }
        state.cells.push_back(std::move(out));
    }
    state.hint = "A  Choose      X  Next save slot      L / R  Pages      B  Back to the game";
}

void Repaint() {
    const Page page = CurrentPage();
    VideoCore::OverlayMenuState state;
    state.visible = s_open.load(std::memory_order_relaxed);
    state.light = GetUiTheme() != 0;
    if (page == Page::Home) {
        PaintHome(state);
        VideoCore::SetOverlayMenuState(state);
        return;
    }
    state.title = PageName(page);
    const std::vector<NavEntry>& nav_list = NavList();
    for (int i = 0; i < static_cast<int>(nav_list.size()); ++i) {
        const NavEntry& entry = nav_list[static_cast<std::size_t>(i)];
        state.sections.push_back({NavName(entry), entry.advanced});
        if (entry.page == page && (page != Page::Settings || entry.category == s_category)) {
            state.active_section = i;
        }
    }
    if (s_selected >= 0 && s_selected < static_cast<int>(s_rows.size())) {
        const Row& focused = s_rows[static_cast<std::size_t>(s_selected)];
        if (focused.item == Item::Setting && focused.index >= 0 &&
            focused.index < static_cast<int>(s_settings.size())) {
            state.description = IsLocked(focused)
                                    ? "Locked while a game is running. Change it from the library."
                                    : s_settings[static_cast<std::size_t>(focused.index)].description;
        }
    }
    if (IsAdvancedPage(page) && state.description.empty()) {
        state.description = "Advanced: change these only if you know what they do.";
    }
    switch (page) {
    case Page::Cheats:
        state.title += ListSuffix(s_cheat_page, CheatPageCount());
        break;
    case Page::States:
        state.title += ListSuffix(s_state_page, StatePageCount());
        break;
    case Page::Amiibo:
        state.title += ListSuffix(s_amiibo_page, AmiiboPageCount());
        break;
    case Page::Camera:
        state.title += ListSuffix(s_camera_page, CameraPageCount());
        break;
    default:
        break;
    }
    state.hint = HintFor(page);

    const int count = static_cast<int>(s_rows.size());
    const int visible = std::min(count, kMaxVisibleRows);
    const int first = std::clamp(s_scroll, 0, std::max(0, count - visible));
    if (count > visible) {
        state.hint += "   " + std::to_string(first + 1) + "-" + std::to_string(first + visible) +
                      " of " + std::to_string(count);
    }

    state.selected = s_selected - first;
    state.items.reserve(static_cast<std::size_t>(visible));
    for (int i = first; i < first + visible; ++i) {
        const Row& row = s_rows[static_cast<std::size_t>(i)];
        std::string value = Value(row);
        const bool no_value = value.empty();
        state.items.push_back({Label(row), std::move(value), no_value, IsSkippable(row)});
        state.items.back().locked = IsLocked(row);
    }
    VideoCore::SetOverlayMenuState(state);
}

} // namespace

bool IsQuickMenuOpen() {
    return s_open.load(std::memory_order_relaxed);
}

bool IsPauseInQuickMenu() {
    return s_pause_in_menu.load(std::memory_order_relaxed);
}

void SetPauseInQuickMenu(bool enabled) {
    s_pause_in_menu.store(enabled, std::memory_order_relaxed);
    if (IsQuickMenuOpen()) {
        SetEmulationPaused(enabled);
    }
}

void OpenQuickMenu() {
    s_page = static_cast<int>(Page::Home);
    s_cell = 0;
    s_selected = 0;
    s_scroll = 0;
    s_cheat_page = 0;
    s_state_page = 0;
    s_amiibo_page = 0;
    s_camera_page = 0;
    RefreshSaveStates();
    RefreshAmiibos();
    RefreshCameraImages();
    RebuildRows();
    s_open.store(true, std::memory_order_relaxed);
    if (IsPauseInQuickMenu()) {
        SetEmulationPaused(true);
    }
    Repaint();
}

void CloseQuickMenu() {
    const bool was_open = s_open.exchange(false, std::memory_order_relaxed);
    SetEmulationPaused(false);
    VideoCore::OverlayMenuState state;
    state.visible = false;
    VideoCore::SetOverlayMenuState(state);
    // Persist the settings the player changed.
    if (was_open) {
        PersistCheats();
        SaveConfig();
    }
}

namespace {
void EnterPage(Page page);
} // namespace

void OpenQuickMenuOnAmiibo() {
    OpenQuickMenu();
    EnterPage(Page::Amiibo);
    Repaint();
    VideoCore::PostOverlayToast("The game is asking for an Amiibo: choose a file", 3500);
}

void ToggleQuickMenu() {
    if (IsQuickMenuOpen()) {
        CloseQuickMenu();
    } else {
        OpenQuickMenu();
    }
}

namespace {

void EnterPage(Page page) {
    s_page = static_cast<int>(page);
    s_selected = 0;
    s_scroll = 0;
    switch (page) {
    case Page::States:
        RefreshSaveStates();
        break;
    case Page::Amiibo:
        RefreshAmiibos();
        break;
    case Page::Camera:
        RefreshCameraImages();
        break;
    default:
        break;
    }
    RebuildRows();
}

void ClampScroll() {
    const int count = static_cast<int>(s_rows.size());
    if (count == 0) {
        s_scroll = 0;
        return;
    }
    const int visible = std::min(count, kMaxVisibleRows);
    s_scroll = std::clamp(s_scroll, s_selected - visible + 1, s_selected);
    s_scroll = std::clamp(s_scroll, 0, std::max(0, count - visible));
}

void MoveSelection(int dir) {
    const int count = static_cast<int>(s_rows.size());
    if (count == 0) {
        return;
    }
    for (int step = 0; step < count; ++step) {
        s_selected = (s_selected + dir + count) % count;
        if (!IsSkippable(s_rows[static_cast<std::size_t>(s_selected)])) {
            break;
        }
    }
    ClampScroll();
}

// Opens entry `index` of the side list.
void EnterNav(int index) {
    const std::vector<NavEntry>& list = NavList();
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    const NavEntry& entry = list[static_cast<std::size_t>(index)];
    if (entry.page == Page::Settings) {
        s_category = entry.category;
    }
    EnterPage(entry.page);
}

int NavIndex() {
    const std::vector<NavEntry>& list = NavList();
    const Page page = CurrentPage();
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const NavEntry& entry = list[static_cast<std::size_t>(i)];
        if (entry.page == page && (page != Page::Settings || entry.category == s_category)) {
            return i;
        }
    }
    return 0;
}

// Moves the home screen's cursor to the nearest cell in a direction.
void MoveCell(int dx, int dy) {
    if (s_cells.empty()) {
        return;
    }
    const Cell& from = s_cells[static_cast<std::size_t>(s_cell)];
    const float fx = from.x + from.w / 2, fy = from.y + from.h / 2;
    int best = -1;
    float best_score = 1e9f;
    for (int i = 0; i < static_cast<int>(s_cells.size()); ++i) {
        if (i == s_cell) {
            continue;
        }
        const Cell& to = s_cells[static_cast<std::size_t>(i)];
        const float tx = to.x + to.w / 2, ty = to.y + to.h / 2;
        // Vertical moves care about rows, so a wide cell above still counts as "above".
        const bool overlaps_x = to.x < from.x + from.w && from.x < to.x + to.w;
        const float along = dx != 0 ? (tx - fx) * dx : (ty - fy) * dy;
        const float across = dx != 0 ? std::abs(ty - fy) * 4.0f
                                     : (overlaps_x ? 0.0f : std::abs(tx - fx));
        if (along <= 0.001f) {
            continue;
        }
        const float score = along + across;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best >= 0) {
        s_cell = best;
    }
}

// Does what the selected cell of the home screen is for.
QuickMenuAction ActivateCell() {
    if (s_cells.empty()) {
        return QuickMenuAction::None;
    }
    const Cell cell = s_cells[static_cast<std::size_t>(s_cell)];
    const auto slot = static_cast<unsigned int>(s_home_slot);
    switch (cell.kind) {
    case CellKind::Resume:
        CloseQuickMenu();
        return QuickMenuAction::Close;
    case CellKind::Layout:
        SetScreenLayoutIndex(cell.index);
        break;
    case CellKind::SaveState:
        if (RequestSaveState(slot)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        break;
    case CellKind::LoadState:
        if (s_slot_status[slot].empty()) {
            VideoCore::PostOverlayToast(SaveStateSlotName(slot) + " is empty");
        } else if (RequestLoadState(slot)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        break;
    case CellKind::GameShare:
        GameShare::SetEnabled(!GameShare::IsEnabled());
        break;
    case CellKind::Fps:
        Settings::values.show_fps = !Settings::values.show_fps.GetValue();
        break;
    case CellKind::Amiibo:
        EnterPage(Page::Amiibo);
        break;
    case CellKind::Cheats:
        EnterPage(Page::Cheats);
        break;
    case CellKind::Settings:
        EnterNav(1); // The live options first; the launcher's pages follow in the side list.
        break;
    case CellKind::Exit:
        CloseQuickMenu();
        return QuickMenuAction::ExitGame;
    }
    return QuickMenuAction::None;
}

bool StepSubList(const QuickMenuNav& nav) {
    if (!nav.page_prev && !nav.page_next) {
        return false;
    }
    const int dir = nav.page_next ? 1 : -1;
    int* target = nullptr;
    int pages = 0;
    switch (CurrentPage()) {
    case Page::Cheats:
        target = &s_cheat_page;
        pages = CheatPageCount();
        break;
    case Page::States:
        target = &s_state_page;
        pages = StatePageCount();
        break;
    case Page::Amiibo:
        target = &s_amiibo_page;
        pages = AmiiboPageCount();
        break;
    case Page::Camera:
        target = &s_camera_page;
        pages = CameraPageCount();
        break;
    default:
        return false;
    }
    *target = (*target + dir + pages) % pages;
    s_selected = 0;
    s_scroll = 0;
    RebuildRows();
    return true;
}

} // namespace

QuickMenuAction UpdateQuickMenu(const QuickMenuNav& nav) {
    if (!IsQuickMenuOpen()) {
        return QuickMenuAction::None;
    }

    if (nav.cancel) {
        s_overwrite_armed = false;
        if (CurrentPage() != Page::Home) {
            const int came_from = s_page;
            EnterPage(Page::Home);
            for (int i = 0; i < static_cast<int>(s_rows.size()); ++i) {
                const Row& row = s_rows[static_cast<std::size_t>(i)];
                if (row.item == Item::Section && row.index == came_from) {
                    s_selected = i;
                    break;
                }
            }
            ClampScroll();
            Repaint();
            return QuickMenuAction::None;
        }
        CloseQuickMenu();
        return QuickMenuAction::Close;
    }

    bool changed = false;

    if (nav.tab_prev != nav.tab_next) {
        const int count = static_cast<int>(NavList().size());
        EnterNav((NavIndex() + (nav.tab_next ? 1 : -1) + count) % count);
        changed = true;
    }

    // The home screen is a grid of cells, not a list.
    if (CurrentPage() == Page::Home) {
        if (nav.left) {
            MoveCell(-1, 0);
        }
        if (nav.right) {
            MoveCell(+1, 0);
        }
        if (nav.up) {
            MoveCell(0, -1);
        }
        if (nav.down) {
            MoveCell(0, +1);
        }
        if (nav.alt) {
            s_home_slot = (s_home_slot + 1) % static_cast<int>(Core::SaveStateSlotCount);
        }
        QuickMenuAction action = QuickMenuAction::None;
        if (nav.confirm) {
            action = ActivateCell();
        }
        if (IsQuickMenuOpen()) {
            Repaint();
        }
        return action;
    }

    changed |= StepSubList(nav);

    if (nav.up) {
        MoveSelection(-1);
        changed = true;
    }
    if (nav.down) {
        MoveSelection(+1);
        changed = true;
    }

    if (changed) {
        s_overwrite_armed = false;
    }

    if (s_rows.empty()) {
        if (changed) {
            Repaint();
        }
        return QuickMenuAction::None;
    }

    const Row row = s_rows[static_cast<std::size_t>(s_selected)];
    if (nav.left && !IsAction(row)) {
        Adjust(row, -1);
        changed = true;
    }
    if (nav.right && !IsAction(row)) {
        Adjust(row, 1);
        changed = true;
    }

    if (row.item == Item::Section && nav.confirm) {
        EnterPage(static_cast<Page>(row.index));
        Repaint();
        return QuickMenuAction::None;
    }

    if ((row.item == Item::SaveHere || row.item == Item::LoadHere) && nav.confirm) {
        const auto slot = static_cast<unsigned int>(s_home_slot);
        if (row.item == Item::SaveHere) {
            if (RequestSaveState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (s_slot_status[slot].empty()) {
            VideoCore::PostOverlayToast(SaveStateSlotName(slot) + " is empty");
        } else if (RequestLoadState(slot)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        Repaint();
        return QuickMenuAction::None;
    }

    if (row.item == Item::SaveStateSlot && (nav.confirm || nav.alt || nav.alt2)) {
        const auto slot = static_cast<unsigned int>(row.index);
        const bool occupied = !s_slot_status[slot].empty();
        if (nav.alt) {
            if (RequestSaveState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (nav.confirm) {
            if (!occupied) {
                VideoCore::PostOverlayToast(SaveStateSlotName(slot) + " is empty");
            } else if (RequestLoadState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (occupied) {
            if (DeleteSaveState(slot)) {
                VideoCore::PostOverlayToast("Deleted " + SaveStateSlotName(slot));
            }
            RefreshSaveStates();
        }
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }

    if (row.item == Item::AmiiboFile && nav.confirm) {
        if (LoadAmiibo(s_amiibos[static_cast<std::size_t>(row.index)].path)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::ScanRealAmiibo && nav.confirm) {
        if (IsRealAmiiboScanning()) {
            CancelRealAmiiboScan();
            VideoCore::PostOverlayToast("Stopped looking for an Amiibo");
            Repaint();
            return QuickMenuAction::None;
        }
        std::string message;
        const bool started = BeginRealAmiiboScan(message);
        VideoCore::PostOverlayToast(message);
        if (started) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::OverwriteAmiibo && nav.confirm) {
        if (!s_overwrite_armed) {
            s_overwrite_armed = true;
            RebuildRows();
            Repaint();
            return QuickMenuAction::None;
        }
        s_overwrite_armed = false;
        std::string message;
        ConfirmAmiiboOverwrite(message);
        VideoCore::PostOverlayToast(message);
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::RemoveAmiibo && nav.confirm) {
        if (IsRealAmiiboActive()) {
            EndRealAmiibo();
            VideoCore::PostOverlayToast("Amiibo removed");
            Repaint();
            return QuickMenuAction::None;
        }
        RemoveAmiibo();
        Repaint();
        return QuickMenuAction::None;
    }

    if (row.item == Item::CameraImage && nav.confirm) {
        SelectCameraImage(s_camera_images[static_cast<std::size_t>(row.index)].path);
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::ClearCamera && nav.confirm) {
        ClearCameraImage();
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }

    if (nav.confirm) {
        if (row.item == Item::Resume) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        if (row.item == Item::ExitGame) {
            CloseQuickMenu();
            return QuickMenuAction::ExitGame;
        }
        Activate(row);
        changed = true;
    }

    if (changed) {
        RebuildRows();
        Repaint();
    }
    return QuickMenuAction::None;
}

// A tap on the overlay: a page in the side list opens it, a row is selected (and acted on when
// it already was), anywhere outside the panel is "back".
QuickMenuAction QuickMenuTouch(float x, float y) {
    if (!IsQuickMenuOpen()) {
        return QuickMenuAction::None;
    }
    // Home: a tap on a cell is that cell's button.
    if (CurrentPage() == Page::Home) {
        for (int i = 0; i < static_cast<int>(s_cells.size()); ++i) {
            const Cell& cell = s_cells[static_cast<std::size_t>(i)];
            if (x >= cell.x && x < cell.x + cell.w && y >= cell.y && y < cell.y + cell.h) {
                s_cell = i;
                const QuickMenuAction action = ActivateCell();
                if (IsQuickMenuOpen()) {
                    Repaint();
                }
                return action;
            }
        }
        return QuickMenuAction::None;
    }
    const std::vector<VideoCore::OverlayHit> hits = VideoCore::GetOverlayHitMap();
    bool inside = false;
    for (const VideoCore::OverlayHit& hit : hits) {
        if (x < hit.x0 || x >= hit.x1 || y < hit.y0 || y >= hit.y1) {
            continue;
        }
        if (hit.kind == 2) {
            inside = true;
        } else if (hit.kind == 1 && hit.index >= 0 &&
                   hit.index < static_cast<int>(NavList().size())) {
            s_overwrite_armed = false;
            EnterNav(hit.index);
            Repaint();
            return QuickMenuAction::None;
        } else if (hit.kind == 0) {
            const int count = static_cast<int>(s_rows.size());
            const int visible = std::min(count, kMaxVisibleRows);
            const int first = std::clamp(s_scroll, 0, std::max(0, count - visible));
            const int target = first + hit.index;
            if (target < 0 || target >= count) {
                return QuickMenuAction::None;
            }
            if (target == s_selected) {
                QuickMenuNav nav{};
                nav.confirm = true;
                return UpdateQuickMenu(nav);
            }
            s_selected = target;
            s_overwrite_armed = false;
            Repaint();
            return QuickMenuAction::None;
        }
    }
    if (!inside) {
        QuickMenuNav nav{};
        nav.cancel = true;
        return UpdateQuickMenu(nav);
    }
    return QuickMenuAction::None;
}

void ShowClosingScreen() {
    VideoCore::OverlayMenuState state;
    state.visible = true;
    state.home = true;
    state.light = GetUiTheme() != 0;
    state.texts.push_back({0.5f, 0.40f, "Closing the game", 1.5f, 0, true});
    state.texts.push_back({0.5f, 0.50f, "Saving and freeing memory. This can take a few seconds.",
                           0.8f, 1, true});
    state.texts.push_back({0.5f, 0.56f, "Please do not turn the console off.", 0.8f, 1, true});
    VideoCore::SetOverlayMenuState(state);
}

} // namespace SwitchFrontend
