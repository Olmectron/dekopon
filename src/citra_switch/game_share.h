// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core {
class System;
}

namespace SwitchFrontend {
struct InputState;
}

// Prototype: share one of the 3DS screens with another device on the local network and take its
// input back, in the spirit of the Switch 2's GameShare.
//
// Protocol (UDP, port 28300, little endian, every packet starts with "DKGS" + a type byte):
//   1 HELLO   receiver -> host   u8 screen (0 top, 1 bottom), u8 jpeg quality, u8 max fps
//                                 Sent once per second; it is also the keep-alive.
//   2 INPUT   receiver -> host   u32 seq, u32 buttons (bit = SwitchFrontend::InputButton),
//                                 s16 stick x, s16 stick y, u8 touching, u16 touch x, u16 touch y
//                                 (touch in bottom-screen pixels, 320x240), then optionally
//                                 s16 right stick x, s16 right stick y
//   3 FRAME   host -> receiver   u32 frame id, u16 chunk index, u16 chunk count, u32 jpeg size,
//                                 u16 width, u16 height, u8 screen, then up to 1200 bytes of JPEG
//   4 WELCOME host -> receiver   u16 width, u16 height, u8 screen   (answer to HELLO)
namespace SwitchFrontend::GameShare {

/// Starts listening for a receiver. Safe to call again while running.
void Start();

/// Stops streaming and listening.
void Stop();

/// Called from the emulation thread between time slices: asks the renderer for the next frame
/// of the shared screen when a receiver is connected and the previous one has been handed over.
void Tick(Core::System& system);

/// Adds the receiver's buttons, stick and touch to the local input (main thread).
void MergeRemoteInput(InputState& state);

} // namespace SwitchFrontend::GameShare
