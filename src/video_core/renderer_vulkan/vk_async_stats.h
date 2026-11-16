// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include "common/common_types.h"

// Counters for the shader / pipeline compilation path, so that stutter and missing geometry
// can be attributed to a cause from the log instead of guessed. Cheap enough to stay on:
// a few relaxed atomic adds per new shader or pipeline, and one per skipped draw.
namespace Vulkan::AsyncStats {

struct Timed {
    std::atomic<u64> micros{};
    std::atomic<u64> count{};
    std::atomic<u64> max_micros{};
};

/// GLSL generation of programmable vertex shaders. Runs on the render thread.
inline Timed vs_generate;
/// Shader stage compiles (GLSL -> SPIR-V -> module). Run on the shader workers.
inline Timed shader_compile;
/// Driver pipeline builds. Run on the pipeline workers, or on the render thread when the
/// driver can serve them from its cache.
inline Timed pipeline_build;
/// Surface downloads (the game reading back what the GPU drew). Each one waits for the GPU
/// to finish everything submitted so far.
inline Timed surface_download;
/// Pipelines the driver served from its cache on the render thread, without a worker.
inline std::atomic<u64> inline_pipeline_hits{};
/// Draws dropped because their pipeline was not ready (asynchronous mode).
inline std::atomic<u64> skipped_draws{};
/// Draws sent through the CPU vertex path because their vertex shader was not ready.
inline std::atomic<u64> software_vertex_draws{};

class ScopedTimer {
public:
    explicit ScopedTimer(Timed& target_) : target{target_} {}
    ~ScopedTimer() {
        const u64 us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
        target.micros.fetch_add(us, std::memory_order_relaxed);
        target.count.fetch_add(1, std::memory_order_relaxed);
        u64 prev = target.max_micros.load(std::memory_order_relaxed);
        while (us > prev &&
               !target.max_micros.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Timed& target;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};

} // namespace Vulkan::AsyncStats
