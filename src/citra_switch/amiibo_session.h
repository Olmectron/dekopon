// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace SwitchFrontend {

bool BeginRealAmiiboScan(std::string& message);
void CancelRealAmiiboScan();

bool IsRealAmiiboScanning();
bool IsRealAmiiboActive();

// Polled every frame while a game runs. Besides serving an open scan it follows the game:
// the reader opens by itself when the game starts looking for an Amiibo and closes when it
// stops (see IsAutoAmiiboEnabled).
void UpdateRealAmiibo();

bool IsAutoAmiiboEnabled();
void SetAutoAmiiboEnabled(bool enabled);

void EndRealAmiibo();

bool HasPendingAmiiboOverwrite();

std::string PendingAmiiboOverwriteOwner();

bool ConfirmAmiiboOverwrite(std::string& message);
void DiscardAmiiboOverwrite();

} // namespace SwitchFrontend
