// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
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

} // namespace VideoCore::CacheStats
