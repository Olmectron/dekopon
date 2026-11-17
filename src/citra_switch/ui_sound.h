// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Short synthesised ticks for the launcher: moving, choosing, going back, changing page.
// Generated here, a few tens of milliseconds of sine each; no sound files.
namespace SwitchFrontend::UiSound {

enum class Kind { Move, Select, Back, Page, Count };

/// Opens the audio output for the launcher. Safe to call again.
void Init();

/// Closes it, so the emulator can have the output to itself.
void Exit();

void Play(Kind kind);

bool IsEnabled();
void SetEnabled(bool enabled);

} // namespace SwitchFrontend::UiSound
