// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Its own translation unit: <switch.h> and the emulator's headers do not mix.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <switch.h>

#include "citra_switch/ui_sound.h"

namespace SwitchFrontend::UiSound {
namespace {

constexpr int RATE = 48000;
constexpr std::size_t BUFFER_BYTES = 0x4000; // 85 ms of stereo; every sound fits.
constexpr int SLOTS = 4;

struct Slot {
    AudioOutBuffer out{};
    s16* data{};
};

Slot s_slots[SLOTS];
bool s_ready = false;
std::atomic<bool> s_enabled{true};
int s_next_slot = 0;

struct Tone {
    float from_hz;
    float to_hz;
    float seconds;
    float level;
};

Tone ToneFor(Kind kind) {
    switch (kind) {
    case Kind::Select:
        return {880.0f, 1320.0f, 0.070f, 0.16f};
    case Kind::Back:
        return {700.0f, 440.0f, 0.070f, 0.14f};
    case Kind::Page:
        return {990.0f, 990.0f, 0.045f, 0.12f};
    default: // Move
        return {1180.0f, 1180.0f, 0.028f, 0.10f};
    }
}

/// Fills `out` with the tone; returns the number of bytes used.
std::size_t Render(Kind kind, s16* out) {
    const Tone tone = ToneFor(kind);
    const int frames =
        std::min(static_cast<int>(tone.seconds * RATE), static_cast<int>(BUFFER_BYTES / 4));
    float phase = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        const float hz = tone.from_hz + (tone.to_hz - tone.from_hz) * t;
        phase += 6.2831853f * hz / RATE;
        // A quick attack and a longer fall, so it reads as a tick and does not click.
        const float envelope = std::min(t * 12.0f, 1.0f) * (1.0f - t) * (1.0f - t);
        const s16 sample = static_cast<s16>(std::sin(phase) * envelope * tone.level * 32767.0f);
        out[i * 2] = sample;
        out[i * 2 + 1] = sample;
    }
    return static_cast<std::size_t>(frames) * 4;
}

} // namespace

void Init() {
    if (s_ready) {
        return;
    }
    if (R_FAILED(audoutInitialize())) {
        return;
    }
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return;
    }
    for (Slot& slot : s_slots) {
        slot.data = static_cast<s16*>(aligned_alloc(0x1000, BUFFER_BYTES));
        if (slot.data == nullptr) {
            return; // Without buffers Play() finds nothing to use; the output is closed on Exit.
        }
    }
    s_ready = true;
}

void Exit() {
    const bool was_ready = s_ready;
    s_ready = false;
    if (was_ready || s_slots[0].data != nullptr) {
        audoutStopAudioOut();
        audoutExit();
    }
    for (Slot& slot : s_slots) {
        std::free(slot.data);
        slot.data = nullptr;
    }
}

void Play(Kind kind) {
    if (!s_ready || !s_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    // Take back whatever has finished, then use the next slot that is not queued.
    AudioOutBuffer* released = nullptr;
    u32 count = 0;
    audoutGetReleasedAudioOutBuffer(&released, &count);
    for (int tries = 0; tries < SLOTS; ++tries) {
        Slot& slot = s_slots[s_next_slot];
        s_next_slot = (s_next_slot + 1) % SLOTS;
        bool queued = false;
        if (R_FAILED(audoutContainsAudioOutBuffer(&slot.out, &queued)) || queued) {
            continue;
        }
        std::memset(slot.data, 0, BUFFER_BYTES);
        const std::size_t used = Render(kind, slot.data);
        slot.out = {};
        slot.out.buffer = slot.data;
        slot.out.buffer_size = BUFFER_BYTES;
        slot.out.data_size = used;
        audoutAppendAudioOutBuffer(&slot.out);
        return;
    }
}

bool IsEnabled() {
    return s_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    s_enabled.store(enabled, std::memory_order_relaxed);
}

} // namespace SwitchFrontend::UiSound
