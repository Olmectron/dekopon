// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// Covers from one console to another, for a console that is kept off the internet.
//
// The receiving console waits; the sending one looks for it. They meet on the network both
// are already on when there is one (a broadcast finds the receiver), and otherwise over a
// direct wireless link the receiver opens just for the transfer (LDN, as GameShare does).
// The files then travel over one TCP connection, each with a checksum and an answer.
namespace SwitchFrontend::CoverShare {

void StartSend();
void StartReceive();
/// Stops whatever is running and waits for it to let go of the network.
void Stop();

bool IsActive();
bool IsSending();
/// A few words for the settings row ("Waiting", "3 / 12", "12 received").
std::string Brief();
/// A sentence on where the transfer stands or how it ended.
std::string Detail();
/// Goes up every time Detail changes to something worth telling the user.
int Serial();
/// The last thing Detail reported was a failure.
bool Failed();

} // namespace SwitchFrontend::CoverShare
