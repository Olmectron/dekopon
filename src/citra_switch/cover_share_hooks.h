// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// What cover_share.cpp needs from the rest of the frontend. It cannot include the
// emulator's headers itself (see the note at its top); covers.cpp implements these.
namespace SwitchFrontend {

/// The folder the covers live in (fronts in it, whole wraps in its "full" folder).
std::string CoverShareFolder();
/// A line for the log.
void CoverShareLog(const std::string& text);
/// A cover was stored: the library may look for it.
void CoverShareArrived();
/// A transfer that stored covers has ended: pictures already loaded may be out of date.
void CoverShareFinished();

} // namespace SwitchFrontend
