// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// libnx JIT wrapper for oaknut
#pragma once

#include <cstddef>

namespace oaknut::horizon {

// `handle` owns the underlying libnx Jit
struct JitAllocation {
    void* handle;
    void* rw;
    void* rx;
};

// Allocates `size` bytes of dual-mapped RW/RX memory via libnx jitCreate.
JitAllocation JitAllocate(std::size_t size);

// Releases an allocation previously returned by JitAllocate.
void JitFree(void* handle);

#if defined(__SWITCH2__)
// Switch 2 (kaiHEN): `rw` is a staging buffer, not an alias of `rx`. Copies the staged
// bytes in [begin, end) to the executable mapping and invalidates its instruction cache.
// Returns false if the range is not inside a JIT staging buffer.
bool PublishStaged(void* begin, void* end);
#endif

} // namespace oaknut::horizon
