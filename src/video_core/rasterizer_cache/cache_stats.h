// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include "common/common_types.h"

// Why the rasterizer cache had to read surfaces back from the GPU. Read and reset by the
// renderer's periodic stats line.
namespace VideoCore::CacheStats {

/// Whole-surface flushes forced by a CPU write of 8 bytes or less into a cached surface
/// (the case the "Skip CPU Write" hack drops).
inline std::atomic<u64> small_write_flushes{};
/// Bytes of surface data those flushes covered.
inline std::atomic<u64> small_write_flush_bytes{};

/// Guest reads of rasterizer-cached memory that asked for a flush, and how many of them were
/// answered from the remembered clean range without reaching the rasterizer.
inline std::atomic<u64> read_flush_requests{};
inline std::atomic<u64> read_flush_skipped{};

/// Where the time of a flush that did reach the rasterizer goes, in microseconds.
inline std::atomic<u64> flush_total_micros{};  ///< Whole GPU::FlushRegion slow path.
inline std::atomic<u64> flush_wait_micros{};   ///< Waiting for the previous frame's copy.
inline std::atomic<u64> flush_record_micros{}; ///< Recording / performing the download.
inline std::atomic<u64> flush_encode_micros{}; ///< Converting and writing to guest memory.

/// Serve read-backs the guest repeats every frame one frame late instead of waiting for the
/// GPU (see RasterizerCache::DownloadSurface). Off by default: it was written while chasing
/// the Majora's Mask sun slowdown, turned out not to be the cause, and makes the guest see
/// that region a frame late. Kept as a switch for future read-back investigations.
inline std::atomic<bool> delayed_readback_enabled{false};

/// Adds the time it lives for to a counter.
class ScopedMicros {
public:
    explicit ScopedMicros(std::atomic<u64>& target_) : target{target_} {}
    ~ScopedMicros() {
        target.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count()),
                         std::memory_order_relaxed);
    }

private:
    std::atomic<u64>& target;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
};

} // namespace VideoCore::CacheStats
