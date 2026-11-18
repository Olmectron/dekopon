// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/logging/log.h"
#include "common/stall_watch.h"

namespace Common::StallWatch {
namespace {

struct Note {
    std::atomic<const char*> what{nullptr};
    std::atomic<std::uint32_t> detail{0};
};

constexpr std::size_t RING = 8;
std::array<Note, RING> s_ring;
std::atomic<std::uint64_t> s_beat{0}; // Notes written so far; the newest is at (beat - 1) % RING.
std::atomic<bool> s_idle{true};
char s_service[48] = "service";

std::string Describe(std::uint64_t beat, std::size_t back) {
    if (beat <= back) {
        return {};
    }
    const Note& note = s_ring[(beat - 1 - back) % RING];
    const char* what = note.what.load(std::memory_order_relaxed);
    if (what == nullptr) {
        return {};
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%.48s (0x%X)", what,
                  static_cast<unsigned>(note.detail.load(std::memory_order_relaxed)));
    return text;
}

} // namespace

void Mark(const char* what, std::uint32_t detail) noexcept {
    const std::uint64_t beat = s_beat.load(std::memory_order_relaxed);
    Note& note = s_ring[beat % RING];
    note.what.store(what, std::memory_order_relaxed);
    note.detail.store(detail, std::memory_order_relaxed);
    s_idle.store(false, std::memory_order_relaxed);
    s_beat.store(beat + 1, std::memory_order_relaxed);
}

void MarkService(const char* name, std::uint32_t command) noexcept {
    // One buffer for the name: an older note about a service shows the newest name. Fine
    // for what this is for.
    std::strncpy(s_service, name, sizeof(s_service) - 1);
    Mark(s_service, command);
}

void MarkIdle() noexcept {
    s_idle.store(true, std::memory_order_relaxed);
}

void Poll() {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::duration d) {
        return std::chrono::duration<double>(d).count();
    };
    static std::uint64_t last_beat = 0;
    static auto last_change = Clock::now();
    static auto last_report = last_change;
    static auto last_poll = last_change;
    static bool reported = false;
    const auto now = Clock::now();
    // The caller comes by every frame. If it was kept away itself, the hold is not the
    // emulation's own: the system held the whole application.
    const auto away = now - last_poll;
    last_poll = now;
    const std::uint64_t beat = s_beat.load(std::memory_order_relaxed);
    const bool idle = s_idle.load(std::memory_order_relaxed);
    if (away > std::chrono::seconds{2} && !idle) {
        LOG_WARNING(Core, "Stall watch: the thread that watches was itself held for {:.1f} s (the "
                          "whole application was)",
                    seconds(away));
    }
    if (idle || beat != last_beat) {
        if (reported) {
            LOG_WARNING(Core, "Stall watch: moving again after {:.1f} s. Now: {}",
                        seconds(now - last_change), Describe(beat, 0));
        }
        reported = false;
        last_beat = beat;
        last_change = now;
        return;
    }
    if (now - last_change < std::chrono::seconds{2} ||
        (reported && now - last_report < std::chrono::seconds{5})) {
        return;
    }
    reported = true;
    last_report = now;
    LOG_WARNING(Core, "Stall watch: no progress for {:.1f} s. In: {} | before it: {} <- {} <- {} <- {}",
                seconds(now - last_change), Describe(beat, 0), Describe(beat, 1), Describe(beat, 2),
                Describe(beat, 3), Describe(beat, 4));
}

} // namespace Common::StallWatch
