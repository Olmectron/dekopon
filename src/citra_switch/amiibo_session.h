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
// what happens when the game starts looking for an Amiibo is set by AmiiboRequestMode.
void UpdateRealAmiibo();

// What to do when the game asks for an Amiibo.
enum class AmiiboRequestMode {
    ScanReal, // Turn the NFC reader on, and off again once the game stops asking.
    PickFile, // Open the in-game menu on its Amiibo page, to choose a dumped file.
    Nothing,  // Leave it to the in-game menu.
    Count,
};

int GetAmiiboRequestMode();
void SetAmiiboRequestMode(int mode);

void EndRealAmiibo();

bool HasPendingAmiiboOverwrite();

std::string PendingAmiiboOverwriteOwner();

bool ConfirmAmiiboOverwrite(std::string& message);
void DiscardAmiiboOverwrite();

} // namespace SwitchFrontend
