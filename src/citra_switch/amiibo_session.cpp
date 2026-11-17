// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <cstdio>

#include "citra_switch/amiibo_convert.h"
#include "citra_switch/amiibo_session.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/sm/sm.h"
#include "video_core/overlay.h"

namespace SwitchFrontend {

namespace {

struct PendingOverwrite {
    bool active{};
    u32 access_id{};
    u64 owner{};
    std::vector<u8> data;
};
PendingOverwrite s_pending;
std::mutex s_pending_mutex;

bool s_have_tag = false;
bool s_handed_over = false;

// Automatic scanning: the reader follows the game. It opens when the game starts
// looking for an Amiibo and closes a moment after the game stops looking, so nobody
// has to go through the in-game menu.
std::atomic<bool> s_auto_enabled{true};
bool s_auto_session = false; // The open session was started by us, not from the menu.
bool s_auto_refused = false; // The reader could not be opened for this search.
std::chrono::steady_clock::time_point s_auto_next_check{};
std::chrono::steady_clock::time_point s_auto_idle_since{};
constexpr auto AUTO_CHECK_INTERVAL = std::chrono::milliseconds(400);
constexpr auto AUTO_CLOSE_AFTER = std::chrono::seconds(3);
std::chrono::steady_clock::time_point s_next_poll{};

bool PollDue() {
    const auto now = std::chrono::steady_clock::now();
    if (now < s_next_poll) {
        return false;
    }
    s_next_poll = now + std::chrono::milliseconds(200);
    return true;
}

std::shared_ptr<Service::NFC::Module::Interface> GetNfc() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return nullptr;
    }
    return system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
}

bool WriteBackToTag(u32 access_id, std::span<const u8> application_area) {
    std::string error;
    switch (WriteAmiiboApplicationArea(access_id, application_area.data(),
                                       application_area.size(), error)) {
    case AmiiboWriteResult::Ok:
        VideoCore::PostOverlayToast("Amiibo saved");
        return true;
    case AmiiboWriteResult::NeedsOverwrite: {
        const std::scoped_lock lock{s_pending_mutex};
        s_pending.active = true;
        s_pending.access_id = access_id;
        s_pending.owner = GetAmiiboAreaOwner();
        s_pending.data.assign(application_area.begin(), application_area.end());
        VideoCore::PostOverlayToast("This Amiibo holds another game's data. Open the menu to "
                                    "overwrite it.",
                                    5000);
        return false;
    }
    case AmiiboWriteResult::Failed:
    default:
        VideoCore::PostOverlayToast("Amiibo write failed: " + error);
        return false;
    }
}

bool GuestIsSearching() {
    auto nfc = GetNfc();
    if (!nfc) {
        return false;
    }
    std::scoped_lock lock{Core::System::GetInstance().Kernel().GetHLELock()};
    return nfc->IsSearchingForAmiibos();
}

void TellGuestTagLeft() {
    if (!s_handed_over) {
        return;
    }
    s_handed_over = false;
    auto nfc = GetNfc();
    if (!nfc) {
        return;
    }
    std::scoped_lock lock{Core::System::GetInstance().Kernel().GetHLELock()};
    if (nfc->IsTagActive()) {
        nfc->RemoveAmiibo();
    }
}

bool HandOverTag(bool announce) {
    auto nfc = GetNfc();
    if (!nfc) {
        return false;
    }

    AmiiboCapture capture;
    std::string error;
    if (!RecaptureAmiibo(capture, error)) {
        VideoCore::PostOverlayToast("Amiibo read failed: " + error);
        return false;
    }

    const std::array<std::uint8_t, 0x21C> plain_tag = BuildPlainTag(capture);

    std::scoped_lock lock{Core::System::GetInstance().Kernel().GetHLELock()};
    if (!nfc->LoadAmiiboFromMemory(plain_tag, WriteBackToTag)) {
        return false;
    }

    s_handed_over = true;
    if (announce) {
        VideoCore::PostOverlayToast(capture.name.empty() ? "Amiibo scanned"
                                                         : capture.name + " scanned");
    }
    return true;
}

} // Anonymous namespace

bool BeginRealAmiiboScan(std::string& message) {
    auto nfc = GetNfc();
    if (!nfc) {
        message = Core::System::GetInstance().IsPoweredOn() ? "The NFC service is unavailable"
                                                            : "No game is running";
        return false;
    }

    if (IsAmiiboSessionOpen()) {
        message = s_have_tag ? "The Amiibo is already on the reader"
                             : "Already looking for an Amiibo";
        return true;
    }

    if (!IsAmiiboReaderAvailable()) {
        message = "This console will not give the emulator its NFC reader";
        return false;
    }

    std::string error;
    if (!StartAmiiboScan(error)) {
        message = error;
        return false;
    }

    s_have_tag = false;
    s_handed_over = false;
    s_next_poll = {};
    message = "Hold the Amiibo against the right stick and leave it there";
    return true;
}

void CancelRealAmiiboScan() {
    EndRealAmiibo();
}

bool IsRealAmiiboScanning() {
    return IsAmiiboSessionOpen() && !s_have_tag;
}

bool IsRealAmiiboActive() {
    return s_have_tag;
}

/// Opens or closes the reader to follow what the game is doing.
void FollowGuest() {
    const auto now = std::chrono::steady_clock::now();
    if (now < s_auto_next_check) {
        return;
    }
    s_auto_next_check = now + AUTO_CHECK_INTERVAL;

    const bool searching = GuestIsSearching();
    if (!IsAmiiboSessionOpen()) {
        s_auto_session = false;
        if (!searching) {
            s_auto_refused = false; // A new search gets a new attempt.
            return;
        }
        if (!s_auto_enabled.load() || s_auto_refused) {
            return;
        }
        std::string message;
        if (BeginRealAmiiboScan(message) && IsAmiiboSessionOpen()) {
            s_auto_session = true;
            s_auto_idle_since = {};
            VideoCore::PostOverlayToast("Hold the Amiibo against the right stick", 4000);
        } else {
            // No reader (or it is busy): say so once, not on every check.
            s_auto_refused = true;
            VideoCore::PostOverlayToast("Amiibo: " + message + ". Amiibo files can still "
                                        "be loaded from the menu.",
                                        5000);
        }
        return;
    }

    // A session we opened closes again once the game has lost interest and no Amiibo is
    // on the reader. One opened from the menu is the user's to close.
    if (!s_auto_session) {
        return;
    }
    if (searching || s_have_tag) {
        s_auto_idle_since = {};
        return;
    }
    if (s_auto_idle_since.time_since_epoch().count() == 0) {
        s_auto_idle_since = now;
    } else if (now - s_auto_idle_since >= AUTO_CLOSE_AFTER) {
        EndRealAmiibo();
    }
}

void UpdateRealAmiibo() {
    FollowGuest();
    if (!IsAmiiboSessionOpen() || !PollDue()) {
        return;
    }

    if (!s_have_tag) {
        AmiiboCapture capture;
        std::string error;
        if (!PollAmiiboScan(capture, error)) {
            if (!error.empty()) {
                VideoCore::PostOverlayToast("Amiibo read failed: " + error);
                s_handed_over = false;
            }
            return;
        }
        s_have_tag = true;
        if (GuestIsSearching()) {
            HandOverTag(true);
        } else {
            VideoCore::PostOverlayToast(capture.name.empty() ? "Amiibo ready"
                                                             : capture.name + " ready");
        }
        return;
    }

    if (!IsAmiiboOnReader()) {
        TellGuestTagLeft();
        DiscardAmiiboOverwrite();
        ResumeAmiiboScanning();
        s_have_tag = false;
        VideoCore::PostOverlayToast("Amiibo lifted");
        return;
    }

    if (GuestIsSearching()) {
        s_handed_over = false;
        HandOverTag(false);
    }
}

bool IsAutoAmiiboEnabled() {
    return s_auto_enabled.load();
}

void SetAutoAmiiboEnabled(bool enabled) {
    s_auto_enabled.store(enabled);
}

void EndRealAmiibo() {
    TellGuestTagLeft();
    s_have_tag = false;
    s_auto_session = false;
    s_auto_idle_since = {};
    DiscardAmiiboOverwrite();
    ReleaseAmiibo();
}

bool HasPendingAmiiboOverwrite() {
    const std::scoped_lock lock{s_pending_mutex};
    return s_pending.active;
}

std::string PendingAmiiboOverwriteOwner() {
    const std::scoped_lock lock{s_pending_mutex};
    if (!s_pending.active) {
        return {};
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(s_pending.owner));
    return s_pending.owner == 0 ? std::string{"unused space"} : std::string{buffer};
}

bool ConfirmAmiiboOverwrite(std::string& message) {
    PendingOverwrite request;
    {
        const std::scoped_lock lock{s_pending_mutex};
        if (!s_pending.active) {
            message = "Nothing is waiting to be written";
            return false;
        }
        request = s_pending;
    }

    std::string error;
    const bool written = OverwriteAmiiboApplicationArea(request.access_id, request.data.data(),
                                                        request.data.size(), error);
    if (!written) {
        message = "Overwrite failed: " + error;
        return false;
    }

    DiscardAmiiboOverwrite();
    message = "Amiibo overwritten and saved";
    return true;
}

void DiscardAmiiboOverwrite() {
    const std::scoped_lock lock{s_pending_mutex};
    s_pending = {};
}

} // namespace SwitchFrontend
