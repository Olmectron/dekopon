// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common {

// Process-wide memory accounting from the Horizon kernel.
std::uint64_t GetHorizonMemoryUsed();
std::uint64_t GetHorizonMemoryTotal();

// What is left of the heap: its size less what malloc has handed out. The kernel's
// figures above do not say this, the whole heap is claimed when the application starts.
// 0 when it is not known (not on a console).
std::uint64_t GetHorizonHeapFree();

} // namespace Common
