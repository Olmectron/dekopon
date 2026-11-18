// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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
std::atomic<bool> s_started{false};

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

void Watch() {
    using Clock = std::chrono::steady_clock;
    constexpr auto STEP = std::chrono::milliseconds{250};
    const auto seconds = [](Clock::duration d) {
        return std::chrono::duration<double>(d).count();
    };
    std::uint64_t last_beat = 0;
    auto last_change = Clock::now();
    auto last_report = last_change;
    auto woke = last_change;
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(STEP);
        const auto now = Clock::now();
        // This thread asks for nothing but a quarter of a second's sleep. If it was kept
        // away much longer, the hold is not the emulation's own: the system held the whole
        // application (or the core this runs on).
        const auto late = now - woke - STEP;
        woke = now;
        if (late > std::chrono::seconds{1}) {
            LOG_WARNING(Core, "Stall watch: this thread itself was held for {:.1f} s (the whole "
                              "application was, or its core)",
                        seconds(late));
        }
        const std::uint64_t beat = s_beat.load(std::memory_order_relaxed);
        if (s_idle.load(std::memory_order_relaxed) || beat != last_beat) {
            if (reported) {
                LOG_WARNING(Core, "Stall watch: moving again after {:.1f} s. Now: {}",
                            seconds(now - last_change), Describe(beat, 0));
            }
            reported = false;
            last_beat = beat;
            last_change = now;
            continue;
        }
        if (now - last_change < std::chrono::seconds{2} ||
            (reported && now - last_report < std::chrono::seconds{5})) {
            continue;
        }
        reported = true;
        last_report = now;
        LOG_WARNING(Core, "Stall watch: no progress for {:.1f} s. In: {} | before it: {} <- {} <- {} <- {}",
                    seconds(now - last_change), Describe(beat, 0), Describe(beat, 1),
                    Describe(beat, 2), Describe(beat, 3), Describe(beat, 4));
    }
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

void Begin() {
    // Not std::call_once: on this toolchain it throws instead of calling.
    if (s_started.exchange(true)) {
        return;
    }
    try {
        std::thread{Watch}.detach();
    } catch (...) {
        // No thread to spare: the emulation runs without being watched.
        LOG_WARNING(Core, "Stall watch: could not start its thread");
    }
}

} // namespace Common::StallWatch
