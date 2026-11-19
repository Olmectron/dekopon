// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>

namespace VideoCore {

// Building a game's known shaders before it starts, while asynchronous shader compilation
// is on. The frontend sets the first before a game is loaded and the second when the user
// asks to stop waiting; the renderer keeps the third true while it waits.

/// Wait, when a game starts, for the builds its disk cache queued.
inline std::atomic_bool g_precompile_on_load{true};

/// Stop waiting: what is left is built while the game runs, as without the wait.
inline std::atomic_bool g_precompile_skip{false};

/// The renderer is in that wait now.
inline std::atomic_bool g_precompile_active{false};

} // namespace VideoCore
