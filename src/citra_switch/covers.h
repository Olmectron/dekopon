// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "citra_switch/ui_views.h"

// Cover artwork for the library shelf.
//
// Files live in <user dir>/covers:
//   covers/full/<ID>.jpg|png   the whole wrap: back, spine and front (the shelf can turn it)
//   covers/<ID>.jpg|png        the front only
// where <ID> is the game's four-character code (the last part of its product code, e.g. AQEE
// for CTR-P-AQEE - what GameTDB names its files after) or its 16-digit title ID.
//
// They can be copied there by hand, sent from a PC (tools/covers/fetch_covers.py), or
// downloaded by the console itself when it has internet access.
namespace SwitchFrontend {

/// The game's four-character code, read from the ROM's NCCH header. Empty when the file is
/// not a cartridge / NCCH image or has no retail product code.
std::string ReadGameCode(const std::string& path);

struct CoverArt {
    Ui::Image image;
    bool full_wrap{};
};

/// The cover for a game, or null. Decoding happens here, at most `budget` files per call
/// across all games (pass the same counter for a whole frame) so a shelf full of new covers
/// does not stall the menu; a game over budget simply reports null until a later frame.
std::shared_ptr<const CoverArt> GetCover(const std::string& game_code, std::uint64_t program_id,
                                         int& budget);

/// Creates the covers folders and a readme saying what goes in them.
void EnsureCoverFolders();

/// Forgets what is cached, including "this game has no cover" answers.
void ResetCoverCache();

/// How the library lays the games out: an index into Ui::ShelfStyle.
int GetLibraryStyle();
void SetLibraryStyle(int style);

enum class CoverSource { Off, GameTdb, Count };

int GetCoverSource();
void SetCoverSource(int source);

struct CoverRequest {
    std::string game_code;
};

/// Downloads the covers that are missing, in the background. Does nothing when the source is
/// Off or a download is already running. Failures are silent: a console without internet
/// access simply gets none.
void StartCoverDownload(std::vector<CoverRequest> requests);
void StopCoverDownload();
bool IsCoverDownloadActive();
/// Covers fetched so far by the current / last download, and how many it set out to try.
int CoverDownloadDone();
int CoverDownloadTotal();
/// Increases every time a download stores a new file; the shelf uses it to look again.
int CoverGeneration();

} // namespace SwitchFrontend
