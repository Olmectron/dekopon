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
//                                 s16 right stick x, s16 right stick y, then optionally
//                                 u32 newest frame id seen (drives congestion control),
//                                 then optionally u8 has motion, float[3] acceleration,
//                                 float[3] angular velocity (libnx six-axis units)
//   3 FRAME   host -> receiver   u32 frame id, u16 chunk index, u16 chunk count, u32 jpeg size,
//                                 u16 width, u16 height, u8 screen, then up to 1200 bytes of JPEG
//   4 WELCOME host -> receiver   u16 width, u16 height, u8 screen   (answer to HELLO)
//   5 COMMAND receiver -> host   u8 what, u8 value: 1 = screen the host shows (0 its own
//                                 layout, 1 top only, 2 bottom only), 2 = host FPS counter
//   6 AUDIO   host -> receiver   u32 seq, u16 frames, then s16 stereo PCM at 32728 Hz
//   7 STATUS  host -> receiver   u8 flags (1 game wants an Amiibo, 2 sound is being sent,
//                                 4 host FPS counter on, 8 host allows sound), u8 host screen,
//                                 u8 the game's frames per second
//   8 AMIIBO  receiver -> host   libnx NfpData (0x298), UID (7), u16 character, u8 variant,
//                                 u8 type, u16 model, u8 series
// HELLO: u8 screen (0 top, 1 bottom, 2 both: top above bottom in 400 x 480), u8 quality,
//        u8 frames per second, then optionally u8 flags (1 = send the sound too)
namespace SwitchFrontend {
struct AmiiboCapture;
}

namespace SwitchFrontend::GameShare {

/// How the receiver reaches this console.
enum class Mode : int {
    Network = 0,       ///< The network the console is already on (Wi-Fi or LAN).
    LocalWireless = 1, ///< Console-to-console: this console creates a local network.
    Usb = 2,           ///< Console-to-console over a USB-C cable.
    Count,
};

/// Settings, persisted by the frontend (GameShare tab).
bool IsEnabled();
void SetEnabled(bool enabled);
/// "Wi-Fi / LAN", "local wireless": for notices.
const char* ModeName();
int GetMode();
void SetMode(int mode);
/// Whether the sound may be sent along with the picture (the receiver still has to ask).
bool IsAudioEnabled();
void SetAudioEnabled(bool enabled);

/// True while a receiver is connected.
bool HasReceiver();

/// Tells the receiver whether the game is looking for an Amiibo, so it can offer its
/// own NFC reader.
void SetAmiiboWanted(bool wanted);

/// An Amiibo the receiver read and sent over, if one arrived since the last call.
bool TakeRemoteAmiibo(AmiiboCapture& out);

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
