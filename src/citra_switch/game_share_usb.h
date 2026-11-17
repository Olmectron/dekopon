// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

/// GameShare over a USB cable between two consoles. Carries the same datagrams as the UDP
/// transport. Shared by Dekopon (host of the stream) and Dekopon Link (receiver), which is why
/// it depends on nothing but libnx / libounce.
///
/// Which console ends up as the USB host and which as the USB device is decided by the
/// consoles' USB-C ports, not by us, so both ends offer both roles and use whichever comes up:
///  - device: the usbComms interface (vendor class, 057e:3000), as nxlink over USB does;
///  - host:   usb:hs, looking for exactly that interface on the other console.
namespace GameShareUsb {

/// Opens whatever USB services are granted and starts looking for the other console.
/// `log` receives each service's result code. False when neither role is available.
bool Start(char* log, std::size_t log_size);

void Stop();

/// True while a link to the other console is up.
bool IsConnected();

/// "device", "host", or "" while not connected.
const char* Role();

/// Queues one datagram. False when not connected or the queue is full (the datagram is
/// dropped, like UDP would).
bool Send(const void* data, std::size_t size);

/// Takes one received datagram; returns its size, or 0 when there is none.
std::size_t Receive(void* buffer, std::size_t capacity);

} // namespace GameShareUsb
