// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <switch.h>

#include <oaknut/horizon_jit.h>

#if defined(__SWITCH2__)
// kaiHEN's JIT provider for libounce's Jit API (linked with -lkaihen).
#include <kaihen.h>
#endif

// Last failing result code and number of failures of the JIT calls below, for
// the frontend's startup diagnostics (stderr goes nowhere on the console).
extern "C" {
unsigned int g_oaknut_horizon_jit_last_rc = 0;
unsigned int g_oaknut_horizon_jit_failures = 0;
// Publications of staged code to the executable mapping (Switch 2): calls, time, bytes.
unsigned long long g_oaknut_horizon_jit_publish_count = 0;
unsigned long long g_oaknut_horizon_jit_publish_micros = 0;
unsigned long long g_oaknut_horizon_jit_publish_bytes = 0;
}

// Horizon only grants a process a handful of JIT objects, far fewer than there are PICA
// shaders in a game. This leads to crashes when objects are exhausted. Instead of giving
// each shader their own object, make them each share larger objects.

namespace oaknut::horizon {
namespace {

void NoteFailure(Result rc) {
    g_oaknut_horizon_jit_last_rc = rc;
    ++g_oaknut_horizon_jit_failures;
}

// Allocations at or above this get a dedicated Jit.
constexpr std::size_t DEDICATED_THRESHOLD = 1024 * 1024;

// Sized to hold every shader a game is likely to compile. May need changed in the future.
constexpr std::size_t CHUNK_SIZE = 16 * 1024 * 1024;

constexpr std::size_t BLOCK_ALIGN = 64;

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// A span of a Chunk. Blocks are kept sorted by offset and tile the chunk with no gaps.
struct Block {
    std::size_t offset;
    std::size_t size;
    bool free;
};

/// One pooled Jit that small allocations are carved out of.
struct Chunk {
    Jit jit{};
    std::uint8_t* rw{};
    std::uint8_t* rx{};
    std::size_t size{};
    std::vector<Block> blocks;
    bool open{};

    ~Chunk() {
        if (open) {
            jitClose(&jit);
        }
    }
};

/// What JitAllocation::handle points at.
struct Allocation {
    Chunk* chunk;  /// Null for a dedicated allocation.
    Jit* jit;      /// Dedicated allocations only.
    std::size_t offset;
};

struct Pool {
    std::mutex mutex;
    std::vector<std::unique_ptr<Chunk>> chunks;
    std::vector<std::unique_ptr<Jit>> dedicated;
    bool closed = false;
};

Pool* live_pool = nullptr;

Pool& GetPool() {
    static Pool* const pool = live_pool = new Pool{};
    return *pool;
}

bool CreateJit(Jit* jit, std::size_t size) {
    if (const Result rc = jitCreate(jit, size); R_FAILED(rc)) {
        NoteFailure(rc);
        // Most likely opened in applet mode.
        std::fprintf(stderr,
                     "oaknut: jitCreate(%zu) failed.\n",
                     size);
        return false;
    }
#if defined(__SWITCH2__)
    // kaiHEN's persistent alias backend: RX is already mapped by jitCreate and
    // stays mapped; rw_addr is a staging buffer, not an alias of RX. Nothing to
    // transition - staged bytes are published per range (see PublishStaged).
    if (jit->rw_addr != jit->src_addr) {
        return true;
    }
#endif
    if (const Result rc = jitTransitionToExecutable(jit); R_FAILED(rc)) {
        NoteFailure(rc);
        std::fprintf(stderr, "oaknut: jitTransitionToExecutable failed.\n");
        jitClose(jit);
        return false;
    }
    return true;
}

Chunk* AddChunk(Pool& pool, std::size_t min_size) {
    auto chunk = std::make_unique<Chunk>();
    chunk->size = std::max(CHUNK_SIZE, AlignUp(min_size, CHUNK_SIZE));
    if (!CreateJit(&chunk->jit, chunk->size)) {
        return nullptr;
    }

    chunk->open = true;
    chunk->rw = static_cast<std::uint8_t*>(jitGetRwAddr(&chunk->jit));
    chunk->rx = static_cast<std::uint8_t*>(jitGetRxAddr(&chunk->jit));
    chunk->blocks.push_back({0, chunk->size, true});
    return pool.chunks.emplace_back(std::move(chunk)).get();
}

/// First-fit.
bool AllocateIn(Chunk& chunk, std::size_t size, std::size_t& offset) {
    for (std::size_t i = 0; i < chunk.blocks.size(); ++i) {
        Block& block = chunk.blocks[i];
        if (!block.free || block.size < size) {
            continue;
        }

        offset = block.offset;
        if (block.size > size) {
            const Block remainder{block.offset + size, block.size - size, true};
            block.size = size;
            block.free = false;
            chunk.blocks.insert(chunk.blocks.begin() + i + 1, remainder);
        } else {
            block.free = false;
        }
        return true;
    }
    return false;
}

void FreeIn(Chunk& chunk, std::size_t offset) {
    auto it = std::lower_bound(chunk.blocks.begin(), chunk.blocks.end(), offset,
                               [](const Block& block, std::size_t off) {
                                   return block.offset < off;
                               });
    if (it == chunk.blocks.end() || it->offset != offset || it->free) {
        return;
    }

    it->free = true;
    const auto next = it + 1;
    if (next != chunk.blocks.end() && next->free) {
        it->size += next->size;
        chunk.blocks.erase(next);
    }
    if (it != chunk.blocks.begin() && (it - 1)->free) {
        (it - 1)->size += it->size;
        chunk.blocks.erase(it);
    }
}

JitAllocation Handle(Chunk* chunk, std::size_t offset) {
    auto* allocation = new Allocation{chunk, nullptr, offset};
    return {allocation, chunk->rw + offset, chunk->rx + offset};
}

void CloseRemainingJits() {
    if (live_pool == nullptr) {
        return;
    }
    std::lock_guard lock{live_pool->mutex};
    live_pool->closed = true;
    for (auto& jit : live_pool->dedicated) {
        jitClose(jit.get());
    }
    live_pool->dedicated.clear();
    live_pool->chunks.clear();
}

[[gnu::constructor(101)]] void InstallJitPoolShutdown() {
    std::atexit(CloseRemainingJits);
}

#if defined(__SWITCH2__)
/// If [begin, end) lies in the staging buffer of `jit`, copies it to the
/// executable mapping through kaiHEN and returns true.
bool PublishStagedIn(Jit& jit, std::uint8_t* begin, std::uint8_t* end) {
    auto* const staging = static_cast<std::uint8_t*>(jit.rw_addr);
    if (jit.rw_addr == jit.src_addr || begin < staging || end > staging + jit.size) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(begin - staging);
    const std::size_t size = static_cast<std::size_t>(end - begin);
    const u64 tick = armGetSystemTick();
    const Result rc = kaihenPublishJitAlias(&jit, offset, begin, size);
    ++g_oaknut_horizon_jit_publish_count;
    g_oaknut_horizon_jit_publish_micros += armTicksToNs(armGetSystemTick() - tick) / 1000;
    g_oaknut_horizon_jit_publish_bytes += size;
    if (R_FAILED(rc)) {
        NoteFailure(rc);
        return true;
    }
    armICacheInvalidate(static_cast<std::uint8_t*>(jit.rx_addr) + offset, size);
    return true;
}
#endif

} // Anonymous namespace

#if defined(__SWITCH2__)
bool PublishStaged(void* begin, void* end) {
    if (live_pool == nullptr) {
        return false;
    }
    auto* const b = static_cast<std::uint8_t*>(begin);
    auto* const e = static_cast<std::uint8_t*>(end);
    std::lock_guard lock{live_pool->mutex};
    if (live_pool->closed) {
        return false;
    }
    for (auto& chunk : live_pool->chunks) {
        if (PublishStagedIn(chunk->jit, b, e)) {
            return true;
        }
    }
    for (auto& jit : live_pool->dedicated) {
        if (PublishStagedIn(*jit, b, e)) {
            return true;
        }
    }
    return false;
}
#endif

JitAllocation JitAllocate(std::size_t size) {
    if (size == 0) {
        return {nullptr, nullptr, nullptr};
    }

    Pool& pool = GetPool();
    std::lock_guard lock{pool.mutex};

    if (size >= DEDICATED_THRESHOLD) {
        auto jit = std::make_unique<Jit>();
        if (!CreateJit(jit.get(), size)) {
            return {nullptr, nullptr, nullptr};
        }
        void* const rw = jitGetRwAddr(jit.get());
        void* const rx = jitGetRxAddr(jit.get());
        Jit* const owned = pool.dedicated.emplace_back(std::move(jit)).get();
        return {new Allocation{nullptr, owned, 0}, rw, rx};
    }

    const std::size_t needed = AlignUp(size, BLOCK_ALIGN);
    for (auto& chunk : pool.chunks) {
        std::size_t offset;
        if (AllocateIn(*chunk, needed, offset)) {
            return Handle(chunk.get(), offset);
        }
    }

    Chunk* const chunk = AddChunk(pool, needed);
    std::size_t offset;
    if (chunk == nullptr || !AllocateIn(*chunk, needed, offset)) {
        return {nullptr, nullptr, nullptr};
    }
    return Handle(chunk, offset);
}

void JitFree(void* handle) {
    if (handle == nullptr) {
        return;
    }

    const std::unique_ptr<Allocation> allocation{static_cast<Allocation*>(handle)};
    Pool& pool = GetPool();
    std::lock_guard lock{pool.mutex};
    if (pool.closed) {
        return;
    }

    if (allocation->chunk == nullptr) {
        const auto it =
            std::find_if(pool.dedicated.begin(), pool.dedicated.end(),
                         [&](const auto& jit) { return jit.get() == allocation->jit; });
        if (it != pool.dedicated.end()) {
            jitClose(it->get());
            pool.dedicated.erase(it);
        }
        return;
    }

    FreeIn(*allocation->chunk, allocation->offset);

    // Hand the Jit area back once nothing is left in it.
    const auto it = std::find_if(pool.chunks.begin(), pool.chunks.end(), [&](const auto& chunk) {
        return chunk.get() == allocation->chunk;
    });
    if (it != pool.chunks.end() && (*it)->blocks.size() == 1 && (*it)->blocks.front().free) {
        pool.chunks.erase(it);
    }
}

} // namespace oaknut::horizon

#if defined(__SWITCH2__)
// Switch 2 / kaiHEN: generated code is written to a staging buffer and only
// reaches the executable mapping when it is published (libkaihen/JIT.md), unlike
// Switch 1 where RW and RX alias the same pages. Every writer already ends with
// oaknut::CodeBlock::invalidate(), which calls __builtin___clear_cache() on the
// RW range and then on the RX range, so the publication point is hooked here by
// replacing libgcc's __clear_cache (what the builtin calls on aarch64): a range
// inside a JIT staging buffer is published; anything else gets the usual cache
// maintenance. This keeps the oaknut and dynarmic submodules unmodified.
extern "C" void __clear_cache(void* begin, void* end) {
    if (end <= begin) {
        return;
    }
    if (oaknut::horizon::PublishStaged(begin, end)) {
        return;
    }
    const std::size_t size =
        static_cast<std::size_t>(static_cast<char*>(end) - static_cast<char*>(begin));
    armDCacheFlush(begin, size);
    armICacheInvalidate(begin, size);
}
#endif
