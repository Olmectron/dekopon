// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

// Short synthesised ticks for the launcher: moving, choosing, going back, changing page.
// Generated here, a few tens of milliseconds of sine each; no sound files.
namespace SwitchFrontend::UiSound {

enum class Kind { Move, Select, Back, Page, Count };

/// Opens the audio output for the launcher. Safe to call again.
void Init();

/// Closes it, so the emulator can have the output to itself.
void Exit();

void Play(Kind kind);

/// A longer sound, played once: a game's banner jingle. `stereo` is interleaved, `rate`
/// frames a second; it is copied. Replaces whichever was playing.
void PlayClip(const short* stereo, std::size_t frames, int rate);
void StopClip();

bool IsEnabled();
void SetEnabled(bool enabled);

} // namespace SwitchFrontend::UiSound
