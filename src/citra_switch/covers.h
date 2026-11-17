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

/// Covers on the shelf at all; off, every game shows its icon as if it had none.
bool IsCoversShown();
void SetCoversShown(bool shown);
/// Covers stand as cases with a side to them, not as flat pictures.
bool IsShelfCasesOn();
void SetShelfCasesOn(bool on);

/// What the library shows around the shelf.
bool IsShelfArrowsShown();
void SetShelfArrowsShown(bool shown);
bool IsShelfTitleShown();
void SetShelfTitleShown(bool shown);
bool IsShelfReflectionOn();
void SetShelfReflectionOn(bool on);
bool IsShelfFloatingOn();
void SetShelfFloatingOn(bool on);
/// The bar of button hints: 0 buttons, 1 plain text, 2 hidden.
int GetShelfBarStyle();
void SetShelfBarStyle(int style);

/// The look of the launcher and the in-game menu: 0 dark, 1 light.
int GetUiTheme();
void SetUiTheme(int theme);

enum class CoverSource { Off, GameTdb, Count };

int GetCoverSource();
void SetCoverSource(int source);

struct CoverRequest {
    std::string game_code;
};

/// Which files to ask GameTDB for: 0 its small ones (340 x 144 for a whole wrap), 1 the
/// large ones (1616 x 680), falling back to the small ones where there is no large one.
int GetCoverQuality();
void SetCoverQuality(int quality);

/// Downloads the covers that are missing, in the background. Does nothing when a download
/// is already running, or when the source is Off and nobody asked (`manual`). What came of
/// it is in CoverDownloadStatus / CoverDownloadDetail.
/// With `overwrite`, covers already on the card are fetched again (in the quality now
/// chosen) and replaced.
void StartCoverDownload(std::vector<CoverRequest> requests, bool manual = false,
                        bool overwrite = false);
/// How many of these games have a cover on the card, and how many have none.
int CountCoversPresent(const std::vector<CoverRequest>& requests);
int CountCoversMissing(const std::vector<CoverRequest>& requests);
/// The quality the last download that stored something ran at; -1 when unknown.
int CoverStoredQuality();
/// A few words on the current / last download ("2 / 5", "3 new", "No connection"); empty
/// when none has run. The detail is a whole sentence, with the reason for a failure.
std::string CoverDownloadStatus();
std::string CoverDownloadDetail();
void StopCoverDownload();
bool IsCoverDownloadActive();
/// Covers fetched so far by the current / last download, and how many it set out to try.
int CoverDownloadDone();
int CoverDownloadTotal();
/// Increases every time a download stores a new file; the shelf uses it to look again.
int CoverGeneration();

} // namespace SwitchFrontend
