// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

// Says what the emulation was doing when it stops making progress.
//
// Some games freeze for twenty or thirty seconds now and then and carry on as if nothing
// had happened, and the log has nothing for that stretch: whatever holds the emulation
// thread holds its logging too. So the emulation thread leaves a note of what it is about
// to do at the points it passes thousands of times a second (a system call, a service
// request, a run of guest code, the JIT's housekeeping), and another thread looks at those
// notes. When they stop changing for two seconds it writes the last ones to the log, and
// again when they start moving.
//
// A note is a pointer and a number stored without locking: a few nanoseconds each.
namespace Common::StallWatch {

/// What the thread is about to do. `what` must be a string literal.
void Mark(const char* what, std::uint32_t detail = 0) noexcept;

/// The same for a request to a service: its name is copied.
void MarkService(const char* name, std::uint32_t command) noexcept;

/// Between runs of the emulation (paused, a menu open): nothing to watch.
void MarkIdle() noexcept;

/// Looks at the notes; writes to the log when they have stopped changing for two seconds,
/// and again when they move. Call it often from a thread other than the emulation's (the
/// frontend's main loop does, every frame). It has no thread of its own: one was tried,
/// and with it the graphics driver's cache thread crashed at the start of every game.
void Poll();

/// Notes `what` now and `after` when it goes out of scope; null `after` means idle.
class Scope {
public:
    Scope(const char* what, std::uint32_t detail, const char* after_) noexcept : after{after_} {
        Mark(what, detail);
    }
    ~Scope() {
        if (after != nullptr) {
            Mark(after);
        } else {
            MarkIdle();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* after;
};

} // namespace Common::StallWatch
