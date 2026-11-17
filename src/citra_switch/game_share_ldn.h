// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

namespace SwitchFrontend::GameShare {

/// Creates the local network the receiver joins. `log` receives every step with its result
/// code: whether the loader grants ldn at all, and whether a Switch 1 can see a network made
/// by a Switch 2, are both unknown until tried on hardware.
bool StartLocalWirelessHost(char* log, std::size_t log_size);

/// Tears the local network down.
void StopLocalWirelessHost();

} // namespace SwitchFrontend::GameShare
